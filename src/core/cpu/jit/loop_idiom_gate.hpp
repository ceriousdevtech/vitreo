// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom kill switch. Default ON; VITREO_WS2=0 turns the matcher off
// for every block compiled afterwards (the environment is read once).
// force_for_test overrides the environment in-process so a benchmark or a
// differential test can A/B the same build.

#pragma once

#include <atomic>
#include <cstdlib>

#include "vitreo/types.hpp"

namespace vitreo::cpu::jit::loop_idiom {

enum class Force : int {
    None = 0,
    On = 1,
    Off = 2,
    /// On, and the measured minimum-iteration thresholds are ignored (no
    /// inline gate, helper runs for any trip count): differential tests use
    /// it to drive the bulk path at small counts.
    OnNoThreshold = 3,
};

namespace detail {
inline std::atomic<int> g_force{0};
inline bool env_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_WS2");
        return v == nullptr || !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}
}  // namespace detail

/// Successful helper runs, process-wide (relaxed). Tests use the delta to
/// prove the bulk path ran; nothing reads it on a hot path.
inline std::atomic<u64> g_bulk_runs{0};
[[nodiscard]] inline u64 bulk_runs() noexcept {
    return g_bulk_runs.load(std::memory_order_relaxed);
}

inline void force_for_test(Force f) noexcept {
    detail::g_force.store(static_cast<int>(f), std::memory_order_relaxed);
}

[[nodiscard]] inline Force forced() noexcept {
    return static_cast<Force>(detail::g_force.load(std::memory_order_relaxed));
}

[[nodiscard]] inline bool enabled() noexcept {
    switch (forced()) {
    case Force::On:
    case Force::OnNoThreshold: return true;
    case Force::Off: return false;
    case Force::None: break;
    }
    return detail::env_enabled();
}

[[nodiscard]] inline bool thresholds_bypassed() noexcept {
    return forced() == Force::OnNoThreshold;
}

/// RAII Force for tests.
class ScopedForce {
public:
    explicit ScopedForce(Force f) noexcept : m_prev(forced()) { force_for_test(f); }
    ~ScopedForce() { force_for_test(m_prev); }
    ScopedForce(const ScopedForce&) = delete;
    ScopedForce& operator=(const ScopedForce&) = delete;
private:
    Force m_prev;
};

}  // namespace vitreo::cpu::jit::loop_idiom
