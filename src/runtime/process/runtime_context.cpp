// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Runtime context: manager construction with dependency wiring

#include <thread>
#include <chrono>
#include "runtime/process/runtime_context.hpp"
#include "winapi/system/shell32/app_user_model_id.hpp"
#include "capsule/vfs/image_mount_store.hpp"
#include "capsule/vfs/iso_filesystem.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/x64_high_arena.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "graphics/metal/metal_surface_presenter.hpp"
#include "graphics/metal/metal_device.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_session.hpp"
#include "winapi/kernel32/system/hardware_description.hpp"
#include "winapi/kernel32/system/machine_identity_registry.hpp"
#include "winapi/advapi32/security/credentials.hpp"
#include "winapi/system/dbghelp/dbghelp.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "core/sync/ntsync.hpp"
#include <cstdlib>
#include <mutex>
#include <set>
#include <filesystem>
#include <utility>

namespace vitreo::cli {

namespace fs = std::filesystem;

namespace {

constexpr GuestAddr kWindowsX64UserCeiling = 0x00007FFF'FFFEFFFFULL;

GuestAddr runtime_user_ceiling(const GuestMemory& memory, bool is_64bit,
                               bool large_address_aware) noexcept {
    if (!is_64bit) {
        return large_address_aware ? 0xFFFE'FFFFULL : 0x7FFE'FFFFULL;
    }
    const GuestAddr reserved_ceiling = memory.maximum_user_address();
    return reserved_ceiling != 0 ? reserved_ceiling
                                 : kWindowsX64UserCeiling;
}

GuestAddr runtime_thunk_base(const GuestMemory& memory,
                             bool is_64bit) noexcept {
    if (!is_64bit) return THUNK_REGION_BASE_X86;
    const GuestAddr ceiling = runtime_user_ceiling(memory, true, true);
    // Full Windows ceiling: the guest address is used directly as a host
    // address, so the thunk region has to land where the host has room. The
    // anchor is probed once and shared with the dynamic-loader and synthetic
    // module arenas so all three stay in their established relative order.
    if (ceiling == kWindowsX64UserCeiling) {
        return static_cast<GuestAddr>(::vitreo::memory::x64_thunk_base());
    }
    // Preserve the existing placement policy: the synthetic thunk arena sits
    // 20 GiB below the top of the available x64 user window.
    constexpr GuestAddr kThunkHeadroom = 0x00000005'00000000ULL;
    return ((ceiling + 1) - kThunkHeadroom) & ~GuestAddr{0xFFFF};
}

class OpticalHostFileSystem final : public capsule::HostFileSystem {
public:
    explicit OpticalHostFileSystem(std::string root)
        : HostFileSystem(std::move(root)) {}
    bool is_optical() const noexcept override { return true; }
};

} // namespace

RuntimeContext::RuntimeContext(GuestMemory& memory, std::string_view pe_path,
                               std::string_view capsule_name, bool is_64bit,
                               bool large_address_aware,
                               bool console_attached,
                               ipc::ProcessSession* process_session,
                               bool elevated,
                               std::string elevation_credential,
                               ipc::WorkerTokenContext token_context,
                               platform::ElevationAuthorizer* elevation_authorizer,
                               std::span<const winapi::Handle> inherited_handles)
    // ---- Core infrastructure ----
    : elevation_state{
          elevated,
          std::move(elevation_credential),
          token_context == ipc::WorkerTokenContext::LocalSystemService
              ? winapi::GuestTokenOrigin::LocalSystemService
              : token_context == ipc::WorkerTokenContext::RestrictedService
                    ? winapi::GuestTokenOrigin::RestrictedService
                    : elevated
                          ? winapi::GuestTokenOrigin::AuthorizedAdministrator
                          : winapi::GuestTokenOrigin::User}
    , handle_table{inherited_handles}
    , vfs{}
    , registry{}
    , thunk_registry{runtime_thunk_base(memory, is_64bit)}
    // ---- kernel32 managers ----
    , process_mgr{}
    , console_mgr{handle_table, console_attached}
    , file_mgr{vfs, handle_table, memory}
    , sysinfo_mgr{&memory}
    , cs_mgr{memory}
    , fls_mgr{}
    , fmm_mgr{handle_table, memory,
              runtime_user_ceiling(memory, is_64bit,
                                   large_address_aware)}
    , fsearch_mgr{}
    , time_mgr{}
    , vmm{memory,
          runtime_user_ceiling(memory, is_64bit, large_address_aware)}
    , vqm{memory, &vmm}
    , heap_mgr{vmm, memory, handle_table}
    , com_task_memory_mgr{heap_mgr}
    , gl_mgr{heap_mgr}
    , heap_ext{heap_mgr, handle_table}
    , tls_mgr{}
    , thread_mgr{handle_table}
    , sync_mgr{handle_table}
    , env_mgr{pe_path, memory}
    , module_mgr{thunk_registry}
    , ndr_proxy_registrar{registry, module_mgr, thunk_registry}
    , locale_str_mgr{}
    , file_ops_mgr{memory, vfs}
    , k32_ext_mgr{memory, vfs}
    // ---- msvcrt managers ----
    , crt_init_mgr{}
    , crt_str_mgr{}
    , crt_math_mgr{}
    , crt_stdio_mgr{}
    , crt_data{}
    , crt_mem_mgr{memory}
    , crt_environment_mgr{env_mgr, memory, crt_mem_mgr}
    , crt_locale_mgr{memory, crt_mem_mgr}
    , crt_time_mgr{memory, crt_mem_mgr, crt_locale_mgr}
    // ---- ntdll managers ----
    , ntdll_mgr{}
    , ntdll_ext_mgr{}
    // ---- advapi32 managers ----
    , reg_thunks{registry, handle_table}
    , sec_stubs{handle_table, &gl_mgr, &elevation_state}
    , scm_mgr{registry, vfs}
    // ---- gdi32 ----
    , gdi_mgr{handle_table}
    // ---- windowing infrastructure ----
    , hwnd_mgr{}
    , msg_queue{}
    , cocoa_bridge{}
    // ---- user32 managers ----
    , wclass_mgr{}
    , window_mgr{hwnd_mgr, wclass_mgr, cocoa_bridge, msg_queue}
    , show_mgr{hwnd_mgr, cocoa_bridge, msg_queue}
    , msgloop_mgr{hwnd_mgr, msg_queue, cocoa_bridge}
    , msgsend_mgr{hwnd_mgr, msg_queue, cocoa_bridge}
    , dwp_mgr{hwnd_mgr, msg_queue, memory}
    , paint_mgr{&hwnd_mgr}
    , resource_mgr{memory}
    , dialog_mgr{hwnd_mgr, wclass_mgr, cocoa_bridge, msg_queue, memory}
    , u32_ext_mgr{memory, vfs, hwnd_mgr, wclass_mgr, cocoa_bridge, msg_queue}
    // ---- ole32 / shell32 / version / ws2_32 / mswsock ----
    , com_mgr{com_task_memory_mgr}
    , combase_mgr{memory, heap_mgr,
                  winapi::detect_host_globalization_preferences()}
    , dwrite_mgr{memory, is_64bit, &vfs, &gdi_mgr}
    , d2d1_mgr{memory, is_64bit, &gdi_mgr}
    , urlmon_mgr{memory}
    , shell_mgr{vfs, cocoa_bridge, memory, process_mgr,
                com_task_memory_mgr, &elevation_state}
    , ver_mgr{}
    , lz32_mgr{vfs}
    , imagehlp_mgr{file_mgr}
    , ws2_mgr{}
    , mswsock_mgr{}
    // ---- comctl32 ----
    , comctl32_mgr{}
    // ---- gdiplus ----
    , gdiplus_mgr{heap_mgr}
    // ---- oleaut32 ----
    , oleaut32_mgr{}
    // ---- authz / sspicli / wkscli / netutils ----
    , authz_mgr{}
    , sspicli_mgr{}
    , wkscli_mgr{}
    , netutils_mgr{}
    , bcrypt_mgr{}
    , wsock32_mgr{}
    // ---- msimg32 / winspool / imm32 / iphlpapi / opengl32 ----
    , msimg32_mgr{}
    , msi_mgr{}
    , winspool_mgr{}
    , imm32_mgr{}
    , iphlpapi_mgr{}
    , dnsapi_mgr{}
    , opengl32_mgr{}
    , glu32_mgr{}
    , msvfw32_mgr{}
    , oledlg_mgr{}
    , audio_bridge{windowing::create_audio_bridge()}
    , wave_output{windowing::create_wave_output()}
    , winmm_mgr{memory, *wave_output, vfs}
    , graphics_presentation{}
    , graphics_surface_presenter{graphics::create_metal_surface_presenter(cocoa_bridge)}
    , ddraw_mgr{graphics_presentation}
    , d3d9_mgr{}
    , dxgi_mgr{memory, is_64bit}
    , d3d11_mgr{memory, is_64bit,
                graphics::MetalDevice::instance(), dxgi_mgr}
    , d3d12_mgr{memory, is_64bit, graphics::MetalDevice::instance()}
    , dcomp_mgr{memory, is_64bit, d3d11_mgr}
    , audio_renderer{audio::create_audio_renderer()}
    , audio_capture{audio::create_audio_capture()}
    , dsound_mgr{audio_renderer.get()}
    // ---- Service groups (aliases to existing members) ----
    , registry_svc{}
    , process_services{process_mgr, thread_mgr}
    , memory_services{memory, vmm, heap_mgr, gl_mgr, heap_ext}
    , file_services{file_mgr, fmm_mgr, fsearch_mgr, console_mgr, file_ops_mgr}
    , module_services{module_mgr, k32_ext_mgr, crt_data}
    , sync_services{sync_mgr, cs_mgr, tls_mgr, fls_mgr}
    , ui_services{hwnd_mgr, msg_queue, cocoa_bridge, wclass_mgr, window_mgr, paint_mgr, gdi_mgr}
    , capsule_services{vfs, registry, env_mgr}
{
    // Opportunistic guest VA (CRT arenas, D3D Map scratch, …) must honor the
    // same user-mode ceiling as VirtualAlloc / MapViewOfFile.
    memory.set_maximum_user_address(
        runtime_user_ceiling(memory, is_64bit, large_address_aware));
    winapi::ThreadSuspendRegistry::instance().bind_guest_memory(&memory);

    // USER window handles identify desktop objects across process boundaries;
    // APIs such as GetWindowThreadProcessId accept a foreign process's HWND.
    // Allocate from the capsule-scoped desktop counter so the browser, GPU,
    // and renderer processes cannot publish colliding window identities.
    hwnd_mgr.set_handle_allocator([] {
        return static_cast<windowing::HWND>(
            ipc::hwnd_registry_allocate_handle());
    });
    // IsWindowUnicode: the window's charset (RegisterClassA/W, or the latest
    // SetWindowLongPtrA/W(GWLP_WNDPROC)) decides DispatchMessageA/W character
    // translation. A window this process does not know is treated as Unicode.
    msgloop_mgr.set_window_unicode_query([this](windowing::HWND hwnd) {
        const windowing::WindowData* window = hwnd_mgr.find(hwnd);
        return window == nullptr || window->unicode;
    });

    shell_mgr.set_thunk_registry(thunk_registry);
    shell_mgr.set_local_memory(&gl_mgr);
    winapi::set_dbghelp_module_lookup(winapi::DbghelpModuleLookup{
        [this](u64 address) -> u64 {
            const auto found = module_mgr.module_for_address(address);
            return found.info ? found.info->base_address : 0;
        },
        [this](u64 pc) -> u64 {
            return module_mgr.lookup_runtime_function_entry(pc);
        },
        [this](u64 handle) -> bool {
            return handle <= 0xFFFFFFFFull &&
                   handle_table.is_valid(static_cast<winapi::Handle>(handle));
        }});
    winapi::set_dbghelp_environment_lookup(
        [this](std::string_view name) -> std::optional<std::string> {
            auto value = env_mgr.get_variable(name);
            if (value.is_err()) return std::nullopt;
            return value.value();
        });
    shell_mgr.set_environment_lookup(
        [this](std::string_view name) -> std::optional<std::string> {
            auto value = env_mgr.get_variable(name);
            if (value.is_err()) return std::nullopt;
            return value.value();
        });
    shell_mgr.set_handle_closer(
        [this](u32 handle) { (void)handle_table.close(handle); });
    shell_mgr.set_executable_path_provider(
        [this] { return std::string(module_mgr.main_module_path()); });
    shell_mgr.set_comctl32_manager(comctl32_mgr);
    // The notification area: Shell_NotifyIcon icons are menu-bar status
    // items, their clicks the owner window's callback messages.
    {
        auto& icons = shell_mgr.notify_icons();
        icons.set_window_check([this](u32 hwnd) {
            return hwnd_mgr.find(static_cast<windowing::HWND>(hwnd)) != nullptr;
        });
        icons.set_poster([this](u32 hwnd, u32 message, u64 wparam, u64 lparam) {
            windowing::MSG msg{};
            msg.hwnd = static_cast<windowing::HWND>(hwnd);
            msg.message = message;
            msg.wParam = wparam;
            msg.lParam = lparam;
            (void)msg_queue.post(msg);
        });
        icons.set_icon_image(
            [this](u32 hicon) -> std::optional<winapi::NotifyIconManager::IconImage> {
                const auto planes = resource_mgr.icon_planes(hicon);
                if (!planes) return std::nullopt;
                winapi::NotifyIconManager::IconImage image;
                image.rgba = winapi::icon_display_rgba(*planes);
                image.width = planes->width;
                image.height = planes->height;
                return image;
            });
        winapi::NotifyIconManager::Presenter presenter;
        presenter.present = [this](u64 key, const windowing::NotifyIconPresentation& view) {
            cocoa_bridge.present_notify_icon(key, view);
        };
        presenter.withdraw = [this](u64 key) { cocoa_bridge.withdraw_notify_icon(key); };
        presenter.frame = [this](u64 key) { return cocoa_bridge.notify_icon_frame(key); };
        presenter.show_balloon = [this](u64 key, const windowing::NotifyIconBalloon& balloon) {
            cocoa_bridge.show_notify_icon_balloon(key, balloon);
        };
        presenter.hide_balloon = [this](u64 key) { cocoa_bridge.hide_notify_icon_balloon(key); };
        icons.set_presenter(std::move(presenter));
        cocoa_bridge.set_notify_icon_input_handler(
            [&icons](u64 key, windowing::NotifyIconInput input, i32 x, i32 y) {
                icons.on_input(key, input, x, y);
            });
        cocoa_bridge.set_notify_icon_reopen_target([&icons] { return icons.reopen_target(); });
    }
    // The Dock groups by AppUserModelID, as the taskbar does: the dock
    // target window's ID, else this process's explicit ID, else its image.
    cocoa_bridge.set_app_user_model_id_resolver([this](windowing::HWND hwnd) {
        return winapi::shell::effective_app_user_model_id(
            hwnd, shell_mgr.explicit_app_user_model_id(), module_mgr.main_module_path());
    });
    comctl32_mgr.set_resource_manager(&resource_mgr);
    u32_ext_mgr.set_resource_manager(&resource_mgr);
    comctl32_mgr.set_hwnd_manager(&hwnd_mgr);
    shell_mgr.set_executable_launcher(
        [this](const std::string& path, const std::string& parameters,
               const std::string& directory, i32 show_cmd,
               bool request_elevation, u32& process_handle, u32& error) {
            winapi::Handle handle = winapi::NULL_HANDLE;
            const bool launched = k32_ext_mgr.launch_shell_executable(
                path, parameters, directory, show_cmd, request_elevation,
                handle, error);
            process_handle = handle;
            return launched;
        });

    // Ensure CloseHandle properly releases VFS-backed file handles and pipe endpoints.
    // Note: kernel32 exports CloseHandle via HandleTable, so this callback is
    // the single point of cleanup for most handle types.
    handle_table.set_close_callback(
        [this](winapi::Handle h, winapi::HandleType type, void* object) {
            if (type == winapi::HandleType::File) {
                k32_ext_mgr.on_directory_watch_close(h);
            }
            if (type == winapi::HandleType::File ||
                type == winapi::HandleType::Pipe ||
                type == winapi::HandleType::Device) {
                k32_ext_mgr.on_iocp_handle_close(h);
            }
            if (type == winapi::HandleType::Device) {
                // An HTTP Server API request queue; the device-name string
                // is still released by FileManager below.
                (void)httpapi_mgr.on_handle_close(h);
            }
            if (type == winapi::HandleType::File) {
                ntdll_ext_mgr.forget_directory_query(h);
            }
            if (type == winapi::HandleType::FileMapping) {
                return fmm_mgr.on_handle_close(h, type);
            }
            if (type == winapi::HandleType::Event ||
                type == winapi::HandleType::Mutex ||
                type == winapi::HandleType::Semaphore) {
                return sync_mgr.on_handle_close(h, type);
            }
            if (type == winapi::HandleType::Token) {
                return sec_stubs.on_handle_close(h);
            }
            if (type == winapi::HandleType::Job) {
                return k32_ext_mgr.on_job_handle_close(h);
            }
            if (type == winapi::HandleType::IoCompletionPort) {
                return k32_ext_mgr.on_iocp_port_close(h);
            }
            if (type == winapi::HandleType::SharedResource) {
                winapi::release_shared_resource_handle_object(object);
                return true;
            }
            if (type == winapi::HandleType::Thread) {
                k32_ext_mgr.forget_external_primary_thread(h);
                sec_stubs.on_thread_handle_close(h);
                thread_mgr.on_thread_handle_close(object);
            }
            if (type == winapi::HandleType::File &&
                ntdll_ext_mgr.owns_file_handle(h)) {
                return ntdll_ext_mgr.on_handle_close(h, type);
            }
            return file_mgr.on_handle_close(h, type, object);
        }
    );

    // Enable dynamic DLL loading (LoadLibraryExW) from capsule VFS.
    module_mgr.attach_dynamic_loader(memory, vfs, is_64bit);
    // A 32-bit guest sees System32 through WOW64 file-system redirection.
    vfs.set_wow64_view(!is_64bit);
    // Every built-in DLL is a file at the path GetModuleFileNameW reports
    // (System32, and SysWOW64 for x86), loaded or not, as on Windows.
    vfs.set_generated_file_source(
        [this](std::string_view path) {
            return module_mgr.builtin_dll_file(path);
        },
        [this](std::string_view directory) {
            return module_mgr.builtin_dll_files_in(directory);
        });

    // Console bridge: FileManager delegates console-handle writes
    file_mgr.set_console_manager(console_mgr);

    // Wire FileManager into FileMappingManager for file-backed mappings
    fmm_mgr.set_file_manager(&file_mgr);
    file_mgr.set_file_mapping_manager(&fmm_mgr);
    vqm.set_file_mapping_manager(&fmm_mgr);
    vqm.set_module_manager(&module_mgr);

    // Wire VFS into FileSearchManager for path translation
    fsearch_mgr.set_vfs(&vfs);

    // Wire guest memory into graphics managers for vtable/struct writes.
    ddraw_mgr.set_memory(&memory, is_64bit);
    ddraw_mgr.set_surface_presenter(graphics_surface_presenter.get());
    d3d11_mgr.set_presentation_view_resolver(
        [this](u64 hwnd) {
            return cocoa_bridge.native_presentation_view(
                static_cast<windowing::HWND>(hwnd));
        });
    d3d11_mgr.set_foreign_surface_view_resolver(
        [this](u64 hwnd) {
            return cocoa_bridge.native_foreign_presentation_view(
                static_cast<windowing::HWND>(hwnd));
        });
    // D3D12 swap chains (on a command queue) use the same window and
    // semaphore services as the D3D11 ones.
    winapi::D3D12Manager::PresentationServices d3d12_presentation;
    d3d12_presentation.presentation_view = [this](u64 hwnd) {
        return cocoa_bridge.native_presentation_view(
            static_cast<windowing::HWND>(hwnd));
    };
    d3d12_presentation.dxgi = &dxgi_mgr;
    // DXGI sizes a swap chain from its window's client area, resizes the
    // window for ResizeTarget and finds the output under it. A window of
    // another guest process is known through the HWND registry.
    {
        winapi::D3D11Manager::WindowServices services;
        services.client_size = [this](u64 hwnd)
            -> std::optional<std::pair<u32, u32>> {
            const auto handle = static_cast<windowing::HWND>(hwnd);
            if (const windowing::WindowData* window = hwnd_mgr.find(handle)) {
                return std::pair<u32, u32>{
                    static_cast<u32>(std::max(0, window->client_rect.width())),
                    static_cast<u32>(std::max(0, window->client_rect.height()))};
            }
            const auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd));
            if (!remote || remote->owner_host_pid == 0) return std::nullopt;
            return std::pair<u32, u32>{
                static_cast<u32>(std::max(0, remote->client_w)),
                static_cast<u32>(std::max(0, remote->client_h))};
        };
        services.resize_client = [this](u64 hwnd, u32 width, u32 height) {
            const auto handle = static_cast<windowing::HWND>(hwnd);
            const windowing::WindowData* window = hwnd_mgr.find(handle);
            if (window == nullptr) return false;
            // Keep the window's own non-client insets: the new outer size is
            // the requested client size plus what the frame takes now.
            const i32 outer_w = static_cast<i32>(width) +
                window->rect.width() - window->client_rect.width();
            const i32 outer_h = static_cast<i32>(height) +
                window->rect.height() - window->client_rect.height();
            constexpr u32 kSwpNoMove = 0x0002;
            constexpr u32 kSwpNoZOrder = 0x0004;
            constexpr u32 kSwpNoActivate = 0x0010;
            return u32_ext_mgr.apply_set_window_pos(
                handle, 0, 0, 0, std::max(0, outer_w), std::max(0, outer_h),
                kSwpNoMove | kSwpNoZOrder | kSwpNoActivate, true);
        };
        services.window_rect = [this](u64 hwnd)
            -> std::optional<std::array<i32, 4>> {
            const windowing::WindowData* window =
                hwnd_mgr.find(static_cast<windowing::HWND>(hwnd));
            if (window == nullptr) return std::nullopt;
            return std::array<i32, 4>{window->rect.left, window->rect.top,
                                      window->rect.right, window->rect.bottom};
        };
        // DXGI exclusive full-screen goes through the display-mode owner
        // DirectDraw and D3D9 exclusive mode use (user32's virtual display
        // mode) and, like theirs, is hosted as a macOS Space.
        services.enter_exclusive = [this](u64 hwnd, std::array<i32, 4> output,
                                          u32 width, u32 height,
                                          bool change_mode) {
            return u32_ext_mgr.enter_exclusive_display(
                static_cast<windowing::HWND>(hwnd),
                windowing::Rect{output[0], output[1], output[2], output[3]},
                width, height, change_mode);
        };
        services.leave_exclusive =
            [this](u64 hwnd, bool mode_changed,
                   std::optional<std::array<i32, 4>> restore) {
                std::optional<windowing::Rect> rect;
                if (restore) {
                    rect = windowing::Rect{(*restore)[0], (*restore)[1],
                                           (*restore)[2], (*restore)[3]};
                }
                u32_ext_mgr.leave_exclusive_display(
                    static_cast<windowing::HWND>(hwnd), mode_changed, rect);
            };
        services.host_fullscreen = [this](u64 hwnd, bool enable) {
            cocoa_bridge.set_window_fullscreen(
                static_cast<windowing::HWND>(hwnd), enable);
        };
        services.application_active = [this] {
            return u32_ext_mgr.application_active();
        };
        services.window_occluded = [this](u64 hwnd) {
            return cocoa_bridge.window_occluded(
                static_cast<windowing::HWND>(hwnd));
        };
        services.has_keyboard_focus = [this](u64 hwnd) {
            if (!u32_ext_mgr.application_active()) return false;
            const auto target = static_cast<windowing::HWND>(hwnd);
            windowing::HWND current = u32_ext_mgr.keyboard_focus_window();
            for (u32 depth = 0; depth < 64 && current != windowing::NULL_HWND &&
                                current != windowing::HWND_DESKTOP;
                 ++depth) {
                if (current == target) return true;
                const windowing::WindowData* window = hwnd_mgr.find(current);
                if (window == nullptr) return false;
                current = window->parent;
            }
            return false;
        };
        d3d12_presentation.client_size = services.client_size;
        d3d12_presentation.resize_client = services.resize_client;
        d3d12_presentation.window_rect = services.window_rect;
        d3d11_mgr.set_window_services(std::move(services));
    }
    // The frame-latency waitable object is a guest semaphore.
    {
        winapi::D3D11Manager::SyncServices services;
        services.create_semaphore = [this](i32 initial, i32 maximum) -> u64 {
            return sync_mgr.create_semaphore(initial, maximum, "");
        };
        services.duplicate_handle = [this](u64 handle) -> u64 {
            // SEMAPHORE_ALL_ACCESS, the access the chain's own handle has.
            constexpr u32 kSemaphoreAllAccess = 0x001F0003u;
            return sync_mgr.duplicate_handle(
                static_cast<winapi::Handle>(handle), 0, kSemaphoreAllAccess);
        };
        services.release_semaphore = [this](u64 handle, i32 count, i32 cap) {
            const auto h = static_cast<winapi::Handle>(handle);
            i32 current = 0;
            if (!sync_mgr.query_semaphore(h, &current, nullptr)) return;
            const i32 room = cap - current;
            if (room > 0) {
                (void)sync_mgr.release_semaphore(h, std::min(count, room),
                                                 nullptr);
            }
        };
        services.close_handle = [this](u64 handle) {
            (void)sync_mgr.close_handle(static_cast<winapi::Handle>(handle));
        };
        d3d12_presentation.create_semaphore = services.create_semaphore;
        d3d12_presentation.duplicate_handle = services.duplicate_handle;
        d3d12_presentation.release_semaphore = services.release_semaphore;
        d3d12_presentation.close_handle = services.close_handle;
        d3d11_mgr.set_sync_services(std::move(services));
    }
    d3d12_mgr.set_presentation_services(std::move(d3d12_presentation));
    // IDXGIResource1::CreateSharedHandle makes kernel handles.
    d3d11_mgr.set_handle_table(&handle_table);
    const auto foreign_present_target = [](u64 hwnd)
        -> std::optional<ipc::HwndRegistryEntry> {
        const auto window = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd));
        if (!window || window->owner_host_pid == 0) return std::nullopt;
        if (window->owner_host_pid != static_cast<u32>(::getpid())) {
            return window;
        }
        // A process that hosts an on-screen peer view for this child is the
        // real destination, and it is the only one that knows. A browser
        // guest parents its compositing child under a GPU-process-local host,
        // so the parent walk below cannot find it.
        if (window->present_host_pid != 0 &&
            window->present_host_pid != static_cast<u32>(::getpid())) {
            ipc::HwndRegistryEntry hosted = *window;
            hosted.owner_host_pid = window->present_host_pid;
            return hosted;
        }
        // SetParent can embed a GPU-process-owned DirectComposition child in
        // a browser-process HWND. AppKit cannot move the GPU NSView into the
        // browser process, so the browser hosts a peer NSView for the same
        // child HWND and receives the IOSurface there. Presenting onto the
        // parent HWND instead covers every sibling (legacy widget windows and
        // GDI) with an opaque Metal plate.
        if (window->parent_hwnd == 0) return std::nullopt;
        const auto parent = ipc::hwnd_registry_lookup(window->parent_hwnd);
        if (!parent || parent->owner_host_pid == 0 ||
            parent->owner_host_pid == static_cast<u32>(::getpid())) {
            return std::nullopt;
        }
        ipc::HwndRegistryEntry hosted = *window;
        hosted.owner_host_pid = parent->owner_host_pid;
        return hosted;
    };
    // VITREO_WIN_DIAG: a cross-process compositing child renders into a
    // DETACHED offscreen view in the process that owns the GPU, and only this
    // transport puts those pixels onto the peer view the owner shows. If it
    // never runs, or never finds a target, the region is simply black -- which
    // is indistinguishable from "the guest drew nothing" without saying so.
    const auto foreign_diag = [](const char* what, u64 hwnd, bool ok) {
        static const bool on = std::getenv("VITREO_WIN_DIAG") != nullptr;
        if (!on) return;
        // Log the first result per (hwnd, outcome). Sampling by a global
        // counter hid the interesting case entirely: a couple of chatty
        // successes filled the sample while the refusals -- the reason a
        // region is black -- never appeared.
        static std::mutex seen_mutex;
        static std::set<std::pair<u64, bool>> seen;
        {
            std::lock_guard<std::mutex> lock(seen_mutex);
            if (!seen.insert({hwnd, ok}).second) return;
        }
        std::fprintf(stderr, "[foreign-present] %s hwnd=0x%llx -> %s\n",
                     what, static_cast<unsigned long long>(hwnd),
                     ok ? "ok" : "NO TARGET/FAIL");
        std::fflush(stderr);
    };

    d3d11_mgr.set_foreign_window_presenter(
        [foreign_present_target, foreign_diag](u64 hwnd) {
            const bool has = foreign_present_target(hwnd).has_value();
            foreign_diag("claims", hwnd, has);
            return has;
        },
        [foreign_present_target, foreign_diag](u64 hwnd, const std::vector<u8>& rgba,
                                 u32 width, u32 height) {
            const auto target = foreign_present_target(hwnd);
            if (!target) return false;
            return ipc::remote_present_window_pixels(
                static_cast<pid_t>(target->owner_host_pid), target->hwnd,
                std::span<const Byte>(rgba), static_cast<i32>(width),
                static_cast<i32>(height));
        },
        [foreign_present_target, foreign_diag](u64 hwnd, u64 shared_handle,
                                 u32 width, u32 height, u32 sync_interval,
                                 u32 src_x, u32 src_y) {
            const auto target = foreign_present_target(hwnd);
            if (!target) { foreign_diag("surface", hwnd, false); return false; }
            const bool sent = ipc::remote_present_window_surface(
                static_cast<pid_t>(target->owner_host_pid), target->hwnd,
                shared_handle, static_cast<i32>(width),
                static_cast<i32>(height), sync_interval, src_x, src_y);
            foreign_diag("surface", hwnd, sent);
            static const bool log_present_view =
                std::getenv("VITREO_LOG_PRESENT_VIEW") != nullptr;
            if (log_present_view) {
                std::fprintf(stderr,
                             "[present-send] t=%.3f hwnd=0x%llx to_pid=%u "
                             "%ux%u src=(%u,%u) sync=%u sent=%d\n",
                             windowing::present_view_timestamp(),
                             static_cast<unsigned long long>(hwnd),
                             target->owner_host_pid, width, height, src_x,
                             src_y, sync_interval, sent ? 1 : 0);
            }
            return sent;
        });
    dxgi_mgr.set_output_resolver([this] {
        const auto monitors = cocoa_bridge.get_all_monitor_info();
        std::vector<winapi::dxgi::OutputInfo> outputs;
        outputs.reserve(monitors.size());
        const auto append_monitor = [&](usize index) {
            const auto& monitor = monitors[index];
            outputs.push_back(winapi::dxgi::OutputInfo{
                .device_name = "\\\\.\\DISPLAY" +
                    std::to_string(index + 1),
                .left = monitor.left,
                .top = monitor.top,
                .right = monitor.right,
                .bottom = monitor.bottom,
                .monitor = 0x000D0001ull + index,
                .bits_per_color = monitor.bits_per_color,
                .refresh_rate_hz = monitor.refresh_rate_hz,
            });
        };
        // IDXGIAdapter::EnumOutputs requires the desktop-primary output first.
        for (usize index = 0; index < monitors.size(); ++index) {
            if (monitors[index].primary) append_monitor(index);
        }
        // While an exclusive mode (DirectDraw, D3D9 or a DXGI full-screen
        // chain) is published, the guest desktop is that one mode on the
        // primary, exactly as GetMonitorInfo / EnumDisplaySettings report
        // it; the output still enumerates the display's own modes.
        if (const auto mode = u32_ext_mgr.virtual_display_size();
            mode && !outputs.empty()) {
            auto& primary = outputs.front();
            primary.native_width =
                static_cast<u32>(std::max(0, primary.right - primary.left));
            primary.native_height =
                static_cast<u32>(std::max(0, primary.bottom - primary.top));
            primary.left = 0;
            primary.top = 0;
            primary.right = static_cast<i32>(mode->first);
            primary.bottom = static_cast<i32>(mode->second);
            outputs.resize(1);
            return outputs;
        }
        for (usize index = 0; index < monitors.size(); ++index) {
            if (!monitors[index].primary) append_monitor(index);
        }
        return outputs;
    });
    // pDevice is a D3D11 device, or for D3D12 a command queue.
    dxgi_mgr.set_swap_chain_creator(
        [this](winapi::Marshaler& marshaler) {
            if (d3d12_mgr.is_command_queue(marshaler.arg_ptr(1))) {
                d3d12_mgr.impl_CreateSwapChain(marshaler);
                return;
            }
            d3d11_mgr.impl_CreateSwapChain(marshaler);
        });
    dxgi_mgr.set_swap_chain_for_hwnd_creator(
        [this](winapi::Marshaler& marshaler) {
            if (d3d12_mgr.is_command_queue(marshaler.arg_ptr(1))) {
                d3d12_mgr.impl_CreateSwapChainForHwnd(marshaler);
                return;
            }
            d3d11_mgr.impl_CreateSwapChainForHwnd(marshaler);
        });
    dxgi_mgr.set_swap_chain_for_composition_creator(
        [this](winapi::Marshaler& marshaler) {
            if (d3d12_mgr.is_command_queue(marshaler.arg_ptr(1))) {
                d3d12_mgr.impl_CreateSwapChainForComposition(marshaler);
                return;
            }
            d3d11_mgr.impl_CreateSwapChainForComposition(marshaler);
        });
    dcomp_mgr.set_window_ownership_resolver(
        [this](u64 hwnd) {
            if (hwnd_mgr.is_valid(static_cast<windowing::HWND>(hwnd))) {
                return winapi::DcompManager::WindowOwnership::CurrentProcess;
            }
            const auto owner = ipc::hwnd_registry_lookup(
                static_cast<u32>(hwnd));
            if (!owner || owner->owner_host_pid == 0) {
                return winapi::DcompManager::WindowOwnership::Invalid;
            }
            return owner->owner_host_pid == static_cast<u32>(::getpid())
                ? winapi::DcompManager::WindowOwnership::CurrentProcess
                : winapi::DcompManager::WindowOwnership::OtherProcess;
        });
    // Let an exclusive full-screen DirectDraw cooperative level drive the
    // native window to cover the display.
    ddraw_mgr.set_fullscreen_callback(
        [this](u32 hwnd, bool enable) {
            cocoa_bridge.set_window_fullscreen(hwnd, enable);
        });
    // An HWND-attached IDirectDrawClipper clips a blit to the window's client
    // area, so ddraw needs the window model's rectangle for that handle. It
    // asks through a callback rather than reaching into windowing directly.
    ddraw_mgr.set_client_region_callback(
        [this](u32 hwnd, i32& left, i32& top, i32& right, i32& bottom) {
            const auto* win =
                hwnd_mgr.find(static_cast<windowing::HWND>(hwnd));
            if (win == nullptr) return false;
            left = win->client_rect.left;
            top = win->client_rect.top;
            right = win->client_rect.right;
            bottom = win->client_rect.bottom;
            return right > left && bottom > top;
        });
    ddraw_mgr.set_display_size_callback(
        [this](u32& width, u32& height) {
            i32 screen_width = 0;
            i32 screen_height = 0;
            cocoa_bridge.get_screen_size(screen_width, screen_height);
            width = static_cast<u32>(screen_width);
            height = static_cast<u32>(screen_height);
        });
    // DirectDraw SetDisplayMode is how an exclusive-mode title publishes its
    // logical resolution. Mirror it into user32 and GDI screen metrics so
    // GetSystemMetrics and GetDeviceCaps agree with the mapped mouse surface.
    ddraw_mgr.set_display_mode_callback(
        [this](u32 hwnd, u32 width, u32 height, u32 bits) {
            u32_ext_mgr.apply_virtual_display_mode(
                static_cast<windowing::HWND>(hwnd), width, height, bits);
        });
    d3d9_mgr.set_memory(&memory, is_64bit);
    d3d9_mgr.set_backend(&graphics::MetalDevice::instance());
    if (!d2d1_mgr.set_backend(&graphics::MetalDevice::instance())) {
        std::fprintf(stderr, "[vitreo] Direct2D graphics backend binding failed\n");
    }
    d2d1_mgr.set_guest_interface_call(
        [this](winapi::Marshaler& m, GuestAddr object, u32 slot,
               const u64* args, usize count) {
            return com_mgr.call_guest_slot(m, object, slot, args, count);
        });

    // Publish the backend to the frontends that need adapter identity without
    // needing to render: DXGI enumerating adapters, GDI and SetupAPI reporting
    // a display device name, user32 describing the primary monitor. They ask
    // graphics::active_backend_device() rather than naming a concrete backend,
    // so the composition root is what decides which one they see.
    graphics::set_active_backend_device(&graphics::MetalDevice::instance());
    // An NT shared-resource handle DuplicateHandle brings into this process
    // keeps the storage alive like the creator's handle does.
    winapi::set_shared_resource_retainer([](u32 surface_id) {
        return graphics::MetalDevice::instance().retain_shared_texture(
            surface_id);
    });
    d3d9_mgr.set_display_size_resolver(
        [this](u32& width, u32& height) {
            i32 screen_width = 0;
            i32 screen_height = 0;
            cocoa_bridge.get_screen_pixel_size(screen_width, screen_height);
            if (screen_width <= 0 || screen_height <= 0) return false;
            width = static_cast<u32>(screen_width);
            height = static_cast<u32>(screen_height);
            return true;
        });
    // A D3D9 swap chain presents into the same native view the rest of the
    // graphics stack uses for a top-level guest window.
    d3d9_mgr.set_presentation_view_resolver(
        [this](u32 hwnd) -> void* {
            if (hwnd == 0) return nullptr;
            return cocoa_bridge.native_presentation_view(
                static_cast<windowing::HWND>(hwnd));
        });
    // D3DSPD_IUNKNOWN private data may store arbitrary guest IUnknown*.
    // Route AddRef/Release through ComCore so D3D9 never links ole32.
    // `memory` is the constructor GuestMemory& (not a member); capture it.
    d3d9_mgr.set_guest_iunknown_refcount(
        [this, &memory](u32 punk) -> u32 {
            winapi::Marshaler m(memory);
            if (cpu::CpuState* cpu = active_guest_cpu()) {
                m.attach_cpu(*cpu);
            }
            return com_mgr.guest_iunknown_addref(m, punk);
        },
        [this, &memory](u32 punk) {
            winapi::Marshaler m(memory);
            if (cpu::CpuState* cpu = active_guest_cpu()) {
                m.attach_cpu(*cpu);
            }
            com_mgr.guest_iunknown_release(m, punk);
        });
    // Windowed CreateDevice/Reset/CreateAdditionalSwapChain with a zero
    // BackBufferWidth/Height take GetClientRect dimensions (Learn).
    d3d9_mgr.set_client_size_resolver(
        [this](u32 hwnd, u32& width, u32& height) {
            const auto* win =
                hwnd_mgr.find(static_cast<windowing::HWND>(hwnd));
            if (win == nullptr) return false;
            const i32 client_w = win->client_rect.width();
            const i32 client_h = win->client_rect.height();
            if (client_w <= 0 || client_h <= 0) return false;
            width = static_cast<u32>(client_w);
            height = static_cast<u32>(client_h);
            return true;
        });
    // Exclusive CreateDevice/Reset (Windowed=0) must cover the display the
    // same way DirectDraw DDSCL_FULLSCREEN does — otherwise RCT3 stays in a
    // titled macOS window with traffic lights.
    d3d9_mgr.set_fullscreen_callback(
        [this](u32 hwnd, bool enable) {
            cocoa_bridge.set_window_fullscreen(hwnd, enable);
        });
    d3d9_mgr.set_display_mode_callback(
        [this](u32 hwnd, u32 width, u32 height, u32 bits) {
            u32_ext_mgr.apply_virtual_display_mode(
                static_cast<windowing::HWND>(hwnd), width, height, bits);
        });
    // D3D9 hardware cursor show/position/image share the same host hooks as
    // user32 ShowCursor / SetCursorPos and Cocoa NSCursor.
    d3d9_mgr.set_cursor_visible_callback(
        [this](bool visible) { cocoa_bridge.set_cursor_visible(visible); });
    d3d9_mgr.set_cursor_position_callback(
        [this](i32 x, i32 y) { (void)cocoa_bridge.set_native_cursor_pos(x, y); });
    d3d9_mgr.set_cursor_image_callback(
        [this](u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
               const u8* rgba, usize bytes) {
            return cocoa_bridge.set_custom_cursor_rgba(
                width, height, hotspot_x, hotspot_y, rgba, bytes);
        });
    vulkan_mgr.set_memory(&memory);
    dsound_mgr.set_memory(&memory);
    dsound_mgr.note_guest_bitness(is_64bit);
    com_mgr.set_audio_renderer(audio_renderer.get());
    // One host input-device list behind every capture API: DirectSound
    // capture, waveIn and the MMDevice eCapture endpoints.
    com_mgr.set_audio_capture(audio_capture.get());
    dsound_mgr.set_audio_capture(audio_capture.get());
    winmm_mgr.set_audio_capture(audio_capture.get());
    // DirectSound position notifications and capture events: SetEvent from
    // the host audio threads (no guest code runs there).
    dsound_mgr.set_event_signaler([this](u64 handle) {
        return sync_mgr.set_event(static_cast<winapi::Handle>(handle));
    });
    // ActivateAudioInterfaceAsync completions and IAudioSessionEvents
    // notifications run on guest threads of their own, which must actually
    // start: a created-but-never-launched thread never completes them.
    com_mgr.set_audio_activation_thread([this](GuestAddr entry, GuestAddr parameter) {
        return thread_mgr.start_detached_thread(entry, parameter);
    });

    // Wire module manager into kernel32 ext for LoadLibraryExA / GetModuleHandleEx
    k32_ext_mgr.set_module_manager(&module_mgr);
    // CreateProcess needs this to resolve an HPCON passed in
    // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
    k32_ext_mgr.set_console_manager(&console_mgr);
    // A mapped SEC_RESERVE section view is a reservation as far as
    // VirtualAlloc(MEM_COMMIT) is concerned; only the file-mapping manager
    // knows about those ranges. See VirtualMemoryManager::MappedViewProbe.
    vmm.set_mapped_view_probe(
        [this](GuestAddr addr, u64 size) {
            return fmm_mgr.range_within_mapped_view(addr, size);
        });
    k32_ext_mgr.set_process_manager(&process_mgr);
    k32_ext_mgr.set_handle_table(&handle_table);
    k32_ext_mgr.set_security_stubs(&sec_stubs);
    sec_stubs.set_thread_manager(&thread_mgr);
    k32_ext_mgr.set_process_session(process_session, std::string(capsule_name));
    k32_ext_mgr.set_pre_process_launch_callback([this] {
        save_registry();
    });
    k32_ext_mgr.set_elevation_context(
        &elevation_state, elevation_authorizer);
    com_mgr.set_elevation_context(&elevation_state);
    com_mgr.set_caller_impersonation(
        [this](bool caller_elevated) {
            return sec_stubs.impersonate_com_caller(caller_elevated);
        },
        [this] { (void)sec_stubs.revert_to_self(); });
    // The SCM launches service processes and needs the same session owner.
    scm_mgr.set_process_session(process_session);
    com_mgr.set_local_service_activator(
        [this](const std::wstring& name, const std::wstring& parameters,
               u32& error) {
            // The new service worker must load registrations committed by the
            // installer before this activation request.
            save_registry();
            std::vector<std::wstring> args;
            if (!parameters.empty()) args.push_back(parameters);
            if (scm_mgr.start_service(name, args, error)) return true;
            return error == winapi::error_codes::ERROR_SERVICE_ALREADY_RUNNING;
        });
    com_mgr.set_local_server_activator(
        [this](const std::wstring& registered_executable,
               const std::wstring& command_line, u32& error) {
            auto split_command = [](const std::wstring& line,
                                    std::wstring& executable,
                                    std::wstring& parameters) {
                const usize first = line.find_first_not_of(L" \t");
                if (first == std::wstring::npos) return;
                usize end = first;
                if (line[first] == L'\"') {
                    const usize quote = line.find(L'\"', first + 1);
                    if (quote == std::wstring::npos) return;
                    executable = line.substr(first + 1, quote - first - 1);
                    end = quote + 1;
                } else {
                    end = line.find_first_of(L" \t", first);
                    executable = line.substr(
                        first, end == std::wstring::npos
                            ? std::wstring::npos : end - first);
                }
                const usize parameter_start = line.find_first_not_of(L" \t", end);
                if (parameter_start != std::wstring::npos) {
                    parameters = line.substr(parameter_start);
                }
            };

            std::wstring parsed_executable;
            std::wstring parameters;
            split_command(command_line, parsed_executable, parameters);
            const std::wstring& executable = registered_executable.empty()
                ? parsed_executable : registered_executable;
            if (executable.empty()) {
                error = winapi::error_codes::ERROR_FILE_NOT_FOUND;
                return false;
            }
            winapi::Handle process = winapi::NULL_HANDLE;
            const bool launched = k32_ext_mgr.launch_shell_executable(
                std::string(executable.begin(), executable.end()),
                std::string(parameters.begin(), parameters.end()),
                {}, 1 /* SW_SHOWNORMAL, as in show_window.cpp */, false,
                process, error);
            if (process != winapi::NULL_HANDLE) {
                (void)handle_table.close(process);
            }
            return launched;
        });
    scm_mgr.set_kernel_driver_callbacks(
        [this](const std::wstring& name, const std::wstring&,
               const std::string& host_path, u32& error) {
            return file_mgr.start_kernel_driver(
                std::string(name.begin(), name.end()), host_path, error);
        },
        [this](const std::wstring& name) {
            file_mgr.stop_kernel_driver(
                std::string(name.begin(), name.end()));
        },
        [this](const std::wstring& name) {
            return file_mgr.kernel_driver_running(
                std::string(name.begin(), name.end()));
        });
    // Per-module resources: LoadAccelerators/FindResource may name a DLL
    // HINSTANCE (localized resource DLLs) rather than the main image.
    u32_ext_mgr.set_module_manager(&module_mgr);

    // Wire sync manager into kernel32 ext for WaitForSingleObjectEx
    k32_ext_mgr.set_sync_manager(&sync_mgr);
    console_mgr.set_sync_manager(&sync_mgr);
    iphlpapi_mgr.set_notification_managers(handle_table, sync_mgr);
    k32_ext_mgr.set_critical_section_manager(&cs_mgr);
    // Learn EnterCriticalSection: ExitProcess must not leave waiters parked in
    // enter_slow. Wake as soon as exit_process latches, not only at late
    // run.cpp teardown.
    process_mgr.set_exit_wake([this] { cs_mgr.begin_shutdown(); });
    reg_thunks.set_sync_manager(&sync_mgr);
    reg_thunks.set_environment_manager(&env_mgr);

    // Wire thread manager into kernel32 ext for DuplicateHandle on thread handles.
    k32_ext_mgr.set_thread_manager(&thread_mgr);
    thread_mgr.set_external_resume_callback(
        [this](winapi::Handle handle) {
            return k32_ext_mgr.resume_external_primary_thread(handle);
        });

    // Alertable waits (SleepEx / WaitForMultipleObjectsEx / SignalObjectAndWait)
    // drain QueueUserAPC via nested guest call once run.cpp wires set_guest_call.
    sync_mgr.set_alertable_apc_drain(
        [this]() { return k32_ext_mgr.try_deliver_user_apcs(); });

    // ...and the other direction: queuing an APC must END an alertable wait,
    // the way Windows does. Without this the waiter only notices on its next
    // poll slice, so a finished asynchronous I/O sits undelivered for
    // milliseconds, which is what paced one loader.
    thread_mgr.set_apc_wake([this](u32 tid) { sync_mgr.note_apc_queued(tid); });

    // Wire environment manager into kernel32 ext for ExpandEnvironmentStringsW
    k32_ext_mgr.set_environment_manager(&env_mgr);

    // Wire file search manager into kernel32 ext for FindFirstFileExW
    k32_ext_mgr.set_file_search_manager(&fsearch_mgr);

    // Wire ntdll ext into kernel32 ext for SetEndOfFile
    k32_ext_mgr.set_ntdll_ext_manager(&ntdll_ext_mgr);
    k32_ext_mgr.set_sysinfo_manager(&sysinfo_mgr);

    // Wire file manager into kernel32 ext for GetFileInformationByHandle
    k32_ext_mgr.set_file_manager(&file_mgr);
    k32_ext_mgr.set_file_mapping_manager(&fmm_mgr);

    // Extended current-process memory APIs share VirtualAlloc's allocation state.
    k32_ext_mgr.set_virtual_memory_manager(&vmm);

    // GetThreadDescription returns storage that callers release with LocalFree.
    k32_ext_mgr.set_global_local_manager(&gl_mgr);
    // CreateStreamOnHGlobal keeps a stream's bytes in its HGLOBAL.
    com_mgr.set_global_local_manager(&gl_mgr);
    // Clipboard GetClipboardData returns HGLOBALs for CF_TEXT / CF_UNICODETEXT.
    u32_ext_mgr.set_global_local_manager(&gl_mgr);

    // Wire file manager into FileOpsManager so handle-based APIs (SetFileTime)
    // can resolve Win32 HANDLEs back to VFS handles.
    file_ops_mgr.set_file_manager(&file_mgr);
    file_ops_mgr.set_registry(&reg_thunks);
    file_mgr.set_file_ops_manager(&file_ops_mgr);

    // Wire sync manager into locale/string thunks for SRW and condition variables
    locale_str_mgr.set_sync_manager(&sync_mgr);
    locale_str_mgr.set_console_attached_query(
        [this] { return console_mgr.is_attached(); });

    // The thread pool runs guest callbacks on guest threads, and needs to
    // know when one returns so a closed work object is freed and a waiter is
    // released at the moment Windows would.
    threadpool_mgr.set_thread_manager(&thread_mgr);
    threadpool_mgr.set_sync_manager(&sync_mgr);
    threadpool_mgr.set_leave_critical_section(
        [this](GuestAddr cs) { cs_mgr.leave(cs); });
    // A service-thread id (RegisterWait / threadpool waiter) that starts or
    // stops being used drops its alert object: a re-armed waiter reusing the
    // id must start unalerted, as a new Windows thread does.
    thread_mgr.set_service_thread_id_callback(
        [this](u32 tid) { sync_mgr.discard_alert_wait_object(tid); });
    thread_mgr.set_thread_exit_callback(
        [this](u32 tid, u32) {
            sync_mgr.abandon_mutexes_for_thread(tid);
            threadpool_mgr.on_thread_exit(tid);
            u32_ext_mgr.discard_raw_input_for_thread(tid);
            // A thread terminated while parked in WaitOnAddress leaves the
            // address's queue (it must not absorb a live waiter's wake).
            (void)winapi::cancel_wait_on_address_for_thread(tid);
        });

    // Wire module manager into psapi for GetModuleBaseNameA/W
    psapi_mgr.set_module_manager(&module_mgr);

    // A guest that ends its own process reports which modules are on the
    // stack at that moment, which needs the loader's module table.
    process_mgr.set_module_manager(&module_mgr);

    // Core process queries and extended file APIs share the guest handle table.
    ntdll_mgr.set_handle_table(&handle_table);
    ntdll_ext_mgr.set_vfs(&vfs);
    ntdll_ext_mgr.set_handle_table(&handle_table);
    ntdll_ext_mgr.set_file_manager(&file_mgr);
    ntdll_ext_mgr.set_file_mapping_manager(&fmm_mgr);
    // NtCreateFile / NtOpenFile handles live in ntdll_ext_mgr; a section can
    // be created over them (NtCreateSection SEC_IMAGE on an NtOpenFile handle).
    fmm_mgr.set_foreign_file_resolver(
        [this](winapi::Handle h) -> std::optional<winapi::FileMappingManager::ForeignFile> {
            const auto vfs_handle = ntdll_ext_mgr.vfs_handle_for(h);
            if (!vfs_handle) return std::nullopt;
            return winapi::FileMappingManager::ForeignFile{*vfs_handle,
                                                   ntdll_ext_mgr.nt_path_for_handle(h)};
        });
    ntdll_ext_mgr.set_sync_manager(&sync_mgr);
    ntdll_ext_mgr.set_virtual_memory_manager(&vmm);
    ntdll_ext_mgr.set_module_manager(&module_mgr);
    ntdll_ext_mgr.set_process_manager(&process_mgr);

    // Wire GdiManager dependencies for software framebuffer rendering
    gdi_mgr.set_guest_memory(&memory);
    gdi_mgr.set_cocoa_bridge(&cocoa_bridge);
    gdi_mgr.set_vfs(&vfs);  // AddFontResourceEx reads font files through the capsule
    com_mgr.set_cocoa_bridge(&cocoa_bridge);
    com_mgr.set_sync_manager(&sync_mgr);
    com_mgr.set_message_send(&msgsend_mgr);
    // MessageBoxA/W must show a real modal alert, not silently self-answer.
    resource_mgr.set_cocoa_bridge(&cocoa_bridge);
    resource_mgr.set_hwnd_manager(&hwnd_mgr);
    // Version queries read the real RT_VERSION resource out of the PE on disk.
    ver_mgr.set_vfs(&vfs);
    // Vitreo-owned synthetic Windows DLLs have no capsule backing file, but
    // are loaded modules and expose consistent version metadata.
    ver_mgr.set_module_manager(&module_mgr);
    ver_mgr.set_builtin_dll_query(
        [this](std::string_view name) { return thunk_registry.has_dll(name); });
    ver_mgr.set_path_lookup([this]() -> std::string {
        auto value = env_mgr.get_variable("PATH");
        return value.is_ok() ? value.value() : std::string{};
    });
    gdi_mgr.set_hwnd_manager(&hwnd_mgr);
    // CreateDIBSection(hSection): the bits live in a FILE_MAP_WRITE view.
    gdi_mgr.set_section_view_mapper(
        [this](u64 section, u64 offset, u64 size) -> GuestAddr {
            return fmm_mgr.map_view_of_file(static_cast<winapi::Handle>(section),
                                            0x0002 /* FILE_MAP_WRITE */, offset, size);
        },
        [this](GuestAddr base) { (void)fmm_mgr.unmap_view_of_file(base); });

    // Route PaintingManager DC allocation through GdiManager
    paint_mgr.set_gdi_manager(&gdi_mgr);
    paint_mgr.set_wclass_manager(&wclass_mgr);
    paint_mgr.set_erase_background_callback(
        [this](u64 hwnd, u64 hdc) {
            return msgsend_mgr.send_message(
                static_cast<windowing::HWND>(hwnd),
                windowing::WM::ERASEBKGND, hdc, 0);
        });

    // WS_VISIBLE creation establishes the initial update region consumed by
    // an immediate UpdateWindow call.
    window_mgr.set_painting_manager(&paint_mgr);
    window_mgr.set_activation_callback(
        [this](windowing::HWND hwnd) {
            (void)u32_ext_mgr.activate_application(hwnd);
        });
    window_mgr.set_destroy_activation_callback(
        [this](windowing::HWND hwnd) {
            u32_ext_mgr.release_activation(hwnd, true);
        });

    // Let the message loop synthesize WM_PAINT for windows with invalid regions
    msgloop_mgr.set_painting_manager(&paint_mgr);
    msgloop_mgr.set_native_control_dispatch(
        [this](winapi::Marshaler& m, const windowing::MSG& msg, bool ansi) {
            return msgsend_mgr.dispatch_syslink_control(
                m, msg.hwnd, msg.message, msg.wParam, msg.lParam, ansi,
                /*force_native=*/false);
        });
    // WM_PAINT is synthesized, not queued: an invalidation must wake the
    // owning thread's GetMessage/MsgWait itself and raise its QS_PAINT change
    // bit (s2msg_queue_probe status_paint).
    paint_mgr.set_invalidate_callback([this](u64 hwnd) {
        const auto owner = hwnd_mgr.owner_thread_id(static_cast<windowing::HWND>(hwnd));
        if (owner) msg_queue.notify_thread(*owner, windowing::QS::PAINT);
    });
    // PostThreadMessage fails with ERROR_INVALID_THREAD_ID, and a thread
    // hook with ERROR_INVALID_PARAMETER, for a thread that does not exist
    // (s2msg_queue_probe post_thread_bogus / hook_bogus_thread).
    const auto thread_alive = [this](u32 tid) {
        return thread_mgr.get_thread(tid) != nullptr &&
               !thread_mgr.is_thread_terminated(tid);
    };
    msgsend_mgr.set_thread_alive_query(thread_alive);
    u32_ext_mgr.set_thread_alive_query(thread_alive);
    // WH_CALLWNDPROC / WH_CALLWNDPROCRET around SendMessage deliveries.
    msgsend_mgr.set_call_wndproc_hook(
        [this](windowing::HWND hwnd, u32 msg, u64 wparam, u64 lparam,
               bool cross_thread, bool after, u64 lresult) {
            u32_ext_mgr.dispatch_call_wndproc_hooks(hwnd, msg, wparam, lparam,
                                                    cross_thread, after, lresult);
        });
    // WH_KEYBOARD_LL / WH_MOUSE_LL see hardware input before it is queued.
    u32_ext_mgr.install_low_level_input_filter();
    // ImmGetContext / ImmAssociateContext reject a window that does not exist.
    imm32_mgr.set_window_validator([this](u32 hwnd) {
        return hwnd_mgr.is_valid(static_cast<windowing::HWND>(hwnd)) ||
               ipc::hwnd_registry_lookup(hwnd).has_value();
    });

    // Let the message loop synthesize timers and maintain queue-owned window state
    msgloop_mgr.set_user32_ext_manager(&u32_ext_mgr);

    // Modal DialogBox must synthesize WM_TIMER too (does not go through GetMessage).
    dialog_mgr.set_user32_ext_manager(&u32_ext_mgr);

    gdi_mgr.set_presentation_scheduler(windowing::CocoaBridge::defer_presentation);

    // Let the message loop flush the framebuffer after dispatching messages
    msgloop_mgr.set_gdi_manager(&gdi_mgr);
    msgloop_mgr.set_sync_manager(&sync_mgr);

    // Route user32_ext GetDC/ReleaseDC through GdiManager
    u32_ext_mgr.set_gdi_manager(&gdi_mgr);

    // Let RedrawWindow invalidate and synchronously dispatch WM_PAINT
    u32_ext_mgr.set_painting_manager(&paint_mgr);
    u32_ext_mgr.set_message_loop_manager(&msgloop_mgr);
    u32_ext_mgr.set_message_send_manager(&msgsend_mgr);
    u32_ext_mgr.set_dialog_manager(&dialog_mgr);
    u32_ext_mgr.set_process_manager(&process_mgr);
    u32_ext_mgr.set_show_window_manager(&show_mgr);
    // CreateProcess passes the foreground start grant on (Learn
    // SetForegroundWindow); on macOS 14+ the active application yields
    // activation to the launched process tree.
    k32_ext_mgr.set_foreground_grant_hooks(
        [this] { return cocoa_bridge.process_may_set_foreground(); },
        [this](int pid) { cocoa_bridge.grant_foreground_to_process_tree(pid); });
    cocoa_bridge.set_native_application_activation_callback(
        [this](windowing::HWND hwnd, bool active, u32 mouse_message) -> u32 {
            winapi::User32ExtManager::NativeDeliveryScope native;
            u32 answer = 0;
            if (active && mouse_message != 0) {
                answer = u32_ext_mgr.native_mouse_activate(
                    hwnd, mouse_message, [this, hwnd](windowing::HWND stays_active) {
                        cocoa_bridge.restore_key_after_declined_activation(hwnd, stays_active);
                    });
                constexpr u32 MA_NOACTIVATE = 3, MA_NOACTIVATEANDEAT = 4;
                if (answer == MA_NOACTIVATE || answer == MA_NOACTIVATEANDEAT ||
                    answer == winapi::User32ExtManager::kMouseActivateDeferred) {
                    return answer;
                }
            }
            (void)u32_ext_mgr.set_application_active(hwnd, active, 0,
                                                     mouse_message != 0);
            return answer;
        });
    cocoa_bridge.set_native_keyboard_focus_callback(
        [this](windowing::HWND hwnd, bool focused) {
            winapi::User32ExtManager::NativeDeliveryScope native;
            (void)u32_ext_mgr.set_native_keyboard_focus(hwnd, focused);
        });

    // Let UpdateWindow synchronously dispatch WM_PAINT
    show_mgr.set_painting_manager(&paint_mgr);
    show_mgr.set_message_loop_manager(&msgloop_mgr);
    show_mgr.set_message_send_manager(&msgsend_mgr);
    show_mgr.set_user32_ext_manager(&u32_ext_mgr);

    // Keep AppKit responsive while Vitreo implements a blocking guest wait,
    // but do not retrieve or dispatch queued Win32 messages. Windows warns
    // that WaitForSingleObject can deadlock a window-owning thread and directs
    // the caller to MsgWaitForMultipleObjects; WFSO itself does not pump that
    // thread's posted-message queue.
    sync_mgr.set_wait_pump([this]() {
        cocoa_bridge.begin_host_input_pump();
        struct HostPumpGuard {
            windowing::CocoaBridge& bridge;
            ~HostPumpGuard() { bridge.end_host_input_pump(); }
        } guard{cocoa_bridge};
        cocoa_bridge.pump_events(msg_queue);
    });
    // The blocking counterpart: park the GUI thread inside AppKit's event
    // wait. ntsync signals and queue posts CFRunLoopWakeUp it (below), and a
    // worker's main-thread block wakes it itself, so the wait is event-driven
    // instead of re-checking every 16 ms.
    sync_mgr.set_wait_park([this](u32 ms) {
        cocoa_bridge.begin_host_input_pump();
        struct HostPumpGuard {
            windowing::CocoaBridge& bridge;
            ~HostPumpGuard() { bridge.end_host_input_pump(); }
        } guard{cocoa_bridge};
        const auto t0 = std::chrono::steady_clock::now();
        const bool waited = cocoa_bridge.wait_events_waited(msg_queue, ms);
        const auto t1 = std::chrono::steady_clock::now();
        cocoa_bridge.pump_events(msg_queue);
        const auto t2 = std::chrono::steady_clock::now();
        // wait_events bails out at once during live resize and teardown; a
        // caller that loops on this park would otherwise spin. A confirmed
        // wake is real progress and must not incur that backoff -- and it
        // cannot be recognised afterwards by vtr_main_wake_pending(), because
        // the wait itself consumed that wake. Back off on a real bailout only.
        const bool instant = ms > 0 && (t1 - t0) < std::chrono::microseconds(200);
        if (ms > 0 && !waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        static const bool diag = std::getenv("VITREO_GUI_PARK_DIAG") != nullptr;
        if (diag) {
            static u64 parks = 0, cap_hits = 0, instants = 0;
            static double park_ms = 0, pump_ms = 0;
            static auto last = std::chrono::steady_clock::now();
            ++parks;
            const double w = std::chrono::duration<double, std::milli>(t1 - t0).count();
            park_ms += w;
            pump_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
            if (w >= ms - 1.0) ++cap_hits;
            if (instant) ++instants;
            if (t2 - last >= std::chrono::seconds(2)) {
                const double sec = std::chrono::duration<double>(t2 - last).count();
                std::fprintf(stderr,
                    "[gui-park] %.0f parks/s  mean park %.2f ms  cap-hits %llu  "
                    "instant %llu  pump %.1f ms/s  wakes posted %llu suppressed %llu "
                    "not-parked %llu consumed %llu\n",
                    parks / sec, parks ? park_ms / parks : 0.0,
                    (unsigned long long)cap_hits, (unsigned long long)instants,
                    pump_ms / sec,
                    (unsigned long long)windowing::g_gui_wake_posted.exchange(0),
                    (unsigned long long)windowing::g_gui_wake_suppressed_flag.exchange(0),
                    (unsigned long long)windowing::g_gui_wake_not_parked.exchange(0),
                    (unsigned long long)windowing::g_gui_wake_consumed.exchange(0));
                parks = cap_hits = instants = 0; park_ms = pump_ms = 0; last = t2;
            }
        }
    });
    sync_mgr.set_wait_park_arm([this](bool parked) {
        cocoa_bridge.set_main_thread_parked(parked);
    });
    ntsync::global().set_host_wake([this]() {
        cocoa_bridge.wake_main_runloop();
    });
    msg_queue.set_host_wake([this]() {
        cocoa_bridge.wake_main_runloop();
    });
    // Long host operations and cold JIT use the same host-only event pump.
    // Native events become queued Win32 input; only guest message APIs may
    // retrieve and dispatch it.
    winapi::set_host_gui_pump([this]() {
        // Host pumps enqueue AppKit→Win32 input only. Mark the drain so Cocoa
        // does not SendMessage(WM_NCHITTEST) for guest-frame caption drag —
        // that reenters the guest mid-Present/Wait and crashes D3D apps whose
        // WM_NCCALCSIZE makes client==outer (client_drawn_frame).
        cocoa_bridge.begin_host_input_pump();
        struct HostPumpGuard {
            windowing::CocoaBridge& bridge;
            ~HostPumpGuard() { bridge.end_host_input_pump(); }
        } guard{cocoa_bridge};
        cocoa_bridge.pump_events(msg_queue);
    });

    // Wire DefWindowProcManager for WM_ERASEBKGND background filling
    dwp_mgr.set_gdi_manager(&gdi_mgr);
    dwp_mgr.set_wclass_manager(&wclass_mgr);
    dwp_mgr.set_window_manager(&window_mgr);
    dwp_mgr.set_paint_manager(&paint_mgr);
    dwp_mgr.set_message_send_manager(&msgsend_mgr);
    dwp_mgr.set_apply_cursor(
        [this](u32 hcursor) { u32_ext_mgr.apply_cursor(hcursor); });
    dwp_mgr.set_syscommand_show(
        [this](windowing::HWND hwnd, int cmd_show) {
            (void)show_mgr.show_window(hwnd, cmd_show);
        });
    dwp_mgr.set_focus_window(
        [this](windowing::HWND hwnd) {
            (void)u32_ext_mgr.set_keyboard_focus(hwnd, true);
        });
    msgsend_mgr.set_edit_clipboard(
        [this](winapi::Marshaler& m, windowing::HWND owner, const std::u16string& text) {
            return u32_ext_mgr.edit_clipboard_copy(m, owner, text);
        },
        [this](winapi::Marshaler& m) { return u32_ext_mgr.edit_clipboard_text(m); });
    msgsend_mgr.set_key_state_provider(
        [this](u32 vk) { return u32_ext_mgr.thread_key_state(vk); });
    dwp_mgr.set_display_text(
        [this](windowing::HWND hwnd, const std::wstring& text) {
            cocoa_bridge.set_title(hwnd, text);
            (void)cocoa_bridge.set_control_text(hwnd, text);
        });
    dwp_mgr.set_dialog_activate(
        [this](windowing::HWND hwnd, bool activating) {
            u32_ext_mgr.dialog_activate_default(hwnd, activating);
        });
    dwp_mgr.set_begin_window_drag(
        [this](windowing::HWND hwnd) {
            return cocoa_bridge.begin_native_window_drag(hwnd);
        });
    dwp_mgr.set_begin_window_resize(
        [this](windowing::HWND hwnd, u32 edge) {
            return cocoa_bridge.begin_native_window_resize(hwnd, edge);
        });

    // Window classes: NULL-hInstance registrations belong to the executable,
    // system classes report user32 as their module and the arrow as their
    // cursor, and a class with live windows cannot be unregistered.
    wclass_mgr.set_executable_instance_provider(
        [this]() { return static_cast<u64>(module_mgr.main_module_base()); });
    wclass_mgr.set_system_module_provider(
        [this]() { return static_cast<u64>(module_mgr.get_module_handle("user32.dll")); });
    wclass_mgr.set_system_cursor_provider(
        [this]() { return static_cast<u64>(resource_mgr.load_cursor(0, 32512 /*IDC_ARROW*/)); });
    wclass_mgr.set_class_in_use_query(
        [this](u32 class_id) { return hwnd_mgr.any_window_of_class(class_id); });
    window_mgr.set_menu_validator([this](u64 hmenu) {
        return u32_ext_mgr.is_menu_handle(static_cast<u32>(hmenu));
    });

    // Auto-load class menu: when CreateWindowExW sees a WNDCLASS with
    // lpszMenuName and hMenu==0, load the menu resource and install it.
    window_mgr.set_menu_load_callback(
        [this](windowing::HWND hwnd, const std::wstring& menu_name) {
            u32_ext_mgr.load_and_install_class_menu(hwnd, menu_name);
        }
    );
    window_mgr.set_destroyed_callback(
        [this](windowing::HWND hwnd) {
            u32_ext_mgr.window_destroyed(hwnd);
            // The window's GL default framebuffer and its swap chain on the
            // dying view go with it, and so does its pixel format: a reused
            // HWND value gets a fresh drawable and no format.
            winapi::gl::GlDrawable::detach(static_cast<u64>(hwnd));
            gdi_mgr.window_destroyed(static_cast<winapi::Handle>(hwnd));
        });
    // CreateWindowEx / DestroyWindow show, hide and change state through the
    // same engine as ShowWindow / SetWindowPos.
    {
        winapi::WindowOps ops;
        ops.show = [this](windowing::HWND hwnd, int cmd) {
            return u32_ext_mgr.show_window_cmd(hwnd, cmd);
        };
        ops.set_pos = [this](windowing::HWND hwnd, i32 x, i32 y, i32 cx, i32 cy, u32 flags) {
            (void)u32_ext_mgr.apply_set_window_pos(hwnd, windowing::HWND_TOP, x, y, cx, cy,
                                                   flags, true);
        };
        ops.initial_state = [this](windowing::HWND hwnd, bool minimize) {
            u32_ext_mgr.apply_initial_state(hwnd, minimize);
        };
        window_mgr.set_window_ops(std::move(ops));
    }
    window_mgr.set_win_event_callback(
        [this](u32 event, windowing::HWND hwnd) {
            u32_ext_mgr.notify_win_event(event, hwnd);
        });
    window_mgr.set_lifecycle_callback(
        [this](bool created, windowing::HWND hwnd) {
            constexpr u32 HSHELL_WINDOWCREATED = 1;
            constexpr u32 HSHELL_WINDOWDESTROYED = 2;
            u32_ext_mgr.notify_shell_event(
                created ? HSHELL_WINDOWCREATED : HSHELL_WINDOWDESTROYED,
                hwnd);
        }
    );

    // Register well-known module addresses for GetModuleHandle/GetProcAddress
    const GuestAddr x64_synthetic_arena =
        module_mgr.synthetic_module_arena();
    module_mgr.register_module("kernel32.dll",
                               is_64bit ? x64_synthetic_arena + 0x00000
                                        : winapi::KERNEL32_BASE,
                               "C:\\Windows\\System32\\kernel32.dll");
    module_mgr.register_module("kernelbase.dll",
                               is_64bit ? x64_synthetic_arena + 0x90000
                                        : winapi::KERNELBASE_BASE,
                               "C:\\Windows\\System32\\kernelbase.dll");
    module_mgr.register_module("ntdll.dll",
                               is_64bit ? x64_synthetic_arena + 0x10000
                                        : winapi::NTDLL_BASE,
                               "C:\\Windows\\System32\\ntdll.dll");
    module_mgr.register_module("user32.dll",
                               is_64bit ? x64_synthetic_arena + 0x20000
                                        : winapi::USER32_BASE,
                               "C:\\Windows\\System32\\user32.dll");
    module_mgr.register_module("ws2_32.dll",
                               is_64bit ? x64_synthetic_arena + 0x30000
                                        : winapi::WS2_32_BASE,
                               "C:\\Windows\\System32\\ws2_32.dll");
    module_mgr.register_module("shell32.dll",
                               is_64bit ? x64_synthetic_arena + 0x40000
                                        : winapi::SHELL32_BASE,
                               "C:\\Windows\\System32\\shell32.dll");
    module_mgr.register_module("gdi32.dll",
                               is_64bit ? x64_synthetic_arena + 0x50000
                                        : winapi::GDI32_BASE,
                               "C:\\Windows\\System32\\gdi32.dll");
    module_mgr.register_module("advapi32.dll",
                               is_64bit ? x64_synthetic_arena + 0x60000
                                        : winapi::ADVAPI32_BASE,
                               "C:\\Windows\\System32\\advapi32.dll");
    module_mgr.register_module("ole32.dll",
                               is_64bit ? x64_synthetic_arena + 0x70000
                                        : winapi::OLE32_BASE,
                               "C:\\Windows\\System32\\ole32.dll");
    module_mgr.register_module("psapi.dll",
                               is_64bit ? x64_synthetic_arena + 0x80000
                                        : winapi::PSAPI_BASE,
                               "C:\\Windows\\System32\\psapi.dll");

    // ---- Default VFS setup (when no capsule is specified) ----
    // Mount C: to a default Windows-like directory structure
    // Layout: ~/.vitreo/capsules/<name>/drives/c/
    const std::string capsule = capsule_name.empty() ? std::string("default")
                                                     : std::string(capsule_name);
    const char* home = std::getenv("HOME");
    m_capsule_root = home ? std::string(home) + "/.vitreo/capsules/" + capsule
                          : "/tmp/vitreo/capsules/" + capsule;
    com_mgr.set_capsule_root(m_capsule_root);
    winapi::set_credential_store_root(m_capsule_root + "/credentials");
    std::string default_c_root = m_capsule_root + "/drives/c";
    
    // Ensure the directory exists
    std::error_code ec;
    fs::create_directories(default_c_root, ec);
    fs::create_directories(default_c_root + "/Program Files", ec);
    fs::create_directories(default_c_root + "/Windows/System32", ec);
    fs::create_directories(default_c_root + "/Users", ec);
    fs::create_directories(default_c_root + "/Vitreo", ec);
    // Shell folders that installers place shortcuts into (must match the guest
    // paths seeded into HKCU\...\Explorer\Shell Folders in Registry::init_defaults).
    fs::create_directories(default_c_root + "/Windows/Start Menu/Programs/Startup", ec);
    fs::create_directories(default_c_root + "/Windows/Desktop", ec);

    // Create registry directory
    std::string registry_dir = m_capsule_root + "/registry";
    fs::create_directories(registry_dir, ec);

    // The SCM keeps per-service status files under the capsule root so that a
    // service process and its controller — separate host processes — observe
    // the same state.
    scm_mgr.set_capsule(m_capsule_root, capsule);

    auto mount_result = vfs.mount_drive('C',
        std::make_unique<capsule::HostFileSystem>(default_c_root));
    if (mount_result.is_err()) {
        VITREO_TRACE( "[vitreo] Warning: Failed to mount default C: drive\n");
    } else {
        VITREO_TRACE( "[vitreo] Mounted C: -> %s\n", default_c_root.c_str());
    }

    // Mount D: (CD-ROM) when the capsule has a drives/d directory.  GetDriveType
    // already reports D: as DRIVE_CDROM and GetLogicalDrives lists it; this backs
    // it with real content (e.g. an extracted game CD) so guests can read D:\.
    std::string default_d_root = m_capsule_root + "/drives/d";
    if (fs::exists(default_d_root, ec) && fs::is_directory(default_d_root, ec)) {
        auto d_mount = vfs.mount_drive('D',
            std::make_unique<OpticalHostFileSystem>(default_d_root));
        if (d_mount.is_err()) {
            VITREO_TRACE( "[vitreo] Warning: Failed to mount D: drive\n");
        } else {
            VITREO_TRACE( "[vitreo] Mounted D: -> %s\n", default_d_root.c_str());
        }
    }

    // Mount persisted read-only optical images. Directory-backed drives keep
    // precedence so a stale record cannot replace capsule storage silently.
    capsule::ImageMountStore image_mounts(m_capsule_root);
    for (const auto& record : image_mounts.list()) {
        if (vfs.is_drive_mounted(record.drive)) {
            VITREO_TRACE("[vitreo] Warning: optical mount %c: conflicts with an existing drive\n",
                         record.drive);
            continue;
        }
        auto image_fs = std::make_unique<capsule::IsoFileSystem>(record.image_path);
        if (!image_fs->valid()) {
            VITREO_TRACE("[vitreo] Warning: failed to mount %c: from '%s': %s\n",
                         record.drive, record.image_path.c_str(),
                         image_fs->error_message().c_str());
            continue;
        }
        auto mounted = vfs.mount_drive(record.drive, std::move(image_fs));
        if (mounted.is_ok()) {
            VITREO_TRACE("[vitreo] Mounted optical image %c: -> %s\n",
                         record.drive, record.image_path.c_str());
        }
    }
    sysinfo_mgr.set_vfs(&vfs);

    // Load persisted registry data from capsule
    registry.load(registry_dir);
    VITREO_TRACE( "[vitreo] Registry loaded from %s\n", registry_dir.c_str());
    // HKLM\HARDWARE\DESCRIPTION describes this machine and is volatile, as on
    // Windows: rebuild it from the host now that the hives are loaded.
    winapi::publish_hardware_description(registry);
    // The machine name and system environment as the registry shows them
    // must match GetComputerName and the process environment.
    winapi::publish_machine_identity(registry, sysinfo_mgr.get_computer_name(), env_mgr);
    // The advertised display-adapter identity is per-capsule configuration and
    // lives in the capsule's own display-class key, so it can only be resolved
    // once the persisted hives are in place. Every graphics frontend reads the
    // resolved value, so this must happen before any of them answer a guest.
    winapi::display_device_identity::configure_adapter_identity_from_registry(
        registry);
    dxgi_mgr.refresh_adapter_info();
    scm_mgr.install_builtin_services();

    // ---- Register service groups in the service registry ----
    registry_svc.register_service<ProcessServices>(&process_services);
    registry_svc.register_service<MemoryServices>(&memory_services);
    registry_svc.register_service<FileServices>(&file_services);
    registry_svc.register_service<ModuleServices>(&module_services);
    registry_svc.register_service<SyncServices>(&sync_services);
    registry_svc.register_service<UIServices>(&ui_services);
    registry_svc.register_service<CapsuleServices>(&capsule_services);
}

RuntimeContext::~RuntimeContext() {
    // Stop the HTTP Server API listeners while the managers their
    // completions reach are still alive.
    httpapi_mgr.shutdown_all();
    gdi_mgr.stop_presentation();
    winapi::clear_host_gui_pump();
    // Detach the close callback so HandleTable's destructor doesn't call
    // into managers that have already been destroyed (e.g. FileManager).
    handle_table.set_close_callback(nullptr);
    // module_mgr is destroyed before vfs.
    vfs.set_generated_file_source(nullptr, nullptr);
}

void RuntimeContext::save_registry() {
    std::string registry_dir = m_capsule_root + "/registry";
    registry.save(registry_dir);
    VITREO_TRACE( "[vitreo] Registry saved to %s\n", registry_dir.c_str());
}
} // namespace vitreo::cli
