#pragma once

/// @file ntsync.hpp
/// @brief Vitreo NT synchronization object manager.
///
/// Implements the architecture described in .docs/nt-synchronization.md:
///  * Centralized object manager for Event / Mutex / Semaphore / Timer
///  * Single global lock for atomic wait resolution, with PER-WAITER condition
///    variables registered on the objects each thread is actually waiting on
///  * Direct handoff: the signaller satisfies waits on the waiters' behalf
///    under the global lock and wakes only the threads it released
///  * Atomic WaitForMultipleObjects (wait-any AND wait-all) with no partial
///    state consumption
///  * Auto-reset event "consume on wake" is performed under the global lock
///  * Recursive Windows-style mutex with abandoned-mutex support
///  * Counting semaphore with bounded maximum
///
/// Host primitives (std::mutex / std::condition_variable) are *internal
/// implementation helpers only* — they are NOT exposed as Windows objects.

#include <vitreo/common.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace vitreo::ntsync {

enum class ObjType : std::uint8_t {
    Event,
    Mutex,
    Semaphore,
    Timer,
};

using ObjectId = std::uint64_t;
constexpr ObjectId kInvalidId = 0;

// Win32-compatible wait return values.
constexpr std::uint32_t WAIT_OBJECT_0    = 0x00000000u;
constexpr std::uint32_t WAIT_ABANDONED_0 = 0x00000080u;
constexpr std::uint32_t WAIT_TIMEOUT     = 0x00000102u;
constexpr std::uint32_t WAIT_FAILED      = 0xFFFFFFFFu;
constexpr std::uint32_t INFINITE_TIMEOUT = 0xFFFFFFFFu;

struct Object {
    ObjType type;
    // -- Event / Timer ------------------------------------------------------
    bool manual_reset = false;
    bool signaled     = false;
    // -- Mutex --------------------------------------------------------------
    std::uint32_t owner_tid = 0;
    std::uint32_t recursion = 0;
    bool abandoned          = false;
    // -- Semaphore ----------------------------------------------------------
    std::int32_t sem_count = 0;
    std::int32_t sem_max   = 0;
    // When this object last became satisfiable. Measuring the gap between that
    // instant and the moment a waiter actually consumes it separates "the
    // producer was slow" from "Vitreo woke the waiter slowly" -- the two look
    // identical from the guest's side, and only one of them is ours to fix.
    std::chrono::steady_clock::time_point signaled_at{};
};

/// Always-on wake accounting (relaxed atomics, no env gate). `spurious_wakes`
/// counts the times a parked thread was woken, found nothing to consume and
/// no reason to return, and parked again; with direct handoff that is zero
/// for every signal, and nonzero only under VITREO_NTSYNC_NOTIFY_ALL.
struct WakeCounters {
    std::uint64_t waits = 0;          ///< Manager::wait() calls that parked.
    std::uint64_t wakes = 0;          ///< cv wakeups (any cause).
    std::uint64_t grants = 0;         ///< waits satisfied by a signaller.
    std::uint64_t spurious_wakes = 0; ///< wakeups that re-parked unsatisfied.
    std::uint64_t notifies = 0;       ///< notify_locked() calls.
};

class Manager {
public:
    Manager() = default;
    Manager(const Manager&)            = delete;
    Manager& operator=(const Manager&) = delete;

    // -- Object creation ----------------------------------------------------
    ObjectId create_event(bool manual_reset, bool initial_state);
    ObjectId create_mutex(bool initial_owner, std::uint32_t owner_tid);
    ObjectId create_semaphore(std::int32_t initial_count,
                              std::int32_t maximum_count);
    ObjectId create_timer(bool manual_reset);

    void destroy(ObjectId id);
    bool   exists(ObjectId id) const;
    ObjType type_of(ObjectId id) const;
    /// Return a consistent copy of an object's current observable state.
    /// Used when a process-local object must be promoted to the cross-process
    /// backend for DuplicateHandle.
    [[nodiscard]] std::optional<Object> snapshot(ObjectId id) const;

    // -- Signal / release ---------------------------------------------------
    /// @p previous (optional) receives the state before the change, taken
    /// under the same lock (NtSetEvent / NtResetEvent / NtPulseEvent
    /// PreviousState).
    bool set_event(ObjectId id, bool* previous = nullptr);
    bool reset_event(ObjectId id, bool* previous = nullptr);
    bool pulse_event(ObjectId id, bool* previous = nullptr);
    bool release_mutex(ObjectId id, std::uint32_t tid);
    bool release_semaphore(ObjectId id, std::int32_t release_count,
                           std::int32_t* previous_out);
    void signal_timer(ObjectId id);
    void cancel_timer(ObjectId id);

    // Called from thread teardown — marks any mutex owned by `tid` as abandoned.
    void abandon_for_thread(std::uint32_t tid);

    /// TerminateThread: every current and future wait by `tid` returns
    /// WAIT_FAILED without consuming object state. The thread will never run
    /// guest code again, so nothing may keep its host thread parked.
    void interrupt_thread(std::uint32_t tid);

    // -- Wait ---------------------------------------------------------------
    /// Atomic wait on 1..N objects.
    ///   wait_all=false → wait-any; returns WAIT_OBJECT_0+i / WAIT_ABANDONED_0+i.
    ///   wait_all=true  → wait-all; returns WAIT_OBJECT_0 (always 0).
    /// `timeout_ms == INFINITE_TIMEOUT` waits forever.
    /// Returns WAIT_TIMEOUT, WAIT_FAILED, or success codes.
    std::uint32_t wait(const ObjectId* ids,
                       std::uint32_t   count,
                       bool            wait_all,
                       std::uint32_t   timeout_ms,
                       std::uint32_t   current_tid,
                       const std::atomic<bool>* cancel_requested = nullptr,
                       std::uint64_t* blocked_ns = nullptr);

    /// Wake blocked waiters so they can re-check an external cancellation
    /// predicate. This does not alter any Windows-visible object state.
    void notify_waiters();

    /// Park until a listed local object is ready, changes, or timeout/cancel.
    /// This is only a wake hint for a wait spanning another object backend:
    /// never consume an event/semaphore or acquire a mutex here. The caller
    /// must resolve the complete Windows wait after waking.
    std::uint32_t wait_for_notification(
        const ObjectId* ids, std::uint32_t count, std::uint32_t timeout_ms,
        std::uint32_t tid, const std::atomic<bool>* cancel_requested,
        bool return_if_ready);

    /// Optional host callback fired on every waiter wake (SetEvent, release,
    /// …). Used so the AppKit thread can leave CFRunLoop when a guest event
    /// signals instead of remaining in pthread_cond_wait.
    void set_host_wake(std::function<void()> fn);
    /// The host (AppKit) thread cannot park on a Waiter cv; it parks in its
    /// run loop. Before it does, it arms the objects it is waiting on here so
    /// that only a signal on one of them fires the host wake. An unconditional
    /// host wake on every notify wakes the AppKit thread at the process-wide
    /// signal rate (launcher login 25.7 s -> 37 s, CPU-s 55 -> 90).
    void arm_host_wake(std::uint32_t tid, const ObjectId* ids,
                       std::uint32_t count);
    void disarm_host_wake(std::uint32_t tid);

    /// Non-consuming probe — returns true if the object would currently
    /// satisfy a wait for `tid`.  Used for status queries.
    bool probe(ObjectId id, std::uint32_t tid) const;

    /// Snapshot of the always-on wake counters.
    [[nodiscard]] WakeCounters wake_counters() const;

    /// One parked thread. Lives on that thread's stack for the duration of
    /// Manager::wait() and is registered, under m_lock, against every object
    /// it is waiting on.
    ///
    /// A single condition variable for the whole manager broadcasts every
    /// SetEvent to every waiter in the process. Each wakes, takes the global
    /// lock, re-scans its own handles, and all but one go back to sleep. On a
    /// browser renderer with ~39 guest threads, `ntsync::Manager::wait` is
    /// simultaneously the largest block of parked time (65.6%) and the largest
    /// source of blocked mutex time. A signal must wake only the threads
    /// waiting on that object.
    ///
    /// Per-object registration is the first half. The second: a signal on an
    /// object with N waiters must not wake all N. Each of them would re-take
    /// m_lock to rescan, and N-1 would go back to sleep. Waking all N on an
    /// idle browser process (110 threads) blocks 17 threads on m_lock at once,
    /// 4.2 on average. The signaller satisfies the wait on the waiter's behalf
    /// while it holds m_lock: it consumes the object, stores the result here,
    /// and wakes only the threads it released. That is what the NT kernel's
    /// wait test does.
    struct Waiter {
        std::condition_variable cv;
        const ObjectId* ids = nullptr;
        std::uint32_t   count = 0;
        bool            wait_all = false;
        bool            notification_only = false;
        std::uint32_t   tid = 0;
        /// Set by a signaller under m_lock: the wait has been satisfied and
        /// its state consumed; `result` is what wait() returns.
        bool            granted = false;
        std::uint32_t   result = 0;
        /// signaled_at of the object that released this waiter, for the
        /// VITREO_WAKE_LATENCY measurement (taken when the waiter runs).
        std::chrono::steady_clock::time_point released_at{};
    };

private:
    mutable std::mutex            m_lock;
    std::function<void()>         m_host_wake;
    // Dense table keyed by monotonic ObjectId (slot 0 unused). Avoids
    // unordered_map bucket-pointer teardown/corruption windows that showed
    // up as host SIGSEGV at a near-null address inside wait().
    std::vector<std::unique_ptr<Object>> m_objects;
    /// Waiters registered per object, indexed exactly like m_objects, in
    /// registration order: that order is the release order (FIFO). Kept
    /// outside Object because Object is copied (snapshot(), and the shadow
    /// copies wait-all uses to simulate consumption); a waiter list has no
    /// meaning in a copy.
    std::vector<std::vector<Waiter*>>    m_object_waiters;
    /// Every currently parked waiter, for the paths that legitimately must
    /// wake everyone (external cancellation).
    std::vector<Waiter*>                 m_all_waiters;
    /// Threads interrupt_thread() ended. Guest thread ids are never reused.
    std::vector<std::uint32_t>           m_interrupted_tids;
    struct HostArm {
        std::uint32_t tid;
        std::vector<ObjectId> ids;
    };
    /// Objects the run-loop-parked host thread(s) are waiting on.
    std::vector<HostArm>                 m_host_armed;
    ObjectId                      m_next_id = 1;
    /// Scratch for wait-all simulation, reused across calls (m_lock held).
    std::vector<Object>                  m_shadow;
    std::vector<Object*>                 m_scan;
    /// True while a signaller consumes on a waiter's behalf (m_lock held).
    bool                                 m_granting = false;

    std::atomic<std::uint64_t> m_c_waits{0}, m_c_wakes{0}, m_c_grants{0},
                               m_c_spurious{0}, m_c_notifies{0};

    // Helpers — caller MUST hold m_lock.
    ObjectId      allocate_locked(std::unique_ptr<Object> obj);
    Object*       find_locked(ObjectId id);
    const Object* find_locked(ObjectId id) const;
    bool          can_satisfy_locked(const Object& o,
                                     std::uint32_t tid) const;
    /// Apply side-effects of a successful wait on a single object.
    /// Returns WAIT_OBJECT_0 or WAIT_ABANDONED_0.
    std::uint32_t consume_locked(Object& o, std::uint32_t tid);
    /// Wait-all resolution: every object satisfiable at once (duplicates
    /// accounted for), else nothing is touched. `pulsed` names an event that
    /// counts as signaled for this resolution without being consumed (a
    /// PulseEvent in flight). With `commit` the consumption is real and the
    /// return value is the wait result; without it, only the decision.
    std::optional<std::uint32_t> satisfy_all_locked(const ObjectId* ids,
                                                    std::uint32_t count,
                                                    std::uint32_t tid,
                                                    ObjectId pulsed,
                                                    bool commit);
    /// Wait-any resolution for the waiter's own scan: first satisfiable
    /// index, consumed. Returns the wait result or nullopt.
    std::optional<std::uint32_t> satisfy_any_locked(const ObjectId* ids,
                                                    std::uint32_t count,
                                                    std::uint32_t tid,
                                                    ObjectId pulsed);
    /// Try to satisfy a parked waiter now, on its behalf. On success its
    /// state is consumed, `granted`/`result` are set, and the caller wakes it.
    bool          try_grant_locked(Waiter& w, ObjectId pulsed);
    /// Release the waiters of `id` the object can satisfy, in registration
    /// order. `pulsed == id` for PulseEvent (one for auto-reset, all for
    /// manual-reset, the event itself stays clear). Caller MUST hold m_lock.
    void notify_locked(ObjectId id, ObjectId pulsed = kInvalidId);
    /// Wake every parked thread. Caller MUST hold m_lock. Reserved for
    /// state changes that are not attributable to one object.
    void notify_all_locked();
    /// Register / unregister a waiter against the objects it waits on.
    /// Caller MUST hold m_lock.
    void register_waiter_locked(Waiter& w, const ObjectId* ids,
                                std::uint32_t count);
    void unregister_waiter_locked(Waiter& w, const ObjectId* ids,
                                  std::uint32_t count);
};

/// Process-wide singleton — initialized lazily and intentionally leaked.
/// Guest worker threads can still be inside wait() when main unwinds after
/// ExitProcess; destroying this Manager then races with find and SIGSEGVs
/// (null hash buckets). Same immortal pattern as the hostcall ring.
Manager& global();

} // namespace vitreo::ntsync
