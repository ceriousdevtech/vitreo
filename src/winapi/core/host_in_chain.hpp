#pragma once
/// @file host_in_chain.hpp
/// @brief The L3-K switch: in-chain calls of hot kernel32 and CRT exports.
///
/// L3 serves an API crossing from translated code (the in-chain stub, the
/// trampoline, cli::serve_in_chain_thunk) instead of leaving
/// JitCompiler::execute() for the dispatcher. L3 itself covers the D3D11/DXGI
/// direct entries. L3-K adds the exports whose InChainClass comes from a
/// contract table rather than a direct entry -- kernel32's clocks, TLS/FLS,
/// last error, critical sections, SRW locks, SwitchToThread, events, and the
/// CRT math functions (winapi/kernel32/kernel32_in_chain.hpp,
/// msvcrt/math/crt_math.cpp). register_dll gives each such export an
/// in-chain-only entry (DirectThunk::in_chain_only) around the body it
/// already has, and the in-chain crossing runs that body exactly as the
/// dispatcher would have: the export's inline fast path when it has one,
/// else its registered handler with the full path's semantics.
///
/// Default ON. `VITREO_L3K=0` turns it off before any entry is built: the
/// slot map then never marks these exports, so their calls take exactly the
/// path they took before (no stub call-out at all). So does a full-path
/// diagnostic (full_path_diagnostic: VITREO_SLOW_THUNK_HIST,
/// VITREO_TEB_TRACE, VITREO_TRACE_BAD_CALLER), logged once. Under
/// `VITREO_AB_CYCLE_S` the crossing also alternates in-process as the
/// ab-cycle feature "l3k" (cli::serve_in_chain_thunk declines on the off
/// arm).
///
/// Nothing guest-visible depends on the switch: both settings run the same
/// body under the same contract, and both stop the thread before it runs
/// more guest code once the process has exited (the dispatcher tests
/// has_exited() after every thunk; the in-chain crossing after every call,
/// leaving for the chain loop's stop). Only where the crossing is served
/// changes.
#include <vitreo/types.hpp>

#include <atomic>

namespace vitreo::winapi::host_in_chain {

/// The first full-path diagnostic set in the environment (its variable
/// name), else null. Each of them reports per call from the dispatcher's
/// full path, so with one on no in-chain-only entry is built.
[[nodiscard]] const char* full_path_diagnostic() noexcept;

namespace detail {
/// -1 not yet decided; else 0/1. Decided once, from VITREO_L3K and
/// full_path_diagnostic().
extern std::atomic<int> g_enabled;
/// Tests only (Force).
extern std::atomic<int> g_force;
[[nodiscard]] bool decide_enabled() noexcept;
}  // namespace detail

/// Whether register_dll builds in-chain-only entries (and so whether any
/// L3-K export can be served in-chain): the test force, else VITREO_L3K
/// with no full_path_diagnostic().
[[nodiscard]] inline bool enabled() noexcept {
    const int forced = detail::g_force.load(std::memory_order_relaxed);
    if (forced != 0) return forced == 1;
    const int decided = detail::g_enabled.load(std::memory_order_relaxed);
    return decided >= 0 ? decided != 0 : detail::decide_enabled();
}

/// Tests only: pin the switch (None returns to the environment).
enum class Force : int { None = 0, On = 1, Off = 2 };
inline void force_for_test(Force force) noexcept {
    detail::g_force.store(static_cast<int>(force), std::memory_order_relaxed);
}

/// Logs, once per process, that an L3-K export was served in-chain (the line
/// a measurement checks to know the lever was live in the guest it measured).
void note_first_in_chain_call(u32 thread_id, const char* export_name,
                              bool is_64bit) noexcept;

}  // namespace vitreo::winapi::host_in_chain
