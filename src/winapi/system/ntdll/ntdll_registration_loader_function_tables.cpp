// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_loader_function_tables.cpp
/// @brief Extended ntdll export table, part 2 of 11: loader entry points,
///        version queries and the dynamic function-table registrations.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/kernel32/module/module.hpp"

namespace vitreo::winapi {

using namespace ntdll;

void append_loader_function_table_exports(NtdllExtManager& mgr,
                                          DllExports& exports, u16& ordinal) {
    exports.entries.push_back(ExportEntry{
        "RtlCaptureStackBackTrace", ordinal++,
        [](Marshaler& m) { impl_RtlCaptureStackBackTrace(m); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "LdrRegisterDllNotification", ordinal++,
        [&mgr](Marshaler& m) {
            GuestAddr cookie = 0;
            const GuestAddr cookie_ptr = m.arg_ptr(3);
            if (cookie_ptr == 0) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }
            const u32 status = mgr.register_dll_notification(
                static_cast<u32>(m.arg(0)), m.arg_ptr(1), m.arg_ptr(2),
                &cookie);
            if (status == 0) (void)m.write_ptr(cookie_ptr, cookie);
            m.set_return(status);
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "LdrUnregisterDllNotification", ordinal++,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.unregister_dll_notification(m.arg_ptr(0)));
        },
        4
    });

    // LdrLockLoaderLock(ULONG Flags, ULONG* State, ULONG_PTR* Cookie)
    // x86 stdcall pops 12 bytes; Cookie is pointer-sized on both ABIs for
    // argument passing (ULONG_PTR).
    exports.entries.push_back(ExportEntry{
        "LdrLockLoaderLock", ordinal++,
        [&mgr](Marshaler& m) { mgr.impl_LdrLockLoaderLock(m); },
        12
    });
    // LdrUnlockLoaderLock(ULONG Flags, ULONG_PTR Cookie) — stdcall 8 on x86.
    exports.entries.push_back(ExportEntry{
        "LdrUnlockLoaderLock", ordinal++,
        [&mgr](Marshaler& m) { mgr.impl_LdrUnlockLoaderLock(m); },
        8
    });

    // NTSTATUS LdrLoadDll(PWSTR PathToFile OPTIONAL, PULONG Flags OPTIONAL,
    //                     PUNICODE_STRING ModuleFileName, PHANDLE ModuleHandle)
    exports.entries.push_back(ExportEntry{
        "LdrLoadDll", ordinal++,
        [&mgr](Marshaler& m) { mgr.impl_LdrLoadDll(m); },
        16
    });
    // NTSTATUS LdrGetProcedureAddress(HMODULE, PANSI_STRING, WORD, PVOID*)
    exports.entries.push_back(ExportEntry{
        "LdrGetProcedureAddress", ordinal++,
        [&mgr](Marshaler& m) { mgr.impl_LdrGetProcedureAddress(m); },
        16
    });

    // RtlNtStatusToDosError (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "RtlNtStatusToDosError", ordinal++,
        [&mgr](Marshaler& m) { impl_RtlNtStatusToDosError(m, mgr); },
        4
    });

    // RtlGetVersion (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "RtlGetVersion", ordinal++,
        [&mgr](Marshaler& m) { impl_RtlGetVersion(m, mgr); },
        4
    });

    // RtlQueryElevationFlags (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "RtlQueryElevationFlags", ordinal++,
        [](Marshaler& m) { impl_RtlQueryElevationFlags(m); },
        4
    });

    // BOOLEAN RtlAddFunctionTable(PRUNTIME_FUNCTION FunctionTable,
    //                             DWORD EntryCount, DWORD64 BaseAddress)
    //
    // This was a no-op returning TRUE, so a JIT's unwind information was
    // accepted and discarded. Registrations now land where the unwinder
    // actually looks (ModuleManager), not in SehManager's own table, which
    // nothing on the unwind path reads.
    exports.entries.push_back(ExportEntry{
        "RtlAddFunctionTable", ordinal++,
        [&mgr](Marshaler& m) {
            const GuestAddr table = m.arg_ptr(0);
            const u32 count = static_cast<u32>(m.arg(1));
            const GuestAddr base = static_cast<GuestAddr>(m.arg(2));
            auto* modules = mgr.module_manager();
            if (modules == nullptr) { m.set_return(0); return; }
            const bool ok =
                modules->add_dynamic_function_table(table, count, base);
            VITREO_TRACE("[seh] RtlAddFunctionTable(table=0x%llx count=%u "
                         "base=0x%llx) -> %d\n",
                         static_cast<unsigned long long>(table), count,
                         static_cast<unsigned long long>(base), ok ? 1 : 0);
            m.set_return(ok ? 1u : 0u);
        },
        12
    });

    // BOOLEAN RtlInstallFunctionTableCallback(DWORD64 TableIdentifier,
    //     DWORD64 BaseAddress, DWORD Length, PGET_RUNTIME_FUNCTION_CALLBACK
    //     Callback, PVOID Context, PCWSTR OutOfProcessCallbackDll)
    //
    // How a JIT registers unwind information it resolves ON DEMAND, rather
    // than as a fixed table. coreclr uses it for JIT-compiled managed code, so
    // without it SEH cannot unwind out of a managed frame and the CLR fails
    // its own execution-engine invariants.
    exports.entries.push_back(ExportEntry{
        "RtlInstallFunctionTableCallback", ordinal++,
        [&mgr](Marshaler& m) {
            const GuestAddr identifier = static_cast<GuestAddr>(m.arg(0));
            const GuestAddr base = static_cast<GuestAddr>(m.arg(1));
            const u32 length = static_cast<u32>(m.arg(2));
            const GuestAddr callback = m.arg_ptr(3);
            const GuestAddr context = m.arg_ptr(4);
            // OutOfProcessCallbackDll is for a debugger unwinding this process
            // from outside. There is no such consumer here, so it is ignored
            // rather than refused -- refusing would fail the registration for
            // a caller that merely supplied one.
            (void)m.arg_ptr(5);
            auto* modules = mgr.module_manager();
            if (modules == nullptr) { m.set_return(0); return; }
            const bool ok = modules->add_dynamic_function_table_callback(
                identifier, base, length, callback, context);
            VITREO_TRACE("[seh] RtlInstallFunctionTableCallback(id=0x%llx "
                         "base=0x%llx len=0x%x cb=0x%llx) -> %d\n",
                         static_cast<unsigned long long>(identifier),
                         static_cast<unsigned long long>(base), length,
                         static_cast<unsigned long long>(callback),
                         ok ? 1 : 0);
            m.set_return(ok ? 1u : 0u);
        },
        24
    });

    exports.entries.push_back(ExportEntry{
        "RtlDeleteFunctionTable", ordinal++,
        [&mgr](Marshaler& m) {
            const GuestAddr id = static_cast<GuestAddr>(m.arg(0));
            auto* modules = mgr.module_manager();
            if (modules == nullptr) { m.set_return(0); return; }
            m.set_return(modules->remove_dynamic_function_table(id) ? 1u : 0u);
        },
        4
    });

    // NTSTATUS RtlAddGrowableFunctionTable(PVOID* DynamicTable,
    //     PRUNTIME_FUNCTION FunctionTable, DWORD EntryCount,
    //     DWORD MaximumEntryCount, ULONG_PTR RangeBase, ULONG_PTR RangeEnd)
    // https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtladdgrowablefunctiontable
    //
    // This used to write the table pointer back as the handle and register
    // nothing, so the call reported success while RtlLookupFunctionEntry
    // stayed blind to every entry. coreclr registers the unwind info for its
    // JIT-compiled managed code this way, and a stack walk that cannot find
    // it does not fail loudly -- it produces wrong frames, and the GC then
    // scans garbage root addresses.
    exports.entries.push_back(ExportEntry{
        "RtlAddGrowableFunctionTable", ordinal++,
        [&mgr](Marshaler& m) {
            const u64 dynamic_table_ptr = m.arg_ptr(0);
            const GuestAddr function_table =
                static_cast<GuestAddr>(m.arg_ptr(1));
            const u32 entry_count = static_cast<u32>(m.arg(2));
            (void)m.arg(3);  // MaximumEntryCount: growth is unbounded here
            const GuestAddr range_base = static_cast<GuestAddr>(m.arg_ptr(4));
            const GuestAddr range_end = static_cast<GuestAddr>(m.arg_ptr(5));
            auto* modules = mgr.module_manager();
            if (dynamic_table_ptr == 0 || function_table == 0 ||
                modules == nullptr ||
                !modules->add_growable_function_table(
                    function_table, entry_count, range_base, range_end) ||
                !m.write_ptr(dynamic_table_ptr, function_table)) {
                m.set_return(0xC000000Du);  // STATUS_INVALID_PARAMETER
                return;
            }
            VITREO_TRACE("[seh] RtlAddGrowableFunctionTable(table=0x%llx "
                         "count=%u range=[0x%llx,0x%llx)) -> ok\n",
                         static_cast<unsigned long long>(function_table),
                         entry_count,
                         static_cast<unsigned long long>(range_base),
                         static_cast<unsigned long long>(range_end));
            m.set_return(0);  // STATUS_SUCCESS
        },
        24
    });

    // NTSTATUS RtlGrowFunctionTable(PVOID DynamicTable, DWORD NewEntryCount)
    // The other half of the growable-table contract, and previously absent
    // entirely: a caller could register a range but never publish the entries
    // it subsequently filled in.
    exports.entries.push_back(ExportEntry{
        "RtlGrowFunctionTable", ordinal++,
        [&mgr](Marshaler& m) {
            const GuestAddr identifier = static_cast<GuestAddr>(m.arg_ptr(0));
            const u32 new_count = static_cast<u32>(m.arg(1));
            auto* modules = mgr.module_manager();
            if (modules == nullptr ||
                !modules->grow_dynamic_function_table(identifier, new_count)) {
                m.set_return(0xC000000Du);  // STATUS_INVALID_PARAMETER
                return;
            }
            m.set_return(0);  // STATUS_SUCCESS
        },
        8
    });

    exports.entries.push_back(ExportEntry{
        "RtlDeleteGrowableFunctionTable", ordinal++,
        [&mgr](Marshaler& m) {
            const GuestAddr identifier = static_cast<GuestAddr>(m.arg_ptr(0));
            auto* modules = mgr.module_manager();
            if (modules != nullptr) {
                (void)modules->remove_dynamic_function_table(identifier);
            }
            m.set_return(0);
        },
        4
    });
}

} // namespace vitreo::winapi
