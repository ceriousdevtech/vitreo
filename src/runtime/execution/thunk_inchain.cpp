#include "runtime/execution/thunk_inchain.hpp"

#include "runtime/execution/thunk_direct.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "winapi/core/api_profile.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/host_in_chain.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vitreo::cli {

// The JIT indexes the registry's in-chain slot map with its own shift.
static_assert(winapi::ThunkRegistry::kInChainSlotShift ==
                  cpu::jit::kInChainSlotShift,
              "JIT and registry must agree on the slot-map index");

namespace {

using cpu::jit::InChainThunkResult;
using Resume = cpu::jit::JitCodeArena::HostCallSafepoint::Resume;

/// -1 not yet decided, 0 off, 1 on.
std::atomic<int> g_in_chain_allowed{-1};
std::atomic<bool> g_count_in_chain{false};
thread_local u64 t_in_chain_crossings = 0;
thread_local u64 t_in_chain_host_crossings = 0;
/// Every call translated code offered the crossing (each stub call-out,
/// served or declined), for the tests that prove a slot never calls out.
thread_local u64 t_in_chain_offers = 0;
std::atomic<bool> g_first_in_chain_noted{false};
std::atomic<bool> g_first_host_in_chain_noted{false};
/// L3-K's own diagnostic gate: -1 not yet decided, 0 off, 1 on.
std::atomic<int> g_host_in_chain_allowed{-1};

/// VITREO_PERF_WINDOW names a window to open: the same test the JIT's
/// codegen decision makes (non-empty), so the two halves agree.
[[nodiscard]] bool perf_window_requested() noexcept {
    const char* w = std::getenv("VITREO_PERF_WINDOW");
    return w != nullptr && w[0] != '\0';
}

/// Every diagnostic that needs the full dispatch path or counts the
/// crossings it serves: with any of them on, nothing is served in-chain, so
/// what they report is what they reported before. Null when none is on.
/// VITREO_SLOW_HOSTCALL_DIAG is the loop's own (InChainThunkEnv).
[[nodiscard]] const char* in_chain_blocker() noexcept {
    return winapi::api_profile::enabled() ? "VITREO_API_PROFILE"
           : !direct_crossing_allowed()
               ? "a diagnostic that needs the full path (see [wp-r1])"
           : std::getenv("VITREO_ABI_ALIGN_TRAP") != nullptr
               ? "VITREO_ABI_ALIGN_TRAP"
           : perf_window_requested() ? "VITREO_PERF_WINDOW"
           : std::getenv("VITREO_PROFILE") != nullptr ? "VITREO_PROFILE"
                                                       : nullptr;
}

bool decide_in_chain_allowed() noexcept {
    const char* off = in_chain_blocker();
    if (off != nullptr) {
        std::fprintf(stderr,
                     "[l3] in-chain API thunk calls off: %s\n", off);
    }
    const bool allowed = off == nullptr;
    g_in_chain_allowed.store(allowed ? 1 : 0, std::memory_order_relaxed);
    return allowed;
}

/// The full path's own diagnostics for registered handlers, beyond the ones
/// every in-chain crossing honours (decide_in_chain_allowed): with any of them
/// on, no L3-K export is served in-chain, so what they report is what they
/// reported before. Logged once.
[[nodiscard]] const char* host_in_chain_blocker() noexcept {
    return hot_hostcall_diagnostics_enabled() ? "VITREO_HOSTCALL_HOT"
           : std::getenv("VITREO_SLOW_THUNK_HIST") != nullptr
               ? "VITREO_SLOW_THUNK_HIST"
           : std::getenv("VITREO_TRACE_BAD_CALLER") != nullptr
               ? "VITREO_TRACE_BAD_CALLER"
           : std::getenv("VITREO_TEB_TRACE") != nullptr ? "VITREO_TEB_TRACE"
                                                        : nullptr;
}

bool decide_host_in_chain_allowed() noexcept {
    const char* off = host_in_chain_blocker();
    if (off != nullptr) {
        std::fprintf(stderr,
                     "[l3k] in-chain kernel32 exports off: %s\n", off);
    }
    const bool allowed = off == nullptr;
    g_host_in_chain_allowed.store(allowed ? 1 : 0, std::memory_order_relaxed);
    return allowed;
}

[[nodiscard]] bool host_in_chain_allowed() noexcept {
    const int decided =
        g_host_in_chain_allowed.load(std::memory_order_relaxed);
    return decided >= 0 ? decided != 0 : decide_host_in_chain_allowed();
}

/// The entry stub at @p rip runs into its gateway unmodified -- its eight
/// bytes are still the NOP sled register_dll wrote -- so a call to the stub
/// is a call to the export, exactly as maybe_skip_thunk_nop_sled decides.
/// A stub a guest patched (a hook engine, a guest accelerator's jmp) is
/// ordinary guest code and stays the dispatcher's.
[[nodiscard]] bool stub_sled_intact(GuestMemory& memory,
                                    GuestAddr rip) noexcept {
    static_assert(winapi::ThunkRegistry::STUB_SIZE == sizeof(u64));
    const u8* const stub =
        memory.linear_backs(rip, winapi::ThunkRegistry::STUB_SIZE)
            ? memory.linear_host(rip)
            : memory.host_ptr(rip, winapi::ThunkRegistry::STUB_SIZE);
    if (stub == nullptr) return false;
    u64 live = 0;
    std::memcpy(&live, stub, sizeof(live));
    constexpr u64 kUnmodifiedNopSled = 0x9090909090909090ULL;
    return live == kUnmodifiedNopSled;
}

}  // namespace

const char* in_chain_full_path_diagnostic() noexcept {
    const char* off = in_chain_blocker();
    return off != nullptr ? off : host_in_chain_blocker();
}

bool in_chain_thunks_allowed() noexcept {
    const int decided = g_in_chain_allowed.load(std::memory_order_relaxed);
    return decided >= 0 ? decided != 0 : decide_in_chain_allowed();
}

bool in_chain_thunks_possible() noexcept {
    // L3 serves the direct crossing and L3-K the kernel32/CRT exports: with
    // both statically off there is nothing to serve, and codegen should not
    // make every API call pay the stub's slot test.
    return (winapi::com_direct::statically_on() ||
            winapi::host_in_chain::enabled()) &&
           direct_crossing_allowed() && in_chain_thunks_allowed();
}

u64 in_chain_crossings_this_thread() noexcept { return t_in_chain_crossings; }

u64 in_chain_host_crossings_this_thread() noexcept {
    return t_in_chain_host_crossings;
}

u64 in_chain_offers_this_thread() noexcept { return t_in_chain_offers; }

void set_in_chain_crossing_counting(bool on) noexcept {
    g_count_in_chain.store(on, std::memory_order_relaxed);
}

InChainThunkResult serve_in_chain_thunk(cpu::CpuState& cpu, void* env_ptr) {
    auto& env = *static_cast<InChainThunkEnv*>(env_ptr);
    const bool counting = g_count_in_chain.load(std::memory_order_relaxed);
    if (counting) ++t_in_chain_offers;
    if (env.registry == nullptr || env.memory == nullptr ||
        env.marshaler == nullptr || env.suspend_guard == nullptr ||
        env.slow_hostcall_diag || !in_chain_thunks_allowed()) {
        return InChainThunkResult::NotServed;
    }
    // A stub or its gateway, located by shift and mask (16-byte slots).
    const GuestAddr target = static_cast<GuestAddr>(cpu.rip);
    bool is_stub = false;
    const winapi::DirectThunk* direct =
        env.registry->in_chain_candidate(target, is_stub);
    if (direct == nullptr ||
        (is_stub && !stub_sled_intact(*env.memory, target))) {
        return InChainThunkResult::NotServed;
    }
    // Only an export whose contract class lets it resume in-chain.
    if (!winapi::in_chain_eligible(direct->in_chain)) {
        return InChainThunkResult::NotServed;
    }
    const GuestAddr gateway =
        is_stub ? target + winapi::ThunkRegistry::STUB_SIZE : target;
    // Each lever's own switches. L3 IS the direct crossing, so it runs only
    // while that is on; L3-K (an in-chain-only entry) has its own.
    // In-process A/B (VITREO_AB_CYCLE_ONLY=l3 / l3k): the off arm declines.
    const bool host = direct->in_chain_only;
    if (host) {
        // The fast set must be known to tell which body the dispatcher
        // would have run (below).
        if (env.fast_thunks == nullptr || !host_in_chain_allowed() ||
            diagnostics::ab_cycle_off_sampled(env.l3k_sampler, "l3k")) {
            return InChainThunkResult::NotServed;
        }
    } else if (diagnostics::ab_cycle_off_sampled(env.ab_sampler, "l3") ||
               !winapi::com_direct::active()) {
        return InChainThunkResult::NotServed;
    }
    // A thread a SuspendThread is waiting for parks at the dispatcher's
    // checkpoint before it starts another call, as it would today.
    if (env.suspend_guard->suspend_pending()) {
        return InChainThunkResult::NotServed;
    }

    // From here on this is try_chain_thunk_inline's crossing, exit for exit:
    // for a direct entry its direct crossing; for an L3-K export the body it
    // would have run -- the inline fast path when the export has one, else
    // the registered handler with the full path's semantics
    // (run_direct_thunk with DirectThunk::sync_last_error). The skipped NOP
    // sled has no architectural effect.
    cpu.rip = gateway;
    const u32 callbacks = cpu.guest_callback_seq;
    cpu::jit::JitCodeArena::HostCallSafepoint safepoint{env.chain_lease};
    DirectThunkResult result = DirectThunkResult::Returned;
    if (host && fast_thunk_has_body(gateway, *env.fast_thunks)) {
        safepoint.park();
        if (!maybe_fast_thunk_call(cpu, *env.memory, *env.fast_thunks)) {
            // The fast path stepped aside and ran nothing (an unbound
            // manager, a call stream that must see the call): the
            // dispatcher's fast-then-full sequence takes it, as before.
            // The lease was parked meanwhile, though: a retirement then
            // means no compiled pointer the chain loop holds may be trusted
            // (chain_may_resume), so that call goes to the OUTER dispatcher.
            const Resume declined = safepoint.unpark();
            cpu.rip = target;
            return declined == Resume::Continue
                       ? InChainThunkResult::NotServed
                       : InChainThunkResult::BreakOuter;
        }
        if (!finish_thunk_ret(cpu, *env.memory, *env.registry, gateway)) {
            result = DirectThunkResult::Fatal;
        }
        (void)safepoint.unpark();
    } else {
        result = run_direct_thunk(*direct, cpu, *env.memory, *env.marshaler,
                                  *env.suspend_guard, &safepoint);
    }
    // Served: the evidence lines and the test counters.
    if (host) {
        if (!g_first_host_in_chain_noted.load(std::memory_order_relaxed) &&
            !g_first_host_in_chain_noted.exchange(true,
                                                  std::memory_order_relaxed)) {
            const std::string name = env.registry->debug_name_for(gateway);
            winapi::host_in_chain::note_first_in_chain_call(
                winapi::ThreadManager::get_current_thread_id_static(),
                name.c_str(), cpu.is_64bit());
        }
        if (counting) {
            ++t_in_chain_crossings;
            ++t_in_chain_host_crossings;
        }
    } else {
        if (!g_first_in_chain_noted.load(std::memory_order_relaxed) &&
            !g_first_in_chain_noted.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[l3] in-chain API thunk calls on: first call T%u "
                         "slot=%u x%d\n",
                         winapi::ThreadManager::get_current_thread_id_static(),
                         direct->handler.slot, cpu.is_64bit() ? 64 : 86);
        }
        if (counting) {
            ++t_in_chain_crossings;
        }
    }
    // The call took the lease back; its outcome stays in the safepoint.
    const Resume resume = safepoint.resume();
    switch (result) {
    case DirectThunkResult::Returned:
        break;
    case DirectThunkResult::Raising:
        // SEH runs from the outer loop, which must not re-run the handler
        // for the gateway rip still names.
        note_chain_thunk_pending_raise();
        return InChainThunkResult::BreakOuter;
    case DirectThunkResult::ControlTransferred:
        return InChainThunkResult::BreakOuter;
    case DirectThunkResult::Unwinding:
        // rip still names the gateway: the outer loop must act on the unwind,
        // not run the handler again (that re-ran RtlUnwind forever).
        note_chain_thunk_unwinding();
        return InChainThunkResult::BreakOuter;
    case DirectThunkResult::Fatal:
        return InChainThunkResult::Fatal;
    }
    // The process has exited (ExitProcess on this or any thread; a blocked
    // EnterCriticalSection then returns without ownership, Learn's
    // "terminates while exiting"): the dispatcher tests has_exited() after
    // every thunk and stops the thread before more guest code, and so does
    // the chain loop after this BreakOuter.
    if (env.process != nullptr && env.process->has_exited()) {
        return InChainThunkResult::BreakOuter;
    }
    // chain_may_resume: a refused lease or a retirement while parked means
    // no compiled pointer cached across the call may be trusted.
    if (resume != Resume::Continue ||
        (env.chain_lease != nullptr && !*env.chain_lease)) {
        // Rate-limited like the out-of-chain report in guest_run_loop.cpp:
        // soft-budget retirements make these routine.
        static std::atomic<u64> exits{0};
        const u64 n = resume != Resume::Continue
            ? exits.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (n != 0 && (n <= 16 || (n & (n - 1)) == 0)) {
            std::fprintf(stderr,
                         "[safepoint] T%u chain exit resume=%s rip=0x%llx "
                         "(in-chain, exit #%llu)\n",
                         winapi::ThreadManager::get_current_thread_id_static(),
                         resume == Resume::LeaseLost ? "LeaseLost"
                                                     : "Redispatch",
                         static_cast<unsigned long long>(cpu.rip),
                         static_cast<unsigned long long>(n));
        }
        return InChainThunkResult::BreakOuter;
    }
    // Guest code ran inside the export (a guest COM AddRef/Release, a user
    // _matherr): the outer dispatcher takes it from here, as after any guest
    // callback.
    if (cpu.guest_callback_seq != callbacks) {
        return InChainThunkResult::BreakOuter;
    }
    if (env.suspend_guard->suspend_pending()) {
        return InChainThunkResult::Resume;
    }
    return InChainThunkResult::Returned;
}

}  // namespace vitreo::cli
