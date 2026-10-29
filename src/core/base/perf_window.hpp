// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file perf_window.hpp
/// @brief Inlinable fast path for the opt-in performance window.
///
/// The window's API lives in `<vitreo/diagnostics.hpp>`; this header exposes
/// only the pieces that a *hot* caller must be able to evaluate without
/// leaving its own translation unit.
///
/// Why this exists: `diagnostics::perf_add_helper_mem()` is called on every
/// JIT helper memory access, and its whole body is `if (!active) return;`.
/// An out-of-line cross-TU call costs a real call/return plus (before this
/// header) the guard load of a function-local static, on every guest load
/// and store that goes through a helper. On a live multi-process browser
/// renderer, switched-off diagnostic gates cost more than running translated
/// guest code.
///
/// The state the fast path needs therefore lives at namespace scope, is
/// constant-initialized (no static-init guard, no `_tlv_get_addr`), and is
/// read through the `inline` accessors below, so a disabled window costs one
/// relaxed load and a branch in the caller's own code. The counters are
/// bumped inline too, so the OFF path emits no call at all.
///
/// This is the same shape as `cpu::jit::coverage::enabled()`.

#pragma once

#include <vitreo/types.hpp>

#include <atomic>

namespace vitreo::diagnostics {

namespace perf_window_detail {

/// True while the window is collecting. Namespace-scope and
/// constant-initialized on purpose: see the file comment. Owned by
/// `perf_window.cpp`; only start/stop/reset write it.
extern std::atomic<bool> g_active;

/// Guest memory accesses serviced by a JIT helper (as opposed to an inline
/// linear-memory lowering). Bumped from the hot path, so they live outside
/// the window's big `State` object to keep the increment inlinable.
struct HelperMemCounters {
    std::atomic<u64> reads;
    std::atomic<u64> writes;
    std::atomic<u64> faults;
};
extern HelperMemCounters g_helper_mem;

}  // namespace perf_window_detail

/// Inlinable form of `perf_window_active()`. Identical value; use this in hot
/// paths so the disabled case never calls out of the translation unit.
[[nodiscard]] inline bool perf_window_active_fast() noexcept {
    return perf_window_detail::g_active.load(std::memory_order_relaxed);
}

/// Inlinable form of `perf_add_helper_mem()`. Same counters, same semantics.
inline void perf_add_helper_mem_fast(bool is_write, bool fault) noexcept {
    if (!perf_window_active_fast()) return;
    auto& c = perf_window_detail::g_helper_mem;
    if (is_write) {
        c.writes.fetch_add(1, std::memory_order_relaxed);
    } else {
        c.reads.fetch_add(1, std::memory_order_relaxed);
    }
    if (fault) {
        c.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace vitreo::diagnostics
