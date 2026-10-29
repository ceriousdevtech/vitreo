// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: ole32, oleacc, shell32, shlwapi, kernelbase and the shell support DLLs

#include "winapi/com/ole32/wic.hpp"
#include "winapi/com/ole32/shell_item_dialogs.hpp"
#include "dll_registry_internal.hpp"
#include "winapi/system/shell32/app_user_model_id.hpp"

#include "ipc/hwnd_registry.hpp"

#include "windowing/cocoa/cocoa_bridge.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "winapi/ui/oleacc/oleacc.hpp"
#include "winapi/system/shlwapi/shlwapi.hpp"
#include "winapi/ui/comdlg32/comdlg32.hpp"
#include "winapi/ui/oledlg/oledlg.hpp"
#include "winapi/networking/mpr/mpr.hpp"
#include "winapi/networking/urlmon/urlmon.hpp"
#include "winapi/system/credui/credui.hpp"
#include "winapi/system/cryptui/cryptui.hpp"
#include "winapi/system/esent/esent.hpp"
#include "winapi/system/imagehlp/imagehlp.hpp"

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace vitreo::cli {

void register_shell_ole_dlls(RuntimeContext& ctx, winapi::DllExports& kernelbase, std::unordered_set<std::string>& kernelbase_seen) {
    auto& reg = ctx.thunk_registry;

    // -- ole32.dll --
    {
        ctx.com_mgr.set_thunk_registry(&ctx.thunk_registry);
        const GuestAddr com_scratch = reserve_scratch_region(
            ctx.vmm, "ole32 COM", winapi::ComCoreManager::COM_SCRATCH_BASE,
            winapi::ComCoreManager::COM_SCRATCH_SIZE);
        if (com_scratch != 0) ctx.com_mgr.set_scratch_base(com_scratch);
        ctx.com_mgr.set_vfs(&ctx.vfs);
        ctx.com_mgr.set_cocoa_bridge(&ctx.cocoa_bridge);
        ctx.com_mgr.set_sync_manager(&ctx.sync_mgr);
        ctx.com_mgr.set_message_send(&ctx.msgsend_mgr);
        ctx.com_mgr.set_registry(&ctx.registry);
        ctx.com_mgr.set_module_manager(&ctx.module_mgr);
        // IsWindow, for SHGetPropertyStoreForWindow's writes: this process's
        // windows, or a peer's published one (its store is shared).
        ctx.com_mgr.set_window_exists([&ctx](u32 hwnd) {
            return ctx.hwnd_mgr.is_valid(hwnd) || ipc::hwnd_registry_is_window(hwnd);
        });
        // Structured storage (StgCreateDocfile / IStorage / IStream) is part
        // of ole32.dll, so its exports merge into the same table.
        ctx.storage_mgr.set_vfs(&ctx.vfs);
        ctx.storage_mgr.set_thunk_registry(&ctx.thunk_registry);
        // Guest-visible memory for the IStorage/IStream objects and vtables.
        ctx.vmm.virtual_alloc(winapi::OLE_STORAGE_SCRATCH_BASE,
                              winapi::OLE_STORAGE_SCRATCH_SIZE,
                              0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                              0x04   /*PAGE_READWRITE*/);
        // Holds re-materialised objects for `throw;` — the original lives in
        // the frame being unwound and does not survive the catch block.
        ctx.vmm.virtual_alloc(winapi::CXX_RETHROW_SCRATCH_BASE,
                              winapi::CXX_RETHROW_SCRATCH_SIZE,
                              0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                              0x04   /*PAGE_READWRITE*/);
        auto ole_exports = winapi::create_com_core_thunks(ctx.com_mgr);
        for (auto& entry : winapi::create_storage_thunks(ctx.storage_mgr).entries) {
            ole_exports.entries.push_back(std::move(entry));
        }
        (void)reg.register_dll("ole32.dll", std::move(ole_exports));
    }

    // -- windowscodecs.dll --
    // WIC is a System32 DLL on every Windows install; its coclasses live in
    // ole32's COM core (wic.cpp), this is the module and its exports.
    (void)reg.register_dll("windowscodecs.dll",
                           winapi::create_windowscodecs_thunks(ctx.com_mgr));

    // -- oleacc.dll --
    {
        auto oleacc_exports = winapi::create_oleacc_thunks(ctx.hwnd_mgr);
        (void)reg.register_dll("oleacc.dll", std::move(oleacc_exports));
    }

    // -- shell32.dll --
    {
        auto shell_exports = winapi::create_shell_path_thunks(ctx.shell_mgr);
        {
            // Audit completions. Shell item parsing needs the namespace; the notification state is a real query this host can answer.
            winapi::DllExports audit;
            audit.entries.push_back({"SHCreateItemFromParsingName", 0,
                [&ctx](winapi::Marshaler& m) {
                    m.set_return(ctx.com_mgr.item_dialogs().create_item_from_parsing_name(m));
                }, 16});
            // HRESULT SHGetPropertyStoreForWindow(HWND, REFIID, void**):
            // the window's own property store (System.AppUserModel.*).
            audit.entries.push_back({"SHGetPropertyStoreForWindow", 0,
                [&ctx](winapi::Marshaler& m) {
                    m.set_return(ctx.com_mgr.create_window_property_store(m));
                }, 12});
            audit.entries.push_back({"SHParseDisplayName", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(2);
                    (void)m.arg(3);
                    (void)m.arg(4);
                    if (const GuestAddr o = m.arg_ptr(1); o != 0) (void)m.write_ptr(o, 0);
                    m.set_return(0x80004001u);
                }, 20});
            audit.entries.push_back({"SHQueryUserNotificationState", 0,
                [](winapi::Marshaler& m) {
                    const GuestAddr out = m.arg_ptr(0);
                    if (out == 0) { m.set_return(0x80070057u); return; }
                    // QUNS_ACCEPTS_NOTIFICATIONS = 5: no presentation mode, no
                    // full-screen app blocking notifications on this desktop.
                    (void)m.write_u32(out, 5u);
                    m.set_return(0u);
                }, 4});
            audit.entries.push_back({"SHGetStockIconInfo", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    (void)m.arg(1);
                    if (const GuestAddr info = m.arg_ptr(2); info != 0)
                        (void)m.write_u32(info + 4, 0);  // hIcon = NULL
                    // Vitreo ships no stock icon set.
                    m.set_return(0x80004001u);
                }, 12});
            // void SHAddToRecentDocs(UINT uFlags, LPCVOID pv): the Recent
            // folder's shortcuts (app_user_model_id.hpp).
            audit.entries.push_back({"SHAddToRecentDocs", 0,
                [&ctx](winapi::Marshaler& m) {
                    winapi::shell::add_to_recent_docs(m, ctx.shell_mgr, ctx.vfs);
                    m.set_return(0);
                }, 8});
            merge_exports(shell_exports, std::move(audit));
        }
        (void)reg.register_dll("shell32.dll", std::move(shell_exports));
        // shfolder.dll is Windows' compatibility forwarder for the
        // SHGetFolderPath family: exactly SHGetFolderPathA (#1) and
        // SHGetFolderPathW (#2), both forwarding to shell32 (Windows 11 export
        // table). An exclusive-mode title LoadLibrary-s it by name.
        {
            winapi::DllExports shfolder;
            for (auto& e : winapi::create_shell_path_thunks(ctx.shell_mgr).entries) {
                if (e.name == "SHGetFolderPathA") e.ordinal = 1;
                else if (e.name == "SHGetFolderPathW") e.ordinal = 2;
                else continue;
                shfolder.entries.push_back(std::move(e));
            }
            (void)reg.register_dll("shfolder.dll", std::move(shfolder));
        }
    }

    // -- shlwapi.dll --
    {
        // SHDeleteKeyA/W are registry operations, so shlwapi gets the same
        // RegistryThunks advapi32 uses rather than a private view of the
        // capsule registry.
        auto shlwapi_exports = winapi::create_shlwapi_thunks(
            &ctx.vfs, &ctx.com_mgr, &ctx.reg_thunks, &ctx.gl_mgr);
        {
            // Audit completions: path helpers are pure string work and are real; association lookup has no database behind it.
            winapi::DllExports audit;
            // PathRemoveExtension{A,W} live in shlwapi_path_extras.cpp.
            audit.entries.push_back({"PathIsNetworkPathW", 0,
                [](winapi::Marshaler& m) {
                    const GuestAddr path = m.arg_ptr(0);
                    if (path == 0) { m.set_return(0u); return; }
                    const auto c0 = m.memory().read_u16(path);
                    const auto c1 = m.memory().read_u16(path + 2);
                    if (c0.is_err() || c1.is_err()) { m.set_return(0u); return; }
                    // A UNC path, or a drive letter mapped to a share. Vitreo
                    // maps no network drives, so only UNC qualifies.
                    const bool unc = (c0.value() == u'\\' && c1.value() == u'\\') ||
                                     (c0.value() == u'/' && c1.value() == u'/');
                    m.set_return(unc ? 1u : 0u);
                }, 4});
            audit.entries.push_back({"AssocQueryStringW", 0,
                [](winapi::Marshaler& m) {
                    // HRESULT AssocQueryStringW(ASSOCF, ASSOCSTR, LPCWSTR assoc,
                    //   LPCWSTR extra, LPWSTR out, DWORD* pcchOut)
                    // pcchOut is required and counts characters including NUL.
                    // A short buffer returns HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
                    // and the required count. Unknown types are
                    // HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) = 0x80070483.
                    (void)m.arg(0);
                    const u32 kind = static_cast<u32>(m.arg(1));
                    const auto assoc_ptr = m.arg_ptr(2);
                    (void)m.arg_ptr(3);
                    const auto out = m.arg_ptr(4);
                    const auto pcch_ptr = m.arg_ptr(5);
                    if (pcch_ptr == 0) {
                        m.set_return(0x80070057u);
                        return;
                    }
                    std::string assoc;
                    if (assoc_ptr != 0) {
                        auto read = m.read_wide_string(assoc_ptr);
                        if (read.is_ok()) assoc = std::move(read.value());
                    }
                    for (char& c : assoc) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    const bool web = assoc == "http" || assoc == "https" ||
                                     assoc == "mailto" ||
                                     assoc.rfind("http\\", 0) == 0 ||
                                     assoc.rfind("https\\", 0) == 0 ||
                                     assoc.rfind("mailto\\", 0) == 0;
                    const auto handler = vitreo::windowing::host_http_handler();
                    if (!web || handler.display_name.empty()) {
                        (void)m.write_u32(pcch_ptr, 0);
                        m.set_return(0x80070483u);
                        return;
                    }
                    std::string value;
                    switch (kind) {
                    case 1: // ASSOCSTR_COMMAND
                        value = handler.command;
                        break;
                    case 2: // ASSOCSTR_EXECUTABLE
                        value = handler.guest_executable;
                        break;
                    case 4: // ASSOCSTR_FRIENDLYAPPNAME
                        value = handler.display_name;
                        break;
                    case 20: // ASSOCSTR_PROGID
                        value = handler.display_name;
                        break;
                    default:
                        (void)m.write_u32(pcch_ptr, 0);
                        m.set_return(0x80070483u);
                        return;
                    }
                    const auto need = static_cast<u32>(value.size() + 1);
                    const u32 have = m.read_u32(pcch_ptr).value_or(0);
                    if (out == 0 || have < need) {
                        (void)m.write_u32(pcch_ptr, need);
                        m.set_return(0x8007007Au);
                        return;
                    }
                    if (m.write_wide_string(out, value.c_str()).is_err()) {
                        m.set_return(0x80070057u);
                        return;
                    }
                    (void)m.write_u32(pcch_ptr, need);
                    std::fprintf(stderr,
                                 "[vitreo] AssocQueryStringW '%s' kind=%u -> %s\n",
                                 assoc.c_str(), kind, value.c_str());
                    m.set_return(0);
                }, 24});
            merge_exports(shlwapi_exports, std::move(audit));
        }
        collect_kernelbase_surface(kernelbase, shlwapi_exports, kernelbase_seen);
        (void)reg.register_dll("shlwapi.dll", std::move(shlwapi_exports));
    }

    // KernelBase: its own entries plus every export it shares with the DLLs
    // above (collected as each was built). A name Vitreo implements nowhere
    // stays absent, so GetProcAddress fails for it exactly as for any other
    // unimplemented export.
    (void)reg.register_dll("kernelbase.dll", std::move(kernelbase));
}

void register_shell_support_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- shcore.dll (Win8.1+ DPI / process-info) --
    {
        winapi::DllExports sh;
        // HRESULT GetDpiForMonitor(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*)
        sh.entries.push_back({"GetDpiForMonitor", 0,
            [&ctx](winapi::Marshaler& m){ ctx.u32_ext_mgr.impl_GetDpiForMonitor(m); }, 16});
        // HRESULT SetProcessDpiAwareness(PROCESS_DPI_AWARENESS value)
        sh.entries.push_back({"SetProcessDpiAwareness", 0,
            [&ctx](winapi::Marshaler& m){ ctx.u32_ext_mgr.impl_SetProcessDpiAwareness(m); }, 4});
        // HRESULT GetProcessDpiAwareness(HANDLE, PROCESS_DPI_AWARENESS*)
        sh.entries.push_back({"GetProcessDpiAwareness", 0,
            [&ctx](winapi::Marshaler& m){ ctx.u32_ext_mgr.impl_GetProcessDpiAwareness(m); }, 8});
        {
            // Audit completion: the monitor scale factor is a real query.
            winapi::DllExports audit;
            audit.entries.push_back({"GetScaleFactorForMonitor", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    const GuestAddr out = m.arg_ptr(1);
                    if (out == 0) { m.set_return(0x80070057u); return; }
                    // DEVICE_SCALE_FACTOR is a percentage; Vitreo presents at 100%
                    // and does its scaling in the compositor.
                    (void)m.write_u32(out, 100u);
                    m.set_return(0u);
                }, 8});
            merge_exports(sh, std::move(audit));
        }
        (void)reg.register_dll("shcore.dll", std::move(sh));
    }

    // -- comdlg32.dll --
    {
        auto comdlg_exports = winapi::create_comdlg32_thunks(&ctx.cocoa_bridge, &ctx.vfs);
        {
            // Audit completion: there is no print dialog UI.
            winapi::DllExports audit;
            audit.entries.push_back({"PrintDlgExW", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg_ptr(0);
                    m.set_return(0x80004001u);
                }, 4});
            merge_exports(comdlg_exports, std::move(audit));
        }
        (void)reg.register_dll("comdlg32.dll", std::move(comdlg_exports));
    }

    // -- oledlg.dll --
    {
        ctx.oledlg_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.oledlg_mgr.set_registry(&ctx.registry);
        (void)reg.register_dll("oledlg.dll", winapi::create_oledlg_thunks(ctx.oledlg_mgr));
    }

    // -- version.dll --
    {
        auto ver_exports = winapi::create_version_thunks(ctx.ver_mgr);
        (void)reg.register_dll("version.dll", std::move(ver_exports));
    }

    // -- amsi.dll --
    {
        auto amsi_exports = winapi::create_amsi_thunks(ctx.amsi_mgr);
        (void)reg.register_dll("amsi.dll", std::move(amsi_exports));
    }

    // -- lz32.dll --
    {
        auto lz_exports = winapi::create_lz32_thunks(ctx.lz32_mgr);
        (void)reg.register_dll("lz32.dll", std::move(lz_exports));
    }

    // -- mpr.dll --
    {
        auto mpr_exports = winapi::create_mpr_thunks();
        (void)reg.register_dll("mpr.dll", std::move(mpr_exports));
    }

    // -- urlmon.dll --
    {
        (void)reg.register_dll(
            "urlmon.dll", winapi::create_urlmon_thunks(ctx.urlmon_mgr));
        const auto address_or_zero = [](const auto& result) {
            return result.is_ok() ? result.value() : GuestAddr{0};
        };
        winapi::UrlmonManager::MethodThunks methods{};
        methods.query_interface = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_QueryInterface"));
        methods.add_ref = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_AddRef"));
        methods.release = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_Release"));
        methods.set_security_site = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_SetSecuritySite"));
        methods.get_security_site = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_GetSecuritySite"));
        methods.map_url_to_zone = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_MapUrlToZone"));
        methods.get_security_id = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_GetSecurityId"));
        methods.process_url_action = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_ProcessUrlAction"));
        methods.query_custom_policy = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_QueryCustomPolicy"));
        methods.set_zone_mapping = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_SetZoneMapping"));
        methods.get_zone_mappings = address_or_zero(
            reg.get_thunk_address("urlmon.dll", "_Urlmon_GetZoneMappings"));
        ctx.urlmon_mgr.set_method_thunks(methods);
    }

    // -- credui.dll --
    {
        (void)reg.register_dll("credui.dll", winapi::create_credui_thunks());
    }

    // -- cryptui.dll --
    {
        (void)reg.register_dll("cryptui.dll", winapi::create_cryptui_thunks());
    }

    // -- esent.dll --
    {
        (void)reg.register_dll("esent.dll", winapi::create_esent_thunks());
    }

    // -- imagehlp.dll --
    {
        auto imagehlp_exports =
            winapi::create_imagehlp_thunks(ctx.imagehlp_mgr);
        (void)reg.register_dll("imagehlp.dll", std::move(imagehlp_exports));
    }
}

} // namespace vitreo::cli
