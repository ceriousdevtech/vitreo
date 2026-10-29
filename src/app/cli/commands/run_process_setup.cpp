// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: configuring the RuntimeContext - process identity, capsule tree, main module

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/mapper/section_mapper.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "ipc/process_registry.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "windowing/resources/resource_walker.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <unistd.h>

namespace vitreo::cli {

namespace fs = std::filesystem;

std::optional<RunResult> configure_process_context(
    RuntimeContext& ctx, const RunConfig& config, const pe::ParsedPe& pe,
    const std::vector<u8>& pe_data, const std::string& win_exe_path) {
    if (ctx.thread_mgr.main_thread_id() == 0)
        return RunResult{1, 0, "Primary thread identity allocation failed", false};
    ctx.msg_queue.set_primary_thread_id(ctx.thread_mgr.main_thread_id());
    if (!ctx.scm_mgr.set_own_service_arguments_utf16(
            config.guest_service_arguments_utf16)) {
        return RunResult{1, 0, "Invalid encoded service argument", false};
    }
    ctx.sysinfo_mgr.set_windows_10_compatible(
        pe::pe_supports_windows_10(pe_data, pe));
    ctx.sysinfo_mgr.set_windows_81_compatible(
        pe::pe_supports_windows_81(pe_data, pe));
    // Child layered windows need a manifest targeting Windows 8 or later
    // (Learn: Window Features, "Layered Windows").
    ctx.window_mgr.set_child_layered_allowed(
        pe::pe_supports_windows_8(pe_data, pe) ||
        pe::pe_supports_windows_81(pe_data, pe) ||
        pe::pe_supports_windows_10(pe_data, pe));
    // An image linked for a subsystem older than 6.0 sees SM_CXFRAME with
    // the padded border folded in (measured, s1win_display_probe).
    ctx.u32_ext_mgr.set_legacy_frame_metrics(pe.major_subsystem_version < 6);
    // A manifest <dpiAwareness> / <dpiAware> sets the process awareness
    // before any code runs (Learn: Setting the default DPI awareness).
    if (const i64 dpi_ctx = pe::pe_dpi_awareness_context(pe_data, pe); dpi_ctx != 0) {
        ctx.u32_ext_mgr.set_manifest_dpi_awareness_context(dpi_ctx);
    }
    // A Common-Controls 6.0 dependency makes the default activation context
    // give windows comctl32 v6's Button (BCM_* messages).
    ctx.msgsend_mgr.set_common_controls_v6(pe::pe_common_controls_v6(pe_data, pe));
    // Fusion activeCodePage=UTF-8 → GetACP / CP_ACP = 65001 (Win10 1903+).
    if (pe::pe_active_code_page_utf8(pe_data, pe)) {
        winapi::codepage::set_ansi_code_page(winapi::codepage::kCpUtf8);
    } else {
        winapi::codepage::set_ansi_code_page(winapi::codepage::kAnsiCodePage);
    }
    for (const auto& [name, value] : config.guest_environment) {
        ctx.env_mgr.set_variable(name, value);
    }
    // WOW64 view: a 32-bit process sees PROCESSOR_ARCHITECTURE=x86 and the
    // (x86) ProgramFiles whatever block it inherited.
    ctx.env_mgr.apply_process_bitness(pe.is_64bit);

    // Supply the stable executable name before any guest Win32 call can
    // initialize Cocoa. The generated host app bundle owns the Dock label;
    // this establishes the matching process and application-menu identity.
    if (pe.subsystem == pe::Subsystem::WindowsGui) {
        std::string requested_name = config.app_name;
        if (requested_name.empty()) {
            if (const char* inherited_name =
                    std::getenv("VITREO_MACOS_APP_NAME")) {
                requested_name = inherited_name;
            }
        }
        const std::string app_name = select_application_display_name(
            requested_name, {}, win_exe_path);
        if (!app_name.empty()) {
            ctx.cocoa_bridge.set_app_name(app_name);
        }
    }

    if (!config.conpty_handles.empty()) {
        // The parent named the pseudoconsole's two pipe ends. Adopt them as
        // this process's console so console output becomes VT in the pipe the
        // parent reads, instead of going to the host stdout where the parent
        // would never see it. Ordered AFTER the inherited pipes are imported:
        // the handles have to exist before anything can write to them.
        unsigned in_h = 0, out_h = 0;
        if (std::sscanf(config.conpty_handles.c_str(), "%u,%u", &in_h, &out_h)
                == 2) {
            ctx.console_mgr.set_handle_writer(
                [&ctx](winapi::Handle h, std::string_view bytes) {
                    return ctx.file_mgr.write_handle_bytes(h, bytes);
                });
            ctx.console_mgr.attach_pseudo_console(
                static_cast<winapi::Handle>(in_h),
                static_cast<winapi::Handle>(out_h));
        } else {
            std::fprintf(stderr,
                         "[conpty] malformed --conpty-handles '%s'\n",
                         config.conpty_handles.c_str());
        }
    }
    if (!config.inherited_pipes.empty() &&
        !ctx.file_mgr.import_inherited_named_pipes(config.inherited_pipes)) {
        return RunResult{1, 0, "Failed to import inherited named-pipe handles", false};
    }
    if (!config.inherited_sync.empty() &&
        !ctx.sync_mgr.import_inherited_handles(config.inherited_sync)) {
        return RunResult{1, 0, "Failed to import inherited sync handles", false};
    }
    if (!config.inherited_mappings.empty() &&
        !ctx.fmm_mgr.import_inherited_handles(config.inherited_mappings)) {
        return RunResult{
            1, 0, "Failed to import inherited file-mapping handles", false};
    }
    // The creator's STARTUPINFO (CreateProcess): GetStartupInfo reports it,
    // and its std handles -- already imported above under the same numeric
    // values -- become this process's GetStdHandle values. Consumed here so a
    // grandchild launched another way does not inherit it.
    if (const char* si_text = std::getenv("VITREO_STARTUP_INFO")) {
        winapi::GuestStartupInfo si{};
        if (winapi::decode_guest_startup_info(si_text, si)) {
            const char* kinds_env = std::getenv("VITREO_STARTUP_STD_KINDS");
            const std::string kinds = kinds_env ? kinds_env : "n,n,n";
            constexpr u32 kStd[3] = {winapi::STD_INPUT_HANDLE,
                                     winapi::STD_OUTPUT_HANDLE,
                                     winapi::STD_ERROR_HANDLE};
            u32* values[3] = {&si.std_input, &si.std_output, &si.std_error};
            for (usize i = 0; i < 3; ++i) {
                const char kind = kinds.size() > i * 2 ? kinds[i * 2] : 'n';
                if (kind == 'n') continue;
                if (kind == 'i') {
                    *values[i] = ctx.console_mgr.console_input_handle();
                } else if (kind == 'o') {
                    *values[i] = ctx.console_mgr.console_output_handle(i == 2);
                }
                (void)ctx.console_mgr.set_std_handle(kStd[i], *values[i]);
            }
            winapi::guest_startup_info() = si;
            // STARTF_TITLEISAPPID: lpTitle is this process's AppUserModelID
            // and its explicit process identity (window_propstore_probe).
            constexpr u32 kStartfTitleIsAppId = 0x00001000u;
            const char* title = std::getenv("VITREO_STARTUP_TITLE");
            if ((si.flags & kStartfTitleIsAppId) != 0 && title && title[0] != '\0') {
                const std::vector<u16> wide = winapi::utf8_to_utf16(title);
                ctx.shell_mgr.set_explicit_app_user_model_id(
                    std::u16string(wide.begin(), wide.end()));
            }
        }
        ::unsetenv("VITREO_STARTUP_TITLE");
        ::unsetenv("VITREO_STARTUP_INFO");
        ::unsetenv("VITREO_STARTUP_STD_KINDS");
    }

    return std::nullopt;
}

void ensure_capsule_directory_skeleton(RuntimeContext& ctx) {
    // Ensure standard Windows directory structure exists in the capsule.
    // This handles capsules created before directory bootstrapping was added.
    std::string c_root = ctx.capsule_root() + "/drives/c";
    std::error_code ec;
    for (const char* dir : {
        "/Windows",
        "/Windows/System32",
        "/Windows/SysWOW64",
        "/Windows/Fonts",
        "/Windows/Temp",
        "/Windows/Installer",  // the Windows Installer's cache and custom-action temp folder
        "/Program Files",
        "/Program Files/Common Files",
        "/Program Files (x86)",
        "/Program Files (x86)/Common Files",
        "/ProgramData",
        "/ProgramData/Microsoft/Windows/Start Menu",
        "/ProgramData/Microsoft/Windows/Start Menu/Programs",
        "/ProgramData/Microsoft/Windows/Start Menu/Programs/Startup",
        // Shell folders — installers place shortcuts here; must match the
        // guest paths in HKCU\...\Explorer\Shell Folders (init_defaults).
        "/Windows/Desktop",
        "/Windows/Start Menu",
        "/Windows/Start Menu/Programs",
        "/Windows/Start Menu/Programs/Startup",
        "/Users",
        "/Users/Public",
        "/Users/Public/Desktop",
        "/Users/Default",
        "/Users/Default/Desktop",
        "/Users/Default/Documents",
        "/Users/Default/Downloads",
        "/Users/Default/Start Menu",
        "/Users/Default/Start Menu/Programs",
        "/Users/Default/Start Menu/Programs/Startup",
        "/Users/Default/AppData",
        "/Users/Default/AppData/Local",
        "/Users/Default/AppData/Local/Temp",
        "/Users/Default/AppData/Roaming",
        "/Users/Default/AppData/LocalLow",
        // The rest of a Windows 10 profile: SHGetKnownFolderPath /
        // SHGetFolderPathW verify that a folder is present unless the
        // caller passes *_FLAG_DONT_VERIFY (shell_paths_probe.expected),
        // and these exist on every freshly created profile.
        "/Users/Default/Music",
        "/Users/Default/Pictures",
        "/Users/Default/Videos",
        "/Users/Default/Saved Games",
        "/Users/Default/Contacts",
        "/Users/Default/Favorites",
        "/Users/Default/Links",
        "/Users/Default/Searches",
        "/Users/Default/3D Objects",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Administrative Tools",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Recent",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/SendTo",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Templates",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Network Shortcuts",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Printer Shortcuts",
        "/Users/Default/AppData/Roaming/Microsoft/Windows/Libraries",
        "/Users/Default/AppData/Local/Microsoft/Windows/INetCache",
        "/Users/Default/AppData/Local/Microsoft/Windows/INetCookies",
        "/Users/Default/AppData/Local/Microsoft/Windows/History",
        "/Users/Default/AppData/Local/Microsoft/Windows/Burn/Burn",
        "/Users/Default/AppData/Local/Programs",
        "/Users/Public/Documents",
        "/Users/Public/Downloads",
        "/Users/Public/Music",
        "/Users/Public/Pictures",
        "/Users/Public/Videos",
        "/Users/Public/Libraries",
        "/ProgramData/Microsoft/Windows/Start Menu/Programs/Startup",
        "/ProgramData/Microsoft/Windows/Start Menu/Programs/Administrative Tools",
        "/ProgramData/Microsoft/Windows/Templates",
        "/Windows/Fonts",
        "/Windows/SysWOW64",
        "/Windows/resources",
        "/Program Files/Common Files",
        "/Program Files (x86)/Common Files",
        "/Vitreo/Apps",
        // The DNS configuration parser requires this file. When
        // the Tcpip registry watch succeeds but hosts is missing, DNS
        // configuration never completes: it needs both the registry config
        // and the hosts file unless the watch itself failed.
        "/Windows/System32/drivers/etc",
    }) {
        fs::create_directories(c_root + dir, ec);
    }
    const fs::path hosts_path =
        c_root + "/Windows/System32/drivers/etc/hosts";
    if (!fs::exists(hosts_path, ec)) {
        std::ofstream hosts(hosts_path);
        if (hosts) {
            hosts << "# Vitreo capsule hosts file.\n"
                     "#\n"
                     "127.0.0.1       localhost\n"
                     "::1             localhost\n";
        }
    }
}

void register_main_module(
    RuntimeContext& ctx, GuestMemory& memory, const RunConfig& config,
    const pe::ParsedPe& pe, const std::vector<u8>& pe_data,
    const pe::MappedImage& image, const std::string& win_exe_path,
    std::optional<ipc::ProcessRegistration>& process_registration) {
    // Register the main executable so GetModuleFileNameW returns a valid
    // Windows path and expose the file inside the capsule so CreateFileW /
    // GetFileAttributesW can find it.
    {
        ctx.module_mgr.set_main_module(
            image.base_address,
            win_exe_path,
            pe.data_directories[pe::DIR_EXCEPTION].VirtualAddress,
            pe.data_directories[pe::DIR_EXCEPTION].Size);
        if (pe.data_directories[pe::DIR_TLS].Size != 0) {
            ctx.module_mgr.set_main_module_tls(
                pe.data_directories[pe::DIR_TLS].VirtualAddress,
                pe.is_64bit ? 64u : 32u);
        }
        ctx.module_mgr.register_main_module_exports(
            image.base_address,
            win_exe_path,
            image.base_address + image.image_size,
            pe.is_64bit ? 64u : 32u,
            std::span<const u8>(pe_data));
        ctx.file_ops_mgr.set_module_manager(&ctx.module_mgr);
        ctx.resource_mgr.set_module_manager(&ctx.module_mgr);

        // Expose the image filename to NtdllExtManager so
        // NtQuerySystemInformation(SystemProcessInformation) lists the
        // current process by its actual name instead of a placeholder.
        {
            std::string image_name = win_exe_path;
            auto sep = image_name.find_last_of("\\/");
            if (sep != std::string::npos) image_name = image_name.substr(sep + 1);
            ctx.ntdll_ext_mgr.set_main_image_name(image_name);
            process_registration.emplace(ctx.capsule_root(), image_name,
                                         win_exe_path, ctx.thread_mgr.main_thread_id());
            const std::string process_capsule = ctx.capsule_root();
            auto snapshot_provider = [process_capsule] {
                return ipc::list_processes(process_capsule);
            };
            if (config.guest_parent_process_id != 0) {
                ctx.ntdll_mgr.set_current_parent_process_id(
                    config.guest_parent_process_id);
            } else {
                const u32 host_parent = static_cast<u32>(::getppid());
                const auto initial_processes = snapshot_provider();
                const bool parent_is_capsule_guest = std::any_of(
                    initial_processes.begin(), initial_processes.end(),
                    [host_parent](const ipc::ProcessSnapshotEntry& process) {
                        return process.pid == host_parent;
                    });
                ctx.ntdll_mgr.set_current_parent_process_id(
                    parent_is_capsule_guest ? host_parent : 0);
            }
            ctx.k32_ext_mgr.set_process_snapshot_provider(snapshot_provider);
            ctx.process_mgr.set_process_snapshot_provider(snapshot_provider);
            ctx.iphlpapi_mgr.set_pid_allowlist_provider(
                [snapshot_provider]() -> std::unordered_set<u32> {
                    std::unordered_set<u32> pids;
                    for (const auto& proc : snapshot_provider()) {
                        pids.insert(proc.pid);
                    }
                    return pids;
                });
        }

        VITREO_TRACE( "[vitreo] Main module: %s\n", win_exe_path.c_str());
    }

    // Wire PE resource directory info into the dialog manager
    if (pe.num_data_directories > pe::DIR_RESOURCE &&
        pe.data_directories[pe::DIR_RESOURCE].VirtualAddress != 0) {
        ctx.dialog_mgr.set_resource_info(
            image.base_address,
            pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
            pe.data_directories[pe::DIR_RESOURCE].Size);

        ctx.resource_mgr.set_resource_info(
            image.base_address,
            pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
            pe.data_directories[pe::DIR_RESOURCE].Size);

        ctx.k32_ext_mgr.set_resource_info(
            image.base_address,
            pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
            pe.data_directories[pe::DIR_RESOURCE].Size);

        ctx.u32_ext_mgr.set_resource_info(
            image.base_address,
            pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
            pe.data_directories[pe::DIR_RESOURCE].Size);

        ctx.winmm_mgr.set_resource_info(
            image.base_address,
            pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
            pe.data_directories[pe::DIR_RESOURCE].Size);

        if (config.verbose) {
            VITREO_TRACE( "[vitreo] Resource dir: RVA=0x%x, size=0x%x\n",
                         pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
                         pe.data_directories[pe::DIR_RESOURCE].Size);
        }

        // Extract the PE application icon and set it as the macOS dock icon
        auto ico_blob = windowing::extract_app_icon(
            memory, image.base_address,
            pe.data_directories[pe::DIR_RESOURCE].VirtualAddress,
            pe.data_directories[pe::DIR_RESOURCE].Size);
        if (!ico_blob.empty()) {
            ctx.cocoa_bridge.set_app_icon(ico_blob);
        }
    }
}

} // namespace vitreo::cli
