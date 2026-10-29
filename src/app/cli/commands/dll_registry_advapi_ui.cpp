// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: advapi32.dll, rpcrt4.dll, uiautomationcore.dll, gdi32.dll and user32.dll

#include "dll_registry_internal.hpp"
#include "winapi/user32/classes/system_classes.hpp"

#include "winapi/kernel32/error.hpp"
#include "winapi/com/rpcrt4/rpcrt4.hpp"
#include "winapi/ui/uiautomationcore/uiautomationcore.hpp"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>

namespace vitreo::cli {

void register_advapi_rpc_dlls(RuntimeContext& ctx, winapi::DllExports& kernelbase, std::unordered_set<std::string>& kernelbase_seen) {
    auto& reg = ctx.thunk_registry;

    // -- advapi32.dll --
    {
        winapi::DllExports advapi;
        merge_exports(advapi, winapi::create_advapi32_registry_thunks(ctx.reg_thunks));
        merge_exports(
            advapi,
            winapi::create_advapi32_security_thunks(
                ctx.sec_stubs, &ctx.vfs, &ctx.k32_ext_mgr));
        merge_exports(advapi, winapi::create_advapi32_scm_thunks(ctx.scm_mgr));
        {
            // RegGetKeySecurity / RegSetKeySecurity: the registry key's
            // descriptor, shared with Get/SetNamedSecurityInfoW.
            winapi::DllExports key_security;
            winapi::add_registry_key_security_exports(key_security, ctx.sec_stubs,
                                                      ctx.reg_thunks);
            merge_exports(advapi, std::move(key_security));
        }
        // -- Audit completions. ETW, LSA policy and the event log have no
        // host counterpart; each reports the value Windows reports when the
        // facility is absent, with correct stdcall cleanup.
        {
            winapi::DllExports extra;
            constexpr u32 ERROR_NOT_SUPPORTED_CODE = 50u;
            constexpr u32 STATUS_NOT_IMPLEMENTED = 0xC0000002u;
            constexpr u32 ERROR_CALL_NOT_IMPLEMENTED_CODE = 120u;

            // ETW (GetTraceEnableFlags/Level, GetTraceLoggerHandle,
            // TraceEvent) lives in advapi32/security/security_registration.cpp
            // (tools/oracle_probe/advapi_etw_probe).

            // LSA policy: no local security authority to open.
            extra.entries.push_back({"LsaOpenPolicy", 0,
                [](winapi::Marshaler& m) {
                    for (u32 i = 0; i < 3; ++i) (void)m.arg_ptr(i);
                    if (const GuestAddr out = m.arg_ptr(3); out != 0) {
                        (void)m.write_ptr(out, 0);
                    }
                    m.set_return(STATUS_NOT_IMPLEMENTED);
                }, 16});
            extra.entries.push_back({"LsaClose", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg_ptr(0);
                    m.set_return(0u);  // STATUS_SUCCESS: closing is harmless
                }, 4});
            extra.entries.push_back({"LsaAddAccountRights", 0,
                [](winapi::Marshaler& m) {
                    for (u32 i = 0; i < 4; ++i) (void)m.arg(i);
                    m.set_return(STATUS_NOT_IMPLEMENTED);
                }, 16});
            // The NTSTATUS -> Win32 mapping is a pure function and is real.
            extra.entries.push_back({"LsaNtStatusToWinError", 0,
                [](winapi::Marshaler& m) {
                    const u32 status = static_cast<u32>(m.arg(0));
                    u32 win32 = ERROR_NOT_SUPPORTED_CODE;
                    switch (status) {
                    case 0x00000000u: win32 = 0u; break;          // SUCCESS
                    case 0xC0000002u: win32 = 120u; break;        // NOT_IMPLEMENTED
                    case 0xC0000008u: win32 = 6u; break;          // INVALID_HANDLE
                    case 0xC000000Du: win32 = 87u; break;         // INVALID_PARAMETER
                    case 0xC0000022u: win32 = 5u; break;          // ACCESS_DENIED
                    case 0xC0000034u: win32 = 2u; break;          // OBJECT_NAME_NOT_FOUND
                    case 0xC000007Fu: win32 = 112u; break;        // DISK_FULL
                    case 0xC0000017u: win32 = 8u; break;          // NO_MEMORY
                    default: break;
                    }
                    m.set_return(win32);
                }, 4});

            // ImpersonateAnonymousToken / ImpersonateNamedPipeClient live in
            // advapi32/security/security_registration.cpp.
            extra.entries.push_back({"LogonUserW", 0,
                [](winapi::Marshaler& m) {
                    for (u32 i = 0; i < 5; ++i) (void)m.arg(i);
                    if (const GuestAddr out = m.arg_ptr(5); out != 0) {
                        (void)m.write_ptr(out, 0);
                    }
                    // No credential authority: the documented failure.
                    winapi::set_last_error(ERROR_NOT_SUPPORTED_CODE);
                    m.set_return(0u);
                }, 24});

            // LUIDs are real, unique, and monotonic per process.
            extra.entries.push_back({"AllocateLocallyUniqueId", 0,
                [](winapi::Marshaler& m) {
                    const GuestAddr out = m.arg_ptr(0);
                    if (out == 0) {
                        winapi::set_last_error(87u);
                        m.set_return(0u);
                        return;
                    }
                    static std::atomic<u64> next_luid{0x10000};
                    const u64 value =
                        next_luid.fetch_add(1, std::memory_order_relaxed);
                    (void)m.write_u32(out, static_cast<u32>(value));
                    (void)m.write_u32(out + 4,
                                      static_cast<u32>(value >> 32));
                    m.set_return(1u);
                }, 4});

            extra.entries.push_back({"AddConditionalAce", 0,
                [](winapi::Marshaler& m) {
                    for (u32 i = 0; i < 7; ++i) (void)m.arg(i);
                    // Conditional ACEs need a full SDDL evaluator.
                    winapi::set_last_error(ERROR_CALL_NOT_IMPLEMENTED_CODE);
                    m.set_return(0u);
                }, 28});

            // The event log has no host sink; ReportEvent writes the strings
            // where they can actually be seen rather than dropping them.
            extra.entries.push_back({"RegisterEventSourceA", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg_ptr(0);
                    auto name = m.read_ansi_string(m.arg_ptr(1));
                    VITREO_TRACE("[advapi32] RegisterEventSourceA('%s')\n",
                                 name.is_ok() ? name.value().c_str() : "");
                    m.set_return(0xE0E0u);  // opaque non-null log handle
                }, 8});
            extra.entries.push_back({"ReportEventA", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    const u32 type = static_cast<u32>(m.arg(1));
                    const u32 count = static_cast<u32>(m.arg(4));
                    const GuestAddr strings = m.arg_ptr(7);
                    for (u32 i = 0; i < count && strings != 0; ++i) {
                        const auto ptr = m.cpu_is_64bit()
                            ? m.memory().read_u64(strings + i * 8)
                            : m.memory().read_u32(strings + i * 4).map(
                                  [](u32 v) { return static_cast<u64>(v); });
                        if (ptr.is_err() || ptr.value() == 0) continue;
                        auto text = m.read_ansi_string(
                            static_cast<GuestAddr>(ptr.value()));
                        std::fprintf(stderr, "[eventlog type=%u] %s\n", type,
                                     text.is_ok() ? text.value().c_str() : "");
                    }
                    m.set_return(1u);
                }, 36});
            merge_exports(advapi, std::move(extra));
        }
        collect_kernelbase_surface(kernelbase, advapi, kernelbase_seen);
        (void)reg.register_dll("advapi32.dll", std::move(advapi));
    }

    // -- rpcrt4.dll --
    {
        auto rpcrt4_exports = winapi::create_rpcrt4_thunks(
            ctx.gl_mgr, ctx.com_task_memory_mgr, ctx.ndr_proxy_registrar);
        (void)reg.register_dll("rpcrt4.dll", std::move(rpcrt4_exports));
    }
}

void register_ui_core_dlls(RuntimeContext& ctx, bool guest_is_64bit, winapi::DllExports& kernelbase, std::unordered_set<std::string>& kernelbase_seen) {
    auto& reg = ctx.thunk_registry;

    // -- uiautomationcore.dll --
    {
        auto uia_exports = winapi::create_uiautomationcore_thunks();
        {
            // Audit completions. No UI Automation provider tree exists, so the
            // reserved sentinels cannot be handed out and events have nowhere to go.
            winapi::DllExports audit;
            audit.entries.push_back({"UiaDisconnectProvider", 0,
                [](winapi::Marshaler& m) {
                    (void)m.arg(0);
                    m.set_return(0x80004001u);
                }, 4});
            audit.entries.push_back({"UiaRaiseStructureChangedEvent", 0,
                [](winapi::Marshaler& m) {
                    // Same contract as the measured UiaRaiseAutomationEvent /
                    // UiaRaiseNotificationEvent: a NULL provider is
                    // E_INVALIDARG; otherwise no client is listening, so
                    // there is nothing to deliver and the call succeeds.
                    const GuestAddr provider = m.arg_ptr(0);
                    (void)m.arg(1);
                    (void)m.arg_ptr(2);
                    (void)m.arg(3);
                    m.set_return(provider == 0 ? 0x80070057u : 0u);
                }, 16});
            audit.entries.push_back({"UiaDisconnectAllProviders", 0,
                [](winapi::Marshaler& m) {
                    // Nothing is connected, so disconnecting everything succeeds.
                    m.set_return(0u);
                }, 0});
            // Measured (s1win_dwm_probe "-- uia"): a NULL out pointer is
            // E_INVALIDARG; otherwise Windows returns its singleton sentinel
            // IUnknown. The sentinel object is not implemented.
            for (const char* name : {"UiaGetReservedNotSupportedValue",
                                     "UiaGetReservedMixedAttributeValue"}) {
                audit.entries.push_back({name, 0,
                    [name](winapi::Marshaler& m) {
                        const GuestAddr o = m.arg_ptr(0);
                        if (o == 0) {
                            m.set_return(0x80070057u);
                            return;
                        }
                        (void)m.write_ptr(o, 0);
                        static std::mutex reported_mutex;
                        static std::set<std::string> reported;
                        std::lock_guard lock(reported_mutex);
                        if (reported.insert(name).second) {
                            std::fprintf(stderr,
                                         "[vitreo] UNIMPLEMENTED uiautomationcore!%s: "
                                         "no sentinel IUnknown\n", name);
                        }
                        m.set_return(0x80004001u);
                    }, 4});
            }
            merge_exports(uia_exports, std::move(audit));
        }
        (void)reg.register_dll("uiautomationcore.dll", std::move(uia_exports));
    }

    // -- gdi32.dll --
    {
        auto gdi_exports = winapi::make_gdi32_exports(ctx.gdi_mgr);
        (void)reg.register_dll("gdi32.dll", std::move(gdi_exports));
        // Map scratch buffer for DIB section pixel data
        ctx.vmm.virtual_alloc(winapi::GdiManager::GDI_DIB_SCRATCH_BASE,
                              winapi::GdiManager::GDI_DIB_SCRATCH_SIZE,
                              0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                              0x04   /*PAGE_READWRITE*/);
    }

    // -- user32.dll --
    {
        ctx.u32_ext_mgr.set_is_64bit(guest_is_64bit);
        ctx.dwp_mgr.set_is_64bit(guest_is_64bit);
        winapi::DllExports u32;
        merge_exports(u32, winapi::make_window_class_exports(ctx.wclass_mgr));
        merge_exports(u32, winapi::make_window_exports(ctx.window_mgr));
        merge_exports(u32, winapi::make_show_window_exports(ctx.show_mgr));
        merge_exports(u32, winapi::make_message_loop_exports(ctx.msgloop_mgr));
        merge_exports(u32, winapi::make_message_send_exports(ctx.msgsend_mgr));
        merge_exports(u32, winapi::make_def_window_proc_exports(ctx.dwp_mgr));
        merge_exports(u32, winapi::create_painting_thunks(ctx.paint_mgr));
        merge_exports(u32, winapi::create_resource_thunks(ctx.resource_mgr));
        merge_exports(u32, winapi::create_dialog_thunks(ctx.dialog_mgr));
        merge_exports(u32, winapi::create_user32_ext_thunks(ctx.u32_ext_mgr));
        // The native-control wndproc must be a REAL callable address: a guest
        // that subclasses a Cocoa-backed control calls the procedure it gets
        // back from SetWindowLongPtr(GWLP_WNDPROC) directly. Register it as a
        // thunk under a Vitreo-private name (no Windows export is named this,
        // so a guest cannot resolve it by accident) and publish its address.
        u32.entries.push_back(winapi::ExportEntry{
            "VitreoNativeControlProc", 0,
            [&ctx](winapi::Marshaler& m) {
                ctx.u32_ext_mgr.impl_VitreoNativeControlProc(m);
            }, 16});
        // A saved base-class WNDPROC retains its identity even when the
        // application registers a different name and procedure (superclass).
        for (auto info = winapi::system_classes_begin(); info != winapi::system_classes_end(); ++info) {
            const std::wstring name(info->name);
            const std::string suffix(name.begin(), name.end());
            for (bool ansi : {false, true}) {
                u32.entries.push_back(winapi::ExportEntry{
                    "VitreoSystemControlProc_" + suffix + (ansi ? "A" : "W"), 0,
                    [&ctx, name, ansi](winapi::Marshaler& m) {
                        ctx.u32_ext_mgr.impl_VitreoSystemControlProc(m, name, ansi);
                    }, 16});
            }
        }
        ctx.u32_ext_mgr.set_system_control_initializer(
            [&ctx](winapi::Marshaler& m, windowing::HWND hwnd,
                   std::wstring_view name, u64 create_struct, bool ansi) {
                return ctx.window_mgr.initialize_system_control(m, hwnd, name, create_struct, ansi);
            });
        collect_kernelbase_surface(kernelbase, u32, kernelbase_seen);
        (void)reg.register_dll("user32.dll", std::move(u32));
        if (auto proc = reg.get_thunk_address("user32.dll",
                                              "VitreoNativeControlProc");
            proc.is_ok()) {
            winapi::User32ExtManager::set_native_control_wndproc(
                static_cast<u64>(proc.value()));
        }
        for (auto info = winapi::system_classes_begin(); info != winapi::system_classes_end(); ++info) {
            const std::string suffix(info->name.begin(), info->name.end());
            for (bool ansi : {false, true}) {
                auto proc = reg.get_thunk_address("user32.dll",
                    "VitreoSystemControlProc_" + suffix + (ansi ? "A" : "W"));
                if (proc.is_ok()) winapi::set_system_class_wndproc(info->name, ansi, proc.value());
            }
        }
        const u64 scratch = ctx.vmm.virtual_alloc(
            0,
            winapi::USER32_CREATESTRUCT_SIZE + winapi::USER32_WINDOWPOS_SIZE,
            0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
            0x04   /*PAGE_READWRITE*/);
        if (scratch != 0) {
            ctx.window_mgr.set_createstruct_scratch(scratch);
            ctx.u32_ext_mgr.set_windowpos_scratch(
                scratch + winapi::USER32_CREATESTRUCT_SIZE);
            ctx.dwp_mgr.set_windowpos_scratch(
                scratch + winapi::USER32_CREATESTRUCT_SIZE);
        }
        // Per-thread slots for the structures USER hands a window procedure
        // (STYLESTRUCT, ...): one 2 KiB stack per guest thread.
        constexpr vitreo::u32 kUserThreadScratchSize = 0x40000;
        const u64 thread_scratch = ctx.vmm.virtual_alloc(
            0, kUserThreadScratchSize, 0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
            0x04 /*PAGE_READWRITE*/);
        if (thread_scratch != 0) {
            ctx.u32_ext_mgr.set_thread_scratch(thread_scratch, kUserThreadScratchSize);
        }
    }
}

} // namespace vitreo::cli
