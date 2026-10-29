// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_object_query.cpp
/// @brief Extended ntdll export table, part 3 of 11: semaphore, virtual-memory
///        and system queries, section calls, NtGetNextThread and privileges.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "ipc/process_handle.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>
#include <unistd.h>

namespace vitreo::winapi {

using namespace ntdll;

void append_object_query_exports(NtdllExtManager& mgr,
                                 DllExports& exports, u16& ordinal) {
    // NtQuerySemaphore(HANDLE, SEMAPHORE_INFORMATION_CLASS, PVOID, ULONG, PULONG)
    // Resolved via GetProcAddress(ntdll) and called through the pointer. A
    // miss leaves EAX=0 and faults at EIP=0 (STATUS_ILLEGAL_INSTRUCTION).
    exports.entries.push_back(ExportEntry{
        "NtQuerySemaphore", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQuerySemaphore(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "ZwQuerySemaphore", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQuerySemaphore(m, mgr); },
        20
    });

    // NtQueryVirtualMemory (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "NtQueryVirtualMemory", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQueryVirtualMemory(m, mgr); },
        24
    });

    // NtQuerySystemInformation (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "NtQuerySystemInformation", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQuerySystemInformation(m, mgr); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "NtQuerySection", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQuerySection(m, mgr); },
        20
    });

    exports.entries.push_back(ExportEntry{
        "NtCreateSection", ordinal++,
        [&mgr](Marshaler& m) { impl_NtCreateSection(m, mgr); },
        28
    });

    exports.entries.push_back(ExportEntry{
        "NtMapViewOfSection", ordinal++,
        [&mgr](Marshaler& m) { impl_NtMapViewOfSection(m, mgr); },
        40
    });

    exports.entries.push_back(ExportEntry{
        "NtUnmapViewOfSection", ordinal++,
        [&mgr](Marshaler& m) { impl_NtUnmapViewOfSection(m, mgr); },
        8
    });

    // NTSTATUS NtGetNextThread(HANDLE ProcessHandle, HANDLE ThreadHandle,
    //   ACCESS_MASK DesiredAccess, ULONG HandleAttributes, ULONG Flags,
    //   PHANDLE NewThreadHandle)
    // Undocumented (dbgcore.dll's minidump writer imports it); measured by
    // tools/oracle_probe/s4_ntgetnextthread_probe.c, x86 == x64: threads come
    // in creation order starting with the main thread; the end is
    // STATUS_NO_MORE_ENTRIES; Flags != 0 is STATUS_INVALID_PARAMETER; a bad
    // process or thread handle is STATUS_INVALID_HANDLE; *NewThreadHandle is
    // 0 on every failure; OBJ_INHERIT makes the new handle inheritable.
    exports.entries.push_back(ExportEntry{
        "NtGetNextThread", ordinal++,
        [&mgr](Marshaler& m) {
            constexpr u32 kStatusNoMoreEntries = 0x8000001A;
            constexpr u32 kStatusInvalidParameter = 0xC000000D;
            constexpr u32 kStatusNotImplemented = 0xC0000002;
            constexpr u32 kObjInherit = 0x2;
            const u64 process_handle = m.arg(0);
            const u64 thread_handle = m.arg(1);
            const u32 access = m.arg32(2);
            const u32 attributes = m.arg32(3);
            const u32 flags = m.arg32(4);
            const GuestAddr out = m.arg_ptr(5);
            auto write_out = [&](u64 value) {
                if (out == 0) return;
                if (m.cpu_is_64bit()) {
                    (void)m.memory().write_u64(out, value);
                } else {
                    (void)m.memory().write_u32(out, static_cast<u32>(value));
                }
            };
            write_out(0);
            if (flags != 0) {
                m.set_return(kStatusInvalidParameter);
                return;
            }
            HandleTable* handles = mgr.handle_table();
            ThreadManager* threads = mgr.thread_manager();
            if (handles == nullptr || threads == nullptr) {
                m.set_return(nt_status::STATUS_INVALID_HANDLE);
                return;
            }
            const bool pseudo = static_cast<u32>(process_handle) == 0xFFFFFFFFu;
            if (!pseudo) {
                const Handle h = static_cast<Handle>(process_handle);
                if (handles->get_type(h) != HandleType::Process) {
                    m.set_return(nt_status::STATUS_INVALID_HANDLE);
                    return;
                }
                auto* process = handles->get_as<ipc::ProcessObject>(h, HandleType::Process);
                if (process == nullptr || process->pid != ::getpid()) {
                    // Another process's threads live in another host process.
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true)) {
                        std::fprintf(stderr,
                            "[vitreo] UNIMPLEMENTED ntdll!NtGetNextThread: "
                            "threads of another process\n");
                    }
                    m.set_return(kStatusNotImplemented);
                    return;
                }
            }
            std::vector<u32> tids = threads->list_thread_ids();
            std::sort(tids.begin(), tids.end());
            usize next = 0;
            if (thread_handle != 0) {
                const Handle th = static_cast<Handle>(thread_handle);
                GuestThread* current = handles->get_type(th) == HandleType::Thread
                    ? threads->get_thread_by_handle(th) : nullptr;
                if (current == nullptr) {
                    m.set_return(nt_status::STATUS_INVALID_HANDLE);
                    return;
                }
                const u32 after = current->thread_id;
                next = static_cast<usize>(
                    std::upper_bound(tids.begin(), tids.end(), after) - tids.begin());
            }
            for (; next < tids.size(); ++next) {
                const Handle opened = threads->open_thread(
                    tids[next], access,
                    (attributes & kObjInherit) != 0 ? HandleFlags::Inherit : 0);
                if (opened == NULL_HANDLE || opened == INVALID_HANDLE) continue;
                write_out(opened);
                m.set_return(nt_status::STATUS_SUCCESS);
                return;
            }
            m.set_return(kStatusNoMoreEntries);
        },
        24
    });

    // NTSTATUS RtlAcquirePrivilege(PULONG Privilege, ULONG NumPriv,
    //   ULONG Flags, PVOID* ReturnedState) / VOID RtlReleasePrivilege(PVOID)
    // Enabling privileges for the caller (by default on a thread
    // impersonation copy of the process token) is not modelled by the
    // capsule token layer, so acquisition fails honestly and no state is
    // handed out; a release can therefore only receive NULL.
    exports.entries.push_back(ExportEntry{
        "RtlAcquirePrivilege", ordinal++,
        [](Marshaler& m) {
            const GuestAddr state_out = m.arg_ptr(3);
            if (state_out != 0) {
                if (m.cpu_is_64bit()) {
                    (void)m.memory().write_u64(state_out, 0);
                } else {
                    (void)m.memory().write_u32(state_out, 0);
                }
            }
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED ntdll!RtlAcquirePrivilege: "
                    "privilege acquisition (thread/process token)\n");
            }
            m.set_return(0xC0000002u);  // STATUS_NOT_IMPLEMENTED
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "RtlReleasePrivilege", ordinal++,
        [](Marshaler& m) { (void)m.arg_ptr(0); },
        4
    });
}

} // namespace vitreo::winapi
