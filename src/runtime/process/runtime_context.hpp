// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Runtime context: owns all WinAPI managers and shared infrastructure

#pragma once

#include <vitreo/common.hpp>
#include <vitreo/service_registry.hpp>
#include <vitreo/services/process_services.hpp>
#include <vitreo/services/memory_services.hpp>
#include <vitreo/services/file_services.hpp>
#include <vitreo/services/module_services.hpp>
#include <vitreo/services/sync_services.hpp>
#include <vitreo/services/ui_services.hpp>
#include <vitreo/services/capsule_services.hpp>
#include <string_view>

// -- Shared infrastructure --
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"

// -- kernel32 managers --
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/fiber_fls.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/heap_ext.hpp"
#include "winapi/kernel32/thread/tls.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/threadpool.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

// -- kernel32 locale/string --
#include "winapi/kernel32/locale/locale_string.hpp"

// -- kernel32 file ops --
#include "winapi/kernel32/filesystem/file_ops.hpp"

// -- kernel32 ext --
#include "winapi/kernel32/kernel32_ext.hpp"

// -- msvcrt managers --
#include "winapi/msvcrt/runtime/crt_init.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/system/crt_environment.hpp"
#include "winapi/msvcrt/text/crt_locale.hpp"
#include "winapi/msvcrt/system/crt_time.hpp"
#include "winapi/msvcrt/io/crt_filesystem.hpp"

// -- ntdll managers --
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"

// -- advapi32 managers --
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/com/rpcrt4/ndr_proxy.hpp"
#include "winapi/com/ole32/storage.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/services/scm.hpp"

// -- gdi32 --
#include "winapi/graphics/gdi32/gdi32.hpp"

// -- user32 managers --
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/windows/def_window_proc.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/user32_ext.hpp"

// -- ole32 / combase / shell32 / version --
#include "winapi/com/ole32/com_core.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/com/combase/combase.hpp"
#include "winapi/graphics/dwrite/dwrite.hpp"
#include "winapi/graphics/d2d1/d2d1.hpp"
#include "winapi/networking/urlmon/urlmon.hpp"
#include "winapi/system/shell32/shell_paths.hpp"
#include "winapi/system/version/version.hpp"
#include "winapi/system/amsi/amsi.hpp"
#include "winapi/system/lz32/lz32.hpp"
#include "winapi/system/imagehlp/imagehlp.hpp"

// -- comctl32 --
#include "winapi/ui/comctl32/comctl32.hpp"

// -- gdiplus --
#include "winapi/graphics/gdiplus/gdiplus.hpp"

// -- ws2_32 / mswsock --
#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/networking/mswsock/mswsock.hpp"

// -- oleaut32 --
#include "winapi/com/oleaut32/oleaut32.hpp"

// -- authz / sspicli / wkscli / netutils / psapi / crypt32 --
#include "winapi/system/authz/authz.hpp"
#include "winapi/system/crypt32/crypt32.hpp"
#include "winapi/system/psapi/psapi.hpp"
#include "winapi/networking/secur32/secur32.hpp"
#include "winapi/networking/sspicli/sspicli.hpp"
#include "winapi/networking/wkscli/wkscli.hpp"
#include "winapi/networking/httpapi/httpapi.hpp"
#include "winapi/networking/netutils/netutils.hpp"

// -- bcrypt / wsock32 --
#include "winapi/system/bcrypt/bcrypt.hpp"
#include "winapi/networking/wsock32/wsock32.hpp"

// -- winhttp / wininet --
#include "winapi/networking/winhttp/winhttp.hpp"
#include "winapi/networking/wininet/wininet.hpp"

// -- msimg32 / winspool / imm32 / iphlpapi / opengl32 --
#include "winapi/graphics/msimg32/msimg32.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/com/msxml/msxml_com.hpp"
#include "winapi/media/mfplat/mf_attributes.hpp"
#include "winapi/media/mfplay/mfplay.hpp"
#include "winapi/system/cabinet/cabinet_fdi.hpp"
#include "winapi/system/setupapi/setupapi.hpp"
#include "winapi/system/winspool/winspool.hpp"
#include "winapi/ui/imm32/imm32.hpp"
#include "winapi/networking/iphlpapi/iphlpapi.hpp"
#include "winapi/networking/dnsapi/dnsapi.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "winapi/graphics/glu32/glu32.hpp"
#include "winapi/media/msvfw32/msvfw32.hpp"
#include "winapi/ui/oledlg/oledlg.hpp"

// -- d3d9 --
#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/media/dinput/dinput.hpp"
#include "winapi/media/dsound/dsound.hpp"
#include "winapi/graphics/d3d9/d3d9.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/dcomp/dcomp.hpp"
#include "winapi/graphics/vulkan/vulkan.hpp"
#include "audio/contracts/audio_renderer.hpp"
#include "audio/contracts/audio_capture.hpp"
#include "graphics/contracts/presentation_state.hpp"
#include "graphics/common/surface_presenter.hpp"

// -- winmm --
#include "winapi/media/winmm/winmm.hpp"

// -- capsule / windowing --
#include "capsule/vfs/vfs.hpp"
#include "capsule/registry/registry.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/contracts/audio_bridge.hpp"
#include "windowing/contracts/wave_output.hpp"

// Forward declarations
class GuestMemory;

namespace vitreo::ipc {
class ProcessSession;
enum class WorkerTokenContext : u8;
}

namespace vitreo::platform {
class ElevationAuthorizer;
}

namespace vitreo::cli {

// Immediately above the synthetic module arena (0x7C000000), keeping the two
// as one block at the top of the 2GB user window instead of splitting the
// guest's usable space down the middle. See SYNTHETIC_MODULE_BASE_X86.
constexpr GuestAddr THUNK_REGION_BASE_X86 = 0x7D000000;
// The x64 thunk region is host-probed alongside the other two high arenas;
// see core/memory/x64_high_arena.hpp. There is no fixed x64 constant.

/// Owns all WinAPI manager instances and shared infrastructure.
///
/// Members are declared in dependency order so that the constructor's
/// initialiser list wires up cross-references correctly.
/// Non-copyable, non-moveable (contains mutex-owning members).
struct RuntimeContext {
    // ---- Core infrastructure ----
    winapi::GuestElevationState  elevation_state;
    winapi::HandleTable           handle_table;
    capsule::Vfs                  vfs;
    capsule::Registry             registry;
    winapi::ThunkRegistry         thunk_registry;

    // ---- kernel32 managers ----
    winapi::ProcessManager        process_mgr;
    winapi::ConsoleManager        console_mgr;
    winapi::FileManager           file_mgr;
    winapi::SysInfoManager        sysinfo_mgr;
    winapi::CriticalSectionManager cs_mgr;
    winapi::FlsManager            fls_mgr;
    winapi::FileMappingManager    fmm_mgr;
    winapi::FileSearchManager     fsearch_mgr;
    winapi::TimeManager           time_mgr;
    winapi::VirtualMemoryManager  vmm;
    winapi::VirtualQueryManager   vqm;
    winapi::HeapManager           heap_mgr;
    winapi::ComTaskMemoryManager  com_task_memory_mgr;
    winapi::GlobalLocalManager    gl_mgr;   // after heap_mgr (uses process heap)
    winapi::HeapExtended          heap_ext;
    winapi::TlsManager            tls_mgr;
    winapi::ThreadManager         thread_mgr;
    winapi::ThreadpoolManager     threadpool_mgr;
    winapi::SyncManager           sync_mgr;
    winapi::EnvironmentManager    env_mgr;
    winapi::ModuleManager         module_mgr;
    // Serves rpcrt4's NdrDll* proxy self-registration; needs module_mgr for the
    // proxy DLL's path and the capsule registry for the HKCR entries.
    winapi::NdrProxyRegistrar     ndr_proxy_registrar;
    winapi::StorageManager        storage_mgr;
    winapi::LocaleStringManager   locale_str_mgr;
    winapi::FileOpsManager        file_ops_mgr;
    winapi::Kernel32ExtManager    k32_ext_mgr;

    // ---- msvcrt managers ----
    winapi::CrtInitManager        crt_init_mgr;
    winapi::CrtStringManager      crt_str_mgr;
    winapi::CrtMathManager        crt_math_mgr;
    winapi::CrtStdioManager       crt_stdio_mgr;
    winapi::CrtDataBlock          crt_data;
    winapi::CrtMemoryManager      crt_mem_mgr;
    winapi::CrtEnvironmentManager crt_environment_mgr;
    winapi::CrtLocaleManager      crt_locale_mgr;
    winapi::CrtTimeManager        crt_time_mgr;

    // ---- ntdll managers ----
    winapi::NtdllManager          ntdll_mgr;
    winapi::NtdllExtManager       ntdll_ext_mgr;

    // ---- advapi32 managers ----
    winapi::RegistryThunks        reg_thunks;
    winapi::SecurityStubs         sec_stubs;
    winapi::ScmManager            scm_mgr;   // Service Control Manager

    // ---- gdi32 ----
    winapi::GdiManager            gdi_mgr;

    // ---- windowing infrastructure ----
    windowing::HwndManager        hwnd_mgr;
    windowing::MessageQueue       msg_queue;
    windowing::CocoaBridge        cocoa_bridge;

    // ---- user32 managers ----
    winapi::WindowClassManager    wclass_mgr;
    winapi::WindowManager         window_mgr;
    winapi::ShowWindowManager     show_mgr;
    winapi::MessageLoopManager    msgloop_mgr;
    winapi::MessageSendManager    msgsend_mgr;
    winapi::DefWindowProcManager  dwp_mgr;
    winapi::PaintingManager       paint_mgr;
    winapi::ResourceManager       resource_mgr;
    winapi::DialogManager          dialog_mgr;
    winapi::User32ExtManager      u32_ext_mgr;

    // ---- ole32 / shell32 / version / ws2_32 / mswsock ----
    winapi::ComCoreManager        com_mgr;
    winapi::CombaseManager        combase_mgr;
    winapi::DWriteManager        dwrite_mgr;
    winapi::D2DManager           d2d1_mgr;
    winapi::UrlmonManager        urlmon_mgr;
    winapi::ShellPathManager      shell_mgr;
    winapi::VersionManager        ver_mgr;
    winapi::AmsiManager           amsi_mgr;
    winapi::Lz32Manager           lz32_mgr;
    winapi::ImageHlpManager       imagehlp_mgr;
    winapi::Ws2Manager            ws2_mgr;
    winapi::MswsockManager        mswsock_mgr;

    // ---- comctl32 ----
    winapi::ComCtl32Manager       comctl32_mgr;

    // ---- gdiplus ----
    winapi::GdiplusManager        gdiplus_mgr;

    // ---- oleaut32 ----
    winapi::OleAut32Manager       oleaut32_mgr;

    // ---- authz / sspicli / wkscli / netutils / psapi ----
    winapi::AuthzManager          authz_mgr;
    winapi::SspiCliManager        sspicli_mgr;
    winapi::WksCliManager         wkscli_mgr;
    winapi::HttpApiManager        httpapi_mgr;
    winapi::NetUtilsManager       netutils_mgr;
    winapi::PsapiManager          psapi_mgr;
    winapi::Crypt32Manager        crypt32_mgr;
    winapi::Secur32Manager        secur32_mgr;

    // ---- bcrypt / wsock32 ----
    winapi::BcryptManager         bcrypt_mgr;
    winapi::Wsock32Manager         wsock32_mgr;

    // ---- winmm ----
    std::unique_ptr<windowing::AudioBridge> audio_bridge;
    std::unique_ptr<windowing::WaveOutput> wave_output;
    winapi::WinmmManager          winmm_mgr;

    // ---- winhttp / wininet ----
    winapi::WinHttpManager        winhttp_mgr;
    winapi::WinInetManager        wininet_mgr;

    // ---- msimg32 / winspool / imm32 / iphlpapi / opengl32 ----
    winapi::Msimg32Manager        msimg32_mgr;
    winapi::MsiManager            msi_mgr;
    winapi::MsxmlManager          msxml_mgr;
    winapi::MfPlatManager         mfplat_mgr;
    winapi::MfPlayManager         mfplay_mgr;
    winapi::CabinetManager        cabinet_mgr;
    winapi::SetupapiGuestServices setupapi_services;
    /// Calls a guest function (or a thunk) from host code running on the
    /// current guest thread; the system programs (vitreo_sysprog.dll) drive
    /// the guest's own LoadLibrary/GetProcAddress/DllRegisterServer with it.
    std::function<u64(GuestAddr, const u64*, usize, bool)> system_program_call;
    winapi::WinspoolManager       winspool_mgr;
    winapi::Imm32Manager          imm32_mgr;
    winapi::IphlpapiManager       iphlpapi_mgr;
    winapi::DnsapiManager         dnsapi_mgr;
    winapi::Opengl32Manager       opengl32_mgr;
    winapi::Glu32Manager          glu32_mgr;
    winapi::Msvfw32Manager        msvfw32_mgr;
    winapi::OledlgManager         oledlg_mgr;

    // ---- DirectX graphics frontends ----
    graphics::PresentationState  graphics_presentation;
    std::unique_ptr<graphics::ISurfacePresenter> graphics_surface_presenter;
    winapi::DirectDrawManager     ddraw_mgr;
    winapi::DirectInputManager    dinput_mgr;
    winapi::D3D9Manager           d3d9_mgr;
    winapi::DxgiManager           dxgi_mgr;
    winapi::D3D11Manager          d3d11_mgr;
    winapi::D3D12Manager          d3d12_mgr;
    winapi::DcompManager          dcomp_mgr;
    winapi::VulkanManager         vulkan_mgr;

    // ---- DirectX audio frontend ----
    std::unique_ptr<audio::IAudioRenderer> audio_renderer;
    /// Host microphone access behind waveIn, DirectSound capture and the
    /// MMDevice capture endpoints (one device list for all three).
    std::unique_ptr<audio::IAudioCapture> audio_capture;
    winapi::DirectSoundManager    dsound_mgr;

    // ---- Service registry ----
    ServiceRegistry               registry_svc;
    ProcessServices               process_services;
    MemoryServices                memory_services;
    FileServices                  file_services;
    ModuleServices                module_services;
    SyncServices                  sync_services;
    UIServices                    ui_services;
    CapsuleServices               capsule_services;

    /// Access the service registry for typed service lookup.
    [[nodiscard]] ServiceRegistry& services() noexcept { return registry_svc; }

    /// Construct all managers, wiring up cross-references.
    /// @param memory       Guest memory (must outlive this context)
    /// @param pe_path      Path to PE file (used for EnvironmentManager command line)
    /// @param capsule_name Name of the capsule whose drives/registry to mount.
    ///                     Empty string means "default".
    /// @param large_address_aware Whether a 32-bit image opted into addresses
    ///                            above the normal 2 GiB user-mode boundary.
    /// @param process_session Session owner used by CreateProcess/ShellExecute.
    RuntimeContext(GuestMemory& memory, std::string_view pe_path,
                   std::string_view capsule_name, bool is_64bit,
                   bool large_address_aware,
                   bool console_attached,
                   ipc::ProcessSession* process_session,
                   bool elevated,
                   std::string elevation_credential,
                   ipc::WorkerTokenContext token_context,
                   platform::ElevationAuthorizer* elevation_authorizer,
                   std::span<const winapi::Handle> inherited_handles = {});

    /// Destructor — detach the handle-close callback before managers are
    /// destroyed so HandleTable's destructor doesn't call into dead objects.
    ~RuntimeContext();

    /// Persist the in-memory registry to the capsule's registry directory.
    void save_registry();

    /// Get the capsule root directory path
    [[nodiscard]] const std::string& capsule_root() const noexcept {
        return m_capsule_root;
    }

    // Non-copyable, non-moveable
    RuntimeContext(const RuntimeContext&)            = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    RuntimeContext(RuntimeContext&&)                 = delete;
    RuntimeContext& operator=(RuntimeContext&&)      = delete;

private:
    std::string m_capsule_root;  ///< Host-side capsule root directory
};

} // namespace vitreo::cli
