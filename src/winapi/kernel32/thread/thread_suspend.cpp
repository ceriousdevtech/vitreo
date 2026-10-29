// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace vitreo::winapi {

namespace {
bool thread_diag_enabled() noexcept {
    static const bool enabled =
        environment_flag_enabled("VITREO_THREAD_DIAG");
    return enabled;
}

[[nodiscard]] bool lock_mutex(pthread_mutex_t& mu, const char* where) noexcept {
    const int rc = pthread_mutex_lock(&mu);
    if (rc == 0) {
        return true;
    }
    std::fprintf(stderr,
                 "[thread-suspend] %s pthread_mutex_lock rc=%d (%s)\n", where,
                 rc, std::strerror(rc));
    return false;
}

void unlock_mutex(pthread_mutex_t& mu) noexcept {
    const int rc = pthread_mutex_unlock(&mu);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[thread-suspend] pthread_mutex_unlock rc=%d (%s)\n", rc,
                     std::strerror(rc));
    }
}

struct PthreadMutexGuard {
    pthread_mutex_t* mu{nullptr};
    explicit PthreadMutexGuard(pthread_mutex_t& m, const char* where) {
        if (lock_mutex(m, where)) {
            mu = &m;
        }
    }
    ~PthreadMutexGuard() {
        if (mu) {
            unlock_mutex(*mu);
        }
    }
    [[nodiscard]] bool locked() const noexcept { return mu != nullptr; }
    PthreadMutexGuard(const PthreadMutexGuard&) = delete;
    PthreadMutexGuard& operator=(const PthreadMutexGuard&) = delete;
};

void timed_wait_ms(pthread_cond_t& cv, pthread_mutex_t& mu, int ms) noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        (void)pthread_cond_wait(&cv, &mu);
        return;
    }
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(&cv, &mu, &ts);
}
}  // namespace

void ThreadSuspendRegistry::bind_guest_memory(GuestMemory* memory) noexcept {
    m_memory = memory;
}

GuestMemory* ThreadSuspendRegistry::guest_memory() const noexcept {
    return m_memory;
}

ThreadSuspendRegistry& ThreadSuspendRegistry::instance() noexcept {
    // Intentionally leaked. A function-local static is destroyed during
    // process teardown while winmm MultimediaTimerScheduler threads still
    // publish CpuState — libc++ unordered_map then SIGSEGVs on null buckets
    // (same immortal pattern as ntsync::global / hostcall ring). Teardown
    // aborted with signal 11 in get_or_create.
    static ThreadSuspendRegistry* s_registry = new ThreadSuspendRegistry();
    return *s_registry;
}

// -----------------------------------------------------------------------------
// Slot::park_slow — the running thread stops here while a suspend is pending.
// -----------------------------------------------------------------------------
void ThreadSuspendRegistry::Slot::park_slow() {
    if (!lock_mutex(mu, "park_slow")) {
        return;
    }
    // A context set while this thread was blocked in an API takes effect now
    // that the call has returned: every register as set, except RAX/EAX,
    // which carries the call's result (measured, see return_ctx_pending).
    // A checkpoint inside a guest callback the API is still running (same
    // host-call depth) is not the return.
    if (return_ctx_pending && live != nullptr &&
        host_call_depth.load() < return_ctx_depth) {
        const u64 result = live->gpr.get64(static_cast<u8>(cpu::RegIndex::RAX));
        *live = return_ctx;
        if (live->is_64bit()) {
            live->gpr.set64(static_cast<u8>(cpu::RegIndex::RAX), result);
        } else {
            live->gpr.set32(static_cast<u8>(cpu::RegIndex::RAX),
                            static_cast<u32>(result));
        }
        return_ctx_pending = false;
        if (suspend_depth <= 0 && !terminate_flag.load()) {
            park_flag.store(0, std::memory_order_relaxed);
        }
    }
    // TerminateThread: acknowledge and return to the dispatch loop, which
    // leaves guest code (PublishGuard::terminate_requested).
    if (terminate_flag.load(std::memory_order_acquire)) {
        terminate_acked = true;
        (void)pthread_cond_broadcast(&cv);
        unlock_mutex(mu);
        return;
    }
    if (suspend_depth <= 0) {
        unlock_mutex(mu);
        return;  // raced with a resume; keep running.
    }

    if (thread_diag_enabled()) {
        std::fprintf(stderr,
                     "[thread-diag] park tid=%u depth=%d rip=0x%llx\n",
                     tid, suspend_depth,
                     static_cast<unsigned long long>(live ? live->rip : 0));
    }

    // Capture a consistent snapshot while the thread is stopped. Materialize
    // lazy flags first so EFLAGS in the snapshot is architecturally correct.
    if (live) {
        live->materialize_lazy_flags();
        snapshot = *live;
        // EFLAGS bit 1 is always set in a running thread's hardware flags.
        snapshot.flags.rflags |= 0x2;
        snapshot_valid = true;
    }
    ctx_modified = false;
    parked = true;
    (void)pthread_cond_broadcast(&cv);

    while (suspend_depth > 0 && !terminate_flag.load()) {
        (void)pthread_cond_wait(&cv, &mu);
    }
    if (terminate_flag.load()) {
        terminate_acked = true;
        parked = false;
        snapshot_valid = false;
        ctx_modified = false;
        (void)pthread_cond_broadcast(&cv);
        unlock_mutex(mu);
        return;
    }

    // Apply a SetThreadContext that arrived while parked, then resume.
    if (ctx_modified && live) {
        *live = snapshot;
        ctx_modified = false;
    }
    parked = false;
    snapshot_valid = false;
    unlock_mutex(mu);
    if (thread_diag_enabled()) {
        std::fprintf(stderr, "[thread-diag] unpark tid=%u\n", tid);
    }
}

// -----------------------------------------------------------------------------
// PublishGuard — re-entrant per host thread.
// -----------------------------------------------------------------------------
namespace {
// Per host thread: the slot it owns and a re-entrancy depth so nested guest
// callbacks (which reuse the same CpuState) do not double-register.
thread_local std::shared_ptr<ThreadSuspendRegistry::Slot> t_owned_slot;
thread_local int t_publish_depth = 0;
}  // namespace

ThreadSuspendRegistry::PublishGuard::PublishGuard(u32 tid, cpu::CpuState* live) {
    if (t_publish_depth == 0) {
        m_slot = ThreadSuspendRegistry::instance().get_or_create(tid, live);
        if (!m_slot) {
            // Slot acquisition failed; leave depth at 0 so later publishes
            // on this host thread can retry cleanly.
            return;
        }
        ++t_publish_depth;
        m_tid = tid;
        m_live = live;
        t_owned_slot = m_slot;
        m_owner = true;
        if (thread_diag_enabled()) {
            std::fprintf(stderr,
                         "[thread-diag] publish tid=%u rip=0x%llx\n",
                         tid, static_cast<unsigned long long>(
                                  live ? live->rip : 0));
        }
    } else {
        ++t_publish_depth;
        // Nested: reuse the already-published slot (same thread, same CpuState).
        m_slot = t_owned_slot;
        m_owner = false;
        if (m_slot) {
            ++m_slot->guest_override_depth;
            // Guest code runs from here at the current host-call depth.
            m_prev_base = m_slot->guest_base_depth.exchange(
                m_slot->host_call_depth.load());
            m_guest_override = true;
        }
    }
}

ThreadSuspendRegistry::PublishGuard::~PublishGuard() {
    if (m_guest_override && m_slot) {
        --m_slot->guest_override_depth;
        m_slot->guest_base_depth.store(m_prev_base);
        // Dropping the override can expose the enclosing host call as a valid
        // suspend boundary. Notify only when a suspender can be waiting.
        // Same Dekker pairing as enter_host_call, with the base restore as
        // this side's store; seq_cst is required, not decorative.
        if (m_slot->park_flag.load(std::memory_order_seq_cst) != 0) {
            PthreadMutexGuard lk(m_slot->mu, "~PublishGuard.override");
            if (lk.locked()) {
                (void)pthread_cond_broadcast(&m_slot->cv);
            }
        }
        m_guest_override = false;
    }
    if (t_publish_depth > 0) {
        --t_publish_depth;
    }
    if (t_publish_depth == 0 && m_owner && m_slot) {
        auto& reg = ThreadSuspendRegistry::instance();
        // Keep the Slot object alive forever once created. Erasing it from the
        // map raced with multimedia-timer PublishGuard construction and left a
        // destroyed pthread mutex in place — libc++ then aborted the process
        // with "mutex lock failed: Invalid argument".
        // Retired slots stay in the map with live == nullptr so capture() and
        // suspend treat them as untracked.
        {
            PthreadMutexGuard map_lk(reg.m_map_mu, "~PublishGuard.map");
            PthreadMutexGuard slk(m_slot->mu, "~PublishGuard.slot");
            if (map_lk.locked() && slk.locked()) {
                auto publisher = std::find(
                    m_slot->live_publishers.rbegin(),
                    m_slot->live_publishers.rend(), m_live);
                if (publisher != m_slot->live_publishers.rend()) {
                    m_slot->live_publishers.erase(std::next(publisher).base());
                }

                if (m_slot->live_publishers.empty()) {
                    m_slot->live = nullptr;
                } else {
                    m_slot->live = m_slot->live_publishers.back();
                }
                (void)pthread_cond_broadcast(&m_slot->cv);
            }
        }
        t_owned_slot.reset();
    }
    m_slot.reset();
}

void ThreadSuspendRegistry::PublishGuard::enter_host_call() {
    if (!m_slot) return;
    ++m_slot->host_call_depth;
    // Normally there is no suspension requester. Synchronize with the
    // condition waiter only after request_suspend publishes park_flag.
    //
    // Both the increment above and the load below are SEQ_CST on purpose:
    // together with request_suspend's park_flag store and its
    // blocked_in_host_call() load they form a Dekker handshake (see the long
    // comment in request_suspend). Downgrading this load to acquire looks free
    // — it is the DMB ISH on every host call out of guest code — but acquire
    // does not order this thread's preceding store to host_call_depth before
    // it, so a suspender can miss the increment while this thread misses
    // park_flag, and SuspendThread then blocks for the entire duration of a
    // host call that may never return. Leave it seq_cst.
    if (m_slot->park_flag.load(std::memory_order_seq_cst) != 0) {
        PthreadMutexGuard lk(m_slot->mu, "enter_host_call");
        if (lk.locked()) {
            (void)pthread_cond_broadcast(&m_slot->cv);
        }
    }
}

ThreadSuspendRegistry::HostCallScope::HostCallScope() noexcept
    : m_slot(t_owned_slot) {
    if (!m_slot) return;
    ++m_slot->host_call_depth;
    // Same Dekker handshake as PublishGuard::enter_host_call: only synchronize
    // with a waiting suspender, which is the uncommon case. The seq_cst load is
    // load-bearing for the same reason — do not weaken it.
    if (m_slot->park_flag.load(std::memory_order_seq_cst) != 0) {
        PthreadMutexGuard lk(m_slot->mu, "HostCallScope");
        if (lk.locked()) {
            (void)pthread_cond_broadcast(&m_slot->cv);
        }
    }
}

ThreadSuspendRegistry::HostCallScope::~HostCallScope() {
    if (!m_slot) return;
    --m_slot->host_call_depth;
}

void ThreadSuspendRegistry::PublishGuard::leave_host_call() {
    if (!m_slot) return;
    --m_slot->host_call_depth;
}

// -----------------------------------------------------------------------------
// Registry map helpers.
// -----------------------------------------------------------------------------
std::shared_ptr<ThreadSuspendRegistry::Slot>
ThreadSuspendRegistry::find(u32 tid) {
    PthreadMutexGuard lk(m_map_mu, "find");
    if (!lk.locked()) {
        return nullptr;
    }
    auto it = m_slots.find(tid);
    return it != m_slots.end() ? it->second : nullptr;
}

std::shared_ptr<ThreadSuspendRegistry::Slot>
ThreadSuspendRegistry::get_or_create(u32 tid, cpu::CpuState* live) {
    std::shared_ptr<Slot> slot;
    {
        PthreadMutexGuard lk(m_map_mu, "get_or_create.map");
        if (!lk.locked()) {
            return nullptr;
        }
        auto& entry = m_slots[tid];
        if (!entry) {
            entry = std::make_shared<Slot>();
        }
        slot = entry;
    }

    PthreadMutexGuard slk(slot->mu, "get_or_create.slot");
    if (!slk.locked()) {
        // Replace a corrupt slot and retry once under the map lock.
        PthreadMutexGuard lk(m_map_mu, "get_or_create.replace");
        if (!lk.locked()) {
            return nullptr;
        }
        m_slots[tid] = std::make_shared<Slot>();
        slot = m_slots[tid];
        PthreadMutexGuard slk2(slot->mu, "get_or_create.slot.retry");
        if (!slk2.locked()) {
            std::fprintf(stderr,
                         "[thread-suspend] get_or_create failed tid=%u\n", tid);
            return nullptr;
        }
        slot->tid = tid;
        slot->live_publishers.push_back(live);
        slot->live = live;
        (void)pthread_cond_broadcast(&slot->cv);
        return slot;
    }

    slot->tid = tid;
    slot->live_publishers.push_back(live);
    slot->live = live;
    (void)pthread_cond_broadcast(&slot->cv);
    return slot;
}

// -----------------------------------------------------------------------------
// SuspendThread / ResumeThread / Get/SetThreadContext entry points.
// -----------------------------------------------------------------------------
bool ThreadSuspendRegistry::request_suspend(u32 tid) {
    // A thread that was launched but has not yet published its register file
    // (its host thread is still starting) must not run guest code either:
    // record the suspension in a slot it will adopt when it publishes, so
    // its first checkpoint parks it. Without this the suspension was dropped
    // and the "suspended" thread ran on.
    std::shared_ptr<Slot> slot;
    {
        PthreadMutexGuard lk(m_map_mu, "request_suspend.map");
        if (!lk.locked()) return false;
        auto& entry = m_slots[tid];
        if (!entry) {
            entry = std::make_shared<Slot>();
            entry->tid = tid;
        }
        slot = entry;
    }

    const u32 self = ThreadManager::get_current_thread_id_static();
    int depth = 0;
    bool unpublished = false;
    {
        if (!lock_mutex(slot->mu, "request_suspend.inc")) {
            return false;
        }
        unpublished = slot->live_publishers.empty() && !slot->parked;
        depth = ++slot->suspend_depth;
        // SEQ_CST, NOT relaxed, and NOT release: this is the suspender half of
        // a Dekker store-load handshake with enter_host_call/HostCallScope.
        //
        //   suspender (here)                target (enter_host_call)
        //     park_flag = 1                   ++host_call_depth
        //     ...                             if (park_flag) broadcast
        //     if (host_call_depth > ...) ok
        //     else cond_wait
        //
        // The target takes no lock on its fast path, so the slot mutex orders
        // nothing between these two threads. If either side is allowed to
        // reorder its store past its load, both can read stale: the target
        // sails into a *blocking* host call without broadcasting while the
        // suspender concludes it is still running guest code and waits on a
        // park that can never happen (the hang the HostCallScope comment in
        // the header describes). Only a total order over all four operations
        // rules that out, so all four are seq_cst. Do not weaken any of them.
        slot->park_flag.store(1, std::memory_order_seq_cst);
        unlock_mutex(slot->mu);
    }
    if (thread_diag_enabled()) {
        std::fprintf(stderr,
                     "[thread-diag] suspend self=%u target=%u depth=%d%s\n",
                     self, tid, depth, tid == self ? " self" : "");
    }

    // Suspending the current thread must not wait for a park that only this
    // thread could perform. Track the depth (so a later self GetThreadContext
    // reads a live snapshot) but return immediately. Nor is there anything to
    // wait for on a thread that has not published yet: it parks at its first
    // checkpoint, before any guest code.
    if (tid == self || unpublished) return true;

    // Windows does not return from SuspendThread while the target is still
    // executing user-mode code. Wait until the guest runner has parked at a
    // checkpoint. A target already blocked inside a host call cannot mutate
    // guest CPU state, so capture() may use its published call-boundary state.
    if (!lock_mutex(slot->mu, "request_suspend.wait")) {
        return true;
    }
    while (!(slot->parked || slot->live == nullptr ||
             slot->blocked_in_host_call())) {
        (void)pthread_cond_wait(&slot->cv, &slot->mu);
    }
    if (thread_diag_enabled()) {
        std::fprintf(stderr,
                     "[thread-diag] suspend-result self=%u target=%u "
                     "parked=%d hostcall=%d live=%d depth=%d\n",
                     self, tid, slot->parked ? 1 : 0,
                     slot->blocked_in_host_call() ? 1 : 0,
                     slot->live ? 1 : 0, slot->suspend_depth);
    }
    unlock_mutex(slot->mu);
    return true;
}

void ThreadSuspendRegistry::release_all_for_shutdown() {
    // Snapshot the slots under the map lock, then release each one, mirroring
    // sample_threads(): never hold the map lock while taking a slot lock.
    std::vector<std::shared_ptr<Slot>> slots;
    if (lock_mutex(m_map_mu, "release_all_for_shutdown")) {
        slots.reserve(m_slots.size());
        for (const auto& [tid, slot] : m_slots) {
            (void)tid;
            if (slot) slots.push_back(slot);
        }
        unlock_mutex(m_map_mu);
    }

    for (const auto& slot : slots) {
        if (!lock_mutex(slot->mu, "release_all_for_shutdown.slot")) {
            continue;
        }
        const int released = slot->suspend_depth;
        slot->suspend_depth = 0;
        slot->park_flag.store(slot->terminate_flag.load() ? 1 : 0,
                              std::memory_order_relaxed);
        (void)pthread_cond_broadcast(&slot->cv);
        unlock_mutex(slot->mu);
        if (released > 0 && thread_diag_enabled()) {
            std::fprintf(stderr,
                         "[thread-diag] shutdown-release tid=%u depth=%d->0\n",
                         slot->tid, released);
        }
    }
}

void ThreadSuspendRegistry::request_terminate(u32 tid) {
    std::shared_ptr<Slot> slot;
    {
        PthreadMutexGuard lk(m_map_mu, "request_terminate.map");
        if (!lk.locked()) return;
        auto& entry = m_slots[tid];
        if (!entry) {
            entry = std::make_shared<Slot>();
            entry->tid = tid;
        }
        slot = entry;
    }
    if (!lock_mutex(slot->mu, "request_terminate")) return;
    slot->terminate_flag.store(true, std::memory_order_seq_cst);
    // Same Dekker pairing with enter_host_call as request_suspend.
    slot->park_flag.store(1, std::memory_order_seq_cst);
    (void)pthread_cond_broadcast(&slot->cv);
    const bool self = tid == ThreadManager::get_current_thread_id_static();
    if (!self) {
        while (!(slot->terminate_acked || slot->live == nullptr ||
                 slot->blocked_in_host_call())) {
            (void)pthread_cond_wait(&slot->cv, &slot->mu);
        }
    }
    if (thread_diag_enabled()) {
        std::fprintf(stderr,
                     "[thread-diag] terminate target=%u self=%d acked=%d "
                     "hostcall=%d live=%d\n",
                     tid, self ? 1 : 0, slot->terminate_acked ? 1 : 0,
                     slot->blocked_in_host_call() ? 1 : 0,
                     slot->live ? 1 : 0);
    }
    unlock_mutex(slot->mu);
}

bool ThreadSuspendRegistry::is_terminating(u32 tid) {
    auto slot = find(tid);
    return slot != nullptr &&
           slot->terminate_flag.load(std::memory_order_acquire);
}

bool ThreadSuspendRegistry::current_thread_terminating() noexcept {
    const auto& slot = t_owned_slot;
    return slot != nullptr &&
           slot->terminate_flag.load(std::memory_order_acquire);
}

void ThreadSuspendRegistry::request_resume(u32 tid) {
    auto slot = find(tid);
    if (!slot) {
        if (thread_diag_enabled()) {
            std::fprintf(stderr,
                         "[thread-diag] resume self=%u target=%u untracked\n",
                         ThreadManager::get_current_thread_id_static(), tid);
        }
        return;
    }
    if (!lock_mutex(slot->mu, "request_resume")) {
        return;
    }
    const int previous_depth = slot->suspend_depth;
    if (slot->suspend_depth > 0) {
        slot->suspend_depth--;
        // A staged post-return context keeps the flag up so the thread's
        // next checkpoint reaches park_slow and applies it.
        if (slot->suspend_depth == 0 && !slot->return_ctx_pending &&
            !slot->terminate_flag.load()) {
            slot->park_flag.store(0, std::memory_order_relaxed);
        }
    }
    (void)pthread_cond_broadcast(&slot->cv);
    const int new_depth = slot->suspend_depth;
    const bool parked = slot->parked;
    unlock_mutex(slot->mu);
    if (thread_diag_enabled()) {
        std::fprintf(stderr,
                     "[thread-diag] resume self=%u target=%u depth=%d->%d "
                     "parked=%d\n",
                     ThreadManager::get_current_thread_id_static(), tid,
                     previous_depth, new_depth, parked ? 1 : 0);
    }
}

cpu::CpuState ThreadSuspendRegistry::blocked_view(
    const cpu::CpuState& live) const {
    cpu::CpuState view = live;
    if (m_api_return != nullptr) {
        const GuestAddr ret = m_api_return(live.rip, live.is_64bit());
        if (ret != 0) view.rip = ret;
    }
    return view;
}

bool ThreadSuspendRegistry::capture(u32 tid, cpu::CpuState& out) {
    auto slot = find(tid);
    if (!slot) return false;
    if (!lock_mutex(slot->mu, "capture")) {
        return false;
    }
    bool ok = false;
    if (slot->return_ctx_pending) {
        // Reads back what SetThreadContext staged, as Windows does.
        out = slot->return_ctx;
        ok = true;
    } else if (slot->parked && slot->snapshot_valid) {
        out = slot->snapshot;
        ok = true;
    } else if (slot->live) {
        // The thread did not park (self-suspend or blocked in a host call).
        // Copy first and materialize the COPY: GetThreadContext never alters
        // the target, and the lazy-flag state belongs to the thread (or JIT)
        // that is running it.
        out = slot->blocked_in_host_call() ? blocked_view(*slot->live)
                                           : *slot->live;
        out.materialize_lazy_flags();
        out.flags.rflags |= 0x2;  // always set in a running thread's EFLAGS
        ok = true;
    }
    unlock_mutex(slot->mu);
    return ok;
}

bool ThreadSuspendRegistry::apply(u32 tid, const cpu::CpuState& in) {
    auto slot = find(tid);
    if (!slot) return false;
    if (!lock_mutex(slot->mu, "apply")) {
        return false;
    }
    bool ok = false;
    if (slot->parked) {
        slot->snapshot = in;
        slot->ctx_modified = true;
        ok = true;
    } else if (slot->live && (slot->return_ctx_pending ||
                              slot->blocked_in_host_call())) {
        // Blocked inside an API: the handler still owns the register file
        // and the dispatcher restores it on return. Apply after the return.
        if (!slot->return_ctx_pending) {
            slot->return_ctx_depth = slot->host_call_depth.load();
        }
        slot->return_ctx = in;
        slot->return_ctx_pending = true;
        slot->park_flag.store(1, std::memory_order_seq_cst);
        ok = true;
    } else if (slot->live) {
        *slot->live = in;
        ok = true;
    }
    unlock_mutex(slot->mu);
    return ok;
}

std::vector<ThreadSuspendRegistry::ThreadSample>
ThreadSuspendRegistry::sample_threads() {
    // Collect the slots under the map lock, then read each register file
    // without taking the slot lock. A hang dump must not be able to block on a
    // thread that is stuck holding its own slot mutex — a torn RIP is a far
    // better outcome than a diagnostic that hangs alongside the bug it is
    // meant to explain.
    std::vector<std::shared_ptr<Slot>> slots;
    if (lock_mutex(m_map_mu, "sample_threads")) {
        slots.reserve(m_slots.size());
        for (const auto& [tid, slot] : m_slots) {
            (void)tid;
            if (slot) slots.push_back(slot);
        }
        unlock_mutex(m_map_mu);
    }

    std::vector<ThreadSample> samples;
    samples.reserve(slots.size());
    for (const auto& slot : slots) {
        const cpu::CpuState* live = slot->live;
        if (live == nullptr) continue;
        ThreadSample sample;
        sample.tid = slot->tid;
        sample.rip = live->rip;
        sample.is_64bit = live->is_64bit();
        sample.sp = live->sp();
        if (sample.is_64bit) {
            sample.bp = live->gpr.get64(5);
            sample.eax = live->gpr.get64(0);
            sample.ecx = live->gpr.get64(1);
            sample.edx = live->gpr.get64(2);
            sample.ebx = live->gpr.get64(3);
            sample.esi = live->gpr.get64(6);
            sample.edi = live->gpr.get64(7);
            sample.r12 = live->gpr.get64(12);
            sample.r13 = live->gpr.get64(13);
            sample.r14 = live->gpr.get64(14);
            sample.r15 = live->gpr.get64(15);
        } else {
            sample.bp = live->gpr.get32(5);
            sample.eax = live->gpr.get32(0);
            sample.ecx = live->gpr.get32(1);
            sample.edx = live->gpr.get32(2);
            sample.ebx = live->gpr.get32(3);
            sample.esi = live->gpr.get32(6);
            sample.edi = live->gpr.get32(7);
        }
        sample.parked = slot->parked;
        sample.in_host_call = slot->blocked_in_host_call();
        sample.suspend_depth = slot->suspend_depth;
        sample.host_call_depth = slot->host_call_depth.load();
        sample.guest_override_depth = slot->guest_override_depth.load();
        sample.guest_base_depth = slot->guest_base_depth.load();
        samples.push_back(sample);
    }
    return samples;
}

}  // namespace vitreo::winapi
