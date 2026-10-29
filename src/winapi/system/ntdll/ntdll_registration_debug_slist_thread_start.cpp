// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_debug_slist_thread_start.cpp
/// @brief Extended ntdll export table, part 11 of 11: the debug-break exports,
///        the Rtl SList family, RtlRestoreContext and the thread start / exit
///        exports.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "core/pe/os_version.hpp"
#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/kernel32/synchronization/slist.hpp"
#include "winapi/kernel32/thread/restore_context64.hpp"
#include "core/cpu/seh/seh32.hpp"
#include <cstdio>

namespace vitreo::winapi {

using namespace ntdll;

void append_debug_slist_thread_start_exports(NtdllExtManager& mgr,
                                             DllExports& exports, u16& ordinal) {
    const auto raise_breakpoint = [](Marshaler& m) {
        auto* cpu = m.cpu();
        if (cpu != nullptr) {
            // DbgBreakPoint and DbgUserBreakPoint execute INT3. Model the
            // resulting software exception through the same pending-raise
            // path used by other API thunks; the dispatcher simulates the
            // routine's return before entering VEH/SEH.
            cpu->pending_raise.active = true;
            cpu->pending_raise.from_helper = false;
            cpu->pending_raise.code = cpu::STATUS_BREAKPOINT;
            cpu->pending_raise.flags = 0;
            cpu->pending_raise.num_args = 0;
        }
        m.set_return(0);
    };
    // Keep these late additions at the tail so the service ordinals of the
    // existing synthetic ntdll image remain stable.
    exports.entries.push_back(
        ExportEntry{ "DbgBreakPoint", 0, raise_breakpoint, 0 });
    exports.entries.push_back(
        ExportEntry{ "DbgUserBreakPoint", 0, raise_breakpoint, 0 });
    // VOID RtlGetNtVersionNumbers(PULONG Major, PULONG Minor, PULONG Build):
    // each pointer optional; the build carries 0xF0000000, the "free build"
    // marker (os_version_sources_probe RtlGetNtVersionNumbers flags=f0000000).
    exports.entries.push_back(ExportEntry{
        "RtlGetNtVersionNumbers", 0,
        [](Marshaler& m) {
            const GuestAddr major = m.arg_ptr(0);
            const GuestAddr minor = m.arg_ptr(1);
            const GuestAddr build = m.arg_ptr(2);
            if (major != 0) (void)m.write_u32(major, pe::kOsMajorVersion);
            if (minor != 0) (void)m.write_u32(minor, pe::kOsMinorVersion);
            if (build != 0) (void)m.write_u32(build, pe::kOsBuildNumber | 0xF0000000u);
        },
        12});
    // Rtl*SList: the same lists as kernel32's (synchronization/slist.hpp).
    // RtlInterlockedPush/Pop/Flush/PushList are __fastcall on x86, which this
    // export path does not marshal; that bitness fails loudly.
    auto x86_fastcall_gap = [](Marshaler& m, const char* name) {
        if (m.cpu_is_64bit()) return false;
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!%s: x86 __fastcall entry\n", name);
        m.set_return(0);
        return true;
    };
    exports.entries.push_back(ExportEntry{ "RtlInitializeSListHead", 0,
        [](Marshaler& m) {
            if (m.arg_ptr(0) != 0) slist::initialize(m.memory(), m.arg_ptr(0), m.cpu_is_64bit());
        }, 4 });
    exports.entries.push_back(ExportEntry{ "RtlQueryDepthSList", 0,
        [](Marshaler& m) {
            m.set_return(m.arg_ptr(0) == 0 ? 0 : slist::depth(m.memory(), m.arg_ptr(0), m.cpu_is_64bit()));
        }, 4 });
    exports.entries.push_back(ExportEntry{ "RtlFirstEntrySList", 0,
        [](Marshaler& m) {
            m.set_return(m.arg_ptr(0) == 0 ? 0 : slist::first(m.memory(), m.arg_ptr(0), m.cpu_is_64bit()));
        }, 4 });
    exports.entries.push_back(ExportEntry{ "RtlInterlockedPushEntrySList", 0,
        [x86_fastcall_gap](Marshaler& m) {
            if (x86_fastcall_gap(m, "RtlInterlockedPushEntrySList")) return;
            if (m.arg_ptr(0) == 0 || m.arg_ptr(1) == 0) { m.set_return(0); return; }
            m.set_return(slist::push(m.memory(), m.arg_ptr(0), m.arg_ptr(1), true));
        }, 0 });
    exports.entries.push_back(ExportEntry{ "RtlInterlockedPopEntrySList", 0,
        [x86_fastcall_gap](Marshaler& m) {
            if (x86_fastcall_gap(m, "RtlInterlockedPopEntrySList")) return;
            m.set_return(m.arg_ptr(0) == 0 ? 0 : slist::pop(m.memory(), m.arg_ptr(0), true));
        }, 0 });
    exports.entries.push_back(ExportEntry{ "RtlInterlockedFlushSList", 0,
        [x86_fastcall_gap](Marshaler& m) {
            if (x86_fastcall_gap(m, "RtlInterlockedFlushSList")) return;
            m.set_return(m.arg_ptr(0) == 0 ? 0 : slist::flush(m.memory(), m.arg_ptr(0), true));
        }, 0 });
    exports.entries.push_back(ExportEntry{ "RtlInterlockedPushListSListEx", 0,
        [](Marshaler& m) {
            if (m.arg_ptr(0) == 0 || m.arg_ptr(1) == 0 || m.arg_ptr(2) == 0) { m.set_return(0); return; }
            m.set_return(slist::push_list(m.memory(), m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
                                          static_cast<u32>(m.arg(3)), m.cpu_is_64bit()));
        }, 16 });

    // RtlRestoreContext (Learn winnt.h): restore a CONTEXT, applying the
    // STATUS_LONGJUMP / STATUS_UNWIND_CONSOLIDATE ExceptionRecord forms.
    exports.entries.push_back(ExportEntry{ "RtlRestoreContext", 0,
        [&mgr](Marshaler& m) { rtl_restore_context(m, mgr.module_manager()); },
        0 });

    // Every thread starts here (thread_lifecycle_probe SUSPENDED_START: the
    // start context of a CREATE_SUSPENDED thread is RIP/EIP =
    // RtlUserThreadStart with the routine in RCX/EAX and its parameter in
    // RDX/EBX). It calls the routine and passes its result to
    // RtlExitUserThread; neither returns.
    exports.entries.push_back(ExportEntry{
        "RtlUserThreadStart", ordinal++,
        [&mgr](Marshaler& m) {
            if (mgr.thread_manager() != nullptr) {
                mgr.thread_manager()->user_thread_start(m);
            }
        },
        0
    });
    // VOID RtlExitUserThread(NTSTATUS Status): ExitThread's native form.
    exports.entries.push_back(ExportEntry{
        "RtlExitUserThread", ordinal++,
        [&mgr](Marshaler& m) {
            if (mgr.thread_manager() != nullptr) {
                mgr.thread_manager()->exit_current_thread_noreturn(
                    m, static_cast<u32>(m.arg(0)));
            }
        },
        4
    });

    // Debug-object and process-creation services Vitreo does not implement.
    // Windows publishes them, and a guest that derives service numbers from
    // the Zw* exports needs every name it resolves to be there; calling one
    // fails with STATUS_NOT_IMPLEMENTED. Stdcall bytes are the WoW64 stubs'
    // `ret n` on Windows.
    add_unsupported_service(exports, ordinal, "NtCreateDebugObject", 16);
    add_unsupported_service(exports, ordinal, "NtDebugActiveProcess", 8);
    add_unsupported_service(exports, ordinal, "NtCreateUserProcess", 44);

    // NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN Alertable,
    //                                PLARGE_INTEGER Timeout)
    exports.entries.push_back(ExportEntry{
        "NtWaitForSingleObject", ordinal++,
        [&mgr](Marshaler& m) { impl_NtWaitForSingleObject(m, mgr); },
        12
    });
}

} // namespace vitreo::winapi
