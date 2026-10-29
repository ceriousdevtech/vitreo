// SPDX-License-Identifier: LGPL-2.1-only
/// @file device_lock_profile.hpp
/// @brief `[d3d11-lock]`: how long guest entries hold, and wait for, the one
///        graphics device lock DXGI and D3D11 share (DxgiManager::api_mutex).
///
/// MEASUREMENT INSTRUMENT, log-only. On unless VITREO_D3D11_LOCK_PROFILE=0.
/// Two kinds of line, both on stderr:
///
///   [d3d11-lock] t_ms=<T> held_ms=<H> method=<iface>[<slot>]
///                thread=<name or tidN> waiters_max=<W>
///     one outermost guest entry held the lock for H >= 20 ms (time inside
///     DxgiManager::ApiUnlock and D3D11Manager::PureWorkUnlock windows is not
///     counted); W is the most threads that waited for it at once meanwhile.
///     At most 20 such lines per second; the rest are counted.
///
///   [d3d11-lock] t_ms=<T> window_ms=<D> wait_ms=<S> waits=<N>
///                max_wait_ms=<M> long_holds=<L> suppressed=<X>
///     once per second with any contention: S ms in total over N contended
///     acquisitions, the longest M.
///
/// t_ms is diagnostics::present_cpu::host_mono_ms, the base every meter line
/// shares. Cost when uncontended: one try_lock and two commpage time reads
/// (mach_approximate_time, a few ms of resolution -- ample for a 20 ms
/// threshold); a contended acquisition also reads mach_absolute_time twice.

#pragma once

#include <vitreo/types.hpp>

#include <cstdio>
#include <mutex>

namespace vitreo::winapi::device_lock_profile {

/// False when VITREO_D3D11_LOCK_PROFILE is "0" (read once).
[[nodiscard]] bool enabled() noexcept;

/// Take @p mutex once, counting the wait when another thread holds it.
void lock(std::recursive_mutex& mutex) noexcept;

/// The calling thread just took the lock for an outermost guest entry (it
/// held it through no other entry).
void begin_hold() noexcept;
/// That entry is about to give its hold up; @p iface and @p slot name it.
void end_hold(const char* iface, u32 slot) noexcept;

/// The calling thread gives every hold up for a while (ApiUnlock,
/// PureWorkUnlock) / has taken them back.
void suspend() noexcept;
void resume() noexcept;

// -- Tests ----------------------------------------------------------------

/// Lines to @p out (nullptr: stderr), long-hold threshold @p long_hold_ns
/// (0: 20 ms), and the meter on regardless of the environment.
void configure_for_test(std::FILE* out, u64 long_hold_ns) noexcept;

}  // namespace vitreo::winapi::device_lock_profile
