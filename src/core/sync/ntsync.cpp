/// @file ntsync.cpp
/// @brief Vitreo NT synchronization object manager — implementation.
///
/// See ntsync.hpp / .docs/nt-synchronization.md for design rationale.

#include "core/sync/ntsync.hpp"

#include <algorithm>

namespace vitreo::ntsync {

namespace {
// Contention on ntsync's ONE global lock, and the size of the herd each signal
// wakes. Every sync object still shares `m_lock`, but each waiter parks on its
// OWN condition variable, registered against the objects it waits on, and a
// signal satisfies waits on the waiters' behalf and wakes only the threads it
// released. Before that, notify_locked() woke every waiter registered on the
// object: each woke, took the single global mutex, re-scanned its handles and
// all but one went back to sleep. Set VITREO_NTSYNC_NOTIFY_ALL=1 to add the
// broadcast back and A/B the difference; these counters are how you see it.
struct NtsyncStats {
    bool on = std::getenv("VITREO_NTSYNC_STATS") != nullptr;
    std::atomic<std::uint64_t> acquires{0}, contended{0}, blocked_ns{0};
    std::atomic<std::uint64_t> notifies{0}, waits{0};
    std::atomic<int> waiters_now{0};
    std::atomic<int> waiters_max{0};
    void note_acquire(std::uint64_t blocked, bool was_contended) {
        const auto n = acquires.fetch_add(1) + 1;
        if (was_contended) {
            contended.fetch_add(1);
            blocked_ns.fetch_add(blocked);
        }
        if ((n % 2000) != 0) return;
        std::fprintf(stderr,
            "[ntsync] acquires=%llu contended=%llu (%.1f%%) avg_blocked=%.3fms "
            "waits=%llu notifies=%llu max_concurrent_waiters=%d\n",
            (unsigned long long)n, (unsigned long long)contended.load(),
            100.0 * static_cast<double>(contended.load()) /
                static_cast<double>(n),
            contended.load() ? static_cast<double>(blocked_ns.load()) /
                                   static_cast<double>(contended.load()) / 1e6
                             : 0.0,
            (unsigned long long)waits.load(),
            (unsigned long long)notifies.load(), waiters_max.load());
    }
    void enter_wait() {
        waits.fetch_add(1);
        const int now = waiters_now.fetch_add(1) + 1;
        int prev = waiters_max.load();
        while (now > prev && !waiters_max.compare_exchange_weak(prev, now)) {}
    }
    void leave_wait() { waiters_now.fetch_sub(1); }
};
NtsyncStats g_ntsync_stats;
} // namespace


// =============================================================================
// Singleton
// =============================================================================

Manager& global() {
    // Intentionally leaked: see ntsync.hpp comment on global().
    static Manager* s_instance = new Manager();
    return *s_instance;
}

// =============================================================================
// Object creation / destruction
// =============================================================================

ObjectId Manager::allocate_locked(std::unique_ptr<Object> obj) {
    const ObjectId id = m_next_id++;
    if (id >= m_objects.size()) {
        m_objects.resize(static_cast<std::size_t>(id) + 1);
    }
    if (id >= m_object_waiters.size()) {
        m_object_waiters.resize(static_cast<std::size_t>(id) + 1);
    }
    m_objects[static_cast<std::size_t>(id)] = std::move(obj);
    return id;
}

ObjectId Manager::create_event(bool manual_reset, bool initial_state) {
    std::lock_guard<std::mutex> lk(m_lock);
    auto o = std::make_unique<Object>();
    o->type           = ObjType::Event;
    o->manual_reset   = manual_reset;
    o->signaled       = initial_state;
    return allocate_locked(std::move(o));
}

ObjectId Manager::create_mutex(bool initial_owner, std::uint32_t tid) {
    std::lock_guard<std::mutex> lk(m_lock);
    auto o = std::make_unique<Object>();
    o->type      = ObjType::Mutex;
    o->owner_tid = initial_owner ? tid : 0;
    o->recursion = initial_owner ? 1 : 0;
    return allocate_locked(std::move(o));
}

ObjectId Manager::create_semaphore(std::int32_t initial, std::int32_t maximum) {
    if (maximum < 1) maximum = 1;
    if (initial < 0) initial = 0;
    if (initial > maximum) initial = maximum;
    std::lock_guard<std::mutex> lk(m_lock);
    auto o = std::make_unique<Object>();
    o->type      = ObjType::Semaphore;
    o->sem_count = initial;
    o->sem_max   = maximum;
    return allocate_locked(std::move(o));
}

ObjectId Manager::create_timer(bool manual_reset) {
    std::lock_guard<std::mutex> lk(m_lock);
    auto o = std::make_unique<Object>();
    o->type         = ObjType::Timer;
    o->manual_reset = manual_reset;
    o->signaled     = false;
    return allocate_locked(std::move(o));
}

void Manager::destroy(ObjectId id) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (id != kInvalidId && id < m_objects.size()) {
        m_objects[static_cast<std::size_t>(id)].reset();
    }
    // Wake this object's waiters so they observe the disappearance. The
    // registration list is deliberately NOT cleared: the waiters own those
    // entries and remove them themselves on the way out.
    notify_locked(id);
}

bool Manager::exists(ObjectId id) const {
    std::lock_guard<std::mutex> lk(m_lock);
    return find_locked(id) != nullptr;
}

ObjType Manager::type_of(ObjectId id) const {
    std::lock_guard<std::mutex> lk(m_lock);
    const Object* o = find_locked(id);
    return o ? o->type : ObjType::Event;
}

std::optional<Object> Manager::snapshot(ObjectId id) const {
    std::lock_guard<std::mutex> lk(m_lock);
    const Object* o = find_locked(id);
    if (!o) return std::nullopt;
    return *o;
}

Object* Manager::find_locked(ObjectId id) {
    if (id == kInvalidId || id >= m_objects.size()) return nullptr;
    return m_objects[static_cast<std::size_t>(id)].get();
}

const Object* Manager::find_locked(ObjectId id) const {
    if (id == kInvalidId || id >= m_objects.size()) return nullptr;
    return m_objects[static_cast<std::size_t>(id)].get();
}

void Manager::set_host_wake(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(m_lock);
    m_host_wake = std::move(fn);
}

WakeCounters Manager::wake_counters() const {
    WakeCounters c;
    c.waits          = m_c_waits.load(std::memory_order_relaxed);
    c.wakes          = m_c_wakes.load(std::memory_order_relaxed);
    c.grants         = m_c_grants.load(std::memory_order_relaxed);
    c.spurious_wakes = m_c_spurious.load(std::memory_order_relaxed);
    c.notifies       = m_c_notifies.load(std::memory_order_relaxed);
    return c;
}

/// VITREO_NTSYNC_NOTIFY_ALL=1 adds the old broadcast-to-everyone wake on top
/// of the handoff, so the targeted wake can be A/B'd on a live guest in one
/// run. Object state is still handed off first: the knob changes who is
/// woken, never who is released.
[[nodiscard]] static bool notify_all_mode() noexcept {
    static const bool on = std::getenv("VITREO_NTSYNC_NOTIFY_ALL") != nullptr;
    return on;
}

// -----------------------------------------------------------------------------
// Release: who a signal satisfies.
//
// Learn (SetEvent, PulseEvent, ReleaseSemaphore, ReleaseMutex,
// SetWaitableTimer, WaitForMultipleObjects):
//  * auto-reset event: "remains signaled until a single waiting thread is
//    released, at which time the system automatically sets the state to
//    nonsignaled"; manual-reset: "any number of waiting threads ... can be
//    released while the object's state is signaled". Synchronization and
//    manual-reset timers follow the same two rules.
//  * semaphore: "each time a waiting thread is released because of the
//    semaphore's signaled state, the count of the semaphore is decreased by
//    one" -- so ReleaseSemaphore(n) releases up to n waiters.
//  * mutex: ownership passes to one thread.
//  * PulseEvent: "releases only those threads that are waiting at the moment
//    it is called"; manual-reset "all waiting threads that can be released
//    immediately", auto-reset "a single waiting thread, even if multiple
//    threads are waiting"; a wait-all waiter whose other objects are not all
//    signaled is NOT released; the event is left nonsignaled either way.
//  * wait-all: "does not modify the states of the specified objects until the
//    states of all objects have been set to signaled".
//
// The order in which waiters are released is not documented by Learn. This
// implementation releases in registration order (FIFO): it is what the NT
// kernel's per-object wait-block list yields in practice, it is what a
// convoy of thread-pool workers expects (the longest-parked worker runs), and
// it makes the wake count testable.
//
// The release is a direct handoff performed by the SIGNALLER while it holds
// m_lock: it consumes the object on the waiter's behalf, records the result
// in the Waiter, and wakes that one thread. A woken thread therefore never
// finds "nothing to consume" -- the race that made every signal wake N
// threads so that N-1 could re-park -- and a waiter that times out cannot
// swallow a release: either the grant landed before it re-took m_lock (it
// returns the grant) or it had not landed (it unregisters and the next
// signal goes to the next waiter).
// -----------------------------------------------------------------------------

std::optional<std::uint32_t> Manager::satisfy_any_locked(const ObjectId* ids,
                                                         std::uint32_t count,
                                                         std::uint32_t tid,
                                                         ObjectId pulsed) {
    // Learn WaitForMultipleObjects: "checks the handles in the array in order
    // starting with index 0 ... returns the index of the first handle in the
    // array whose object was signaled".
    for (std::uint32_t i = 0; i < count; ++i) {
        Object* o = find_locked(ids[i]);
        if (o == nullptr) return std::nullopt;
        if (ids[i] == pulsed && pulsed != kInvalidId) {
            // A pulse in flight: released without consuming (the event is
            // nonsignaled by the time anyone looks).
            return WAIT_OBJECT_0 + i;
        }
        if (can_satisfy_locked(*o, tid)) {
            return consume_locked(*o, tid) + i;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Manager::satisfy_all_locked(const ObjectId* ids,
                                                         std::uint32_t count,
                                                         std::uint32_t tid,
                                                         ObjectId pulsed,
                                                         bool commit) {
    // All-or-nothing: simulate consumption on shadow copies so duplicate
    // handles (e.g. same semaphore listed N times) are correctly accounted
    // for, and nothing is touched unless every object is satisfiable.
    m_scan.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        m_scan[i] = find_locked(ids[i]);
        if (m_scan[i] == nullptr) return std::nullopt;
    }
    m_shadow.clear();
    for (std::uint32_t i = 0; i < count; ++i) m_shadow.push_back(*m_scan[i]);
    for (std::uint32_t i = 0; i < count; ++i) {
        const bool is_pulsed = (pulsed != kInvalidId && ids[i] == pulsed);
        if (is_pulsed) continue;
        if (!can_satisfy_locked(m_shadow[i], tid)) return std::nullopt;
        (void)consume_locked(m_shadow[i], tid);
        for (std::uint32_t j = i + 1; j < count; ++j) {
            if (ids[j] == ids[i]) m_shadow[j] = m_shadow[i];
        }
    }
    if (!commit) return WAIT_OBJECT_0;
    // Commit for real. Learn wait-all: if any mutex was abandoned, return
    // WAIT_ABANDONED_0 + the lowest such index (ownership is still granted
    // for every object).
    bool abandoned = false;
    std::uint32_t abandoned_index = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pulsed != kInvalidId && ids[i] == pulsed) continue;
        const std::uint32_t code = consume_locked(*m_scan[i], tid);
        if (code == WAIT_ABANDONED_0 && !abandoned) {
            abandoned = true;
            abandoned_index = i;
        }
    }
    return abandoned ? (WAIT_ABANDONED_0 + abandoned_index) : WAIT_OBJECT_0;
}

bool Manager::try_grant_locked(Waiter& w, ObjectId pulsed) {
    if (w.granted) return false;
    // The latency of a handoff is measured when the released thread runs,
    // not here (consume_locked would otherwise record ~0 at grant time).
    m_granting = true;
    std::optional<std::uint32_t> r =
        w.wait_all ? satisfy_all_locked(w.ids, w.count, w.tid, pulsed, true)
                   : satisfy_any_locked(w.ids, w.count, w.tid, pulsed);
    m_granting = false;
    if (!r) return false;
    w.granted = true;
    w.result  = *r;
    m_c_grants.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Manager::notify_locked(ObjectId id, ObjectId pulsed) {
    if (g_ntsync_stats.on) g_ntsync_stats.notifies.fetch_add(1);
    m_c_notifies.fetch_add(1, std::memory_order_relaxed);
    // Caller holds m_lock, so a waiter cannot slip between its own state scan
    // and parking on its cv — there is no lost-wakeup window. Each Waiter's cv
    // has exactly one thread on it, so notify_one is the whole wake.
    if (id != kInvalidId && id < m_object_waiters.size()) {
        Object* o = find_locked(id);
        auto& list = m_object_waiters[static_cast<std::size_t>(id)];
        if (o == nullptr) {
            // Destroyed under the waiters: they rescan and report WAIT_FAILED.
            for (Waiter* w : list) w->cv.notify_one();
        } else {
            const bool is_pulse = (pulsed == id);
            // An auto-reset pulse releases exactly one thread; nothing else
            // needs an explicit budget, because consuming on a waiter's
            // behalf (auto-reset -> clear, semaphore -> count-1, mutex ->
            // owned) is what makes the next waiter ineligible.
            const bool one_shot = is_pulse && !o->manual_reset;
            // try_grant_locked never edits the list (waiters unregister
            // themselves on the way out), so iterating by index is exact.
            for (std::size_t k = 0; k < list.size(); ++k) {
                Waiter* w = list[k];
                if (w->granted) continue;
                if (w->notification_only) {
                    w->cv.notify_one();
                    continue;
                }
                if (!is_pulse && !can_satisfy_locked(*o, w->tid)) break;
                const auto released_at = o->signaled_at;
                if (try_grant_locked(*w, pulsed)) {
                    w->released_at = released_at;
                    w->cv.notify_one();
                    if (one_shot) break;
                }
            }
        }
    }
    if (notify_all_mode()) {
        notify_all_locked();
        return;
    }
    // Targeted: only a signal on an object the parked host thread armed.
    if (m_host_wake && !m_host_armed.empty()) {
        for (const HostArm& arm : m_host_armed) {
            for (const ObjectId armed : arm.ids) {
                if (armed == id) {
                    m_host_wake();
                    return;
                }
            }
        }
    }
}

void Manager::arm_host_wake(std::uint32_t tid, const ObjectId* ids,
                            std::uint32_t count) {
    std::lock_guard<std::mutex> lk(m_lock);
    for (HostArm& arm : m_host_armed) {
        if (arm.tid == tid) {
            arm.ids.assign(ids, ids + count);
            return;
        }
    }
    m_host_armed.push_back(HostArm{tid, std::vector<ObjectId>(ids, ids + count)});
}

void Manager::disarm_host_wake(std::uint32_t tid) {
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto it = m_host_armed.begin(); it != m_host_armed.end(); ++it) {
        if (it->tid == tid) {
            m_host_armed.erase(it);
            return;
        }
    }
}

void Manager::notify_all_locked() {
    for (Waiter* w : m_all_waiters) {
        w->cv.notify_one();
    }
    if (m_host_wake) {
        m_host_wake();
    }
}

void Manager::register_waiter_locked(Waiter& w, const ObjectId* ids,
                                     std::uint32_t count) {
    m_all_waiters.push_back(&w);
    for (std::uint32_t i = 0; i < count; ++i) {
        const ObjectId id = ids[i];
        if (id == kInvalidId) continue;
        // A wait may name the same handle twice; one registration is enough
        // and keeps unregistration exact.
        bool duplicate = false;
        for (std::uint32_t j = 0; j < i; ++j) {
            if (ids[j] == id) { duplicate = true; break; }
        }
        if (duplicate) continue;
        if (id >= m_object_waiters.size()) {
            m_object_waiters.resize(static_cast<std::size_t>(id) + 1);
        }
        m_object_waiters[static_cast<std::size_t>(id)].push_back(&w);
    }
}

void Manager::unregister_waiter_locked(Waiter& w, const ObjectId* ids,
                                       std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const ObjectId id = ids[i];
        if (id == kInvalidId || id >= m_object_waiters.size()) continue;
        auto& list = m_object_waiters[static_cast<std::size_t>(id)];
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (*it == &w) { list.erase(it); break; }
        }
    }
    for (auto it = m_all_waiters.begin(); it != m_all_waiters.end(); ++it) {
        if (*it == &w) { m_all_waiters.erase(it); break; }
    }
}

void Manager::notify_waiters() {
    // Taking the same lock used by wait() prevents a lost wake between the
    // cancellation check and condition-variable parking. This one genuinely
    // must reach everyone: the predicate it asks waiters to re-check is
    // external to every object.
    std::lock_guard<std::mutex> lk(m_lock);
    notify_all_locked();
}

// =============================================================================
// Signal / release
// =============================================================================

bool Manager::set_event(ObjectId id, bool* previous) {
    std::lock_guard<std::mutex> lk(m_lock);
    Object* o = find_locked(id);
    if (!o || (o->type != ObjType::Event && o->type != ObjType::Timer)) return false;
    if (previous) *previous = o->signaled;
    o->signaled = true;
    o->signaled_at = std::chrono::steady_clock::now();
    notify_locked(id);
    return true;
}

bool Manager::reset_event(ObjectId id, bool* previous) {
    std::lock_guard<std::mutex> lk(m_lock);
    Object* o = find_locked(id);
    if (!o || (o->type != ObjType::Event && o->type != ObjType::Timer)) return false;
    if (previous) *previous = o->signaled;
    o->signaled = false;
    return true;
}

bool Manager::pulse_event(ObjectId id, bool* previous) {
    std::lock_guard<std::mutex> lk(m_lock);
    Object* o = find_locked(id);
    if (!o || o->type != ObjType::Event) return false;
    if (previous) *previous = o->signaled;
    // Learn PulseEvent: release the threads waiting at this instant (one for
    // auto-reset, every immediately-releasable one for manual-reset), then
    // leave the event nonsignaled. The handoff releases them directly, so
    // the event's own state never has to be signaled for them to see it;
    // toggling `signaled` around a wake instead released nobody, because the
    // flag was already false again before any waiter could re-test it.
    o->signaled_at = std::chrono::steady_clock::now();
    notify_locked(id, /*pulsed=*/id);
    o->signaled = false;
    return true;
}

bool Manager::release_mutex(ObjectId id, std::uint32_t tid) {
    std::lock_guard<std::mutex> lk(m_lock);
    Object* o = find_locked(id);
    if (!o || o->type != ObjType::Mutex) return false;
    if (o->owner_tid != tid || o->recursion == 0) return false;
    if (--o->recursion == 0) {
        o->owner_tid = 0;
        o->signaled_at = std::chrono::steady_clock::now();
        notify_locked(id);
    }
    return true;
}

bool Manager::release_semaphore(ObjectId id, std::int32_t release_count,
                                std::int32_t* previous_out) {
    if (release_count <= 0) return false;
    std::lock_guard<std::mutex> lk(m_lock);
    Object* o = find_locked(id);
    if (!o || o->type != ObjType::Semaphore) return false;
    if (o->sem_count + release_count > o->sem_max) return false;
    if (previous_out) *previous_out = o->sem_count;
    o->sem_count += release_count;
    o->signaled_at = std::chrono::steady_clock::now();
    notify_locked(id);
    return true;
}

void Manager::signal_timer(ObjectId id) {
    (void)set_event(id);
}

void Manager::cancel_timer(ObjectId id) {
    (void)reset_event(id);
}

void Manager::interrupt_thread(std::uint32_t tid) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (std::find(m_interrupted_tids.begin(), m_interrupted_tids.end(), tid) ==
        m_interrupted_tids.end()) {
        m_interrupted_tids.push_back(tid);
    }
    for (Waiter* w : m_all_waiters) w->cv.notify_all();
}

void Manager::abandon_for_thread(std::uint32_t tid) {
    std::lock_guard<std::mutex> lk(m_lock);
    for (std::size_t index = 0; index < m_objects.size(); ++index) {
        auto& slot = m_objects[index];
        if (!slot) continue;
        Object& o = *slot;
        if (o.type == ObjType::Mutex && o.owner_tid == tid && o.recursion > 0) {
            o.owner_tid = 0;
            o.signaled_at = std::chrono::steady_clock::now();
            o.recursion = 0;
            o.abandoned = true;
            // Wake per abandoned mutex: the id IS the slot index.
            notify_locked(static_cast<ObjectId>(index));
        }
    }
}

// =============================================================================
// Wait — atomic scan + commit
// =============================================================================

bool Manager::can_satisfy_locked(const Object& o, std::uint32_t tid) const {
    switch (o.type) {
        case ObjType::Event:
        case ObjType::Timer:
            return o.signaled;
        case ObjType::Mutex:
            return o.abandoned ||
                   o.recursion == 0 ||
                   o.owner_tid == tid;  // recursive entry
        case ObjType::Semaphore:
            return o.sem_count > 0;
    }
    return false;
}

namespace {
// Signal-to-wake latency, gated on VITREO_WAKE_LATENCY. Reported as a running
// aggregate rather than a histogram so it is readable in a normal run log.
struct WakeLatency {
    std::atomic<bool> on{std::getenv("VITREO_WAKE_LATENCY") != nullptr};
    std::atomic<std::uint64_t> n{0}, total_us{0}, max_us{0}, over_1ms{0}, over_10ms{0};
    void add(std::uint64_t us) {
        const auto c = n.fetch_add(1) + 1;
        total_us.fetch_add(us);
        if (us > 1000) over_1ms.fetch_add(1);
        if (us > 10000) over_10ms.fetch_add(1);
        auto m = max_us.load();
        while (us > m && !max_us.compare_exchange_weak(m, us)) {}
        if ((c % 2000) == 0) {
            std::fprintf(stderr,
                "[wake-latency] n=%llu avg=%.3fms max=%.3fms >1ms=%llu (%.1f%%) "
                ">10ms=%llu (%.1f%%)\n",
                (unsigned long long)c,
                total_us.load() / 1000.0 / c,
                max_us.load() / 1000.0,
                (unsigned long long)over_1ms.load(),
                100.0 * over_1ms.load() / c,
                (unsigned long long)over_10ms.load(),
                100.0 * over_10ms.load() / c);
        }
    }
    void add_since(std::chrono::steady_clock::time_point at) {
        if (!on.load(std::memory_order_relaxed) ||
            at.time_since_epoch().count() == 0) {
            return;
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - at).count();
        if (us >= 0) add(static_cast<std::uint64_t>(us));
    }
};
WakeLatency g_wake;
} // namespace

std::uint32_t Manager::consume_locked(Object& o, std::uint32_t tid) {
    // Self-consume (the object was already satisfiable when the thread
    // looked): the gap is measured here. A handoff measures it when the
    // released thread actually runs, from Waiter::released_at.
    if (!m_granting) g_wake.add_since(o.signaled_at);
    o.signaled_at = {};
    switch (o.type) {
        case ObjType::Event:
        case ObjType::Timer:
            if (!o.manual_reset) o.signaled = false;
            return WAIT_OBJECT_0;
        case ObjType::Mutex: {
            bool was_abandoned = o.abandoned;
            o.abandoned = false;
            if (o.owner_tid == tid && o.recursion > 0) {
                ++o.recursion;
            } else {
                o.owner_tid = tid;
                o.recursion = 1;
            }
            return was_abandoned ? WAIT_ABANDONED_0 : WAIT_OBJECT_0;
        }
        case ObjType::Semaphore:
            --o.sem_count;
            return WAIT_OBJECT_0;
    }
    return WAIT_OBJECT_0;
}

bool Manager::probe(ObjectId id, std::uint32_t tid) const {
    std::lock_guard<std::mutex> lk(m_lock);
    const Object* o = find_locked(id);
    if (!o) return false;
    return can_satisfy_locked(*o, tid);
}

std::uint32_t Manager::wait_for_notification(
    const ObjectId* ids, std::uint32_t count, std::uint32_t timeout_ms,
    std::uint32_t tid, const std::atomic<bool>* cancel_requested,
    bool return_if_ready) {
    if (ids == nullptr || count == 0) return WAIT_FAILED;
    std::unique_lock<std::mutex> lock(m_lock);
    auto cancelled = [&] {
        return (cancel_requested &&
                cancel_requested->load(std::memory_order_acquire)) ||
            std::find(m_interrupted_tids.begin(), m_interrupted_tids.end(),
                      tid) != m_interrupted_tids.end();
    };
    if (cancelled()) return WAIT_FAILED;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!find_locked(ids[i])) return WAIT_FAILED;
    }
    if (return_if_ready) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (can_satisfy_locked(*find_locked(ids[i]), tid)) return WAIT_OBJECT_0;
        }
    }
    if (timeout_ms == 0) return WAIT_TIMEOUT;

    Waiter waiter;
    waiter.ids = ids;
    waiter.count = count;
    waiter.tid = tid;
    waiter.notification_only = true;
    struct Registration {
        Manager& mgr;
        Waiter& waiter;
        ~Registration() {
            mgr.unregister_waiter_locked(waiter, waiter.ids, waiter.count);
        }
    } registration{*this, waiter};
    register_waiter_locked(waiter, ids, count);
    // Scan, registration and parking share the signaller's lock. Signals
    // cannot slip between the readiness check and condition-variable wait.
    m_c_waits.fetch_add(1, std::memory_order_relaxed);
    bool timed_out = false;
    if (timeout_ms == INFINITE_TIMEOUT) {
        waiter.cv.wait(lock);
    } else {
        timed_out = waiter.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms))
            == std::cv_status::timeout;
    }
    m_c_wakes.fetch_add(1, std::memory_order_relaxed);
    if (cancelled()) return WAIT_FAILED;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!find_locked(ids[i])) return WAIT_FAILED;
    }
    return timed_out ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}

std::uint32_t Manager::wait(const ObjectId* ids,
                            std::uint32_t   count,
                            bool            wait_all,
                            std::uint32_t   timeout_ms,
                            std::uint32_t   tid,
                            const std::atomic<bool>* cancel_requested,
                            std::uint64_t* blocked_ns) {
    if (count == 0 || ids == nullptr) return WAIT_FAILED;

    const bool infinite = (timeout_ms == INFINITE_TIMEOUT);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(infinite ? 0 : timeout_ms);

    std::unique_lock<std::mutex> lk(m_lock, std::defer_lock);
    if (g_ntsync_stats.on) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool got = lk.try_lock();
        if (!got) lk.lock();
        g_ntsync_stats.note_acquire(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count()),
            !got);
    } else {
        lk.lock();
    }

    // Park on THIS thread's own condition variable, registered against the
    // objects it is waiting on, so a signal wakes only the threads that could
    // possibly be released by it. Registration and removal both happen under
    // m_lock; the guard is declared after `lk` so it unregisters while the
    // lock is still held, on every one of this function's many return paths.
    Waiter waiter;
    waiter.ids      = ids;
    waiter.count    = count;
    waiter.wait_all = wait_all;
    waiter.tid      = tid;
    struct Registration {
        Manager& mgr;
        Waiter& w;
        const ObjectId* ids;
        std::uint32_t count;
        ~Registration() { mgr.unregister_waiter_locked(w, ids, count); }
    } registration{*this, waiter, ids, count};
    register_waiter_locked(waiter, ids, count);

    bool parked = false;
    while (true) {
        // A signaller satisfied this wait on our behalf while we were parked:
        // the object state is already consumed, so this is the only answer
        // that does not lose a release -- it outranks a deadline that passed
        // in the meantime and even a cancellation that landed after it.
        if (waiter.granted) {
            g_wake.add_since(waiter.released_at);
            return waiter.result;
        }
        // Process teardown is a host-lifetime boundary, not a synthetic signal
        // on any guest object. Return through the callback loop without
        // consuming event/semaphore/mutex state.
        if (cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_acquire)) {
            return WAIT_FAILED;
        }
        if (!m_interrupted_tids.empty() &&
            std::find(m_interrupted_tids.begin(), m_interrupted_tids.end(),
                      tid) != m_interrupted_tids.end()) {
            return WAIT_FAILED;
        }

        // ---- Scan: validate all handles + decide whether wait can succeed.
        for (std::uint32_t i = 0; i < count; ++i) {
            if (find_locked(ids[i]) == nullptr) return WAIT_FAILED;
        }
        // Self-consume. This thread is still registered until the
        // Registration guard runs, so it is marked finished BEFORE the
        // post-consume notify below: otherwise notify_locked() would find
        // its own Waiter in the list and hand it a second unit (a semaphore
        // count stolen, a mutex recursion inflated).
        std::optional<std::uint32_t> self;
        if (wait_all) {
            self = satisfy_all_locked(ids, count, tid, kInvalidId,
                                      /*commit=*/true);
        } else {
            // Wait-any: take the first satisfiable object.
            self = satisfy_any_locked(ids, count, tid, kInvalidId);
        }
        if (self) {
            waiter.granted = true;
            waiter.result  = *self;
            // Consumption only ever reduces what an object can satisfy, so
            // this releases nobody under the handoff; it is kept for the
            // VITREO_NTSYNC_NOTIFY_ALL A/B arm, where it is the old broadcast.
            if (wait_all) {
                for (std::uint32_t i = 0; i < count; ++i) notify_locked(ids[i]);
            } else {
                notify_locked(ids[(*self >= WAIT_ABANDONED_0)
                                      ? (*self - WAIT_ABANDONED_0)
                                      : (*self - WAIT_OBJECT_0)]);
            }
            return *self;
        }

        // ---- Not satisfied — block or report timeout.
        if (timeout_ms == 0) return WAIT_TIMEOUT;
        if (!infinite && std::chrono::steady_clock::now() >= deadline) {
            return WAIT_TIMEOUT;
        }
        if (parked) {
            // Woken, nothing granted, nothing to consume, nothing ended the
            // wait: the herd case the handoff exists to remove.
            m_c_spurious.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_c_waits.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_ntsync_stats.on) g_ntsync_stats.enter_wait();
        const auto blocked_at = blocked_ns != nullptr
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if (infinite) {
            waiter.cv.wait(lk);
        } else {
            waiter.cv.wait_until(lk, deadline);
        }
        parked = true;
        m_c_wakes.fetch_add(1, std::memory_order_relaxed);
        if (blocked_ns != nullptr) {
            *blocked_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - blocked_at).count());
        }
        if (g_ntsync_stats.on) g_ntsync_stats.leave_wait();
        // Loop: return the grant, or re-scan after an unrelated wake.
    }
}

} // namespace vitreo::ntsync
