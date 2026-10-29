// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_thread_wait.cpp
/// @brief Extended ntdll export table, part 4 of 11: NtQueryInformationThread,
///        NtClose, NtDelayExecution and the alert-by-thread-id park primitives.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "core/sync/ntsync.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/system/ntdll/nt_timeout.hpp"
#include <cstdio>

namespace vitreo::winapi {

using namespace ntdll;

void append_thread_wait_exports(NtdllExtManager& mgr,
                                DllExports& exports, u16& ordinal) {
    // NtQueryInformationThread(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG)
    exports.entries.push_back(ExportEntry{
        "NtQueryInformationThread", ordinal++,
        [&mgr](Marshaler& m) {
            if (static_cast<u32>(m.arg(1)) == kThreadHideFromDebugger) {
                thread_hide_information(m, mgr, false);
                return;
            }
            const u64 thread_handle = m.arg(0);
            u32 info_class = static_cast<u32>(m.arg(1));
            GuestAddr info_ptr = m.arg_ptr(2);
            u32 info_len = static_cast<u32>(m.arg(3));
            GuestAddr ret_len_ptr = m.arg_ptr(4);

            if (environment_flag_enabled("VITREO_THREAD_DIAG")) {
                std::fprintf(stderr,
                             "[thread-diag] NtQueryInformationThread handle=0x%llx "
                             "class=%u info=0x%llx len=%u retlen=0x%llx\n",
                             static_cast<unsigned long long>(thread_handle),
                             info_class,
                             static_cast<unsigned long long>(info_ptr), info_len,
                             static_cast<unsigned long long>(ret_len_ptr));
            }

            auto write_return_length = [&](u32 value) {
                if (ret_len_ptr != 0) {
                    (void)m.memory().write_u32(ret_len_ptr, value);
                }
            };

            if (info_ptr == 0) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            if (info_class == 0) {
                struct ThreadBasicInformation64 {
                    u32 exit_status;
                    u32 reserved0;
                    u64 teb_base_address;
                    u64 unique_process;
                    u64 unique_thread;
                    u64 affinity_mask;
                    i32 priority;
                    i32 base_priority;
                } info{};
                info.unique_process = 1;
                info.unique_thread = 1;
                info.affinity_mask = 1;
                write_return_length(static_cast<u32>(sizeof(info)));
                if (info_len < sizeof(info)) {
                    m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
                    return;
                }
                (void)m.memory().write_bytes(info_ptr,
                                             reinterpret_cast<const Byte*>(&info),
                                             sizeof(info));
                m.set_return(nt_status::STATUS_SUCCESS);
                return;
            }

            if (info_class == 9) {
                write_return_length(8);
                if (info_len < 8) {
                    m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
                    return;
                }
                auto* threads = mgr.thread_manager();
                const GuestThread* thread = nullptr;
                if (threads != nullptr) {
                    const u64 current_thread64 = ~u64{1};
                    const u64 current_thread32 = 0xFFFFFFFEu;
                    if (thread_handle == current_thread64 ||
                        thread_handle == current_thread32) {
                        thread = threads->get_thread(
                            ThreadManager::get_current_thread_id_static());
                    } else if (thread_handle <= 0xFFFFFFFFu) {
                        thread = threads->get_thread_by_handle(
                            static_cast<Handle>(thread_handle));
                    }
                }
                if (thread == nullptr) {
                    m.set_return(nt_status::STATUS_INVALID_HANDLE);
                    return;
                }
                const u64 start_address = thread->start_address;
                (void)m.memory().write_bytes(info_ptr,
                                             reinterpret_cast<const Byte*>(&start_address),
                                             sizeof(start_address));
                m.set_return(nt_status::STATUS_SUCCESS);
                return;
            }

            write_return_length(0);
            m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        },
        20
    });

    // NtClose (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "NtClose", ordinal++,
        [&mgr](Marshaler& m) { impl_NtClose(m, mgr); },
        4
    });

    // NtDelayExecution (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "NtDelayExecution", ordinal++,
        [&mgr](Marshaler& m) { impl_NtDelayExecution(m, mgr); },
        8
    });

    // NtWaitForAlertByThreadId (2 args, stdcall_bytes=8)
    // NTSTATUS NtWaitForAlertByThreadId(PVOID Address, PLARGE_INTEGER Timeout)
    // The Windows 8+ primitive every SRW lock, condition variable and
    // critical section parks on: the calling thread sleeps until
    // NtAlertThreadByThreadId names it or the timeout elapses. Not on Learn;
    // every status is nt_alert_wait_probe.expected:
    //   - STATUS_ALERTED (0x101) when alerted, STATUS_TIMEOUT (0x102) when
    //     the timeout ran out first; an alert that arrived before the wait is
    //     kept and two alerts are one (alert_self_1/_2, wait_zero_first/
    //     _second). Address is a hint only; NULL is fine.
    //   - Timeout is the NT convention (nt_timeout.hpp): NULL = none,
    //     negative relative, positive ABSOLUTE system time, 0 = poll; an
    //     unreadable pointer is STATUS_ACCESS_VIOLATION.
    //   - Not alertable: a queued user APC neither ends the wait nor runs
    //     (wait_with_queued_apc).
    exports.entries.push_back(ExportEntry{
        "NtWaitForAlertByThreadId", ordinal++,
        [&mgr](Marshaler& m) {
            SyncManager* sync = mgr.sync_manager();
            if (sync == nullptr) { m.set_return(nt_status::STATUS_TIMEOUT); return; }
            const auto deadline = nt_timeout::read_timeout(m.memory(), m.arg_ptr(1));
            if (!deadline) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            const u32 tid = ThreadManager::get_current_thread_id_static();
            const u64 ev = sync->alert_wait_object_for_thread(tid);
            for (;;) {
                // An absolute wait parks in slices and re-reads the system
                // clock between them, so a clock change moves its expiry.
                const u32 ms = deadline->next_slice_ms();
                const u32 rc = ntsync::global().wait(&ev, 1, false, ms, tid, nullptr);
                if (rc == ntsync::WAIT_OBJECT_0) {
                    m.set_return(nt_status::STATUS_ALERTED);
                    return;
                }
                // WAIT_FAILED: the thread is being terminated.
                if (rc != ntsync::WAIT_TIMEOUT || deadline->expired()) {
                    m.set_return(nt_status::STATUS_TIMEOUT);
                    return;
                }
            }
        },
        8
    });

    // NtAlertThreadByThreadId (1 arg, stdcall_bytes=4)
    // NTSTATUS NtAlertThreadByThreadId(HANDLE ThreadId)
    // nt_alert_wait_probe.expected: STATUS_SUCCESS for a thread of this
    // process, live or exited while a handle keeps its object
    // (alert_exited_thread_handle_open); STATUS_INVALID_CID for 0, an
    // unknown id, or an exited thread whose object is gone
    // (alert_exited_thread_handle_closed). Known gaps (.docs/contracts/ntdll.md):
    // a thread of ANOTHER process is STATUS_ACCESS_DENIED on Windows and
    // STATUS_INVALID_CID here (no capsule-wide thread registry), and Windows
    // ignores the low two bits of a client id while Vitreo's ids are
    // consecutive (alert_tid_odd).
    exports.entries.push_back(ExportEntry{
        "NtAlertThreadByThreadId", ordinal++,
        [&mgr](Marshaler& m) {
            SyncManager* sync = mgr.sync_manager();
            if (sync == nullptr) { m.set_return(nt_status::STATUS_INVALID_PARAMETER); return; }
            const u64 cid = m.arg(0);
            ThreadManager* threads = mgr.thread_manager();
            // Vitreo's own service threads (timer, wave, COM-RPC, WinHTTP,
            // RegisterWait and threadpool waiters) run guest code under ids
            // outside the thread table; they are registered so that they are
            // threads of this process here too.
            if (cid == 0 || cid > 0xFFFFFFFFu || threads == nullptr ||
                !threads->is_thread_of_this_process(static_cast<u32>(cid))) {
                m.set_return(nt_status::STATUS_INVALID_CID);
                return;
            }
            (void)ntsync::global().set_event(
                sync->alert_wait_object_for_thread(static_cast<u32>(cid)));
            m.set_return(nt_status::STATUS_SUCCESS);
        },
        4
    });
}

} // namespace vitreo::winapi
