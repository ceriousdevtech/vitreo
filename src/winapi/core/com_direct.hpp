#pragma once

/// @file com_direct.hpp
/// @brief The WP-R1 "crossing diet" switch: the direct COM crossing
/// (ThunkRegistry::direct_thunk, cli::run_direct_thunk) and the D3D11
/// per-call frontend cuts it pays for, behind one switch.
///
/// Default ON. `VITREO_D3D11_COM_DIRECT=0` turns ALL of it off -- every
/// D3D11/DXGI COM crossing then takes the full marshaled path, and the
/// frontend runs its original per-call lookups -- for bisection and for the
/// off arm of a cross-run A/B. Under `VITREO_AB_CYCLE_S` the switch also
/// alternates in-process as the ab-cycle feature "wp-r1" (set
/// `VITREO_AB_CYCLE_ONLY=wp-r1` so no other cycled change toggles with it).
///
/// Nothing guest-visible depends on the switch: both settings implement the
/// same Windows contract (see run_direct_thunk for the crossing, and each cut
/// in the D3D11 frontend for why it is equivalent).

#include <vitreo/types.hpp>

#include <atomic>

namespace vitreo::winapi::com_direct {

namespace detail {

struct Config {
    /// False under VITREO_D3D11_COM_DIRECT=0.
    bool enabled{false};
    /// VITREO_AB_CYCLE_S is set: the arm is sampled per thread.
    bool cycled{false};
};

/// Read once from the environment at static initialisation. Zero-initialised
/// (off) for any code that runs before it; no D3D11 call happens that early.
extern const Config g_config;

/// Tests only (Force).
extern std::atomic<int> g_force;

/// The ab-cycle arm of "wp-r1" for the calling thread, re-read every 1024
/// calls of that thread (diagnostics::ab_cycle_off_sampled).
[[nodiscard]] bool cycled_on() noexcept;

}  // namespace detail

/// True while the lever runs on the calling thread.
///
/// Off the cycle this is two loads of constants and a predictable branch; it
/// is consulted a few times per D3D11 call.
[[nodiscard]] inline bool active() noexcept {
    const int forced = detail::g_force.load(std::memory_order_relaxed);
    if (forced != 0) return forced == 1;
    if (!detail::g_config.enabled) return false;
    return !detail::g_config.cycled || detail::cycled_on();
}

/// The switch without its in-process cycle: the test force, else
/// VITREO_D3D11_COM_DIRECT. A crossing still asks active() per call.
[[nodiscard]] inline bool statically_on() noexcept {
    const int forced = detail::g_force.load(std::memory_order_relaxed);
    if (forced != 0) return forced == 1;
    return detail::g_config.enabled;
}

/// Tests only: pin the switch (None returns to the environment).
enum class Force : int { None = 0, On = 1, Off = 2 };
inline void force_for_test(Force force) noexcept {
    detail::g_force.store(static_cast<int>(force), std::memory_order_relaxed);
}

/// Logs, once per process, that a direct crossing ran (the line a
/// measurement checks to know the lever was live in the guest it measured).
void note_first_direct_crossing(u32 thread_id, u32 slot, bool is_64bit) noexcept;

}  // namespace vitreo::winapi::com_direct
