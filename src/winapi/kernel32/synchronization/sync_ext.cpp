// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_ext.cpp
/// @brief One-time initialisation, condition variables and slim reader/writer locks.
///
/// The synchronisation primitives whose state lives in guest memory rather than
/// in a kernel object, which is why they are here and not in sync.cpp: an
/// INIT_ONCE or an SRWLOCK is a pointer-sized word the guest owns, and Vitreo
/// reads and writes it in place.
///
/// InitOnceExecuteOnce was 4,000 lines away in a run named "Additional Win32
/// stubs", while its two siblings and all six of its helpers were here. Moving
/// the section without it would have left six symbols leaking across the cut;
/// gathering it in leaves none.

#include "srw_word.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/common/host_yield.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"

#include <vitreo/diagnostics.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "winapi/kernel32/synchronization/slist.hpp"

namespace vitreo::winapi {

// =============================================================================
// Synchronization Primitives
// =============================================================================

// SList layouts and atomicity: synchronization/slist.hpp (winnt.h layouts for
// both bitnesses). The former versions used the 8-byte x86 header on x64 too,
// truncating every entry pointer, and only Push took a lock.
void Kernel32ExtManager::impl_InitializeSListHead(Marshaler& m) {
    // void InitializeSListHead(PSLIST_HEADER ListHead)
    const GuestAddr head = m.arg_ptr(0);
    if (head != 0) slist::initialize(m_memory, head, m.cpu_is_64bit());
}

void Kernel32ExtManager::impl_InterlockedPushEntrySList(Marshaler& m) {
    // PSLIST_ENTRY InterlockedPushEntrySList(PSLIST_HEADER, PSLIST_ENTRY)
    const GuestAddr head = m.arg_ptr(0);
    const GuestAddr entry = m.arg_ptr(1);
    if (head == 0 || entry == 0) {
        m.set_return(0);
        return;
    }
    m.set_return(slist::push(m_memory, head, entry, m.cpu_is_64bit()));
}

void Kernel32ExtManager::impl_InitializeSRWLock(Marshaler& m) {
    // void InitializeSRWLock(PSRWLOCK SRWLock)
    GuestAddr lock = m.arg_ptr(0);
    if (lock != 0) {
        // SRWLOCK is pointer-sized; SRWLOCK_INIT is all zero.
        if (m.cpu_is_64bit()) (void)m_memory.write_u64(lock, 0);
        else (void)m_memory.write_u32(lock, 0);
        if (m_sync_manager != nullptr) {
            m_sync_manager->initialize_srw_lock(lock);
        }
    }
}

void Kernel32ExtManager::impl_TryAcquireSRWLockExclusive(Marshaler& m) {
    // BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK SRWLock)
    GuestAddr lock = m.arg_ptr(0);
    {
        srw_word::Word w;
        if (srw_word::resolve(m_memory, lock, m.cpu_is_64bit(), w)) {
            m.set_return(srw_word::try_acquire_exclusive(
                w, ThreadManager::get_current_thread_id_static()) ? 1u : 0u);
            return;
        }
    }
    if (m_sync_manager == nullptr) {
        // Unwired process: do not fake acquisition (Learn TryAcquire returns
        // FALSE when the lock is not taken).
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set()) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED kernel32!TryAcquireSRWLockExclusive: "
                "no SyncManager\n");
        }
        m.set_return(0u);
        return;
    }
    bool ok = m_sync_manager->try_acquire_srw_lock_exclusive(lock);
    if (ok) {
        (void)m_memory.write_u32(lock, 1);
    }
    m.set_return(ok ? 1u : 0u);
}

namespace {

// INIT_ONCE / RTL_RUN_ONCE status lives in the low INIT_ONCE_CTX_RESERVED_BITS
// (2) of the pointer-sized control word. Vitreo's InitOnceExecuteOnce already
// persists completion as (context & ~3) | 2; Begin/Complete share that encoding.
constexpr u64 kInitOnceCompleteBit = 2u;
constexpr u64 kInitOnceStatusMask = 3u;
constexpr u64 kInitOnceSyncPending = 1u;
// Async-in-progress must keep bit 1 clear (that bit marks completion) while
// remaining distinct from sync-pending so mismatched ASYNC flags fail.
constexpr u64 kInitOnceAsyncPending = 5u;  // bit0 | bit2

constexpr u32 kInitOnceCheckOnly = 0x00000001u;
constexpr u32 kInitOnceAsync = 0x00000002u;
constexpr u32 kInitOnceInitFailed = 0x00000004u;

[[nodiscard]] bool initonce_diag_enabled() noexcept {
    return std::getenv("VITREO_INITONCE_DIAG") != nullptr;
}

void initonce_diag(const char* format, ...) noexcept {
    if (!initonce_diag_enabled()) {
        return;
    }
    std::fprintf(stderr, "[initonce] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

[[nodiscard]] bool read_init_once_state(GuestMemory& memory, GuestAddr addr,
                                        bool is_64bit, u64& out) noexcept {
    if (is_64bit) {
        auto result = memory.read_u64(addr);
        if (result.is_err()) {
            return false;
        }
        out = result.value();
        return true;
    }
    auto result = memory.read_u32(addr);
    if (result.is_err()) {
        return false;
    }
    out = result.value();
    return true;
}

[[nodiscard]] bool write_init_once_state(GuestMemory& memory, GuestAddr addr,
                                         bool is_64bit, u64 value) noexcept {
    if (is_64bit) {
        return memory.write_u64(addr, value).is_ok();
    }
    return memory.write_u32(addr, static_cast<u32>(value)).is_ok();
}

[[nodiscard]] bool init_once_is_complete(u64 state) noexcept {
    return (state & kInitOnceCompleteBit) != 0;
}

[[nodiscard]] u64 init_once_context(u64 state) noexcept {
    return state & ~kInitOnceStatusMask;
}

}  // namespace

void Kernel32ExtManager::impl_InitOnceBeginInitialize(Marshaler& m) {
    // BOOL InitOnceBeginInitialize(LPINIT_ONCE lpInitOnce, DWORD dwFlags,
    //                              PBOOL fPending, LPVOID* lpContext)
    // Contract: learn.microsoft.com/.../nf-synchapi-initoncebegininitialize
    const GuestAddr init_once = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    const GuestAddr pending_ptr = m.arg_ptr(2);
    const GuestAddr context_out = m.arg_ptr(3);
    const bool check_only = (flags & kInitOnceCheckOnly) != 0;
    const bool async = (flags & kInitOnceAsync) != 0;

    if (init_once == 0 || pending_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if ((flags & ~(kInitOnceCheckOnly | kInitOnceAsync)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    std::unique_lock<std::mutex> lock(m_init_once_mutex);

    auto write_pending = [&](bool pending) -> bool {
        return m_memory.write_u32(pending_ptr, pending ? 1u : 0u).is_ok();
    };
    auto write_context = [&](u64 context) -> bool {
        if (context_out == 0) {
            return true;
        }
        return m.write_ptr(context_out, context).is_ok();
    };

    for (;;) {
        u64 state = 0;
        if (!read_init_once_state(m_memory, init_once, is_64bit, state)) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }

        if (init_once_is_complete(state)) {
            if (!write_pending(false) ||
                !write_context(init_once_context(state))) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            initonce_diag(
                "Begin complete addr=0x%llx flags=0x%x ctx=0x%llx -> TRUE pending=0\n",
                static_cast<unsigned long long>(init_once), flags,
                static_cast<unsigned long long>(init_once_context(state)));
            m.set_return(1);
            return;
        }

        if (check_only) {
            if (!write_pending(true)) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            initonce_diag(
                "Begin check-only incomplete addr=0x%llx state=0x%llx -> FALSE\n",
                static_cast<unsigned long long>(init_once),
                static_cast<unsigned long long>(state));
            m.set_return(0);
            return;
        }

        if (state == 0) {
            const u64 pending =
                async ? kInitOnceAsyncPending : kInitOnceSyncPending;
            if (!write_init_once_state(m_memory, init_once, is_64bit, pending) ||
                !write_pending(true)) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            initonce_diag(
                "Begin claim addr=0x%llx flags=0x%x pending_state=0x%llx -> TRUE pending=1\n",
                static_cast<unsigned long long>(init_once), flags,
                static_cast<unsigned long long>(pending));
            m.set_return(1);
            return;
        }

        if (async) {
            if (state != kInitOnceAsyncPending) {
                // Sync initialization already claimed this object.
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                initonce_diag(
                    "Begin async mismatch addr=0x%llx state=0x%llx -> FALSE\n",
                    static_cast<unsigned long long>(init_once),
                    static_cast<unsigned long long>(state));
                m.set_return(0);
                return;
            }
            if (!write_pending(true)) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            initonce_diag(
                "Begin async parallel addr=0x%llx -> TRUE pending=1\n",
                static_cast<unsigned long long>(init_once));
            m.set_return(1);
            return;
        }

        if (state != kInitOnceSyncPending) {
            // Async initialization already claimed this object.
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            initonce_diag(
                "Begin sync mismatch addr=0x%llx state=0x%llx -> FALSE\n",
                static_cast<unsigned long long>(init_once),
                static_cast<unsigned long long>(state));
            m.set_return(0);
            return;
        }

        // Another thread owns sync initialization — wait for Complete/fail.
        initonce_diag(
            "Begin sync wait addr=0x%llx state=0x%llx\n",
            static_cast<unsigned long long>(init_once),
            static_cast<unsigned long long>(state));
        m_init_once_cv.wait(lock, [&] {
            u64 current = 0;
            if (!read_init_once_state(m_memory, init_once, is_64bit, current)) {
                return true;
            }
            return current != kInitOnceSyncPending;
        });
    }
}

void Kernel32ExtManager::impl_InitOnceComplete(Marshaler& m) {
    // BOOL InitOnceComplete(LPINIT_ONCE lpInitOnce, DWORD dwFlags, LPVOID lpContext)
    // Contract: learn.microsoft.com/.../nf-synchapi-initoncecomplete
    const GuestAddr init_once = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    const u64 context = m.arg(2);
    const bool async = (flags & kInitOnceAsync) != 0;
    const bool failed = (flags & kInitOnceInitFailed) != 0;

    if (init_once == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if ((flags & ~(kInitOnceAsync | kInitOnceInitFailed)) != 0 ||
        (async && failed)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!failed && (context & kInitOnceStatusMask) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    std::lock_guard<std::mutex> lock(m_init_once_mutex);

    u64 state = 0;
    if (!read_init_once_state(m_memory, init_once, is_64bit, state)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    if (failed) {
        if (state != kInitOnceSyncPending) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (!write_init_once_state(m_memory, init_once, is_64bit, 0)) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        m_init_once_cv.notify_all();
        initonce_diag(
            "Complete FAILED reset addr=0x%llx -> TRUE\n",
            static_cast<unsigned long long>(init_once));
        m.set_return(1);
        return;
    }

    const u64 expected_pending =
        async ? kInitOnceAsyncPending : kInitOnceSyncPending;
    if (state != expected_pending) {
        // Lost an async race, or Complete without a matching Begin.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        initonce_diag(
            "Complete reject addr=0x%llx flags=0x%x state=0x%llx -> FALSE\n",
            static_cast<unsigned long long>(init_once), flags,
            static_cast<unsigned long long>(state));
        m.set_return(0);
        return;
    }

    const u64 completed = (context & ~kInitOnceStatusMask) | kInitOnceCompleteBit;
    if (!write_init_once_state(m_memory, init_once, is_64bit, completed)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m_init_once_cv.notify_all();
    initonce_diag(
        "Complete ok addr=0x%llx flags=0x%x ctx=0x%llx -> TRUE\n",
        static_cast<unsigned long long>(init_once), flags,
        static_cast<unsigned long long>(context));
    m.set_return(1);
}

void Kernel32ExtManager::impl_CreateEventA(Marshaler& m) {
    // HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes,
    //                     BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
    // Note: Primary CreateEventA is handled by SyncManager in sync.cpp.
    // This fallback delegates to sync_manager if available.
    (void)m.arg_ptr(0); // security attributes
    bool manual_reset = m.arg(1) != 0;
    bool initial_state = m.arg(2) != 0;
    GuestAddr name_ptr = m.arg_ptr(3);

    if (m_sync_manager != nullptr) {
        std::string name;
        if (name_ptr != 0) {
            auto name_res = m.read_ansi_string(name_ptr, 260);
            if (name_res.is_ok()) {
                name = name_res.value();
            }
        }
        Handle h = m_sync_manager->create_event(manual_reset, initial_state, name);
        VITREO_TRACE("[kernel32_ext] CreateEventA(manual=%d, init=%d, name=\"%s\") -> 0x%x\n",
                    manual_reset, initial_state, name.c_str(), h);
        m.set_return(static_cast<u64>(h));
        return;
    }

    // Fallback: incrementing fake handle
    u32 handle = 0xEE000000 + (++m_event_counter);
    VITREO_TRACE("[kernel32_ext] CreateEventA(manual=%d, init=%d) -> 0x%08x (no sync mgr)\n",
                manual_reset, initial_state, handle);
    m.set_return(handle);
}

void Kernel32ExtManager::impl_WaitForSingleObjectEx(Marshaler& m) {
    // DWORD WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable)
    // Learn: when bAlertable is TRUE, pending user APCs end the wait with
    // WAIT_IO_COMPLETION after the APC functions run.
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 milliseconds = static_cast<u32>(m.arg(1));
    const bool alertable = m.arg32(2) != 0;
    m.set_return(static_cast<u64>(wait_handle_ex(handle, milliseconds, alertable)));
}

u32 Kernel32ExtManager::wait_handle_ex(Handle handle, u32 milliseconds,
                                       bool alertable) {
    u64 blocked_ns = 0;
    const auto finish_wait = [&]() {
        diagnostics::phase_add_wfso_wall(blocked_ns);
    };

    auto deliver_apcs = [this]() -> bool { return try_deliver_user_apcs(); };

    if (alertable && deliver_apcs()) {
        finish_wait();
        return WAIT_IO_COMPLETION;
    }

    if (!m_sync_manager) {
        // Learn: failure is WAIT_FAILED, never WAIT_OBJECT_0. A zero return
        // is "signaled" and lets a void lock() proceed to unlock an unheld
        // mutex (QBasicMutex::unlockInternal then faults on d_ptr==0).
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    if (!alertable) {
        const u32 result =
            m_sync_manager->wait_for_single_object(
                handle, milliseconds, {}, &blocked_ns);
        finish_wait();
        return result;
    }

    // Slice the wait so APCs queued after entry are observed promptly.
    // Park on the handle and this thread's APC event together; a queued APC
    // ends the wait immediately instead of at the next slice.
    const bool infinite = milliseconds == INFINITE_WAIT;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(infinite ? 0 : milliseconds);
    const u64 apc_ev = m_sync_manager->apc_wait_object_for_current_thread();
    for (bool first_probe = true;; first_probe = false) {
        if (deliver_apcs()) {
            finish_wait();
            return WAIT_IO_COMPLETION;
        }
        u32 remaining = INFINITE_WAIT;
        if (!infinite) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                // Learn: with a zero (or elapsed) timeout the object is still
                // tested once -- a signaled handle is WAIT_OBJECT_0 and is
                // acquired (handles_waits_probe wfsoex_*_0_alertable).
                if (!first_probe) {
                    finish_wait();
                    return WAIT_TIMEOUT;
                }
                remaining = 0;
            } else {
                remaining = static_cast<u32>(std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now).count());
            }
        }
        const auto t_park = std::chrono::steady_clock::now();
        const u32 result = m_sync_manager->wait_for_multiple_objects(
            1, &handle, /*wait_all=*/false, remaining, apc_ev);
        blocked_ns += static_cast<u64>(std::chrono::duration_cast<
            std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_park)
                .count());
        if (result == WAIT_OBJECT_0 + 1) continue;
        finish_wait();
        return result;
    }
}

bool Kernel32ExtManager::try_deliver_user_apcs() {
    if (!m_thread_manager || !m_guest_call) {
        return false;
    }
    const u32 tid = m_thread_manager->get_current_thread_id();
    const bool pending_user = m_thread_manager->has_pending_user_apcs(tid);
    const bool pending_io =
        m_thread_manager->has_pending_io_completion_apcs(tid);
    const bool pending_nt = m_thread_manager->has_pending_nt_io_apcs(tid);
    if (!pending_user && !pending_io && !pending_nt) {
        return false;
    }
    const bool is_64bit = m_module_manager != nullptr &&
                          m_module_manager->is_64bit_process();
    // One FIFO across user, I/O-completion and native I/O APCs.
    const usize delivered = m_thread_manager->drain_all_apcs(
        tid,
        [this, is_64bit](GuestAddr pfn, u64 parameter) {
            const u64 args[1] = {parameter};
            (void)m_guest_call(pfn, args, 1, is_64bit);
        },
        [this, is_64bit](GuestAddr pfn, u64 arg0, u64 arg1, u64 arg2, u64 arg3,
                         u8 argc) {
            if (argc >= 4) {
                const u64 args[4] = {arg0, arg1, arg2, arg3};
                (void)m_guest_call(pfn, args, 4, is_64bit);
            } else {
                const u64 args[3] = {arg0, arg1, arg2};
                (void)m_guest_call(pfn, args, 3, is_64bit);
            }
        },
        [this, is_64bit](GuestAddr pfn, GuestAddr context, GuestAddr iosb) {
            const u64 args[3] = {context, iosb, 0};
            (void)m_guest_call(pfn, args, 3, is_64bit);
        });
    if (delivered > 0) {
        ipc::ipc_log("[k32ext] delivered %zu alertable APC(s) tid=%u\n",
                     delivered, tid);
    }
    return delivered > 0;
}

void Kernel32ExtManager::impl_SwitchToThread(Marshaler& m) {
    // BOOL SwitchToThread(void)
    // Learn: yields execution to another ready thread; returns zero when none
    // are ready. A lone guest thread spinning on SwitchToThread must see FALSE
    // so it can fall back to Sleep(0)/Sleep(1).
    const u32 self = ThreadManager::get_current_thread_id_static();
    const bool others =
        m_thread_manager != nullptr &&
        m_thread_manager->any_live_thread_except(self);
    host_yield_timeslice();
    m.set_return(others ? 1u : 0u);
}

void Kernel32ExtManager::impl_InitOnceExecuteOnce(Marshaler& m) {
    execute_once(m, false);
}

void Kernel32ExtManager::impl_RtlRunOnceExecuteOnce(Marshaler& m) {
    execute_once(m, true);
}

void Kernel32ExtManager::execute_once(Marshaler& m, bool native_status) {
    // Same control word and wait domain for Win32 and NT callers. The NT
    // entry returns NTSTATUS and never overwrites the callback's LastError.
    const auto fail = [&](u32 error, u32 status) {
        if (!native_status) set_last_error(error);
        m.set_return(native_status ? status : 0u);
    };
    const GuestAddr init_once = m.arg_ptr(0);
    const GuestAddr init_fn = m.arg_ptr(1);
    const GuestAddr parameter = m.arg_ptr(2);
    const GuestAddr context_out = m.arg_ptr(3);
    if (init_once == 0) {
        fail(error_codes::ERROR_INVALID_PARAMETER, nt_status::STATUS_INVALID_PARAMETER);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    {
        std::unique_lock<std::mutex> lock(m_init_once_mutex);
        for (;;) {
            u64 state = 0;
            if (!read_init_once_state(m_memory, init_once, is_64bit, state)) {
                fail(error_codes::ERROR_NOACCESS, nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }

            if (init_once_is_complete(state)) {
                if (context_out != 0 &&
                    m.write_ptr(context_out, init_once_context(state)).is_err()) {
                    fail(error_codes::ERROR_NOACCESS, nt_status::STATUS_ACCESS_VIOLATION);
                    return;
                }
                initonce_diag(
                    "Execute complete addr=0x%llx ctx=0x%llx -> TRUE\n",
                    static_cast<unsigned long long>(init_once),
                    static_cast<unsigned long long>(init_once_context(state)));
                m.set_return(native_status ? 0u : 1u);
                return;
            }

            if (state == 0) {
                if (init_fn == 0) {
                    fail(error_codes::ERROR_INVALID_PARAMETER, nt_status::STATUS_INVALID_PARAMETER);
                    return;
                }
                if (!m_guest_call) {
                    static std::once_flag missing_callback_bridge;
                    std::call_once(missing_callback_bridge, [] {
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!RtlRunOnceExecuteOnce: guest callback bridge unavailable\n");
                    });
                    fail(error_codes::ERROR_CALL_NOT_IMPLEMENTED, nt_status::STATUS_NOT_IMPLEMENTED);
                    return;
                }
                if (!write_init_once_state(m_memory, init_once, is_64bit,
                                           kInitOnceSyncPending)) {
                    fail(error_codes::ERROR_NOACCESS, nt_status::STATUS_ACCESS_VIOLATION);
                    return;
                }
                initonce_diag("Execute claim addr=0x%llx\n",
                              static_cast<unsigned long long>(init_once));
                break;
            }

            if (state != kInitOnceSyncPending) {
                // InitOnceExecuteOnce is synchronous and cannot join an
                // asynchronous InitOnceBeginInitialize attempt.
                fail(error_codes::ERROR_INVALID_PARAMETER, nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            // The mutex protects only the control-word transition.  It must
            // not be held across the guest callback: an initializer may
            // legitimately initialize a different INIT_ONCE object.
            initonce_diag("Execute wait addr=0x%llx\n",
                          static_cast<unsigned long long>(init_once));
            m_init_once_cv.wait(lock, [&] {
                u64 current = 0;
                if (!read_init_once_state(m_memory, init_once, is_64bit,
                                          current)) {
                    return true;
                }
                return current != kInitOnceSyncPending;
            });
        }
    }

    const u64 args[3] = {init_once, parameter, context_out};
    if (static_cast<u32>(m_guest_call(init_fn, args, 3, is_64bit)) == 0) {
        {
            std::lock_guard<std::mutex> lock(m_init_once_mutex);
            (void)write_init_once_state(m_memory, init_once, is_64bit, 0);
        }
        m_init_once_cv.notify_all();
        initonce_diag("Execute callback failed addr=0x%llx -> FALSE\n",
                      static_cast<unsigned long long>(init_once));
        // STATUS_UNSUCCESSFUL, measured by rtl_runonce_probe{32,64}.expected.
        m.set_return(native_status ? 0xc0000001u : 0u);
        return;
    }

    u64 context = 0;
    if (context_out != 0) {
        auto result = is_64bit ? m_memory.read_u64(context_out)
                               : m_memory.read_u32(context_out).map([](u32 value) {
                                     return static_cast<u64>(value);
                                 });
        if (result.is_err()) {
            {
                std::lock_guard<std::mutex> lock(m_init_once_mutex);
                (void)write_init_once_state(m_memory, init_once, is_64bit, 0);
            }
            m_init_once_cv.notify_all();
            fail(error_codes::ERROR_NOACCESS, nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        context = result.value();
    }
    if ((context & 3u) != 0) {
        // RtlRunOnceComplete raises STATUS_INVALID_PARAMETER_3 here; it
        // does not make the once object retryable. Captured by the Windows
        // vectored-handler case in rtl_runonce_probe{32,64}.expected.
        if (auto* cpu = m.cpu()) {
            cpu->pending_raise.active = true;
            cpu->pending_raise.code = 0xc00000f1u;
            cpu->pending_raise.flags = 0x81u;
            cpu->pending_raise.num_args = 0;
        }
        return;
    }

    const u64 completed_state = context | 2u;
    {
        std::lock_guard<std::mutex> lock(m_init_once_mutex);
        if (!write_init_once_state(m_memory, init_once, is_64bit,
                                   completed_state)) {
            m_init_once_cv.notify_all();
            fail(error_codes::ERROR_NOACCESS, nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
    }
    m_init_once_cv.notify_all();
    initonce_diag("Execute complete addr=0x%llx ctx=0x%llx -> TRUE\n",
                  static_cast<unsigned long long>(init_once),
                  static_cast<unsigned long long>(context));
    m.set_return(native_status ? 0u : 1u);
}

} // namespace vitreo::winapi
