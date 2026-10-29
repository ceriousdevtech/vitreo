// SPDX-License-Identifier: LGPL-2.1-only
/// @file threadpool.cpp
/// @brief kernel32 thread-pool objects: work, timer, wait, callback instance.
///
/// Learn: https://learn.microsoft.com/en-us/windows/win32/procthread/thread-pool-api
/// Unstated behaviour measured by tools/oracle_probe/s4_threadpool_probe.c
/// (s4_threadpool_probe.expected, identical on x86 and x64).

#include "winapi/kernel32/thread/threadpool.hpp"

#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace vitreo::winapi {
namespace {

/// A work/timer callback is
/// `VOID CALLBACK(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK|PTP_TIMER)`,
/// a wait callback adds `TP_WAIT_RESULT`, and a simple callback is
/// `VOID CALLBACK(PTP_CALLBACK_INSTANCE, PVOID context)`.
constexpr usize OBJECT_CALLBACK_ARGS = 3;
constexpr usize WAIT_CALLBACK_ARGS = 4;
constexpr usize SIMPLE_CALLBACK_ARGS = 2;

using Clock = std::chrono::steady_clock;

/// FILETIME due time / timeout (Learn, SetThreadpoolTimer / SetThreadpoolWait):
/// negative = relative, in 100 ns units; positive = absolute UTC FILETIME;
/// 0 = now. Returns the delay in 100 ns units (never negative).
u64 filetime_delay_100ns(i64 value) {
    if (value < 0) return static_cast<u64>(-value);
    if (value == 0) return 0;
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    constexpr i64 kFiletimeUnixOffset = 116444736000000000LL;
    const i64 now_ft = kFiletimeUnixOffset +
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count() / 100;
    return value > now_ft ? static_cast<u64>(value - now_ft) : 0;
}

/// Reads a guest FILETIME; false when unreadable.
bool read_filetime(Marshaler& m, GuestAddr at, i64& out) {
    auto v = m.memory().read_u64(at);
    if (v.is_err()) return false;
    out = static_cast<i64>(v.value());
    return true;
}

void log_unimplemented_once(std::atomic<bool>& flag, const char* what) {
    if (!flag.exchange(true)) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!%s\n", what);
    }
}

} // namespace

ThreadpoolManager::~ThreadpoolManager() { begin_shutdown(); }

void ThreadpoolManager::begin_shutdown() {
    std::vector<std::shared_ptr<std::atomic<bool>>> cancels;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        for (auto& [id, wait] : m_waits) {
            (void)id;
            if (wait.cancel) cancels.push_back(wait.cancel);
        }
    }
    for (auto& c : cancels) c->store(true, std::memory_order_release);
    if (m_sync != nullptr && !cancels.empty()) m_sync->notify_waiters();
    m_timer_cv.notify_all();
    if (m_timer_thread.joinable() &&
        m_timer_thread.get_id() != std::this_thread::get_id()) {
        m_timer_thread.join();
    }
    // Waiter threads touch this object after their wait returns; they have
    // all been cancelled above, so this wait is short.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait_for(lock, std::chrono::seconds(5),
                    [&] { return m_live_waiters == 0; });
}

usize ThreadpoolManager::live_work_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_work.size();
}

u32 ThreadpoolManager::new_object_id() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const u32 id = m_next_work;
    m_next_work += 4;
    return id;
}

bool ThreadpoolManager::dispatch(u32 object, GuestAddr callback,
                                 GuestAddr context, u64 third, u64 fourth,
                                 usize arg_count) {
    if (m_threads == nullptr || callback == 0) return false;

    // One guest thread per callback. Windows reuses a pool of threads and
    // makes no promise about which one runs a callback, so this is a valid
    // implementation of the observable contract. The thread handle is closed
    // at once: nothing waits on it, and the thread object is then destroyed
    // when the callback returns (ThreadManager::exit_thread).
    u64 instance = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        instance = m_next_instance;
        m_next_instance += 4;
        m_instances[instance] = CallbackInstance{object, false, {}};
    }
    u32 tid = 0;
    const Handle thread = m_threads->create_thread(
        0, callback, static_cast<GuestAddr>(instance), 0, &tid, context);
    if (thread == INVALID_HANDLE || tid == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_instances.erase(instance);
        return false;
    }
    if (arg_count >= WAIT_CALLBACK_ARGS) {
        if (GuestThread* gt = m_threads->get_thread(tid)) gt->fourth_parameter = fourth;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running[tid] = RunningCallback{object, instance};
    }
    m_threads->run_async(tid, callback, static_cast<GuestAddr>(instance),
                         context, third, arg_count);
    m_threads->close_thread_handle(thread);
    return true;
}

bool ThreadpoolManager::read_environment_ok(Marshaler& m, GuestAddr environment) {
    // Learn: pcbe may be NULL (default environment) or a TP_CALLBACK_ENVIRON
    // from InitializeThreadpoolEnvironment. Callers pass a non-null
    // environ even when Pool/CleanupGroup remain NULL; refusing every non-null
    // pointer dropped all work submissions (Submit returns void).
    if (environment == 0) return true;
    // TP_CALLBACK_ENVIRON_V3 (winnt.h): Version, Pool, CleanupGroup, ...
    const bool is64 = m.cpu_is_64bit();
    const u32 pool_off = is64 ? 8u : 4u;
    const u32 cleanup_off = is64 ? 16u : 8u;
    auto rd = [&](u32 off) -> std::optional<u64> {
        if (is64) {
            auto v = m.memory().read_u64(environment + off);
            return v.is_ok() ? std::optional<u64>(v.value()) : std::nullopt;
        }
        auto v = m.memory().read_u32(environment + off);
        return v.is_ok() ? std::optional<u64>(v.value()) : std::nullopt;
    };
    const auto pool = rd(pool_off);
    const auto cleanup = rd(cleanup_off);
    if (!pool || !cleanup) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    if (*cleanup != 0) {
        // Cleanup-group membership is observable via
        // CloseThreadpoolCleanupGroupMembers; ignoring it would hang or skip
        // cancellations. CreateThreadpoolCleanupGroup itself fails, so a
        // guest can only get here with a group it did not obtain from us.
        ipc::ipc_log("[threadpool] refused: cleanup group 0x%llx env=0x%llx",
                     static_cast<unsigned long long>(*cleanup),
                     static_cast<unsigned long long>(environment));
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return false;
    }
    // Non-null Pool: Vitreo has one pool; CreateThreadpool handles run here.
    return true;
}

// =============================================================================
// Work objects
// =============================================================================

void ThreadpoolManager::impl_CreateThreadpoolWork(Marshaler& m) {
    // PTP_WORK CreateThreadpoolWork(PTP_WORK_CALLBACK pfnwk, PVOID pv,
    //                               PTP_CALLBACK_ENVIRON pcbe)
    const GuestAddr callback = m.arg_ptr(0);
    const GuestAddr context = m.arg_ptr(1);
    const GuestAddr environment = m.arg_ptr(2);
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!read_environment_ok(m, environment)) {
        m.set_return(0);
        return;
    }
    const u32 handle = new_object_id();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_work.emplace(handle, ThreadpoolWork{callback, context, 0, 0, false});
    }
    VITREO_TRACE("[threadpool] CreateThreadpoolWork(cb=0x%llx ctx=0x%llx) "
                 "-> work=0x%08x\n",
                 static_cast<unsigned long long>(callback),
                 static_cast<unsigned long long>(context), handle);
    m.set_return(handle);
}

void ThreadpoolManager::impl_SubmitThreadpoolWork(Marshaler& m) {
    // VOID SubmitThreadpoolWork(PTP_WORK pwk). No return value, so a dropped
    // submission is invisible to the caller — it must actually run.
    const auto work = static_cast<u32>(m.arg(0));
    GuestAddr callback = 0;
    GuestAddr context = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_work.find(work);
        if (found == m_work.end() || found->second.closed) return;
        callback = found->second.callback;
        context = found->second.context;
        ++found->second.outstanding;
        ++found->second.queued;
    }
    const bool ok = dispatch(work, callback, context, work, 0, OBJECT_CALLBACK_ARGS);
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_work.find(work);
    if (found != m_work.end()) {
        if (found->second.queued > 0) --found->second.queued;
        if (!ok && found->second.outstanding > 0) --found->second.outstanding;
    }
    if (!ok) m_idle.notify_all();
}

void ThreadpoolManager::on_thread_exit(u32 tid) {
    RunningCallback running;
    CallbackInstance instance;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_running.find(tid);
        if (found == m_running.end()) return;
        running = found->second;
        m_running.erase(found);
        const auto inst = m_instances.find(running.instance);
        if (inst != m_instances.end()) {
            instance = std::move(inst->second);
            m_instances.erase(inst);
        }
    }
    // Learn (*WhenCallbackReturns): the action happens once the callback
    // returns, on its behalf — this runs on the callback's own thread.
    for (auto& action : instance.on_return) action();
    if (!instance.disassociated && running.object != 0) {
        on_work_callback_finished(running.object);
    }
}

void ThreadpoolManager::release_object_if_idle(u32 object) {
    // Caller holds m_mutex.
    if (auto w = m_work.find(object); w != m_work.end()) {
        if (w->second.closed && w->second.outstanding == 0) m_work.erase(w);
    } else if (auto t = m_timers.find(object); t != m_timers.end()) {
        if (t->second.closed && t->second.outstanding == 0) m_timers.erase(t);
    } else if (auto wt = m_waits.find(object); wt != m_waits.end()) {
        if (wt->second.closed && wt->second.outstanding == 0) m_waits.erase(wt);
    }
}

void ThreadpoolManager::on_work_callback_finished(u32 object) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto w = m_work.find(object); w != m_work.end()) {
        if (w->second.outstanding > 0) --w->second.outstanding;
    } else if (auto t = m_timers.find(object); t != m_timers.end()) {
        if (t->second.outstanding > 0) --t->second.outstanding;
    } else if (auto wt = m_waits.find(object); wt != m_waits.end()) {
        if (wt->second.outstanding > 0) --wt->second.outstanding;
    }
    // Windows frees a closed object once its callbacks have finished.
    release_object_if_idle(object);
    m_idle.notify_all();
}

bool ThreadpoolManager::outstanding_zero(u32 object) const {
    // Caller holds m_mutex.
    if (auto w = m_work.find(object); w != m_work.end()) return w->second.outstanding == 0;
    if (auto t = m_timers.find(object); t != m_timers.end()) return t->second.outstanding == 0;
    if (auto wt = m_waits.find(object); wt != m_waits.end()) return wt->second.outstanding == 0;
    return true;
}

void ThreadpoolManager::impl_CloseThreadpoolWork(Marshaler& m) {
    // VOID CloseThreadpoolWork(PTP_WORK pwk). The object is released, but
    // callbacks already submitted still run; Windows frees it afterwards.
    const auto work = static_cast<u32>(m.arg(0));
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_work.find(work);
    if (found == m_work.end()) return;
    found->second.closed = true;
    release_object_if_idle(work);
}

void ThreadpoolManager::impl_WaitForThreadpoolWorkCallbacks(Marshaler& m) {
    // VOID WaitForThreadpoolWorkCallbacks(PTP_WORK pwk,
    //                                     BOOL fCancelPendingCallbacks)
    // Every submission is handed to its own thread at once, so there is
    // never a queued-but-not-started callback left to cancel.
    const auto work = static_cast<u32>(m.arg(0));
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_work.find(work) == m_work.end()) return;
    m_idle.wait(lock, [&] { return m_shutdown || outstanding_zero(work); });
}

void ThreadpoolManager::impl_TrySubmitThreadpoolCallback(Marshaler& m) {
    // BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK pfns, PVOID pv,
    //                                  PTP_CALLBACK_ENVIRON pcbe)
    const GuestAddr callback = m.arg_ptr(0);
    const GuestAddr context = m.arg_ptr(1);
    const GuestAddr environment = m.arg_ptr(2);
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!read_environment_ok(m, environment)) {
        m.set_return(0);
        return;
    }
    // A simple callback has no object: two arguments, nothing to wait on.
    if (!dispatch(0, callback, context, 0, 0, SIMPLE_CALLBACK_ARGS)) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

// =============================================================================
// Timer objects
// =============================================================================

void ThreadpoolManager::impl_CreateThreadpoolTimer(Marshaler& m) {
    // PTP_TIMER CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti, PVOID pv,
    //                                 PTP_CALLBACK_ENVIRON pcbe)
    const GuestAddr callback = m.arg_ptr(0);
    const GuestAddr context = m.arg_ptr(1);
    const GuestAddr environment = m.arg_ptr(2);
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!read_environment_ok(m, environment)) {
        m.set_return(0);
        return;
    }
    const u32 handle = new_object_id();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ThreadpoolTimer t;
        t.callback = callback;
        t.context = context;
        m_timers.emplace(handle, t);
    }
    m.set_return(handle);
}

void ThreadpoolManager::ensure_timer_thread() {
    // Caller holds m_mutex.
    if (m_timer_thread_started || m_shutdown) return;
    m_timer_thread_started = true;
    m_timer_thread = std::thread([this] { timer_thread_main(); });
}

void ThreadpoolManager::timer_thread_main() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_shutdown) {
        Clock::time_point next = Clock::time_point::max();
        for (const auto& [id, t] : m_timers) {
            (void)id;
            if (t.armed && t.due < next) next = t.due;
        }
        if (next == Clock::time_point::max()) {
            m_timer_cv.wait(lock);
            continue;
        }
        if (Clock::now() < next) {
            m_timer_cv.wait_until(lock, next);
            continue;
        }
        struct Fire { u32 id; GuestAddr callback; GuestAddr context; };
        std::vector<Fire> fires;
        const auto now = Clock::now();
        for (auto& [id, t] : m_timers) {
            if (!t.armed || t.due > now) continue;
            if (t.period_ms != 0) {
                t.due += std::chrono::milliseconds(t.period_ms);
                if (t.due < now) t.due = now + std::chrono::milliseconds(t.period_ms);
            } else {
                // One-shot: no further expiry, but IsThreadpoolTimerSet keeps
                // reporting TRUE (tpt_oneshot isset_after=1).
                t.armed = false;
            }
            ++t.outstanding;
            fires.push_back({id, t.callback, t.context});
        }
        lock.unlock();
        for (const auto& f : fires) {
            if (!dispatch(f.id, f.callback, f.context, f.id, 0, OBJECT_CALLBACK_ARGS)) {
                on_work_callback_finished(f.id);
            }
        }
        lock.lock();
    }
}

namespace {

/// Shared by SetThreadpoolTimer / SetThreadpoolTimerEx.
struct TimerSetArgs {
    u32 timer;
    bool has_due;
    i64 due;
    u32 period;
};

} // namespace

void ThreadpoolManager::impl_SetThreadpoolTimer(Marshaler& m) {
    // VOID SetThreadpoolTimer(PTP_TIMER pti, PFILETIME pftDueTime,
    //                         DWORD msPeriod, DWORD msWindowLength)
    // msWindowLength only lets the system delay an expiry to batch timers;
    // firing on time is within that contract.
    const auto timer = static_cast<u32>(m.arg(0));
    const GuestAddr due_ptr = m.arg_ptr(1);
    const u32 period = m.arg32(2);
    i64 due = 0;
    const bool has_due = due_ptr != 0 && read_filetime(m, due_ptr, due);
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_timers.find(timer);
    if (found == m_timers.end() || found->second.closed) return;
    auto& t = found->second;
    if (!has_due) {
        // NULL pftDueTime: stop queuing callbacks; running ones finish.
        t.set = false;
        t.armed = false;
    } else {
        t.set = true;
        t.armed = true;
        t.period_ms = period;
        t.due = Clock::now() +
                std::chrono::nanoseconds(filetime_delay_100ns(due) * 100);
        ensure_timer_thread();
    }
    m_timer_cv.notify_all();
}

void ThreadpoolManager::impl_SetThreadpoolTimerEx(Marshaler& m) {
    // BOOL SetThreadpoolTimerEx(PTP_TIMER, PFILETIME, DWORD, DWORD): as
    // SetThreadpoolTimer, returning TRUE when a previously set timer was
    // replaced or cancelled (Learn).
    const auto timer = static_cast<u32>(m.arg(0));
    bool was_set = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_timers.find(timer);
        was_set = found != m_timers.end() && found->second.armed;
    }
    impl_SetThreadpoolTimer(m);
    m.set_return(was_set ? 1u : 0u);
}

void ThreadpoolManager::impl_IsThreadpoolTimerSet(Marshaler& m) {
    // BOOL IsThreadpoolTimerSet(PTP_TIMER pti)
    const auto timer = static_cast<u32>(m.arg(0));
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_timers.find(timer);
    m.set_return(found != m_timers.end() && found->second.set ? 1u : 0u);
}

void ThreadpoolManager::impl_WaitForThreadpoolTimerCallbacks(Marshaler& m) {
    // VOID WaitForThreadpoolTimerCallbacks(PTP_TIMER, BOOL fCancelPending):
    // expiries are dispatched to their own thread at once, so there is no
    // queued callback to cancel; running ones are waited for.
    const auto timer = static_cast<u32>(m.arg(0));
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_timers.find(timer) == m_timers.end()) return;
    m_idle.wait(lock, [&] { return m_shutdown || outstanding_zero(timer); });
}

void ThreadpoolManager::impl_CloseThreadpoolTimer(Marshaler& m) {
    // VOID CloseThreadpoolTimer(PTP_TIMER): released now if idle, otherwise
    // after its running callbacks return (tpt_close_while_running count=1).
    const auto timer = static_cast<u32>(m.arg(0));
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_timers.find(timer);
    if (found == m_timers.end()) return;
    found->second.closed = true;
    found->second.armed = false;
    found->second.set = false;
    release_object_if_idle(timer);
    m_timer_cv.notify_all();
}

// =============================================================================
// Wait objects
// =============================================================================

void ThreadpoolManager::impl_CreateThreadpoolWait(Marshaler& m) {
    // PTP_WAIT CreateThreadpoolWait(PTP_WAIT_CALLBACK pfnwa, PVOID pv,
    //                               PTP_CALLBACK_ENVIRON pcbe)
    const GuestAddr callback = m.arg_ptr(0);
    const GuestAddr context = m.arg_ptr(1);
    const GuestAddr environment = m.arg_ptr(2);
    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!read_environment_ok(m, environment)) {
        m.set_return(0);
        return;
    }
    if (m_sync == nullptr || m_threads == nullptr) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    const u32 handle = new_object_id();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ThreadpoolWait w;
        w.callback = callback;
        w.context = context;
        m_waits.emplace(handle, w);
    }
    m.set_return(handle);
}

void ThreadpoolManager::arm_wait(u32 wait, u64 generation, Handle handle,
                                 u32 timeout_ms,
                                 std::shared_ptr<std::atomic<bool>> cancel) {
    // Caller holds m_mutex and has counted this waiter in m_live_waiters.
    std::thread([this, wait, generation, handle, timeout_ms, cancel] {
        // Never a real guest tid: keeps the SyncManager from treating this
        // host thread as the GUI thread (same reasoning as RegisterWait).
        const u32 waiter_tid = 0xE0000000u | (wait & 0x0FFFFFFFu);
        ThreadManager::set_current_thread_id(waiter_tid);
        // It runs the guest's wait callback: a thread of this process.
        // (Unregistered before m_live_waiters drops, which is what lets
        // shutdown proceed.)
        if (m_threads != nullptr) m_threads->add_service_thread(waiter_tid);
        const u32 ret = m_sync->wait_for_single_object_cancellable(
            handle, timeout_ms, *cancel);
        GuestAddr callback = 0;
        GuestAddr context = 0;
        bool fire = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_waits.find(wait);
            if (!m_shutdown && !cancel->load(std::memory_order_acquire) &&
                found != m_waits.end() && found->second.generation == generation &&
                !found->second.closed && ret != WAIT_FAILED) {
                // One-shot: the wait is satisfied and must be set again.
                found->second.cancel.reset();
                ++found->second.outstanding;
                callback = found->second.callback;
                context = found->second.context;
                fire = true;
            }
        }
        if (fire) {
            // TP_WAIT_RESULT: WAIT_OBJECT_0 when signalled, WAIT_TIMEOUT on
            // timeout (tpw_signaled result=0, tpw_timeout result=102).
            if (!dispatch(wait, callback, context, wait, ret, WAIT_CALLBACK_ARGS)) {
                on_work_callback_finished(wait);
            }
        }
        if (m_threads != nullptr) m_threads->remove_service_thread(waiter_tid);
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_live_waiters;
        m_idle.notify_all();
    }).detach();
}

void ThreadpoolManager::impl_SetThreadpoolWait(Marshaler& m) {
    // VOID SetThreadpoolWait(PTP_WAIT pwa, HANDLE h, PFILETIME pftTimeout)
    const auto wait = static_cast<u32>(m.arg(0));
    const auto handle = static_cast<Handle>(m.arg(1));
    const GuestAddr timeout_ptr = m.arg_ptr(2);
    u32 timeout_ms = INFINITE_WAIT;
    if (timeout_ptr != 0) {
        i64 ft = 0;
        if (!read_filetime(m, timeout_ptr, ft)) return;
        // Learn: 0 times out at once; relative/absolute in 100 ns units.
        // Round up so a wait never ends before its deadline.
        const u64 delay = filetime_delay_100ns(ft);
        const u64 ms = (delay + 9999) / 10000;
        timeout_ms = ms >= INFINITE_WAIT ? INFINITE_WAIT - 1 : static_cast<u32>(ms);
    }
    std::shared_ptr<std::atomic<bool>> previous;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_waits.find(wait);
        if (found == m_waits.end() || found->second.closed || m_shutdown) return;
        auto& w = found->second;
        ++w.generation;
        previous = std::move(w.cancel);
        if (handle != 0) {
            w.cancel = std::make_shared<std::atomic<bool>>(false);
            ++m_live_waiters;
            arm_wait(wait, w.generation, handle, timeout_ms, w.cancel);
        }
    }
    if (previous) {
        // h == NULL (or a new handle) stops the previous wait
        // (tpw_cancel_null: no callback, the event is not consumed).
        previous->store(true, std::memory_order_release);
        m_sync->notify_waiters();
    }
}

void ThreadpoolManager::impl_SetThreadpoolWaitEx(Marshaler& m) {
    // BOOL SetThreadpoolWaitEx(PTP_WAIT, HANDLE, PFILETIME, PVOID Reserved):
    // TRUE when a wait that was still pending was replaced (Learn).
    const auto wait = static_cast<u32>(m.arg(0));
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_waits.find(wait);
        pending = found != m_waits.end() && found->second.cancel != nullptr;
    }
    impl_SetThreadpoolWait(m);
    m.set_return(pending ? 1u : 0u);
}

void ThreadpoolManager::impl_WaitForThreadpoolWaitCallbacks(Marshaler& m) {
    // VOID WaitForThreadpoolWaitCallbacks(PTP_WAIT, BOOL fCancelPending)
    const auto wait = static_cast<u32>(m.arg(0));
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_waits.find(wait) == m_waits.end()) return;
    m_idle.wait(lock, [&] { return m_shutdown || outstanding_zero(wait); });
}

void ThreadpoolManager::impl_CloseThreadpoolWait(Marshaler& m) {
    // VOID CloseThreadpoolWait(PTP_WAIT): stops a pending wait; the object
    // goes away once its running callbacks return.
    const auto wait = static_cast<u32>(m.arg(0));
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_waits.find(wait);
        if (found == m_waits.end()) return;
        found->second.closed = true;
        ++found->second.generation;
        cancel = std::move(found->second.cancel);
        release_object_if_idle(wait);
    }
    if (cancel) {
        cancel->store(true, std::memory_order_release);
        if (m_sync != nullptr) m_sync->notify_waiters();
    }
}

// =============================================================================
// Legacy pool and callback-instance functions
// =============================================================================

void ThreadpoolManager::impl_QueueUserWorkItem(Marshaler& m) {
    // BOOL QueueUserWorkItem(LPTHREAD_START_ROUTINE Function, PVOID Context,
    //                        ULONG Flags)
    //
    // Unlike a TP_SIMPLE_CALLBACK this callback takes ONE argument — it has
    // exactly the signature of a thread start routine, DWORD (PVOID) — so it
    // cannot go through dispatch(), which passes a callback instance first.
    const GuestAddr function = m.arg_ptr(0);
    const GuestAddr context = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    if (function == 0 || m_threads == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // The WT_* flags select which pool thread runs the item (I/O,
    // long-running, persistent). One guest thread per submission satisfies
    // every one of them.
    u32 tid = 0;
    const Handle thread =
        m_threads->create_thread(0, function, context, 0, &tid);
    if (thread == INVALID_HANDLE || tid == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    m_threads->run_async(tid, function, context, 0, 0, 1);
    m_threads->close_thread_handle(thread);
    (void)flags;
    m.set_return(1);
}

bool ThreadpoolManager::add_instance_action(u64 instance, std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_instances.find(instance);
    if (found == m_instances.end()) return false;
    found->second.on_return.push_back(std::move(fn));
    return true;
}

void ThreadpoolManager::impl_CallbackMayRunLong(Marshaler& m) {
    // BOOL CallbackMayRunLong(PTP_CALLBACK_INSTANCE pci): TRUE when another
    // thread is available for other callbacks. Every callback gets its own
    // thread here, so that always holds (callbackmayrunlong r=1).
    (void)m.arg(0);
    m.set_return(1);
}

void ThreadpoolManager::impl_SetEventWhenCallbackReturns(Marshaler& m) {
    // VOID SetEventWhenCallbackReturns(PTP_CALLBACK_INSTANCE, HANDLE evt)
    const u64 instance = m.arg(0);
    const auto event = static_cast<Handle>(m.arg(1));
    SyncManager* sync = m_sync;
    (void)add_instance_action(instance, [sync, event] {
        if (sync != nullptr) (void)sync->set_event(event);
    });
}

void ThreadpoolManager::impl_ReleaseSemaphoreWhenCallbackReturns(Marshaler& m) {
    // VOID ReleaseSemaphoreWhenCallbackReturns(PTP_CALLBACK_INSTANCE,
    //                                          HANDLE sem, DWORD crel)
    const u64 instance = m.arg(0);
    const auto sem = static_cast<Handle>(m.arg(1));
    const auto count = static_cast<i32>(m.arg32(2));
    SyncManager* sync = m_sync;
    (void)add_instance_action(instance, [sync, sem, count] {
        if (sync != nullptr) (void)sync->release_semaphore(sem, count, nullptr);
    });
}

void ThreadpoolManager::impl_ReleaseMutexWhenCallbackReturns(Marshaler& m) {
    // VOID ReleaseMutexWhenCallbackReturns(PTP_CALLBACK_INSTANCE, HANDLE mut):
    // released as the callback's thread, which owns it.
    const u64 instance = m.arg(0);
    const auto mutex = static_cast<Handle>(m.arg(1));
    SyncManager* sync = m_sync;
    (void)add_instance_action(instance, [sync, mutex] {
        if (sync != nullptr) (void)sync->release_mutex(mutex);
    });
}

void ThreadpoolManager::impl_LeaveCriticalSectionWhenCallbackReturns(Marshaler& m) {
    // VOID LeaveCriticalSectionWhenCallbackReturns(PTP_CALLBACK_INSTANCE,
    //                                              PCRITICAL_SECTION pcs)
    const u64 instance = m.arg(0);
    const GuestAddr cs = m.arg_ptr(1);
    LeaveCriticalSection leave = m_leave_cs;
    if (!leave) {
        static std::atomic<bool> logged{false};
        log_unimplemented_once(logged,
            "LeaveCriticalSectionWhenCallbackReturns: no critical-section manager");
        return;
    }
    (void)add_instance_action(instance, [leave, cs] { leave(cs); });
}

void ThreadpoolManager::impl_FreeLibraryWhenCallbackReturns(Marshaler& m) {
    // VOID FreeLibraryWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci,
    //                                     HMODULE mod)
    //
    // The module must stay mapped until the callback returns and then be
    // released exactly once. Releasing the last reference runs the module's
    // DLL_PROCESS_DETACH, which is guest code, and the callback thread has no
    // guest context left once its callback has returned. The reference is
    // therefore kept (the module stays loaded) and the gap is reported.
    (void)m.arg(0);
    (void)m.arg(1);
    static std::atomic<bool> logged{false};
    log_unimplemented_once(logged,
        "FreeLibraryWhenCallbackReturns: module reference retained");
}

void ThreadpoolManager::impl_DisassociateCurrentThreadFromCallback(Marshaler& m) {
    // VOID DisassociateCurrentThreadFromCallback(PTP_CALLBACK_INSTANCE):
    // the callback counts as complete for WaitForThreadpool*Callbacks from
    // now on, although it keeps running (Learn).
    const u64 instance = m.arg(0);
    u32 object = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_instances.find(instance);
        if (found == m_instances.end() || found->second.disassociated) return;
        found->second.disassociated = true;
        object = found->second.object;
    }
    if (object != 0) on_work_callback_finished(object);
}

void add_threadpool_exports(DllExports& exports, ThreadpoolManager& mgr) {
    const auto add = [&](const char* name, auto fn, u16 bytes) {
        exports.entries.push_back(ExportEntry{name, 0, fn, bytes});
    };
    // x86 stdcall byte counts from threadpoolapiset.h / winbase.h prototypes.
    add("CreateThreadpoolWork",
        [&mgr](Marshaler& m) { mgr.impl_CreateThreadpoolWork(m); }, 12);
    add("SubmitThreadpoolWork",
        [&mgr](Marshaler& m) { mgr.impl_SubmitThreadpoolWork(m); }, 4);
    add("CloseThreadpoolWork",
        [&mgr](Marshaler& m) { mgr.impl_CloseThreadpoolWork(m); }, 4);
    add("WaitForThreadpoolWorkCallbacks",
        [&mgr](Marshaler& m) { mgr.impl_WaitForThreadpoolWorkCallbacks(m); }, 8);
    add("TrySubmitThreadpoolCallback",
        [&mgr](Marshaler& m) { mgr.impl_TrySubmitThreadpoolCallback(m); }, 12);
    add("CreateThreadpoolTimer",
        [&mgr](Marshaler& m) { mgr.impl_CreateThreadpoolTimer(m); }, 12);
    add("SetThreadpoolTimer",
        [&mgr](Marshaler& m) { mgr.impl_SetThreadpoolTimer(m); }, 16);
    add("SetThreadpoolTimerEx",
        [&mgr](Marshaler& m) { mgr.impl_SetThreadpoolTimerEx(m); }, 16);
    add("IsThreadpoolTimerSet",
        [&mgr](Marshaler& m) { mgr.impl_IsThreadpoolTimerSet(m); }, 4);
    add("WaitForThreadpoolTimerCallbacks",
        [&mgr](Marshaler& m) { mgr.impl_WaitForThreadpoolTimerCallbacks(m); }, 8);
    add("CloseThreadpoolTimer",
        [&mgr](Marshaler& m) { mgr.impl_CloseThreadpoolTimer(m); }, 4);
    add("CreateThreadpoolWait",
        [&mgr](Marshaler& m) { mgr.impl_CreateThreadpoolWait(m); }, 12);
    add("SetThreadpoolWait",
        [&mgr](Marshaler& m) { mgr.impl_SetThreadpoolWait(m); }, 12);
    add("SetThreadpoolWaitEx",
        [&mgr](Marshaler& m) { mgr.impl_SetThreadpoolWaitEx(m); }, 16);
    add("WaitForThreadpoolWaitCallbacks",
        [&mgr](Marshaler& m) { mgr.impl_WaitForThreadpoolWaitCallbacks(m); }, 8);
    add("CloseThreadpoolWait",
        [&mgr](Marshaler& m) { mgr.impl_CloseThreadpoolWait(m); }, 4);
    add("QueueUserWorkItem",
        [&mgr](Marshaler& m) { mgr.impl_QueueUserWorkItem(m); }, 12);
    add("CallbackMayRunLong",
        [&mgr](Marshaler& m) { mgr.impl_CallbackMayRunLong(m); }, 4);
    add("SetEventWhenCallbackReturns",
        [&mgr](Marshaler& m) { mgr.impl_SetEventWhenCallbackReturns(m); }, 8);
    add("ReleaseSemaphoreWhenCallbackReturns",
        [&mgr](Marshaler& m) { mgr.impl_ReleaseSemaphoreWhenCallbackReturns(m); }, 12);
    add("ReleaseMutexWhenCallbackReturns",
        [&mgr](Marshaler& m) { mgr.impl_ReleaseMutexWhenCallbackReturns(m); }, 8);
    add("LeaveCriticalSectionWhenCallbackReturns",
        [&mgr](Marshaler& m) { mgr.impl_LeaveCriticalSectionWhenCallbackReturns(m); }, 8);
    add("FreeLibraryWhenCallbackReturns",
        [&mgr](Marshaler& m) { mgr.impl_FreeLibraryWhenCallbackReturns(m); }, 8);
    add("DisassociateCurrentThreadFromCallback",
        [&mgr](Marshaler& m) { mgr.impl_DisassociateCurrentThreadFromCallback(m); }, 4);

    // Cleanup groups and I/O completion objects are not modelled. Creation
    // fails with the documented NULL result (so no guest ever holds such an
    // object), and the functions that take one are registered with their
    // real stdcall sizes instead of falling to the missing-export stub, which
    // would return a TRUE-looking 0xFFFFFFFF and leave the x86 stack
    // unbalanced.
    static std::atomic<bool> cleanup_logged{false};
    static std::atomic<bool> io_logged{false};
    add("CreateThreadpoolCleanupGroup", [](Marshaler& m) {
        log_unimplemented_once(cleanup_logged, "CreateThreadpoolCleanupGroup");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
    }, 0);
    add("CloseThreadpoolCleanupGroup", [](Marshaler& m) { (void)m.arg(0); }, 4);
    add("CloseThreadpoolCleanupGroupMembers", [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    }, 12);
    add("CreateThreadpoolIo", [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
        log_unimplemented_once(io_logged, "CreateThreadpoolIo");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
    }, 16);
    add("StartThreadpoolIo", [](Marshaler& m) { (void)m.arg(0); }, 4);
    add("CancelThreadpoolIo", [](Marshaler& m) { (void)m.arg(0); }, 4);
    add("CloseThreadpoolIo", [](Marshaler& m) { (void)m.arg(0); }, 4);
    add("WaitForThreadpoolIoCallbacks", [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1);
    }, 8);
}

} // namespace vitreo::winapi
