// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - guest-side implementations of hot CRT primitives

#pragma once

/// @file guest_accel.hpp
/// @brief Runs the hottest CRT primitives as real x86 in the guest instead of
///        as host thunks.
///
/// Every imported function costs a guest->host transition: a compatibility-mode
/// exit on native32, a JIT->host call otherwise. That is the right trade for an
/// API that does real work. It is the wrong trade for `memset` — measured at
/// **9.5 million calls in two minutes on one thread** of one load, where
/// the transition costs far more than the handful of instructions it guards.
/// See the profile in docs and `VITREO_NATIVE32_PROFILE`.
///
/// The mechanism exploits the shape the thunk arena already has. Each 16-byte
/// slot is an 8-byte entry sled followed by a dispatch gateway at
/// `slot + STUB_SIZE`; the address handed to the guest's IAT is the slot base.
/// Writing a `jmp rel32` over the sled diverts the call to real guest code
/// before it ever reaches the gateway, and both execution paths honour it:
/// native32 simply executes the jump, and the interpreter's NOP-sled
/// fast-forward deliberately skips only *unmodified* sleds.
///
/// Only locale-independent, semantically unambiguous primitives belong here.
/// `tolower` and friends are excluded on purpose: their behaviour depends on
/// the guest's current locale, which lives in host state.

#include <vitreo/types.hpp>
#include <vitreo/result.hpp>

#include <string>
#include <utility>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class ThunkRegistry;

/// Guest address of the accelerator code arena (x86 guests). Sits directly
/// above the thunk arena so the whole synthetic block stays contiguous at the
/// top of the user window.
constexpr GuestAddr CRT_ACCEL_BASE_X86 = 0x7D200000;
constexpr usize CRT_ACCEL_SIZE = 0x1000;

/// The emitted code plus the guest entry point of each function.
struct CrtAccelBlob {
    std::vector<u8> code;
    std::vector<std::pair<std::string, u32>> entries;  ///< name -> guest address
    /// rel32 slots that must be patched with the address of a named export
    /// once the thunk arena has assigned one: {byte offset in `code`,
    /// "dll.dll!Export"}. Used by the critical-section fast paths to tail-jump
    /// into their blocking half.
    std::vector<std::pair<usize, std::string>> extern_fixups;
};

/// Build the accelerator code as if it were loaded at @p base. Exposed so the
/// hand-written machine code can be executed and checked against the C
/// semantics it claims to implement, rather than trusted by inspection.
[[nodiscard]] CrtAccelBlob build_guest_crt_code(
    GuestAddr base, bool include_critical_sections = false);

/// Emit the guest-side primitives and repoint their msvcrt exports at them.
///
/// Must run *after* whatever writes the thunk arena's entry stubs, because it
/// overwrites some of them. Safe and MEANINGFUL to call more than once: the
/// code arena is built once, but the sled patches are re-applied every time, so
/// a path that rewrites the arena (native32_prepare does) can restore them by
/// calling again.
/// Failure is not fatal: every accelerated function keeps working through its
/// existing host thunk, just slower.
///
/// @return the number of exports successfully redirected
usize install_guest_crt_accelerators(GuestMemory& memory,
                                     ThunkRegistry& registry) noexcept;

// -----------------------------------------------------------------------------
// x64 guests
// -----------------------------------------------------------------------------
//
// The x64 thunk arena is never JIT-compiled, so a patched sled would still cost
// a dispatcher round trip per call. Instead each accelerated export gets a
// guest implementation in the thunk registry (ThunkRegistry::
// set_guest_implementation): import binding and GetProcAddress hand out the
// code's address, and calls link like any guest function. No sled is patched,
// so a guest that reaches the thunk through the export table still gets the
// host implementation, and an inline-hook check on a resolved function sees
// ordinary code. Measured at the menu (VITREO_API_PROFILE): about
// 860k calls/s to these exports, each a ~50-90 ns round trip for a few
// instructions of work (tools/stress/sc_apicall: 43-62x native).

/// Offset of the x64 accelerator arena above the thunk base: inside the thunk
/// region's own 4 GiB stride (x64_high_arena.hpp), past every thunk slot.
constexpr GuestAddr X64_ACCEL_OFFSET = 0x10000000;
constexpr usize X64_ACCEL_SIZE = 0x10000;

/// Build the x64 accelerator code as if it were loaded at @p base. Exposed so
/// the bytes can be executed and checked against the contracts they implement.
/// Entries are named "dll.dll!Export" and hold the OFFSET from @p base (an x64
/// address does not fit their u32); several names may share one offset.
[[nodiscard]] CrtAccelBlob build_guest_x64_code(GuestAddr base);

/// Map the x64 accelerator code and register it as the guest implementation of
/// its exports. Must run before imports are bound. Failure is not fatal: every
/// export keeps working through its host thunk.
/// @return the number of exports given a guest implementation
usize install_guest_x64_accelerators(GuestMemory& memory,
                                     ThunkRegistry& registry) noexcept;

} // namespace vitreo::winapi
