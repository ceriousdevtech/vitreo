// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: API thunk crossings served inside a JIT chain
// (try_chain_thunk_inline). Split out of guest_run_loop.cpp as verbatim
// code motion.

#include "runtime/execution/guest_run_loop_internal.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "graphics/common/perf_diag.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include "core/base/perf_window.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "runtime/execution/thunk_inchain.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <unistd.h>
#include <vector>

/// In-chain slow thunk completed with pending_raise still at the gateway.
/// Outer thunk path must dispatch SEH without re-running the host handler.
constinit thread_local bool t_chain_slow_thunk_pending_raise = false;
/// In-chain thunk handler requested an unwind (RtlUnwindEx from a host
/// handler, __C_specific_handler): it already ran, and rip still names the
/// gateway. The outer loop must act on pending_unwind without re-running it --
/// a handler that called guest code (an __except filter) would run it twice.
constinit thread_local bool t_chain_slow_thunk_unwinding = false;

namespace vitreo::cli {
// thunk_inchain.hpp: an in-chain (L3) crossing raised, which is exactly the
// direct path's Raising exit from try_chain_thunk_inline.
void note_chain_thunk_pending_raise() noexcept {
    t_chain_slow_thunk_pending_raise = true;
}
void note_chain_thunk_unwinding() noexcept {
    t_chain_slow_thunk_unwinding = true;
}

namespace {

/// After a host call made from a chain: take the lease back and decide
/// whether the chain may continue into compiled code. False means leave
/// through the outer dispatcher, which re-enters at the guest RIP via the
/// normal block lookup: either the gate is closed (lease lost) or a slot was
/// retired while the thread was out, so no compiled pointer cached across the
/// call may be trusted.
[[nodiscard]] bool chain_may_resume(
    cpu::jit::JitCodeArena::HostCallSafepoint& safepoint,
    const cpu::jit::JitCodeArena::ArenaReadLease* chain_lease,
    GuestAddr resume_rip) noexcept {
    using Resume = cpu::jit::JitCodeArena::HostCallSafepoint::Resume;
    const Resume resume = safepoint.unpark();
    if (resume != Resume::Continue) {
        // The only place a chain leaves compiled code because of the arena
        // rather than the guest, so name it: a wrong-execution report that
        // follows one of these lines has a suspect. Every retirement under
        // the arena's soft budget can produce one per parked thread, so the
        // line is rate-limited (first 16, then powers of two) with a count.
        static std::atomic<u64> exits{0};
        const u64 n = exits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16 || (n & (n - 1)) == 0) {
            std::fprintf(stderr,
                         "[safepoint] T%u chain exit resume=%s rip=0x%llx "
                         "(exit #%llu)\n",
                         winapi::ThreadManager::get_current_thread_id_static(),
                         resume == Resume::LeaseLost ? "LeaseLost" : "Redispatch",
                         static_cast<unsigned long long>(resume_rip),
                         static_cast<unsigned long long>(n));
        }
        return false;
    }
    // A chain that arrived here without its lease held has already left the
    // arena; never re-enter compiled code on its behalf.
    return chain_lease == nullptr || static_cast<bool>(*chain_lease);
}

/// Charges the inclusive wall time of one thunk crossing to the path that
/// served it, on whichever of this function's many exits is taken.
///
/// Zero cost when the perf window is off: no clock is read and no counter is
/// touched. That matters here more than almost anywhere else: a live browser
/// renderer crosses this boundary millions of times per minute, and the
/// window's own file comment records switched-off gates outcosting translated
/// guest code.
class ThunkCrossingTimer {
public:
    ThunkCrossingTimer() noexcept
        : m_on(diagnostics::perf_window_active_fast()) {
        if (m_on) m_start = std::chrono::steady_clock::now();
    }
    ThunkCrossingTimer(const ThunkCrossingTimer&) = delete;
    ThunkCrossingTimer& operator=(const ThunkCrossingTimer&) = delete;

    void served_by(diagnostics::PerfThunkKind kind) noexcept { m_kind = kind; }

    ~ThunkCrossingTimer() {
        if (!m_on) return;
        const auto elapsed = std::chrono::steady_clock::now() - m_start;
        diagnostics::perf_add_thunk(
            m_kind,
            static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    .count()));
    }

private:
    bool m_on;
    std::chrono::steady_clock::time_point m_start{};
    diagnostics::PerfThunkKind m_kind{diagnostics::PerfThunkKind::NotServed};
};

} // namespace

/// Before chain `lookup_fast`: if RIP is in the thunk region, skip an
/// unmodified NOP sled and dispatch in-chain (fast or full resolve_handler).
[[nodiscard]] ChainThunkOutcome try_chain_thunk_inline(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    const winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard,
    const FastThunkAddrs& fast_thunks,
    cpu::jit::BlockCache& block_cache,
    cpu::jit::BasicBlock*& block,
    winapi::ModuleManager* mods,
    cpu::jit::JitCodeArena::ArenaReadLease* chain_lease,
    bool slow_hostcall_diag,
    u64 slow_hostcall_threshold_ms) {
    if (!thunk_registry.is_in_thunk_region(cpu.rip)) {
        return ChainThunkOutcome::NotThunk;
    }
    // Only crossings that reached the thunk region are timed; the test above
    // runs on every chain step and is not a crossing.
    ThunkCrossingTimer crossing;
    // Microsoft x64 ABI: RSP is 16-byte aligned at every call site, so a
    // callee entered by CALL always sees RSP ≡ 8 (mod 16). A thunk is reached
    // by a guest CALL, so anything else means this thread's stack is already
    // shifted, and every aligned XMM spill the guest's compiler emitted at a
    // fixed frame offset will #GP. Report the first violation per thread: the
    // return address names the call site that inherited the bad alignment.
    //
    // DIAGNOSTIC (VITREO_ABI_ALIGN_TRAP, the same switch as the per-CALL
    // check in ops64.cpp): tracking the last aligned crossing reads [rsp]
    // from guest memory and updates four thread_locals on every API-thunk
    // crossing, ~1.85M/s in a game, so it runs only when that trap is on.
    static const bool abi_align_trap =
        std::getenv("VITREO_ABI_ALIGN_TRAP") != nullptr;
    if (abi_align_trap && cpu.is_64bit()) {
        static thread_local bool align_violation_reported = false;
        static thread_local u64 last_aligned_thunk_rip = 0;
        static thread_local u64 last_aligned_return_to = 0;
        static thread_local u64 last_aligned_rsp = 0;
        static thread_local u64 aligned_thunk_calls = 0;
        if ((cpu.sp() & 0xFu) == 8u) {
            last_aligned_thunk_rip = cpu.rip;
            last_aligned_return_to = memory.read_u64(cpu.sp()).value_or(0);
            last_aligned_rsp = cpu.sp();
            ++aligned_thunk_calls;
        } else if (!align_violation_reported) {
            align_violation_reported = true;
            // The last correctly aligned call bounds the window the shift
            // happened in; without it the violation names only where the
            // damage surfaced.
            std::fprintf(stderr,
                "[abi-align] VIOLATION x64 thunk rsp=0x%llx mod16=%llu "
                "thunk_rip=0x%llx return_to=0x%llx\n"
                "[abi-align]   last good: rsp=0x%llx thunk_rip=0x%llx "
                "return_to=0x%llx (after %llu aligned calls)\n",
                static_cast<unsigned long long>(cpu.sp()),
                static_cast<unsigned long long>(cpu.sp() & 0xFu),
                static_cast<unsigned long long>(cpu.rip),
                static_cast<unsigned long long>(
                    memory.read_u64(cpu.sp()).value_or(0)),
                static_cast<unsigned long long>(last_aligned_rsp),
                static_cast<unsigned long long>(last_aligned_thunk_rip),
                static_cast<unsigned long long>(last_aligned_return_to),
                static_cast<unsigned long long>(aligned_thunk_calls));
        }
    }
    maybe_skip_thunk_nop_sled(cpu, memory, thunk_registry);
    // Control Flow Guard dispatch (Microsoft Learn, Control Flow Guard):
    // `__guard_dispatch_icall_fptr` takes the call target in RAX, validates it,
    // and TAIL-JUMPS to it -- it does not return, so the return address the
    // `call [fptr]` pushed stays on the stack for the target's own RET.
    // Vitreo maintains no CFG bitmap, so the validation is a no-op and the
    // whole export is "RIP := RAX".
    //
    // A CFG-compiled image routes EVERY indirect call through this pointer, so
    // paying the full marshaling path for it is ruinous: measured on a
    // browser helper, 6,545,925 of 8,500,000 slow thunk dispatches (77%)
    // were this one export. Serve it here, inside the chain, so it costs a
    // register move instead of a handler resolve, a hostcall snapshot, a
    // Marshaler and a std::string.
    if (cpu.is_64bit() && cpu.rip != 0 &&
        cpu.rip == thunk_registry.guard_dispatch_icall_address()) {
        // DIAGNOSTIC (VITREO_TRAP_NULL_ICALL=1): a CFG-compiled image routes
        // EVERY indirect call through here, so a wrong RAX at this point is a
        // jump to an arbitrary address, and RAX==0 lands on address zero,
        // whose padding decodes as an illegal instruction. That is the shape
        // of a null indirect-call target. Naming the source of the bad target
        // separates a miscompiled indirect call from a stale RAX left in
        // CpuState because a pin was not spilled.
        if (cpu.gpr.rax == 0) {
            static const bool trap_null =
                std::getenv("VITREO_TRAP_NULL_ICALL") != nullptr;
            if (trap_null) {
                std::fprintf(stderr,
                    "[icall-null] __guard_dispatch_icall with RAX=0 "
                    "rsp=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx r10=0x%llx "
                    "r11=0x%llx\n",
                    static_cast<unsigned long long>(cpu.gpr.rsp),
                    static_cast<unsigned long long>(cpu.gpr.rbx),
                    static_cast<unsigned long long>(cpu.gpr.rcx),
                    static_cast<unsigned long long>(cpu.gpr.rdx),
                    static_cast<unsigned long long>(cpu.gpr.r10),
                    static_cast<unsigned long long>(cpu.gpr.r11));
                // The return address the caller pushed still sits at [rsp];
                // it names the guest call site that produced this target.
                for (int i = 0; i < 4; ++i) {
                    const auto w = memory.read_u64(
                        static_cast<GuestAddr>(cpu.gpr.rsp) +
                        static_cast<GuestAddr>(i) * 8);
                    std::fprintf(stderr, "[icall-null]   [rsp+%2d] = 0x%llx\n",
                                 i * 8,
                                 static_cast<unsigned long long>(
                                     w.is_ok() ? w.value() : 0));
                }
                std::fflush(stderr);
            }
        }
        cpu.rip = cpu.gpr.rax;
        diagnostics::perf_note_chain_thunk_inline_fast();
        crossing.served_by(diagnostics::PerfThunkKind::GuardIcall);
        block = block_cache.lookup_fast(cpu.rip);
        return ChainThunkOutcome::Handled;
    }

    if (!thunk_registry.is_dispatch_address(cpu.rip)) {
        // A sled patched with `jmp rel32` is ordinary guest code, not a
        // dispatch point: guest_accel.cpp overwrites the entry stub of the
        // accelerated CRT and critical-section exports so the call lands in
        // real x86 instead of a host thunk.
        //
        // Breaking out to the outer interpreter for that one jump costs a
        // chain break, a single interpreted step and a re-entry on EVERY such
        // call. Measured on one load: the memcpy slot alone executed
        // 4,068,195 times as a 1-instruction non-JIT block, and the
        // accelerators together account for ~30M calls -- which is why they
        // moved Intel from 55s to 15s but bought nothing on the JIT path.
        // Letting the JIT compile through the jump removes all of it.
        //
        // Only a jmp is treated this way. An unmodified or partially executed
        // sled must still break out, because compiling through it would run
        // into the dispatch gateway as if it were code.
        const auto first_byte = memory.read_u8(cpu.rip);
        if (first_byte.is_ok() && first_byte.value() == 0xE9u) {
            return ChainThunkOutcome::NotThunk;
        }
        // Patched with something else, or mid-sled — outer interpreter owns it.
        // DIAGNOSTIC (VITREO_THUNK_MISS_BYTES=1): 19.7M of these per 32-bit
        // title load, topped by memset / Enter+LeaveCriticalSection / _setjmp3 --
        // all of which guest_accel.cpp is supposed to have patched with a
        // `jmp rel32` so the branch above sends them to the JIT. Record what
        // is actually at the RIP, once per distinct address, instead of
        // guessing which stage left it looking like this.
        {
            static const bool log_bytes =
                std::getenv("VITREO_THUNK_MISS_BYTES") != nullptr;
            if (log_bytes) {
                static std::mutex bmu;
                static std::unordered_map<u32, u64> seen;
                std::lock_guard<std::mutex> lk(bmu);
                auto& n = seen[static_cast<u32>(cpu.rip)];
                if (n++ == 0) {
                    u8 b[4] = {0, 0, 0, 0};
                    for (int i = 0; i < 4; ++i) {
                        const auto r = memory.read_u8(cpu.rip +
                                                      static_cast<GuestAddr>(i));
                        b[i] = r.is_ok() ? r.value() : 0xFFu;
                    }
                    std::fprintf(stderr,
                        "[thunk-miss] rip=0x%08llx bytes=%02x %02x %02x %02x "
                        "dispatch_target=0x%08llx\n",
                        (unsigned long long)cpu.rip, b[0], b[1], b[2], b[3],
                        (unsigned long long)
                            thunk_registry.stub_dispatch_target(cpu.rip));
                }
            }
        }
        diagnostics::perf_note_chain_miss_thunk();
        return ChainThunkOutcome::BreakOuter;
    }

    // RIP is a live dispatch gateway, so from here on a host handler runs.
    // Every host call is a safepoint: the chain lease is parked for its
    // duration and taken back before any compiled pointer is looked up again
    // (JitCodeArena::HostCallSafepoint). The fast set includes waits — Sleep,
    // WaitForSingleObject, EnterCriticalSection, the SRW acquires,
    // SleepConditionVariableCS — that park the thread for arbitrarily long.
    // Holding the lease across them pinned the arena for as long as any
    // thread stayed parked, and an exclusive-mode title always had one, so no
    // slot could ever be retired and every block compiled after the arena
    // filled ran interpreted. The patched-jmp and guard-dispatch paths above
    // run no host code and pay nothing here; the fast set only ever matches a dispatch
    // gateway, so checking it after the sled test loses nothing.
    cpu::jit::JitCodeArena::HostCallSafepoint safepoint{chain_lease};
    safepoint.park();
    // The direct COM crossing (thunk_direct.hpp), checked before the fast
    // set: a D3D11/DXGI method is never in it, and this is the RHI thread's
    // hottest crossing. The exits are the slow path's below, case by case.
    if (const winapi::DirectThunk* direct =
            direct_thunk_for(thunk_registry, cpu.rip, slow_hostcall_diag)) {
        const DirectThunkResult result = run_direct_thunk(
            *direct, cpu, memory, marshaler, suspend_guard, &safepoint);
        const bool chain_resumable =
            chain_may_resume(safepoint, chain_lease, cpu.rip);
        if (result == DirectThunkResult::Fatal) {
            return ChainThunkOutcome::Fatal;
        }
        crossing.served_by(diagnostics::PerfThunkKind::Direct);
        diagnostics::perf_note_chain_thunk_inline_direct();
        if (result == DirectThunkResult::Raising) {
            // SEH runs from the outer loop, which must not re-run the
            // handler for the gateway rip still names.
            t_chain_slow_thunk_pending_raise = true;
            return ChainThunkOutcome::BreakOuter;
        }
        if (result == DirectThunkResult::Unwinding) {
            t_chain_slow_thunk_unwinding = true;
            return ChainThunkOutcome::BreakOuter;
        }
        if (!chain_resumable) {
            return ChainThunkOutcome::BreakOuter;
        }
        block = block_cache.lookup_fast(cpu.rip);
        return ChainThunkOutcome::Handled;
    }
    if (maybe_fast_thunk_call(cpu, memory, fast_thunks)) {
        const GuestAddr thunk_addr = cpu.rip;
        if (!finish_thunk_ret(cpu, memory, thunk_registry, thunk_addr)) {
            return ChainThunkOutcome::Fatal;
        }
        diagnostics::perf_note_chain_thunk_inline_fast();
        crossing.served_by(diagnostics::PerfThunkKind::Fast);
        if (!chain_may_resume(safepoint, chain_lease, cpu.rip)) {
            return ChainThunkOutcome::BreakOuter;
        }
        block = block_cache.lookup_fast(cpu.rip);
        return ChainThunkOutcome::Handled;
    }

    // VITREO_SLOW_THUNK_HIST=1: which exports actually take the full
    // marshaling path in a live guest, by count. The aggregate fast/slow
    // counters say only how many slow dispatches happened, never which export
    // caused them -- and the answer is routinely dominated by one entry that
    // belongs on the fast path. This is how __guard_dispatch_icall was found
    // at 6,545,925 of 8,500,000 slow dispatches. Zero cost when unset.
    {
        static const bool hist_on =
            std::getenv("VITREO_SLOW_THUNK_HIST") != nullptr;
        if (hist_on) {
            static std::mutex hmu;
            static std::unordered_map<GuestAddr, u64> hist;
            static u64 total = 0;
            static const u64 every = [] {
                const char* v = std::getenv("VITREO_SLOW_THUNK_HIST_EVERY");
                const u64 n = v != nullptr ? std::strtoull(v, nullptr, 0) : 0;
                return n != 0 ? n : u64{500000};
            }();
            std::lock_guard<std::mutex> lk(hmu);
            ++hist[cpu.rip];
            if (++total % every == 0) {
                std::vector<std::pair<GuestAddr, u64>> rows(hist.begin(),
                                                            hist.end());
                std::sort(rows.begin(), rows.end(),
                          [](const auto& a, const auto& b) {
                              return a.second > b.second;
                          });
                std::fprintf(stderr,
                             "[slow-thunk-hist pid=%d] total=%llu distinct=%zu\n",
                             static_cast<int>(::getpid()),
                             static_cast<unsigned long long>(total),
                             rows.size());
                for (usize i = 0; i < rows.size() && i < 25; ++i) {
                    std::fprintf(stderr,
                                 "[slow-thunk-hist pid=%d]   %-52s %llu\n",
                                 static_cast<int>(::getpid()),
                                 thunk_registry.debug_name_for(rows[i].first)
                                     .c_str(),
                                 static_cast<unsigned long long>(rows[i].second));
                }
                std::fflush(stderr);
            }
        }
    }

    const auto slow = dispatch_slow_api_thunk(
        cpu, memory, thunk_registry, marshaler, suspend_guard, fast_thunks,
        mods, &safepoint, slow_hostcall_diag, slow_hostcall_threshold_ms,
        /*direct_checked=*/true);
    // Lease refused, or a slot retired underneath the call: the chain must
    // not continue into compiled code. Unresolved/Fatal never ran a handler;
    // they leave the chain regardless.
    const bool chain_resumable = chain_may_resume(safepoint, chain_lease, cpu.rip);
    // Ok / ControlTransferred both ran a handler, so both are slow crossings.
    // Fatal and Unresolved never did and stay NotServed.
    if (slow == SlowThunkDispatch::Ok ||
        slow == SlowThunkDispatch::ControlTransferred) {
        crossing.served_by(diagnostics::PerfThunkKind::Slow);
    }
    switch (slow) {
    case SlowThunkDispatch::Ok:
        // RaiseException: helper left rip at the gateway. Break outer so SEH
        // can run; do not RET here (x86 SEH lives only on the thunk path) and
        // do not re-dispatch the host handler.
        if (cpu.pending_raise.active) {
            t_chain_slow_thunk_pending_raise = true;
            diagnostics::perf_note_chain_thunk_inline_slow();
            return ChainThunkOutcome::BreakOuter;
        }
        diagnostics::perf_note_chain_thunk_inline_slow();
        if (!chain_resumable) {
            return ChainThunkOutcome::BreakOuter;
        }
        block = block_cache.lookup_fast(cpu.rip);
        return ChainThunkOutcome::Handled;
    case SlowThunkDispatch::ControlTransferred:
        diagnostics::perf_note_chain_thunk_inline_slow();
        // pending_unwind / longjmp / lost lease / retired code: leave the
        // chain.
        if (cpu.pending_unwind.active) {
            t_chain_slow_thunk_unwinding = true;
            return ChainThunkOutcome::BreakOuter;
        }
        if (!chain_resumable) {
            return ChainThunkOutcome::BreakOuter;
        }
        block = block_cache.lookup_fast(cpu.rip);
        return ChainThunkOutcome::Handled;
    case SlowThunkDispatch::Fatal:
        return ChainThunkOutcome::Fatal;
    case SlowThunkDispatch::Unresolved:
        return ChainThunkOutcome::BreakOuter;
    }
    return ChainThunkOutcome::BreakOuter;
}

}  // namespace vitreo::cli
