#pragma once

/// @file thread.hpp
/// @brief Thread creation and lifecycle (THREAD-001)

#include <vitreo/common.hpp>
#include "platform/host_hardware.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/common/thread_query_access.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <atomic>
#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class ThreadCpuAccounting;

// =============================================================================
// Constants
// =============================================================================

constexpr u32 CREATE_SUSPENDED = 0x4;
constexpr u32 STACK_SIZE_PARAM_IS_A_RESERVATION = 0x10000;
constexpr u32 STILL_ACTIVE     = 259;

/// Reserve/commit a new thread's stack gets (Learn "Thread Stack Size";
/// rounding measured by tools/oracle_probe/thread_lifecycle_probe.c):
///  - dwStackSize is the commit size, or the reserve size with
///    STACK_SIZE_PARAM_IS_A_RESERVATION; 0 (or the other one) comes from the
///    EXE's SizeOfStackReserve / SizeOfStackCommit.
///  - commit is rounded up to a page; if it is >= the reserve, the reserve
///    becomes the commit rounded up to 1 MB; the reserve is rounded up to the
///    64 KB allocation granularity.
struct ThreadStackSizes {
    u64 reserve = 0;
    u64 commit = 0;
};
[[nodiscard]] ThreadStackSizes compute_thread_stack_sizes(
    u64 requested, u32 creation_flags, u64 default_reserve, u64 default_commit);
/// The same rounding for the native form, NtCreateThreadEx, which takes the
/// commit (SizeOfStackCommit) and reserve (MaximumStackSize) separately; a 0
/// one comes from the EXE header. CreateThread is this with one of them 0.
[[nodiscard]] ThreadStackSizes compute_native_thread_stack_sizes(
    u64 commit, u64 reserve, u64 default_reserve, u64 default_commit);

// =============================================================================
// Host scheduling policy (guest priority / power throttling -> host QoS)
// =============================================================================

/// processthreadsapi.h (SDK 10.0.26100): THREAD_POWER_THROTTLING_* and
/// PROCESS_POWER_THROTTLING_*. EXECUTION_SPEED is EcoQoS when on, HighQoS
/// when controlled and off, system-managed when not controlled.
constexpr u32 POWER_THROTTLING_CURRENT_VERSION = 1;
constexpr u32 POWER_THROTTLING_EXECUTION_SPEED = 0x1;
constexpr u32 PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION = 0x4;

/// Validation of a THREAD_POWER_THROTTLING_STATE for SetThreadInformation,
/// measured by tools/oracle_probe/power_throttling_probe.c (x86 = x64): size
/// < 12, Version > 1, a ControlMask bit outside EXECUTION_SPEED, or a
/// StateMask bit outside ControlMask -> ERROR_INVALID_PARAMETER, before the
/// handle is looked at. Size >= 12 and Version 0 are accepted. 0 = valid.
[[nodiscard]] u32 validate_thread_power_throttling(u32 size, u32 version,
                                                   u32 control, u32 state) noexcept;
/// The same for PROCESS_POWER_THROTTLING_STATE (SetProcessInformation), same
/// probe: size != 12 -> ERROR_BAD_LENGTH; Version != 1, ControlMask outside
/// 0x7 (bit 0x2 is accepted though the SDK names no flag for it) or StateMask
/// outside ControlMask -> ERROR_INVALID_PARAMETER; all before the handle.
[[nodiscard]] u32 validate_process_power_throttling(u32 size, u32 version,
                                                    u32 control, u32 state) noexcept;

/// Explicit EXECUTION_SPEED request of a thread or process.
enum class PowerThrottlingQos : u8 { SystemManaged, Eco, High };
[[nodiscard]] constexpr PowerThrottlingQos power_throttling_qos(
    u32 control, u32 state) noexcept {
    if ((control & POWER_THROTTLING_EXECUTION_SPEED) == 0) {
        return PowerThrottlingQos::SystemManaged;
    }
    return (state & POWER_THROTTLING_EXECUTION_SPEED) != 0
               ? PowerThrottlingQos::Eco
               : PowerThrottlingQos::High;
}

/// VITREO_THREAD_QOS_FLOOR: lowest host QoS class a guest thread of base
/// priority 2..7 is given (host policy, not a Windows contract; Learn
/// "Quality of Service": reduced priority only *can* imply a lower QoS).
enum class HostQosFloor : u8 { Utility, Default, UserInitiated };

/// Learn "Scheduling Priorities": base priority of a thread from its
/// process's priority class and its SetThreadPriority level.
[[nodiscard]] int windows_base_priority(u32 priority_class, i32 level) noexcept;

/// The host QoS class (sys/qos.h value) for a guest thread. Background mode
/// (thread or process) and base <= 1 are BACKGROUND; explicit EcoQoS is
/// UTILITY; explicit HighQoS is at least USER_INITIATED; otherwise base >= 9
/// USER_INTERACTIVE, 8 USER_INITIATED, 7 DEFAULT, 2..6 UTILITY, with base
/// 2..7 raised to @p floor. Defined in thread.cpp.
[[nodiscard]] unsigned host_qos_class_for(int base, bool background,
                                          PowerThrottlingQos qos,
                                          HostQosFloor floor) noexcept;

// =============================================================================
// Guest Thread
// =============================================================================

/// Represents a single guest thread
struct GuestThread {
    u32         thread_id;
    GuestAddr   start_address;
    GuestAddr   parameter;
    GuestAddr   stack_base;
    usize       stack_size;     ///< reserve (compute_thread_stack_sizes)
    usize       stack_commit = 0;
    u64         creation_time;
    u64         exit_time;
    /// Opaque owner of the executing host thread's CPU counters. Kept with
    /// the Windows thread object so exited-thread handles retain the totals.
    std::shared_ptr<ThreadCpuAccounting> cpu_accounting;
    cpu::CpuState cpu;
    u32         exit_code;
    bool        terminated;
    u32         suspend_count;  ///< CV-07: track suspend count, not just a boolean
    bool        launched;      ///< True once run_async has been called
    /// SetThreadContext changed `cpu` before the thread first ran: it must
    /// start at exactly that context (not at start_address/parameter).
    bool        context_set = false;
    i32         priority = 0;          ///< SetThreadPriority level
    bool        background = false;    ///< THREAD_MODE_BACKGROUND_BEGIN
    /// SetThreadInformation(ThreadPowerThrottling) EXECUTION_SPEED request.
    PowerThrottlingQos power_throttling = PowerThrottlingQos::SystemManaged;
    /// SetThreadInformation(ThreadMemoryPriority); MEMORY_PRIORITY_NORMAL.
    u32         memory_priority = 5;
    /// Bumped when `priority`, `background` or `power_throttling` changes, so the thread's own
    /// host thread re-applies its scheduling (refresh_current_thread_
    /// scheduling). Shared so the host thread can watch it lock-free.
    std::shared_ptr<std::atomic<u32>> scheduling_epoch =
        std::make_shared<std::atomic<u32>>(1);
    bool        hidden_from_debugger = false; ///< Native ThreadHideFromDebugger
    /// NtCreateThreadEx THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH: the thread runs
    /// no DLL thread attach/detach notification or TLS callback, and its TEB
    /// SameTebFlags has SkipThreadAttach (bit 3) set (measured,
    /// skip_thread_attach_probe, x86 = x64).
    bool        skip_thread_attach = false;
    /// SetThreadAffinityMask / SetThreadGroupAffinity: the logical processors
    /// (group 0) this thread may run on; 0 = the process affinity. Shared with
    /// the host thread running it (platform::bind_current_thread_affinity), so
    /// CPUID and GetCurrentProcessorNumber report the processor it is pinned to.
    std::shared_ptr<platform::ProcessorAffinity> affinity =
        std::make_shared<platform::ProcessorAffinity>(0);
    /// SetThreadIdealProcessor(Ex). Windows assigns a new thread's ideal
    /// processor round-robin; so does create_thread.
    u32         ideal_processor = 0;
    /// Guest stack/TEB/TLS the runtime allocated for this thread at creation
    /// (opaque to kernel32; owned by the thread-context callbacks).
    std::shared_ptr<void> launch_context;

    /// Fourth argument for a run_async launch with arg_count 4 (a thread-pool
    /// wait callback's TP_WAIT_RESULT); set before run_async.
    u64 fourth_parameter = 0;

    /// Thread-completion signal — a manual-reset ntsync event that
    /// becomes signaled when this thread terminates.  Win32 thread handles
    /// behave exactly like manual-reset events, so we model them uniformly
    /// alongside Event / Mutex / Semaphore in the ntsync manager.
    std::uint64_t nt_id = 0;

    GuestThread() noexcept
        : thread_id(0), start_address(0), parameter(0),
          stack_base(0), stack_size(0), creation_time(0), exit_time(0), cpu{},
          exit_code(STILL_ACTIVE), terminated(false), suspend_count(0),
          launched(false) {}
};

// =============================================================================
// Thread Manager
// =============================================================================

/// Manages guest threads
class ThreadManager {
public:
    explicit ThreadManager(HandleTable& handles, u32 primary_thread_id = 0);

    /// Reserve a system-wide ID before publishing or launching a thread.
    [[nodiscard]] u32 allocate_thread_id();

    /// Revokes the diagnostics snapshot hook. The perf window dumps from an
    /// atexit handler that runs after RuntimeContext has already destroyed
    /// this manager; without this the hook locks a destroyed std::mutex and
    /// aborts the process during an otherwise clean exit.
    ~ThreadManager();

    /// Create a new thread
    /// @param stack_size Stack size (0 = default 1 MB)
    /// @param start_address Guest address of thread function
    /// @param parameter Parameter to pass to thread function
    /// @param creation_flags CREATE_SUSPENDED etc.
    /// @param thread_id_out Where to store the thread ID (may be null)
    /// @return Thread handle or INVALID_HANDLE on failure
    [[nodiscard]] Handle create_thread(usize stack_size, GuestAddr start_address,
                                       GuestAddr parameter, u32 creation_flags,
                                       u32* thread_id_out,
                                       u64 second_parameter = 0,
                                       u32 handle_flags = 0,
                                       std::optional<ThreadStackSizes>
                                           native_stack = std::nullopt);

    /// The EXE's SizeOfStackReserve / SizeOfStackCommit: the stack a thread
    /// created with dwStackSize 0 gets.
    void set_default_stack(u64 reserve, u64 commit) noexcept {
        m_default_stack_reserve = reserve;
        m_default_stack_commit = commit;
    }

    /// Exit the current thread (called from within a thread)
    void exit_thread(u32 thread_id, u32 exit_code);

    /// Called by the actual execution worker, never by a temporary guest-TID
    /// switch in a native callback. Host setup before this point is excluded.
    void bind_current_host_thread(u32 thread_id);
    /// Freeze CPU counters before the executing host thread disappears.
    void finish_thread_cpu_accounting(u32 thread_id);
    /// GetThreadTimes snapshot under the thread-object lifecycle lock.
    [[nodiscard]] bool get_thread_times(Handle handle, std::array<u64, 4>& times,
                                         u32& error);

    /// Guest tid of the process primary thread (created with ThreadManager).
    [[nodiscard]] u32 main_thread_id() const noexcept { return m_main_thread_id; }

    /// True when `thread_id` has already been marked terminated.
    [[nodiscard]] bool is_thread_terminated(u32 thread_id) const;

    /// `ExitThread` / `FreeLibraryAndExitThread` contract: mark the current
    /// guest thread terminated and transfer control to a return sentinel so
    /// the caller never resumes (MSVC places `int3` after noreturn calls).
    void exit_current_thread_noreturn(Marshaler& m, u32 exit_code);

    /// True once the primary thread called ExitThread while other threads were
    /// still alive. Learn ExitThread: only the LAST thread's exit ends the
    /// process, so the runtime must keep the process up until then
    /// (thread_lifecycle_probe: "AFTER_MAIN_EXIT ... exit=42").
    [[nodiscard]] bool primary_exit_pending() const noexcept {
        return m_primary_exit_pending.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 primary_exit_code() const noexcept { return m_primary_exit_code; }

    /// Exit code of the most recent thread exit: the process exit code when
    /// that thread was the last one.
    [[nodiscard]] u32 last_exit_code() const noexcept {
        return m_last_exit_code.load(std::memory_order_acquire);
    }

    /// Whether any thread object other than @p self has not terminated,
    /// including CREATE_SUSPENDED threads that never ran.
    [[nodiscard]] bool any_live_thread_except(u32 self) const;

    /// Vitreo's own threads that run guest code under an id outside the
    /// thread table: the timer, wave, COM-RPC and WinHTTP service threads
    /// and the RegisterWait / threadpool waiters. To the guest they are
    /// threads of this process. Registration is counted, so two waiters
    /// that share an id stay known until both have gone.
    void add_service_thread(u32 thread_id);
    void remove_service_thread(u32 thread_id);
    /// Called (outside the manager's lock) when a service id starts or stops
    /// being registered: an id a waiter reuses must not inherit the previous
    /// thread's per-thread state (its pending alert).
    using ServiceThreadIdCallback = std::function<void(u32 thread_id)>;
    void set_service_thread_id_callback(ServiceThreadIdCallback callback) {
        m_service_thread_id_callback = std::move(callback);
    }

    /// Whether `thread_id` names a thread of this process that a client-id
    /// API (NtAlertThreadByThreadId) can reach: a thread object (running, or
    /// exited while a handle keeps it) or a registered service thread.
    [[nodiscard]] bool is_thread_of_this_process(u32 thread_id) const;

    /// RAII add_service_thread / remove_service_thread for a waiter thread.
    class ServiceThreadScope {
    public:
        ServiceThreadScope(ThreadManager* threads, u32 thread_id)
            : m_threads(threads), m_tid(thread_id) {
            if (m_threads != nullptr) m_threads->add_service_thread(m_tid);
        }
        ~ServiceThreadScope() {
            if (m_threads != nullptr) m_threads->remove_service_thread(m_tid);
        }
        ServiceThreadScope(const ServiceThreadScope&) = delete;
        ServiceThreadScope& operator=(const ServiceThreadScope&) = delete;
    private:
        ThreadManager* m_threads;
        u32 m_tid;
    };

    /// Get exit code of a thread
    [[nodiscard]] Result<u32, Error> get_exit_code(Handle thread_handle) const;

    /// TerminateThread (terminate_thread_probe). The target never executes
    /// another guest instruction and gets no DLL_THREAD_DETACH; its object is
    /// signaled with @p exit_code once it can no longer run guest code. A
    /// thread that already exited or was terminated keeps its exit code and
    /// the call still succeeds. Fails with ERROR_INVALID_HANDLE for a non-
    /// thread handle and ERROR_ACCESS_DENIED without THREAD_TERMINATE.
    bool terminate_thread(Handle thread_handle, u32 exit_code);

    /// Resume a suspended thread
    /// @return Previous suspend count, or ~0u on error
    u32 resume_thread(Handle thread_handle);

    using ExternalResumeCallback =
        std::function<std::optional<u32>(Handle)>;
    void set_external_resume_callback(ExternalResumeCallback callback) {
        m_external_resume_callback = std::move(callback);
    }

    /// Windows caps a thread's suspend count at MAXIMUM_SUSPEND_COUNT (127)
    /// and refuses to suspend a thread that has already run to completion.
    /// Both cases are how a caller's suspend loop terminates, so they must be
    /// reported instead of silently succeeding forever.
    enum class SuspendResult {
        Ok,             ///< suspend count incremented
        InvalidHandle,  ///< no such thread object
        Terminating,    ///< thread already exited (STATUS_THREAD_IS_TERMINATING)
        CountExceeded,  ///< already at MAXIMUM_SUSPEND_COUNT
        AccessDenied,   ///< handle lacks THREAD_SUSPEND_RESUME
    };
    static constexpr u32 MAXIMUM_SUSPEND_COUNT = 127;

    /// Suspend a thread
    /// @param status_out Optional detailed outcome (see SuspendResult)
    /// @return Previous suspend count, or ~0u on error
    u32 suspend_thread(Handle thread_handle,
                       SuspendResult* status_out = nullptr);

    /// winnt.h thread access rights enforced by the thread APIs.
    static constexpr u32 THREAD_TERMINATE = 0x0001;
    static constexpr u32 THREAD_SUSPEND_RESUME = 0x0002;
    static constexpr u32 THREAD_GET_CONTEXT = 0x0008;
    static constexpr u32 THREAD_SET_CONTEXT = 0x0010;
    static constexpr u32 THREAD_SET_INFORMATION = 0x0020;
    static constexpr u32 THREAD_QUERY_INFORMATION = thread_query_access::THREAD_QUERY_INFORMATION;
    static constexpr u32 THREAD_SET_LIMITED_INFORMATION = 0x0400;
    static constexpr u32 THREAD_QUERY_LIMITED_INFORMATION = thread_query_access::THREAD_QUERY_LIMITED_INFORMATION;

    /// Resolve a thread handle -- including the GetCurrentThread() pseudo
    /// handle, which carries every right -- checking that it grants every bit
    /// of @p required_access (0 = no check). Returns nullptr and sets
    /// @p error to ERROR_INVALID_HANDLE / ERROR_ACCESS_DENIED on failure.
    [[nodiscard]] GuestThread* resolve_thread_handle(Handle h,
                                                     u32 required_access,
                                                     u32& error);

    /// 0 if @p h names a thread granting @p required_access, else
    /// ERROR_INVALID_HANDLE / ERROR_ACCESS_DENIED.
    [[nodiscard]] u32 thread_handle_error(Handle h, u32 required_access);

    /// Guest TID for @p h when it resolves and grants @p required_access;
    /// otherwise 0 and @p error set.
    [[nodiscard]] u32 thread_id_for_handle(Handle h, u32 required_access,
                                           u32& error);

    /// SetThreadAffinityMask / SetThreadGroupAffinity storage. @p mask is the
    /// validated new affinity (already inside the process affinity); the
    /// stored one (0 = the process affinity) is returned in @p previous.
    /// Returns 0 or the Win32 error for the handle.
    [[nodiscard]] u32 set_thread_affinity(Handle h, u64 mask, u64& previous);
    /// The stored affinity of @p h (0 = the process affinity), or an error.
    [[nodiscard]] u32 get_thread_affinity(Handle h, u64& mask);
    /// SetThreadIdealProcessor(Ex) / GetThreadIdealProcessorEx storage: checks
    /// @p required_access, returns the stored value in @p previous and, with
    /// @p set, stores @p requested.
    [[nodiscard]] u32 thread_ideal_processor(Handle h, u32 required_access, bool set,
                                             u32 requested, u32& previous);

    /// SetThreadPriority / GetThreadPriority (measured contract in thread.cpp).
    /// set returns 0 on success or the Win32 error; get returns
    /// THREAD_PRIORITY_ERROR_RETURN and sets the last error on failure.
    [[nodiscard]] u32 set_thread_priority(Handle h, i32 priority);
    /// The level/background bookkeeping of set_thread_priority, under m_mutex.
    [[nodiscard]] u32 set_thread_priority_locked(Handle h, i32 priority,
                                                 bool& self);

    /// Give the CALLING host thread the scheduling its guest thread's Windows
    /// priority asks for: the base priority Learn's "Scheduling Priorities"
    /// table assigns the process class and thread level, as the host QoS
    /// class (thread.cpp, host_scheduling_for). Called when a guest thread's
    /// host thread starts, and by SetThreadPriority on the calling thread.
    void apply_current_thread_scheduling();
    /// A priority set on ANOTHER thread, or a new process priority class,
    /// reaches each thread here: one relaxed load when nothing changed. The
    /// wait APIs call it on entry, where every scheduled guest thread
    /// arrives often.
    static void refresh_current_thread_scheduling() noexcept {
        ThreadManager* const mgr = t_scheduling_mgr;
        if (mgr != nullptr &&
            (t_thread_epoch->load(std::memory_order_relaxed) !=
                 t_thread_epoch_seen ||
             mgr->m_scheduling_epoch.load(std::memory_order_relaxed) !=
                 t_scheduling_epoch ||
             s_host_policy_epoch.load(std::memory_order_relaxed) !=
                 t_host_policy_epoch)) {
            mgr->apply_current_thread_scheduling();
        }
    }
    /// The host QoS policy itself changed (the VITREO_AB_CYCLE_S "qos-floor"
    /// arm flipped): every guest thread of every ThreadManager re-applies its
    /// QoS at its next refresh. macOS sets QoS only on the calling thread
    /// (pthread_set_qos_class_self_np), so this is the only way to move
    /// another thread; a thread that never waits keeps its class until it does.
    static void notify_host_scheduling_policy_changed() noexcept {
        s_host_policy_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    /// VITREO_THREAD_QOS_FLOOR (read once), and the floor in force now: the
    /// configured one, or Utility while the "qos-floor" A/B arm is off.
    [[nodiscard]] static HostQosFloor configured_qos_floor() noexcept;
    [[nodiscard]] static HostQosFloor active_qos_floor() noexcept;
    /// Tests only: replace the configured floor (nullopt returns to the env).
    static void set_qos_floor_for_test(std::optional<HostQosFloor> floor) noexcept {
        s_qos_floor_override.store(floor ? static_cast<int>(*floor) : -1,
                                   std::memory_order_relaxed);
    }
    /// SetPriorityClass / PROCESS_MODE_BACKGROUND_* on the current process.
    void set_process_scheduling(u32 priority_class, bool background);
    /// SetThreadInformation(ThreadPowerThrottling) after
    /// validate_thread_power_throttling: needs THREAD_SET_INFORMATION (the
    /// limited right is refused, power_throttling_probe). Any thread; the
    /// caller takes it now, another thread at its next wait. 0 or the error.
    [[nodiscard]] u32 set_thread_power_throttling(Handle h, u32 control,
                                                  u32 state);
    /// SetThreadInformation(ThreadMemoryPriority), value already validated
    /// (MEMORY_PRIORITY_VERY_LOW..NORMAL): THREAD_SET_INFORMATION required.
    [[nodiscard]] u32 set_thread_memory_priority(Handle h, u32 priority);
    /// SetProcessInformation(ProcessPowerThrottling) on the current process,
    /// already validated. EXECUTION_SPEED reaches every thread's QoS.
    void set_process_power_throttling(u32 control, u32 state);
    [[nodiscard]] u32 set_thread_hidden_from_debugger(Handle h);
    [[nodiscard]] u32 get_thread_hidden_from_debugger(Handle h, bool& hidden);
    [[nodiscard]] i32 get_thread_priority(Handle h);

    /// Get the current thread pseudo-handle (-2 convention)
    [[nodiscard]] static Handle get_current_thread() noexcept {
        return static_cast<Handle>(-2);
    }

    /// Get the current thread ID (per-host-thread via thread_local).
    [[nodiscard]] u32 get_current_thread_id() const noexcept {
        return t_current_guest_tid;
    }

    /// Set the current host thread's guest TID.
    static void set_current_thread_id(u32 tid) noexcept {
        t_current_guest_tid = tid;
        g_vitreo_trace_tid = tid;
    }

    /// Static accessor for current guest TID (usable without a ThreadManager instance).
    /// The calling guest thread was created with SKIP_THREAD_ATTACH.
    [[nodiscard]] static bool current_thread_skips_thread_attach();
    [[nodiscard]] static u32 get_current_thread_id_static() noexcept {
        return t_current_guest_tid;
    }

    /// Get thread by ID
    [[nodiscard]] GuestThread* get_thread(u32 thread_id);
    [[nodiscard]] const GuestThread* get_thread(u32 thread_id) const;

    /// Snapshot of live guest thread IDs (for Toolhelp Thread32*).
    [[nodiscard]] std::vector<u32> list_thread_ids() const;

    /// Get thread by handle (via HandleTable)
    [[nodiscard]] GuestThread* get_thread_by_handle(Handle h) {
        return m_handles.get_as<GuestThread>(h, HandleType::Thread);
    }

    /// Open a thread by its ID, returning a new handle with @p access and
    /// optional HANDLE_FLAG_INHERIT. Unknown TID -> NULL_HANDLE.
    [[nodiscard]] Handle open_thread(u32 thread_id, u32 access = HandleAccess::All,
                                     u32 flags = 0);

    /// Last handle to a terminated thread closed: drop the thread object so a
    /// later OpenThread(tid) fails (ERROR_INVALID_PARAMETER), matching Windows.
    void on_thread_handle_close(void* object);

    /// Release a thread handle the runtime created for its own use (pool
    /// callback threads). The thread keeps running; its object is destroyed
    /// when it exits, exactly as after the guest's CloseHandle.
    void close_thread_handle(Handle h) { (void)m_handles.close(h); }

    /// Duplicate a waitable thread handle.
    /// Supports both the current-thread pseudo-handle and existing real thread handles.
    [[nodiscard]] Handle duplicate_thread_handle(Handle source_handle,
                                                 u32 flags = 0,
                                                 u32 access = HandleAccess::All);

    bool set_thread_description(Handle thread_handle, std::u16string description,
                                u32* hresult = nullptr);
    bool set_thread_description_for_id(u32 thread_id, std::u16string description);
    bool get_thread_description(Handle thread_handle, std::u16string& description,
                                u32* hresult = nullptr);

    /// QueueUserAPC — enqueue a user-mode APC for later alertable delivery.
    /// Returns false and sets LastError on invalid handle / null pfn.
    [[nodiscard]] bool queue_user_apc(Handle thread_handle, GuestAddr pfn,
                                      u64 parameter);

    [[nodiscard]] bool has_pending_user_apcs(u32 thread_id) const;

    /// Drain pending user APCs FIFO, invoking @p invoke for each. Returns the
    /// number delivered. Used by alertable waits (WaitForSingleObjectEx, …).
    using UserApcInvoke = std::function<void(GuestAddr pfn, u64 parameter)>;
    usize drain_user_apcs(u32 thread_id, const UserApcInvoke& invoke);

    /// ReadFileEx / WriteFileEx completion routine — queued on the issuing
    /// thread and delivered in the next alertable wait (SleepEx, …).
    /// Learn FileIOCompletionRoutine: (dwErrorCode, dwNumberOfBytesTransfered,
    /// lpOverlapped).
    [[nodiscard]] bool queue_io_completion_apc(GuestAddr pfn, u32 error_code,
                                               u32 bytes_transferred,
                                               GuestAddr overlapped);

    /// WSARecv/WSASend LPWSAOVERLAPPED_COMPLETION_ROUTINE — four args
    /// (dwError, cbTransferred, lpOverlapped, dwFlags) on the issuing thread.
    [[nodiscard]] bool queue_wsa_completion_apc(u32 thread_id, GuestAddr pfn,
                                               u32 error_code,
                                               u32 bytes_transferred,
                                               GuestAddr overlapped,
                                               u32 flags);

    /// Queue a 3-arg alertable APC on a specific guest thread (ReadFileEx,
    /// PTIMERAPCROUTINE). The drain path always passes three ULONG_PTR args.
    /// Called whenever an APC becomes deliverable, so an alertable wait can
    /// end at once rather than on its next poll. Wired to
    /// SyncManager::note_apc_queued by the runtime; unset in tests.
    using ApcWake = std::function<void(u32 thread_id)>;
    void set_apc_wake(ApcWake fn) { m_apc_wake = std::move(fn); }

    [[nodiscard]] bool queue_apc3_for_thread(u32 thread_id, GuestAddr pfn,
                                             u64 arg0, u64 arg1, u64 arg2);
    /// Queue like queue_apc3_for_thread but do NOT wake an alertable wait
    /// yet; the caller calls notify_apc_queued once it has published the
    /// state the APC accompanies. A waitable timer queues its
    /// PTIMERAPCROUTINE, then signals, then wakes: on Windows the issuer's
    /// alertable wait on the timer returns WAIT_OBJECT_0 (never
    /// WAIT_IO_COMPLETION) and the APC is already pending when any wait on
    /// the timer returns (s4_timer_apc_probe, 100/100 x86 and x64).
    [[nodiscard]] bool queue_apc3_for_thread_quiet(u32 thread_id, GuestAddr pfn,
                                                   u64 arg0, u64 arg1, u64 arg2);
    void notify_apc_queued(u32 thread_id);

    [[nodiscard]] bool has_pending_io_completion_apcs(u32 thread_id) const;

    /// argc is 3 (FileIOCompletionRoutine / timer) or 4 (WSA completion).
    using IoCompletionApcInvoke =
        std::function<void(GuestAddr pfn, u64 arg0, u64 arg1, u64 arg2,
                           u64 arg3, u8 argc)>;
    usize drain_io_completion_apcs(u32 thread_id,
                                   const IoCompletionApcInvoke& invoke);

    /// NtReadFile/NtWriteFile PIO_APC_ROUTINE — (ApcContext, IoStatusBlock,
    /// Reserved). Learn NtReadFile: optional APC runs on the issuing thread
    /// in the next alertable wait.
    [[nodiscard]] bool queue_nt_io_apc(GuestAddr pfn, GuestAddr context,
                                       GuestAddr io_status_block);
    /// The same, for a request issued by @p thread_id and completing on
    /// another thread (an asynchronous device request finishing later).
    [[nodiscard]] bool queue_nt_io_apc_to(u32 thread_id, GuestAddr pfn, GuestAddr context,
                                          GuestAddr io_status_block);

    [[nodiscard]] bool has_pending_nt_io_apcs(u32 thread_id) const;

    using NtIoApcInvoke =
        std::function<void(GuestAddr pfn, GuestAddr context,
                           GuestAddr io_status_block)>;
    usize drain_nt_io_apcs(u32 thread_id, const NtIoApcInvoke& invoke);

    /// Deliver every pending APC of all three kinds in the single FIFO order
    /// they were queued in (Learn QueueUserAPC; thread_lifecycle_probe
    /// APC_KINDS order=1R2: user, ReadFileEx completion, user).
    usize drain_all_apcs(u32 thread_id, const UserApcInvoke& user,
                         const IoCompletionApcInvoke& io,
                         const NtIoApcInvoke& nt);

    // -------------------------------------------------------------------------
    // Asynchronous Thread Execution
    // -------------------------------------------------------------------------

    /// Callback type used to launch a guest thread on a real host thread.
    /// Signature:
    ///   fn(tid, start_address, param, second_param, third_param, arg_count)
    /// The callback is responsible for spawning a std::thread.
    ///
    /// Three parameters rather than one because a thread-pool work callback
    /// is `(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK)`, and the pool
    /// runs its callbacks on guest threads through this same path.
    using ThreadRunCallback =
        std::function<void(u32 tid, GuestAddr, GuestAddr, u64, u64, usize)>;

    /// Allocates a new thread's guest stack/TEB/TLS when the thread object is
    /// created -- before CreateThread returns, as on Windows -- and seeds
    /// GuestThread::cpu with the start context (seed_start_context). Returns
    /// false when the allocation fails; CreateThread then fails with
    /// ERROR_NOT_ENOUGH_MEMORY. `release` frees it for a thread that is
    /// terminated before it ever ran.
    using ThreadContextPrepare = std::function<bool(GuestThread&)>;
    using ThreadContextRelease = std::function<void(GuestThread&)>;
    void set_thread_context_callbacks(ThreadContextPrepare prepare,
                                      ThreadContextRelease release) {
        m_prepare_callback = std::move(prepare);
        m_release_callback = std::move(release);
    }

    /// ntdll!RtlUserThreadStart / ntdll!RtlExitUserThread entry addresses in
    /// this process. A new thread's start context is RtlUserThreadStart with
    /// the start routine and parameter in RCX/RDX (x64) or EAX/EBX (x86)
    /// (thread_lifecycle_probe SUSPENDED_START).
    void set_start_thunks(GuestAddr user_thread_start,
                          GuestAddr exit_user_thread) noexcept {
        m_user_thread_start = user_thread_start;
        m_exit_user_thread = exit_user_thread;
    }
    [[nodiscard]] GuestAddr user_thread_start_address() const noexcept {
        return m_user_thread_start;
    }

    /// Seed `cpu` (already holding the thread's stack and TEB) with the
    /// measured Windows start context for `start(parameter)`.
    void seed_start_context(cpu::CpuState& cpu, GuestAddr start,
                            GuestAddr parameter) const;

    /// ntdll!RtlUserThreadStart: call the start routine in RCX/EAX with the
    /// parameter in RDX/EBX; its return value (RAX/EAX) becomes the thread
    /// exit code via the callback sentinel (Windows moves it into
    /// RtlExitUserThread's Status argument).
    void user_thread_start(Marshaler& m) const;

    /// Register the callback used by CreateThread to run thread functions.
    void set_run_callback(ThreadRunCallback callback) noexcept {
        m_run_callback = std::move(callback);
    }

    /// Notified when a guest thread's entry point returns. The thread pool
    /// uses this to know a work callback has completed, which is what lets
    /// WaitForThreadpoolWorkCallbacks return and a closed work object be
    /// freed at the moment Windows frees it.
    using ThreadExitCallback = std::function<void(u32 tid, u32 exit_code)>;
    void set_thread_exit_callback(ThreadExitCallback callback) noexcept {
        m_thread_exit_callback = std::move(callback);
    }

    /// Launch thread function asynchronously via the registered callback.
    void run_async(u32 tid, GuestAddr start_addr, GuestAddr param,
                   u64 second_param = 0, u64 third_param = 0,
                   usize arg_count = 1);

    /// Create a guest thread that starts running `start(parameter)` at once,
    /// exactly as CreateThread without CREATE_SUSPENDED does (created,
    /// marked launched, handed to run_async), and close the creating handle.
    /// For threads the runtime itself starts on the guest's behalf, such as
    /// COM completion and notification threads. False when the thread could
    /// not be created.
    [[nodiscard]] bool start_detached_thread(GuestAddr start, GuestAddr parameter);

    /// Signal that the process is shutting down. Worker threads that are
    /// still running will skip guest-object mutations in exit_thread.
    void begin_shutdown();

    /// Increment active worker thread count (called before thread starts).
    void thread_started() { m_active_threads.fetch_add(1, std::memory_order_acq_rel); }

    /// Decrement active worker thread count (called when thread finishes).
    /// Runs on the finishing host thread as its last guest-visible act, which
    /// marks that host thread as joinable-without-waiting (see
    /// track_host_thread).
    void thread_finished() {
        note_host_thread_done();
        if (m_active_threads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(m_shutdown_mutex);
            m_shutdown_cv.notify_all();
        }
    }

    /// Guest workers still running. Used by teardown to report why it is
    /// terminating rather than joining.
    [[nodiscard]] u32 active_thread_count() const {
        return m_active_threads.load(std::memory_order_acquire);
    }

    /// Wait for all worker threads to finish (with timeout).
    [[nodiscard]] bool wait_for_threads(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_shutdown_mutex);
        return m_shutdown_cv.wait_for(lk, timeout, [this] {
            return m_active_threads.load(std::memory_order_acquire) == 0;
        });
    }

    /// Retain ownership of a host worker. Guest threads must never outlive
    /// RuntimeContext's managers and message queue, so every worker is joined
    /// before teardown; workers that have already called thread_finished()
    /// are joined here, as new ones start, so a thread-churning guest (pool
    /// callbacks and similar) does not keep one unjoined host thread
    /// and its stack per guest thread it ever created.
    void track_host_thread(std::thread thread);

    /// Called by a tracked host worker (not a guest thread) as its last act,
    /// so the next track_host_thread joins it instead of keeping it until
    /// teardown.
    void note_host_thread_done() {
        std::lock_guard<std::mutex> lk(m_host_threads_mutex);
        m_finished_host_threads.push_back(std::this_thread::get_id());
    }

    /// Join every tracked worker after blocking APIs have been woken.
    void join_host_threads();

    /// Snapshot guest/host thread counts for the performance window.
    void snapshot_perf_window();

private:
    /// [present-cpu] meter (VITREO_PRESENT_RATE): register @p thread under
    /// its new description. Caller holds m_mutex.
    void note_present_cpu_name(const GuestThread& thread,
                               const std::u16string& name);

    /// Name the HOST thread after guest thread @p thread_id's new description,
    /// so sample, xctrace and Instruments show it. macOS can name only the
    /// calling thread, so when @p thread_id is another thread the stored
    /// description is all there is. Every way a guest names a thread
    /// (SetThreadDescription, the MSVC 0x406D1388 exception) comes through
    /// set_thread_description / set_thread_description_for_id and so here.
    void apply_host_thread_name(u32 thread_id, const std::u16string& description);

    ApcWake m_apc_wake;
    HandleTable& m_handles;
    mutable std::mutex m_mutex;
    struct UserApc {
        GuestAddr pfn{0};
        u64 parameter{0};
        u64 seq{0};
    };

    std::unordered_map<u32, std::unique_ptr<GuestThread>> m_threads;
    /// Registered service-thread ids -> registration count (under m_mutex).
    std::unordered_map<u32, u32> m_service_threads;
    ServiceThreadIdCallback m_service_thread_id_callback;
    std::unordered_map<u32, std::u16string> m_thread_descriptions;
    std::unordered_map<u32, std::vector<UserApc>> m_user_apcs;
    struct IoCompletionApc {
        GuestAddr pfn{0};
        u64 arg0{0};
        u64 arg1{0};
        u64 arg2{0};
        u64 arg3{0};
        u8 argc{3};
        u64 seq{0};
    };
    std::unordered_map<u32, std::vector<IoCompletionApc>> m_io_completion_apcs;
    struct NtIoApc {
        GuestAddr pfn{0};
        GuestAddr context{0};
        GuestAddr io_status_block{0};
        u64 seq{0};
    };
    u64 m_apc_seq{0};  ///< queue order across the three APC kinds (m_mutex)
    std::unordered_map<u32, std::vector<NtIoApc>> m_nt_io_apcs;
    std::atomic<bool> m_primary_exit_pending{false};
    u32 m_primary_exit_code{0};
    std::atomic<u32> m_last_exit_code{0};
    std::atomic<u32> m_next_tid{1};
    /// Round-robin source of new threads' ideal processors.
    std::atomic<u32> m_next_ideal_processor{1};
    std::atomic<bool> m_shutdown{false};
    std::atomic<u32> m_active_threads{0};
    std::mutex m_shutdown_mutex;
    std::condition_variable m_shutdown_cv;
    std::mutex m_host_threads_mutex;
    std::vector<std::thread> m_host_threads;
    /// Host threads that passed thread_finished() and have not been joined.
    std::vector<std::thread::id> m_finished_host_threads;
    u32 m_main_thread_id{1};
    ThreadRunCallback m_run_callback;
    ThreadContextPrepare m_prepare_callback;
    ThreadContextRelease m_release_callback;
    GuestAddr m_user_thread_start{0};
    GuestAddr m_exit_user_thread{0};
    ThreadExitCallback m_thread_exit_callback;
    ExternalResumeCallback m_external_resume_callback;

    /// Per-host-thread guest TID.
    static inline thread_local u32 t_current_guest_tid = 1;

    /// Bumped by a process-wide scheduling change (SetPriorityClass); a
    /// thread's own level change bumps GuestThread::scheduling_epoch instead.
    std::atomic<u32> m_scheduling_epoch{1};
    std::atomic<u32> m_process_priority_class{0x00000020u};  // NORMAL
    std::atomic<bool> m_process_background{false};
    /// ProcessPowerThrottling EXECUTION_SPEED request (PowerThrottlingQos).
    std::atomic<u8> m_process_power_throttling{0};
    static inline std::atomic<u32> s_host_policy_epoch{1};
    static inline std::atomic<int> s_qos_floor_override{-1};
    static inline thread_local u32 t_host_policy_epoch = 0;
    static inline thread_local ThreadManager* t_scheduling_mgr = nullptr;
    static inline thread_local u32 t_scheduling_epoch = 0;
    static inline thread_local std::shared_ptr<std::atomic<u32>>
        t_thread_epoch_owner;
    static inline thread_local const std::atomic<u32>* t_thread_epoch = nullptr;
    static inline thread_local u32 t_thread_epoch_seen = 0;

    // Used only when no image defaults were supplied (unit tests): the
    // linker defaults, 1 MB reserve / one page committed.
    u64 m_default_stack_reserve{1024 * 1024};
    u64 m_default_stack_commit{0x1000};
};

// =============================================================================
// Thunk Registration
// =============================================================================

/// Register thread thunks into kernel32
DllExports create_thread_thunks(ThreadManager& mgr);

} // namespace vitreo::winapi
