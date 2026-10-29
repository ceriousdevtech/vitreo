#pragma once

/// @file sync.hpp
/// @brief Synchronization primitives (THREAD-002)

#include <vitreo/common.hpp>
#include "winapi/kernel32/handle.hpp"
#include "winapi/common/object_access.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <shared_mutex>
#include <core/sync/host_rwlock.hpp>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::ipc { class SharedSyncObject; }

namespace vitreo::winapi {

/// Bisection kill switch for the event-driven wait mechanisms. A mechanism
/// is OFF when the environment variable VITREO_SYNC_OFF_<NAME> is set or the
/// marker file /tmp/vitreo-sync-off-<name> exists (a store-launched guest
/// does not reliably inherit the environment; a file reaches every process).
/// Diagnostic only; every mechanism is on by default.
bool sync_mechanism_off(const char* name) noexcept;

// Forward declaration
class Marshaler;
class ThreadManager;
class CriticalSectionManager;

// =============================================================================
// Wait Return Values
// =============================================================================

constexpr u32 WAIT_OBJECT_0       = 0x00000000;
constexpr u32 WAIT_ABANDONED      = 0x00000080;
constexpr u32 WAIT_IO_COMPLETION  = 0x000000C0; // alertable wait drained user APCs
constexpr u32 WAIT_TIMEOUT        = 0x00000102;
constexpr u32 WAIT_FAILED         = 0xFFFFFFFF;
constexpr u32 INFINITE_WAIT       = 0xFFFFFFFF;

// =============================================================================
// Critical Section (guest-side structure, host-backed by mutex)
// =============================================================================

/// Host-side critical section
struct CriticalSection {
    std::recursive_mutex mutex;
    u32 owning_thread_id{0};
    u32 recursion_count{0};
    bool initialized{false};

    CriticalSection() noexcept : initialized(true) {}
};

struct ConditionVariable {
    /// Mutex used when waiting without holding a CRITICAL_SECTION / SRW lock
    /// (SleepConditionVariableCS must release CriticalSectionManager's CS first).
    std::mutex wait_mutex;
    std::condition_variable_any cv;
    std::atomic<u64> generation{0};
};

struct SrwLock {
    /// A real reader/writer lock, NOT a plain mutex.
    ///
    /// AcquireSRWLockShared used to be implemented by taking this exclusively,
    /// on the reasoning that "exclusive >= shared, so the contract holds, just
    /// less parallel". The contract does hold — but a guest that takes SRW
    /// READ locks constantly across dozens of threads turns Windows'
    /// scalable reader/writer lock into one global mutex. Measured: most
    /// non-parked time can sit in acquire_srw_lock_exclusive, and the UI
    /// freezes for seconds. Readers must not exclude each other.
    std::shared_mutex rw;
    std::condition_variable_any cv;
    /// Number of shared holders. Only a guard against releasing a lock nobody
    /// holds (the exclusive path has the equivalent owner check); shared
    /// ownership is not per-thread tracked, exactly as on Windows.
    std::atomic<i32> shared_holders{0};
    /// Owner thread id, published while holding `mutex` and cleared before
    /// releasing it. The acquire paths test it *without* holding `mutex`, so it
    /// must be atomic: only the owner ever stores its own id, which makes
    /// `owner == me` proof that this thread holds the lock — but only if the
    /// read cannot observe a stale or torn value. As a plain `u32` a thread
    /// could see its own previously-written id next to another thread's fresh
    /// acquisition and conclude it held a lock it had already released.
    std::atomic<u32> owning_thread_id{0};
    /// Owner-only bookkeeping: touched exclusively by the thread that owns the
    /// lock, so it needs no synchronisation of its own.
    u32 exclusive_recursion{0};
    bool exclusively_locked{false};
};

// =============================================================================
// Sync Objects
// =============================================================================
// The actual Windows-semantics objects live in vitreo::ntsync::Manager.
// These wrapper structs only hold the ntsync ObjectId so HandleTable can
// continue to own a void* per allocation.

// When `shared` is set (cross-process IPC, named object), the object lives in
// shared memory and all operations route through it instead of ntsync.
struct SyncMutex {
    u64 nt_id{0};
    std::shared_ptr<ipc::SharedSyncObject> shared;
    std::string name;
    std::string transfer_name;
    object_access::Descriptor security;
};

struct SyncTraceContext {
    std::string_view api;
    GuestAddr caller{0};
    std::array<GuestAddr, 4> parents{};
    u8 parent_count{0};
};

struct SyncTraceRecord {
    u64 sequence{0};
    std::string api;
    u32 thread_id{0};
    GuestAddr caller{0};
    std::array<GuestAddr, 4> parents{};
    u8 parent_count{0};
};

struct EventCausality {
    std::mutex mutex;
    bool manual_reset{false};
    bool initial_state{false};
    SyncTraceRecord created;
    std::optional<SyncTraceRecord> last_transition;
    std::string last_transition_kind;
};

struct SyncEvent {
    u64 nt_id{0};
    std::shared_ptr<ipc::SharedSyncObject> shared;
    std::string name;  ///< named events only (for cross-process handshake diag)
    std::string transfer_name; ///< Internal name for DuplicateHandle transport
    /// Allocated only when VITREO_SYNC_CAUSAL_TRACE is enabled.
    std::unique_ptr<EventCausality> causality;
    object_access::Descriptor security;
    /// True when the handle was created by CreateWaitableTimer*: SetEvent /
    /// ResetEvent / PulseEvent must fail with ERROR_INVALID_HANDLE.
    bool waitable_timer{false};
};

struct SyncSemaphore {
    u64 nt_id{0};
    std::shared_ptr<ipc::SharedSyncObject> shared;
    std::string name;
    std::string transfer_name;
};

// =============================================================================
// SyncManager
// =============================================================================

class SyncManager {
public:
    explicit SyncManager(HandleTable& handles);

    /// No copy
    SyncManager(const SyncManager&) = delete;
    SyncManager& operator=(const SyncManager&) = delete;

    /// No move (referenced by thunks)
    SyncManager(SyncManager&&) = delete;
    SyncManager& operator=(SyncManager&&) = delete;

    ~SyncManager();

    void set_thread_manager(ThreadManager* threads) noexcept {
        m_thread_manager = threads;
    }
    [[nodiscard]] ThreadManager* thread_manager() const noexcept {
        return m_thread_manager;
    }
    /// CancelWaitableTimer / a new SetWaitableTimer invalidates in-flight
    /// period workers for this handle.
    u64 bump_waitable_timer_generation(Handle h);
    [[nodiscard]] u64 waitable_timer_generation(Handle h) const;
    /// Resolve a handle to its underlying ntsync ObjectId.
    /// Returns 0 if not an ntsync-managed handle.
    [[nodiscard]] u64 resolve_nt_id(Handle h) const;
    /// Sleep up to `d`, waking immediately if this handle's generation moves
    /// off `gen`. Returns true only if the full delay elapsed with `gen` still
    /// current, i.e. the caller should fire. Without this a superseded timer
    /// worker stays parked until its original deadline, so a guest that
    /// re-arms a long timer repeatedly accumulates one live host thread per
    /// arm.
    [[nodiscard]] bool waitable_timer_sleep(Handle h, u64 gen,
                                            std::chrono::milliseconds d) const;

    // -------------------------------------------------------------------------
    // Critical Sections (stored by guest address)
    // -------------------------------------------------------------------------

    void initialize_critical_section(GuestAddr cs_addr);
    void initialize_critical_section_and_spin_count(GuestAddr cs_addr, u32 spin_count);
    void enter_critical_section(GuestAddr cs_addr);
    [[nodiscard]] bool try_enter_critical_section(GuestAddr cs_addr);
    void leave_critical_section(GuestAddr cs_addr);
    void delete_critical_section(GuestAddr cs_addr);

    [[nodiscard]] bool sleep_condition_variable_cs(GuestAddr cv_addr,
                                                   GuestAddr cs_addr,
                                                   u32 milliseconds);
    /// Production CONDITION_VARIABLE path for critical sections owned by the
    /// guest-layout CriticalSectionManager. Releases the CS, waits, and
    /// re-acquires it under the same contract as SleepConditionVariableCS.
    [[nodiscard]] bool sleep_condition_variable_cs(
        CriticalSectionManager& critical_sections,
        GuestAddr cv_addr,
        GuestAddr cs_addr,
        u32 milliseconds);
    /// Wait on a condition variable using an already-observed generation.
    /// Does not touch any CRITICAL_SECTION — callers that entered via
    /// CriticalSectionManager must leave/re-enter around this wait.
    [[nodiscard]] bool wait_condition_variable(GuestAddr cv_addr,
                                               u64 observed_generation,
                                               u32 milliseconds);
    [[nodiscard]] u64 condition_variable_generation(GuestAddr cv_addr);
    void wake_condition_variable(GuestAddr cv_addr);
    void wake_all_condition_variable(GuestAddr cv_addr);

    void initialize_srw_lock(GuestAddr lock_addr);
    void acquire_srw_lock_exclusive(GuestAddr lock_addr);
    [[nodiscard]] bool try_acquire_srw_lock_exclusive(GuestAddr lock_addr);
    void release_srw_lock_exclusive(GuestAddr lock_addr);
    /// Real shared acquisition — readers do not exclude other readers.
    void acquire_srw_lock_shared(GuestAddr lock_addr);
    [[nodiscard]] bool try_acquire_srw_lock_shared(GuestAddr lock_addr);
    /// Returns true when this released the LAST shared holder, so the caller
    /// knows whether to clear the guest-visible lock word.
    bool release_srw_lock_shared(GuestAddr lock_addr);
    [[nodiscard]] bool sleep_condition_variable_srw(GuestAddr cv_addr,
                                                    GuestAddr lock_addr,
                                                    u32 milliseconds,
                                                    u32 flags);

    // -------------------------------------------------------------------------
    // Mutexes
    // -------------------------------------------------------------------------

    [[nodiscard]] Handle create_mutex(bool initial_owner, const std::string& name);
    using SecurityFactory = std::function<bool(object_access::Descriptor&)>;
    void set_object_token_provider(std::function<object_access::Token()> provider) {
        m_object_token_provider=std::move(provider);
    }
    [[nodiscard]] object_access::Token object_token() const {
        return m_object_token_provider ? m_object_token_provider() : object_access::Token{};
    }
    [[nodiscard]] Handle create_mutex(bool initial_owner, const std::string& name,
        bool inheritable,u32 desired_access,SecurityFactory security_factory);
    [[nodiscard]] bool authorize_object(const object_access::Descriptor& security,
        u32 desired_access,bool mutex,u32& granted) const;
    [[nodiscard]] bool capture_object_security(object_access::Descriptor& security,
        const SecurityFactory& factory) const;

    [[nodiscard]] Handle open_mutex(const std::string& name);
    [[nodiscard]] Handle open_mutex(const std::string& name,u32 desired_access,bool inheritable);
    bool release_mutex(Handle h);
    /// Apply Windows mutex abandonment to shared named mutexes owned by a
    /// terminating guest thread. Local mutexes are handled by ntsync.
    void abandon_mutexes_for_thread(u32 tid);

    // -------------------------------------------------------------------------
    // Events
    // -------------------------------------------------------------------------

    [[nodiscard]] Handle create_event(bool manual_reset, bool initial_state,
                                      const std::string& name,
                                      bool inheritable = false,
                                      SyncTraceContext trace = {},
                                      bool waitable_timer = false);
    /// Register a non-Event/Mutex/Semaphore handle (console input, …) as
    /// waitable via an ntsync event id. Used by ConsoleManager.
    void register_waitable(Handle h, u64 nt_id);
    void unregister_waitable(Handle h);
    [[nodiscard]] Handle create_event(bool manual_reset, bool initial_state,
                                      const std::string& name, bool inheritable,
                                      SyncTraceContext trace, u32 desired_access,
                                      SecurityFactory security_factory = {},
                                      bool waitable_timer = false);
    [[nodiscard]] Handle open_event(u32 desired_access, const std::string& name,
                                    bool inheritable = false);
    [[nodiscard]] std::string prepare_inherited_handles();
    [[nodiscard]] std::string prepare_inherited_handles(
        std::span<const Handle> selected_handles);
    [[nodiscard]] bool import_inherited_handles(std::string_view descriptors);
    /// Serialize one event/mutex/semaphore for runtime DuplicateHandle into a
    /// different guest process. The Windows-visible name remains unchanged;
    /// unnamed objects receive a private transport name.
    [[nodiscard]] std::optional<std::string> prepare_transferred_handle(
        Handle source, u32 access, bool inheritable);
    /// Import a runtime-transferred synchronization object and allocate a new
    /// HANDLE value in this process.
    [[nodiscard]] Handle import_transferred_handle(
        std::string_view descriptor);
    /// Same-process DuplicateHandle for Event/Mutex/Semaphore: allocate a new
    /// HANDLE that aliases the same underlying sync object. One waiter uses
    /// the duplicate while another signals the original.
    [[nodiscard]] Handle duplicate_handle(Handle source, u32 flags, u32 access);
    bool set_event(Handle h, SyncTraceContext trace = {});
    bool pulse_event(Handle h, SyncTraceContext trace = {});
    bool reset_event(Handle h, SyncTraceContext trace = {});
    /// NtSetEvent / NtPulseEvent / NtResetEvent: the Win32 operation, also
    /// reporting whether the event was signaled before it (PreviousState).
    bool set_event(Handle h, SyncTraceContext trace, bool* previous_state);
    bool pulse_event(Handle h, SyncTraceContext trace, bool* previous_state);
    bool reset_event(Handle h, SyncTraceContext trace, bool* previous_state);
    /// NtQueryEvent EventBasicInformation: the event's type and current
    /// state, without consuming it. False when @p h is not an event (a
    /// waitable timer is not). The caller checks the handle's access.
    [[nodiscard]] bool query_event(Handle h, bool& manual_reset, bool& signaled) const;
    /// The Windows-visible name of a named event, mutex or semaphore handle
    /// (Local\ names without the prefix, Global\ names with it), or empty
    /// for an unnamed object -- including one carrying an internal transfer
    /// name for DuplicateHandle / inheritance.
    [[nodiscard]] std::string object_name(Handle h) const;
    /// Timer expiry: signal a waitable-timer handle. SetEvent/ResetEvent
    /// reject timer handles, so the timer service cannot use them. Fails with
    /// ERROR_INVALID_HANDLE when @p h is not a waitable timer.
    bool signal_waitable_timer(Handle h, SyncTraceContext trace = {});
    /// SetWaitableTimer: return an armed timer to the nonsignaled state.
    /// Requires TIMER_MODIFY_STATE; fails with ERROR_INVALID_HANDLE when
    /// @p h is not a waitable timer.
    bool reset_waitable_timer(Handle h, SyncTraceContext trace = {});
    [[nodiscard]] bool is_waitable_timer(Handle h) const;
    bool close_handle(Handle h);
    bool on_handle_close(Handle h, HandleType type) noexcept;

    // -------------------------------------------------------------------------
    // Semaphores
    // -------------------------------------------------------------------------

    [[nodiscard]] Handle create_semaphore(i32 initial_count, i32 max_count,
                                          const std::string& name,
                                          bool inheritable = false,
                                          u32 desired_access = 0x001F0003u);
    [[nodiscard]] Handle open_semaphore(const std::string& name);
    [[nodiscard]] Handle open_semaphore(const std::string& name,
                                        u32 desired_access, bool inheritable);
    bool release_semaphore(Handle h, i32 release_count, i32* previous_out);
    /// NtQuerySemaphore(SemaphoreBasicInformation): current and maximum count.
    [[nodiscard]] bool query_semaphore(Handle h, i32* current_out,
                                       i32* maximum_out) const;

    // -------------------------------------------------------------------------
    // Process handles (cross-process IPC, Stage 1)
    // -------------------------------------------------------------------------

    /// Create a HANDLE that tracks a real host process by pid. The handle is
    /// waitable (fires when the process exits) and carries the exit code.
    /// Returns 0 if the pid could not be tracked.
    [[nodiscard]] Handle create_process_handle(int pid);

    /// Create a waitable HANDLE for a process that has already finished with
    /// @p exit_code and never had a host process behind it. Used for system
    /// executables the runtime implements internally.
    [[nodiscard]] Handle create_completed_process_handle(
        u32 process_exit_code_value);

    /// If @p h is a process handle, fill @p code (STILL_ACTIVE=259 while alive,
    /// else the real exit code) and return true. Returns false for non-process
    /// handles.
    [[nodiscard]] bool process_exit_code(Handle h, u32& code) const;

    /// If @p h is a process handle, fill @p pid with the real host pid that
    /// backs the tracked guest process and return true. Returns false for
    /// non-process handles, which is what lets GetProcessId distinguish a
    /// genuinely invalid handle from a live one.
    [[nodiscard]] bool process_pid(Handle h, u32& out_pid) const;

    // -------------------------------------------------------------------------
    // Wait Functions
    // -------------------------------------------------------------------------

    [[nodiscard]] u32 wait_for_single_object(
        Handle h, u32 milliseconds, SyncTraceContext trace = {},
        u64* blocked_ns = nullptr);
    [[nodiscard]] u32 wait_for_single_object_cancellable(
        Handle h, u32 milliseconds,
        const std::atomic<bool>& cancel_requested,
        SyncTraceContext trace = {});
    /// SignalObjectAndWait — signal one object then wait on another.
    /// Supports Event/Mutex/Semaphore as the signal target. When @p alertable
    /// is true, pending QueueUserAPC entries are drained (WAIT_IO_COMPLETION).
    [[nodiscard]] u32 signal_object_and_wait(
        Handle signal, Handle wait, u32 milliseconds, bool alertable);
    /// @p extra_local_id, when nonzero, is an ntsync object appended to the
    /// wait set at index @p count (it has no guest handle). MsgWait uses it
    /// for the thread's message-arrival event, so a post wakes the wait
    /// through the same park as its handles. Its satisfaction reports as
    /// WAIT_OBJECT_0 + count.
    [[nodiscard]] u32 wait_for_multiple_objects(u32 count, const Handle* handles,
                                                 bool wait_all, u32 milliseconds,
                                                 u64 extra_local_id = 0);
    /// True when @p h still resolves to a local ntsync object or a
    /// SharedSyncObject (including after DuplicateHandle promote).
    [[nodiscard]] bool is_waitable_sync_handle(Handle h) const;
    [[nodiscard]] bool causal_trace_enabled() const noexcept {
        return m_causal_trace_enabled;
    }
    /// Interrupt host-side waits after the guest process has exited. This is
    /// an internal lifetime signal and does not signal any guest object.
    void begin_shutdown();
    /// Wake local NT-object waiters so they can observe an external
    /// cancellation predicate. This does not mutate guest-visible state.
    void notify_waiters();
    /// The process shutdown flag, for parks that must observe teardown.
    [[nodiscard]] const std::atomic<bool>* shutdown_flag() const noexcept {
        return &m_shutdown;
    }

    /// Optional Cocoa/AppKit pump invoked while the GUI thread (tid 1) blocks
    /// in WaitForSingleObject (shared *and* local ntsync waits). Without this,
    /// a worker's CreateWindowEx / CreateDialogIndirectParam →
    /// `vtr_run_on_main_sync` deadlocks against the main thread's wait
    /// (an early named event, often the splash gate).
    using WaitPump = std::function<void()>;
    void set_wait_pump(WaitPump pump) { m_wait_pump = std::move(pump); }
    /// Blocking form of the pump: park the GUI thread in the host run loop
    /// for at most @p ms, returning early when the run loop is woken (an
    /// ntsync signal, a queue post, or a worker's main-thread block all wake
    /// it). With this installed the GUI thread's waits stop re-checking on a
    /// fixed slice and sleep until something happens.
    using WaitPark = std::function<void(u32 ms)>;
    void set_wait_park(WaitPark park) { m_wait_park = std::move(park); }
    /// Arms the park's wake before the probe that precedes it (true) and
    /// disarms it after the park (false).
    using WaitParkArm = std::function<void(bool)>;
    void set_wait_park_arm(WaitParkArm arm) { m_wait_park_arm = std::move(arm); }
    /// Arm the GUI thread's run-loop wake for exactly these handles (call
    /// before the readiness probe that precedes a park), and disarm it after
    /// the park. Shared-object handles cannot be armed and rely on the cap.
    void arm_gui_wake(u32 count, const Handle* handles);
    void disarm_gui_wake();

    /// Alertable-wait APC poll. Returns true when pending user APCs were
    /// delivered (caller should return WAIT_IO_COMPLETION). Wired from
    /// Kernel32ExtManager once guest nested-call is available.
    using AlertableApcDrain = std::function<bool()>;
    void set_alertable_apc_drain(AlertableApcDrain drain) {
        m_alertable_apc_drain = std::move(drain);
    }
    [[nodiscard]] bool try_drain_alertable_apcs() {
        return m_alertable_apc_drain && m_alertable_apc_drain();
    }

    /// Announce that an APC became deliverable.
    ///
    /// An alertable wait on Windows ends the moment an APC is queued -- the
    /// queuing side wakes the sleeper. Vitreo used to discover APCs by polling
    /// on a 5ms slice, so every asynchronous I/O completion sat finished-but-
    /// unnoticed for up to 5ms. A loader driven by ReadFileEx plus alertable
    /// SleepEx is paced entirely by that tick.
    void note_apc_queued(u32 thread_id = 0) {
        {
            std::lock_guard<std::mutex> lk(m_apc_mutex);
            ++m_apc_generation;
        }
        m_apc_cv.notify_all();
        // An alertable wait that is parked on HANDLES (WFMOEx, WFSOEx,
        // SignalObjectAndWait) sits in ntsync, which the generation cannot
        // reach; it waits on its thread's APC event beside the handles.
        if (thread_id != 0) signal_apc_wait_object(thread_id);
        static const bool diag = std::getenv("VITREO_APC_DIAG") != nullptr;
        if (diag) {
            const auto n = ++m_apc_queued_count;
            if ((n % 200) == 0) {
                std::fprintf(stderr,
                    "[apc] queued=%llu early_wakes=%llu alertable_sleeps=%llu\n",
                    (unsigned long long)n,
                    (unsigned long long)m_apc_early_wakes.load(),
                    (unsigned long long)m_alertable_sleeps.load());
            }
        }
    }

    /// Sleep up to @p milliseconds, returning early if an APC is queued.
    /// @param generation_in_out caller's last-observed APC generation; updated
    ///        on return so a wake cannot be missed between slices.
    /// @return true if an APC arrived (caller should re-check its APC queue)
    [[nodiscard]] bool alertable_sleep(u32 milliseconds,
                                       u64& generation_in_out) {
        std::unique_lock<std::mutex> lk(m_apc_mutex);
        ++m_alertable_sleeps;
        const bool woken = m_apc_cv.wait_for(
            lk, std::chrono::milliseconds(milliseconds),
            [&] { return m_apc_generation != generation_in_out; });
        generation_in_out = m_apc_generation;
        if (woken) ++m_apc_early_wakes;
        return woken;
    }

    /// The calling thread's auto-reset ntsync event that note_apc_queued
    /// signals. Waited on beside the handles of an alertable wait so an APC
    /// ends the wait at once. Created on first use, lives for the process.
    [[nodiscard]] u64 apc_wait_object_for_current_thread();
    void signal_apc_wait_object(u32 thread_id);
    /// NtWaitForAlertByThreadId / NtAlertThreadByThreadId: one auto-reset
    /// alert per guest thread, created on first use.
    [[nodiscard]] u64 alert_wait_object_for_thread(u32 thread_id);
    /// Drop `thread_id`'s alert object, so a thread that later runs under
    /// the same id starts unalerted, as a new Windows thread does.
    void discard_alert_wait_object(u32 thread_id);

    [[nodiscard]] u64 apc_generation() {
        std::lock_guard<std::mutex> lk(m_apc_mutex);
        return m_apc_generation;
    }

    // -------------------------------------------------------------------------
    // Sleep
    // -------------------------------------------------------------------------

    void sleep(u32 milliseconds);

private:
    [[nodiscard]] Handle import_transferred_handle_impl(
        std::string_view descriptor,
        std::optional<Handle> inherited_handle);

    HandleTable& m_handles;
    ThreadManager* m_thread_manager{nullptr};
    mutable std::mutex m_waitable_timer_mutex;
    mutable std::condition_variable m_waitable_timer_cv;
    std::unordered_map<Handle, u64> m_waitable_timer_gen;

    /// One armed SetWaitableTimer expiry.
    ///
    /// Each arm used to get its OWN detached std::thread that slept to its
    /// deadline. A guest may arm a waitable timer per delayed task. Thousands
    /// of host thread creations, each allocating a stack under the process VM
    /// lock, stall every other thread in the process.
    /// One service thread with a deadline-ordered queue replaces all of it.
    struct WaitableTimerArm {
        Handle h{0};
        u64 gen{0};
        i32 period_ms{0};
        GuestAddr pfn{0};
        GuestAddr arg{0};
        u32 issuer_tid{0};
    };
    std::multimap<std::chrono::steady_clock::time_point, WaitableTimerArm>
        m_waitable_timer_queue;
    std::thread m_waitable_timer_thread;
    bool m_waitable_timer_thread_started{false};
    /// Set by the destructor. Distinct from m_shutdown, which is the guest's
    /// process-teardown signal and may never be set in a unit test.
    bool m_waitable_timer_stop{false};

    void waitable_timer_service();
    void stop_waitable_timer_service();

public:
    /// Queue an expiry with the timer service, starting it on first use.
    /// `period_ms > 0` re-arms after each fire until superseded.
    void arm_waitable_timer(Handle h, u64 gen,
                            std::chrono::milliseconds delay, i32 period_ms,
                            GuestAddr pfn, GuestAddr arg, u32 issuer_tid);

private:
    std::atomic<bool> m_shutdown{false};
    WaitPump m_wait_pump;
    WaitPark m_wait_park;
    WaitParkArm m_wait_park_arm;
    AlertableApcDrain m_alertable_apc_drain;
    std::mutex m_apc_events_mutex;
    std::map<u32, u64> m_apc_events;   // guest tid -> ntsync event id
    std::map<u32, u64> m_alert_events; // guest tid -> ntsync event id
    std::mutex m_apc_mutex;
    std::condition_variable m_apc_cv;
    u64 m_apc_generation{0};
    std::atomic<u64> m_apc_queued_count{0};
    std::atomic<u64> m_apc_early_wakes{0};
    std::atomic<u64> m_alertable_sleeps{0};
    const bool m_causal_trace_enabled;
    std::atomic<u64> m_causal_sequence{0};
    std::mutex m_causal_wait_mutex;
    struct CausalWaitKey {
        u32 thread_id;
        Handle handle;
        GuestAddr caller;
        bool operator==(const CausalWaitKey&) const = default;
    };
    std::vector<CausalWaitKey> m_causal_wait_seen;
    // shared_mutex so that hot lookup paths (EnterCriticalSection / Leave / set_event /
    // reset_event / release_mutex / wait_for_single_object) can take a shared lock
    // and run concurrently across guest worker threads.  Insertions / erasures
    // (Initialize* / Create* / Delete* / close_handle) take exclusive lock.
    // Reader-scalable (core/sync/host_rwlock.hpp): the SRW-lock and critical
    // section lookups take this shared on every guest Acquire/Release, and
    // std::shared_mutex serialised those readers on its internal mutex.
    mutable HostRwLock m_mutex;

    // Critical sections keyed by guest address
    std::unordered_map<GuestAddr, std::unique_ptr<CriticalSection>> m_critical_sections;
    std::unordered_map<GuestAddr, std::unique_ptr<ConditionVariable>> m_condition_variables;
    std::unordered_map<GuestAddr, std::unique_ptr<SrwLock>> m_srw_locks;

    // Sync objects keyed by handle. Multiple HANDLEs (named open, same-process
    // DuplicateHandle) may alias one object; destroy ntsync only on last close.
    std::unordered_map<Handle, std::shared_ptr<SyncMutex>> m_mutexes;
    std::unordered_map<std::string, std::weak_ptr<SyncMutex>> m_named_mutexes;
    std::unordered_map<Handle, std::shared_ptr<SyncEvent>> m_events;
    std::unordered_map<std::string, std::weak_ptr<SyncEvent>> m_named_events;
    std::unordered_map<Handle, std::shared_ptr<SyncSemaphore>> m_semaphores;
    std::unordered_map<std::string, std::weak_ptr<SyncSemaphore>> m_named_semaphores;
    /// Extra waitable handles (ConsoleInput, memory-resource notification, …)
    /// that are not in m_events/m_mutexes/m_semaphores.
    std::unordered_map<Handle, u64> m_extra_waitables;


    /// Resolve a handle to its cross-process shared sync object, or nullptr if
    /// the handle is not a shared (named, ipc) event/mutex/semaphore.
    [[nodiscard]] std::shared_ptr<ipc::SharedSyncObject> resolve_shared(Handle h) const;

    [[nodiscard]] CriticalSection* get_or_create_critical_section(GuestAddr cs_addr);
    [[nodiscard]] ConditionVariable* get_or_create_condition_variable(GuestAddr cv_addr);
    [[nodiscard]] SrwLock* get_or_create_srw_lock(GuestAddr lock_addr);

    /// Resolve the SrwLock for @p lock_addr without serializing every SRW
    /// operation in the process behind one EXCLUSIVE lock on m_mutex. The
    /// steady state is "the lock already exists", which only reads the map, so
    /// take m_mutex shared for that and escalate only to create. Entries are
    /// never erased and the mapped values are unique_ptr, so a pointer read
    /// under the shared lock stays valid after it is dropped.
    [[nodiscard]] SrwLock* srw_lock_for(GuestAddr lock_addr);

    /// Same, but never creates: returns nullptr when the lock is unknown.
    [[nodiscard]] SrwLock* srw_lock_if_present(GuestAddr lock_addr);
    void initialize_event_causality(
        SyncEvent& event, bool manual_reset, bool initial_state,
        SyncTraceContext trace);
    void record_event_transition(
        const std::shared_ptr<SyncEvent>& event, std::string_view kind,
        SyncTraceContext trace);
    void trace_event_wait(
        Handle handle, u32 milliseconds, SyncTraceContext trace);
    void log_wait_kind(Handle h, u32 milliseconds);
    bool set_event_state(Handle h, SyncTraceContext trace, bool timer,
                         bool check_access, bool* previous_state = nullptr);
    bool reset_event_state(Handle h, SyncTraceContext trace, bool timer,
                           bool* previous_state = nullptr);
    [[nodiscard]] u32 wait_for_single_object_impl(
        Handle h, u32 milliseconds, SyncTraceContext trace,
        const std::atomic<bool>* cancel_requested, u64* blocked_ns);
    std::function<object_access::Token()> m_object_token_provider;
};

// =============================================================================
// Win32 Thunk Declarations
// =============================================================================

void impl_InitializeCriticalSection(Marshaler& m, SyncManager& mgr);
void impl_InitializeCriticalSectionAndSpinCount(Marshaler& m, SyncManager& mgr);
void impl_EnterCriticalSection(Marshaler& m, SyncManager& mgr);
void impl_TryEnterCriticalSection(Marshaler& m, SyncManager& mgr);
void impl_LeaveCriticalSection(Marshaler& m, SyncManager& mgr);
void impl_DeleteCriticalSection(Marshaler& m, SyncManager& mgr);
void impl_CreateMutexA(Marshaler& m, SyncManager& mgr);
void impl_CreateMutexW(Marshaler& m, SyncManager& mgr);
void impl_ReleaseMutex(Marshaler& m, SyncManager& mgr);
void impl_CreateEventW(Marshaler& m, SyncManager& mgr);
void impl_CreateEventA(Marshaler& m, SyncManager& mgr);
void impl_CreateEventExW(Marshaler& m, SyncManager& mgr);
void impl_SetEvent(Marshaler& m, SyncManager& mgr);
void impl_ResetEvent(Marshaler& m, SyncManager& mgr);
void impl_WaitForSingleObject(Marshaler& m, SyncManager& mgr);
void impl_WaitForMultipleObjects(Marshaler& m, SyncManager& mgr);
void impl_WaitForMultipleObjectsEx(Marshaler& m, SyncManager& mgr);
void impl_SignalObjectAndWait(Marshaler& m, SyncManager& mgr);
void impl_Sleep(Marshaler& m, SyncManager& mgr);
void impl_SleepEx(Marshaler& m, SyncManager& mgr);

void impl_CreateWaitableTimerW(Marshaler& m, SyncManager& mgr);
void impl_CreateWaitableTimerExW(Marshaler& m, SyncManager& mgr);
void impl_SetWaitableTimer(Marshaler& m, SyncManager& mgr);
void impl_SetWaitableTimerEx(Marshaler& m, SyncManager& mgr);
void impl_CancelWaitableTimer(Marshaler& m, SyncManager& mgr);

void impl_WaitOnAddress(Marshaler& m);
void impl_WakeByAddressSingle(Marshaler& m);
void impl_WakeByAddressAll(Marshaler& m);

/// Test seam: WaitOnAddress calls this after its last compare and
/// immediately before it parks, so a test can land a WakeByAddress* exactly
/// in that window. Null in production (one relaxed load per park).
using WaitOnAddressParkHook = void (*)(GuestAddr address);
void set_wait_on_address_park_hook(WaitOnAddressParkHook hook) noexcept;
/// Test seam: called right after the waiter has joined its address's queue
/// and before its last compare (a wake landing there must also be kept).
void set_wait_on_address_registered_hook(WaitOnAddressParkHook hook) noexcept;

/// TerminateThread / thread exit: take every WaitOnAddress entry of
/// `thread_id` out of its queue and end its park, so a terminated waiter
/// cannot absorb a WakeByAddressSingle meant for a live one (Windows removes
/// a terminated thread's wait block). Returns how many entries were removed.
usize cancel_wait_on_address_for_thread(u32 thread_id);

/// Register sync thunks into kernel32
[[nodiscard]] DllExports create_sync_thunks(SyncManager& mgr);

/// FlushProcessWriteBuffers (Learn processthreadsapi): a process-wide
/// asymmetric memory barrier. Forces every other thread of this process
/// through a serialising point, so writes each of them made before the call
/// are visible to the caller afterwards and the caller's earlier writes are
/// visible to them. Returns the number of other threads that were serialised.
u32 flush_process_write_buffers() noexcept;

/// The SecurityFactory CreateEvent/CreateMutex use for an lpSecurityAttributes
/// argument: reads its SECURITY_DESCRIPTOR (owner, DACL, SACL labels/audit).
[[nodiscard]] SyncManager::SecurityFactory sync_security_attributes_factory(
    Marshaler& m, GuestAddr attributes);

} // namespace vitreo::winapi
