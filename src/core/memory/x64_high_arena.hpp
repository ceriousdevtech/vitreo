// SPDX-License-Identifier: LGPL-2.1-only

/// @file x64_high_arena.hpp
/// @brief Host-validated placement for the x64 guest's high arenas.
///
/// A 64-bit guest needs three Vitreo-owned regions near the top of its user
/// window: dynamically loaded DLLs, the thunk instruction region, and the
/// synthetic system-module arena. Windows puts its system DLLs up there, so a
/// guest that reasons about module addresses sees a plausible layout.
///
/// Those guest addresses were hard-coded at 0x7FFA/0x7FFB/0x7FFC, which is
/// only safe while the HOST leaves that range alone. It does on arm64 macOS.
/// It does not on x86_64 macOS: the dyld shared cache and every system dylib
/// occupy 0x7FF8'00000000 through roughly 0x7FFC'FFFFFFFF, so each region maps
/// straight onto memory the host already owns, every claim fails, and the
/// guest faults on the first system-module reference.
///
/// The guest-visible user ceiling is a Windows contract and does not move.
/// Where inside the host address space Vitreo places its own bookkeeping is
/// not, so the anchor below is probed against the live host map and stepped
/// down until it lands in free space.

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::memory {

/// Preferred anchor: the historical 0x7FFA base, kept so a host that leaves
/// the range free (arm64 macOS) gets bit-identical placement to before.
inline constexpr u64 kX64HighArenaDefaultBase = 0x00007FFA'00000000ULL;

/// Spacing between the three regions, and the step used when relocating.
inline constexpr u64 kX64HighArenaStride = 0x0000'0001'00000000ULL; // 4 GiB

/// Total span that must be free: dynamic + thunk + synthetic.
inline constexpr u64 kX64HighArenaSpan = 3 * kX64HighArenaStride;   // 12 GiB

/// Lowest anchor worth trying before giving up and returning the default.
inline constexpr u64 kX64HighArenaFloor = 0x0000'6000'00000000ULL;

/// Headroom below the end of a guest-base reservation at which the arenas are
/// anchored when they live inside it: dynamic DLLs at end-24 GiB, thunks at
/// -20 GiB, synthetic modules at -16 GiB -- the same relative shape the
/// arenas have below the 128 TiB Windows ceiling.
inline constexpr u64 kX64HighArenaWindowHeadroom = 6 * kX64HighArenaStride;

/// Smallest guest-base reservation the arenas are placed inside: the
/// smallest window the x64 linear model ever reserves (2^39).
inline constexpr u64 kX64HighArenaMinWindow = u64{1} << 39;

/// Declare the x64 guest-base reservation as [0, window_bytes), so the arenas
/// are anchored INSIDE it and every DLL, thunk and synthetic-module byte is
/// reservation-backed (reachable by the JIT's direct access path). Zero, or
/// never calling this, keeps the host-probed placement below. Must run before
/// the first x64_high_arena_base() query, i.e. during process construction;
/// a later call has no effect on the frozen anchor.
///
/// This moves where Vitreo loads relocatable DLLs and its own synthetic
/// modules. It does NOT change the guest-visible user ceiling
/// (lpMaximumApplicationAddress stays 0x7FFF'FFFEFFFF): Windows assigns no
/// contract to where a relocated DLL loads.
void set_x64_high_arena_window(u64 window_bytes) noexcept;

/// The anchor x64_high_arena_base() returns for a reservation of
/// `window_bytes` (0 = none): end-of-window minus the headroom when the
/// window is large enough, otherwise the host-probed placement.
[[nodiscard]] u64 x64_high_arena_anchor_for(u64 window_bytes) noexcept;

/// Anchor for the three high arenas, validated against the host address map.
///
/// Returns the base of a `kX64HighArenaSpan` window that no host mapping
/// intersects, preferring `kX64HighArenaDefaultBase` and stepping down by
/// `kX64HighArenaStride` while the preferred window is occupied. The result is
/// computed once and cached: the arenas are placed during process construction
/// and every later caller must agree with that placement.
///
/// If no free window exists down to `kX64HighArenaFloor` the default is
/// returned unchanged, so the failure surfaces as the same claim error as
/// before rather than as a silently different layout.
[[nodiscard]] u64 x64_high_arena_base() noexcept;

/// Region bases derived from the anchor. The order matches the original
/// constants: dynamic DLLs lowest, then thunks, then the synthetic arena.
[[nodiscard]] inline u64 x64_dynamic_base() noexcept {
    return x64_high_arena_base();
}
[[nodiscard]] inline u64 x64_thunk_base() noexcept {
    return x64_high_arena_base() + kX64HighArenaStride;
}
[[nodiscard]] inline u64 x64_synthetic_arena_base() noexcept {
    return x64_high_arena_base() + 2 * kX64HighArenaStride;
}

} // namespace vitreo::memory
