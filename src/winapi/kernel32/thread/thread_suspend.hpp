// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file thread_suspend.hpp
/// @brief Cooperative guest-thread suspension + live register capture.
///
/// Windows SuspendThread genuinely stops a thread; GetThreadContext then reads
/// that stopped thread's real register file. Vitreo runs each guest thread on a
/// host thread whose live CpuState lives on that host thread's stack, so a
/// suspending thread cannot simply read it. This registry lets each running
/// guest thread publish a pointer to its live CpuState and poll a park request
/// at basic-block boundaries; SuspendThread flips the request and blocks until
/// the target parks, at which point a consistent snapshot is captured for
/// GetThreadContext. Without this, GetThreadContext had no state to return and
/// handed callers a zeroed CONTEXT (RIP=0), which faults a sampler or
/// watchdog that treats RIP 0 as a real instruction pointer.
///
/// Synchronization uses pthread_mutex/cond exclusively. libc++ `std::mutex::lock`
/// throws `system_error` on EINVAL (destroyed/corrupt mutex), which terminates
/// the process. The registry singleton is intentionally leaked so static destruction cannot
/// yank the slot map from under timer/PublishGuard threads (SIGSEGV in
/// get_or_create on ccb39/ccb41).

#include <vitreo/types.hpp>
#include "core/cpu/state/cpu_state.hpp"

#include <atomic>
#include <memory>
#include <pthread.h>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// Process-global registry of running guest threads keyed by guest TID.
class ThreadSuspendRegistry {
public:
    static ThreadSuspendRegistry& instance() noexcept;

    /// Guest memory used by hang-dump stack/frame walks. Optional; samples
    /// still report RIP/SP/BP/GPRs when unset.
    void bind_guest_memory(GuestMemory* memory) noexcept;
    [[nodiscard]] GuestMemory* guest_memory() const noexcept;

    /// Per-thread control block. Heap-stable (held via shared_ptr) so a
    /// suspender can keep a reference even as the target unpublishes.
    struct Slot {
        pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
        u32 tid = 0;                       ///< guest TID owning this slot
        cpu::CpuState* live = nullptr;   ///< live register file of the runner
        int suspend_depth = 0;           ///< outstanding SuspendThread calls
        bool parked = false;             ///< runner is stopped at a checkpoint
        /// Crossed for every API thunk, so these counters must not require the
        /// slot mutex unless a suspend is actually pending.
        std::atomic<int> host_call_depth{0};
        /// Nested guest callbacks currently running (diagnostic count).
        std::atomic<int> guest_override_depth{0};
        /// host_call_depth when the innermost running guest callback began (0
        /// with none). The thread is blocked in a host call exactly when it is
        /// deeper than this: its innermost activation is then a host call,
        /// wherever the callback was entered from. Counting overrides instead
        /// assumed every callback starts inside a counted host call; one run
        /// from uncounted host code (a host-side message delivery, a fast
        /// thunk) and then blocked read 1/1, "running guest code", and
        /// SuspendThread waited out the whole wait (a title froze this way
        /// on 2026-10-08).
        std::atomic<int> guest_base_depth{0};
        bool ctx_modified = false;       ///< SetThreadContext touched snapshot
        /// SetThreadContext on a thread blocked inside an API: Windows applies
        /// the new context when that call returns, with RAX/EAX holding the
        /// call's result (thread_lifecycle_probe BLOCKED_SET:
        /// ran_before_signal=0, rax_at_redirect=0). Staged here and applied
        /// by the owning thread at its first checkpoint once its host-call
        /// depth drops below `return_ctx_depth`.
        bool return_ctx_pending = false;
        int return_ctx_depth = 0;
        cpu::CpuState return_ctx{};
        cpu::CpuState snapshot{};        ///< consistent state captured at park
        bool snapshot_valid = false;
        /// Active outer publications for this guest TID. More than one means
        /// separate host threads are temporarily executing as the same guest
        /// thread; retain every live state so one publisher cannot invalidate
        /// another publisher's Slot or leave `live` dangling on exit.
        std::vector<cpu::CpuState*> live_publishers;
        /// Cheap mirror of (suspend_depth > 0) for the hot per-iteration poll.
        ///
        /// Two different readers with two different requirements:
        ///  - checkpoint() below polls it RELAXED. A missed poll only defers
        ///    the park to the next basic-block boundary, so eventual visibility
        ///    is enough.
        ///  - enter_host_call()/HostCallScope read it SEQ_CST, paired with the
        ///    seq_cst store in request_suspend. That one is a Dekker
        ///    store-load handshake against host_call_depth and must not be
        ///    weakened to acquire; a missed edge there is not deferred, it
        ///    strands SuspendThread for the whole (possibly unbounded) host
        ///    call. See request_suspend for the full argument.
        std::atomic<int> park_flag{0};
        /// TerminateThread: the runner must leave guest code at its next
        /// checkpoint and never execute another guest instruction. park_flag
        /// stays raised for good so every nested dispatch level unwinds too.
        std::atomic<bool> terminate_flag{false};
        /// The runner reached a checkpoint after terminate_flag was raised.
        bool terminate_acked = false;

        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        ~Slot() {
            (void)pthread_cond_destroy(&cv);
            (void)pthread_mutex_destroy(&mu);
        }

        /// Called by the running thread at every basic-block boundary. Fast
        /// path is a single relaxed load; parks only when a suspend is pending.
        inline void checkpoint() {
            if (park_flag.load(std::memory_order_relaxed) == 0) return;
            park_slow();
        }

    private:
        [[nodiscard]] bool blocked_in_host_call() const noexcept {
            return host_call_depth.load() > guest_base_depth.load();
        }
        void park_slow();
        friend class ThreadSuspendRegistry;
        friend class PublishGuard;
    };

    /// RAII: publish the current thread's live CpuState for the duration of a
    /// dispatch loop. Re-entrant per host thread (nested guest callbacks reuse
    /// the same CpuState), so only the outermost guard owns the registration.
    class PublishGuard {
    public:
        PublishGuard(u32 tid, cpu::CpuState* live);
        ~PublishGuard();
        PublishGuard(const PublishGuard&) = delete;
        PublishGuard& operator=(const PublishGuard&) = delete;

        inline void checkpoint() { if (m_slot) m_slot->checkpoint(); }
        /// True while another thread is waiting in SuspendThread for this one
        /// to park. A dispatch loop that chains JIT blocks without returning
        /// to its checkpoint must leave the chain when this is set: Windows
        /// suspends a thread wherever it is, and a SuspendThread caller waits
        /// with no timeout, so a hot guest loop that never reaches
        /// checkpoint() hangs the caller forever. One relaxed load.
        [[nodiscard]] inline bool suspend_pending() const {
            return m_slot != nullptr &&
                   m_slot->park_flag.load(std::memory_order_relaxed) != 0;
        }
        /// True once TerminateThread targeted this thread. Checked right after
        /// checkpoint(): the dispatch loop must then return to its sentinel
        /// without executing the instruction at RIP.
        [[nodiscard]] inline bool terminate_requested() const {
            return m_slot != nullptr &&
                   m_slot->park_flag.load(std::memory_order_relaxed) != 0 &&
                   m_slot->terminate_flag.load(std::memory_order_acquire);
        }
        void enter_host_call();
        void leave_host_call();

    private:
        std::shared_ptr<Slot> m_slot;
        u32 m_tid = 0;
        cpu::CpuState* m_live = nullptr;
        bool m_owner = false;
        bool m_guest_override = false;
        /// The enclosing callback's guest_base_depth, restored on exit.
        int m_prev_base = 0;
    };

    [[nodiscard]] PublishGuard publish(u32 tid, cpu::CpuState* live) {
        return PublishGuard(tid, live);
    }

    /// RAII: mark the calling thread as blocked in host code without holding a
    /// PublishGuard.
    ///
    /// The thunk dispatcher wraps every handler in
    /// PublishGuard::enter_host_call, which is what lets SuspendThread return
    /// while the target sits in a blocking API. The fast-thunk path services
    /// some of those same APIs (EnterCriticalSection, Sleep, ...) without
    /// reaching a handler, so a thread blocked there looked to a suspender
    /// like one still running guest code that had simply not reached a
    /// checkpoint yet — and request_suspend waited for a park that could never
    /// happen. Every fast path that can block must hold one of these.
    class HostCallScope {
    public:
        HostCallScope() noexcept;
        ~HostCallScope();
        HostCallScope(const HostCallScope&) = delete;
        HostCallScope& operator=(const HostCallScope&) = delete;

    private:
        std::shared_ptr<Slot> m_slot;
    };

    /// SuspendThread: increment suspend depth and (for a running, not-self
    /// thread) block until the target parks so a consistent snapshot exists.
    /// Returns true if the thread is tracked.
    bool request_suspend(u32 tid);

    /// Process teardown: drop every outstanding suspend and wake all parked
    /// threads.
    ///
    /// Windows ends every other thread on ExitProcess without regard to its
    /// suspend count, so a thread the guest left suspended must not be able to
    /// keep the process alive. Without this, a guest that suspends a worker and
    /// then exits leaves that worker parked forever and join_host_threads()
    /// blocks for good (observed in SimCity 4's city load).
    void release_all_for_shutdown();

    /// TerminateThread on another thread (or on the caller itself). Marks the
    /// target so its runner leaves guest code at the next checkpoint, wakes it
    /// if SuspendThread parked it, and, for a running target that is not the
    /// caller, blocks until the target can no longer execute guest code: it
    /// acknowledged at a checkpoint, is blocked inside a host call (it is
    /// redirected when that call returns), or has unpublished.
    void request_terminate(u32 tid);

    /// True once request_terminate targeted @p tid.
    [[nodiscard]] bool is_terminating(u32 tid);

    /// is_terminating for the calling thread, through its own published slot:
    /// one thread-local read and one load, no registry lock. False for a
    /// thread that never published (host-only threads).
    [[nodiscard]] static bool current_thread_terminating() noexcept;

    /// ResumeThread: decrement suspend depth; wake the target at zero.
    void request_resume(u32 tid);

    /// GetThreadContext: copy the target's captured (or best-effort live)
    /// register file into @p out. Returns false if the thread is untracked.
    bool capture(u32 tid, cpu::CpuState& out);

    /// SetThreadContext: stage @p in to be applied to the target on resume
    /// (or written live if it is not currently parked). Returns false if
    /// untracked.
    bool apply(u32 tid, const cpu::CpuState& in);

    /// Maps the API gateway a blocked thread is executing to a guest address
    /// holding that API's return instruction (`ret` / `ret N`), or 0.
    /// GetThreadContext reports a thread blocked in an API AT that
    /// instruction with the stack as the caller left it -- the position of a
    /// Windows thread parked in a system-call stub, one `ret` from its
    /// caller. Resuming there (or a hijack that pushes it as a return
    /// address) completes the call instead of running the API again.
    using ApiReturnResolver = GuestAddr (*)(GuestAddr gateway, bool is_64bit);
    void set_api_return_resolver(ApiReturnResolver resolver) noexcept {
        m_api_return = resolver;
    }

    /// One guest thread's position, for diagnostics only.
    struct ThreadSample {
        u32 tid = 0;
        u64 rip = 0;
        u64 sp = 0;
        u64 bp = 0;
        u64 eax = 0;
        u64 ecx = 0;
        u64 edx = 0;
        u64 ebx = 0;
        u64 esi = 0;
        u64 edi = 0;
        /// x64 callee-saved r12-r15: survive into leaf calls, so a sample that
        /// lands in an allocator still shows the caller's live pointers.
        u64 r12 = 0, r13 = 0, r14 = 0, r15 = 0;
        bool is_64bit = false;
        bool parked = false;
        bool in_host_call = false;
        int suspend_depth = 0;
        /// Raw counters behind in_host_call. A suspender waits for
        /// host_call_depth > guest_base_depth (or a park), so when a
        /// SuspendThread never returns these say why.
        int host_call_depth = 0;
        int guest_override_depth = 0;
        int guest_base_depth = 0;
    };

    /// Best-effort snapshot of where every published guest thread currently is.
    /// The running threads are NOT stopped, so each sample is a torn read of a
    /// register file being mutated concurrently: treat a single sample as a
    /// hint, and compare samples over time to tell a spin from progress. Used
    /// by the hang dump; never use it to make runtime decisions.
    [[nodiscard]] std::vector<ThreadSample> sample_threads();

private:
    ThreadSuspendRegistry() = default;

    std::shared_ptr<Slot> find(u32 tid);
    std::shared_ptr<Slot> get_or_create(u32 tid, cpu::CpuState* live);

    pthread_mutex_t m_map_mu = PTHREAD_MUTEX_INITIALIZER;
    std::unordered_map<u32, std::shared_ptr<Slot>> m_slots;
    GuestMemory* m_memory = nullptr;
    ApiReturnResolver m_api_return = nullptr;

    /// The state a thread blocked in a host call is reported in (see
    /// set_api_return_resolver). Caller holds the slot lock.
    [[nodiscard]] cpu::CpuState blocked_view(const cpu::CpuState& live) const;
};

}  // namespace vitreo::winapi
