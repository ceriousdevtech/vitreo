// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - FXSAVE/FXRSTOR 512-byte legacy extended-state image

#pragma once

/// @file fx_image.hpp
/// @brief Marshals CpuState's x87/MMX/SSE file to and from the 512-byte legacy
///        extended-state image (Intel SDM Vol. 2, FXSAVE / FXRSTOR).
///
/// Two callers need this layout and they must agree exactly:
///
///   1. The FXSAVE_M512 / FXRSTOR_M512 interpreter ops, where the image lives
///      in *guest* memory because the guest asked for it.
///   2. The native32 compatibility-mode hand-off, where the image lives in the
///      trampoline page and is written by the *hardware* `fxsave64` on the way
///      out of guest mode and consumed by `fxrstor64` on the way back in.
///
/// A second, separately-written copy of this layout is the failure mode to
/// avoid: the two sides would drift and the guest's FP register file would be
/// silently reinterpreted at the boundary between them.

#include <vitreo/types.hpp>

namespace vitreo::cpu {

struct CpuState;

/// Size of the legacy extended-state image, in bytes.
inline constexpr usize kFxImageBytes = 512;

/// Write `cpu`'s x87/MMX/SSE file into a 512-byte image.
/// @param cpu   Source register file
/// @param image Destination, exactly kFxImageBytes
/// @param wide  true for the FXSAVE64 layout (64-bit FIP/FDP), as produced by a
///              REX.W-prefixed FXSAVE and by the native32 trampoline
/// @param xmm_count Number of XMM registers to transfer (8 outside long mode)
void fx_image_save(const CpuState& cpu, u8* image, bool wide,
                   u8 xmm_count) noexcept;

/// Read a 512-byte image into `cpu`'s x87/MMX/SSE file. Inverse of
/// fx_image_save; see that function for the parameters.
void fx_image_restore(CpuState& cpu, const u8* image, bool wide,
                      u8 xmm_count) noexcept;

/// fx_image_restore without its host side effect: loads the register file
/// only, leaving the calling host thread's accumulated FP exceptions alone.
/// For restoring an image into a CpuState that is NOT the calling thread's
/// running state (e.g. SetThreadContext on a suspended thread); the caller
/// restoring its own state uses fx_image_restore.
void fx_image_restore_registers(CpuState& cpu, const u8* image, bool wide,
                                u8 xmm_count) noexcept;

} // namespace vitreo::cpu
