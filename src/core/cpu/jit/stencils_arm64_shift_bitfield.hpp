// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_shift_bitfield.hpp
/// @brief ARM64 shift, rotate, extract, bitfield-move and extend encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 SBFM Xd, Xn, #immr, #imms (64-bit signed bitfield move).
constexpr u32 sbfm_x(u8 d, u8 n, u64 immr, u64 imms) noexcept {
    return 0x93400000u | (uimm_field("sbfm_x", immr, 63) << 16)
                       | (uimm_field("sbfm_x", imms, 63) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SXTW Xd, Wn (sign-extend word to 64-bit; alias of SBFM Xd, Xn, #0, #31).
constexpr u32 sxtw_x(u8 d, u8 n) noexcept {
    return sbfm_x(d, n, 0, 31);
}

/// Encode ARM64 SXTB Xd, Wn (sign-extend byte to 64-bit; alias of SBFM Xd, Xn, #0, #7).
constexpr u32 sxtb_x(u8 d, u8 n) noexcept {
    return sbfm_x(d, n, 0, 7);
}

/// Encode ARM64 SXTH Xd, Wn (sign-extend halfword to 64-bit; alias of SBFM Xd, Xn, #0, #15).
constexpr u32 sxth_x(u8 d, u8 n) noexcept {
    return sbfm_x(d, n, 0, 15);
}

/// Encode ARM64 LSL Wd, Wn, #shift (alias of UBFM)
constexpr u32 lsl_w_imm(u8 d, u8 n, u64 shift) noexcept {
    // UBFM Wd, Wn, #((32 - shift) mod 32), #(31 - shift).
    const u32 s = uimm_field("lsl_w_imm", shift, 31);
    const u32 immr = (32u - s) % 32u;
    const u32 imms = 31u - s;
    return 0x53000000u | (immr << 16)
                       | (imms << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSR Wd, Wn, #shift (alias of UBFM)
constexpr u32 lsr_w_imm(u8 d, u8 n, u64 shift) noexcept {
    return 0x53000000u | (uimm_field("lsr_w_imm", shift, 31) << 16)
                       | (31u << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ASR Wd, Wn, #shift (alias of SBFM)
constexpr u32 asr_w_imm(u8 d, u8 n, u64 shift) noexcept {
    return 0x13000000u | (uimm_field("asr_w_imm", shift, 31) << 16)
                       | (31u << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSLV Wd, Wn, Wm (variable shift)
constexpr u32 lslv_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC02000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSRV Wd, Wn, Wm
constexpr u32 lsrv_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC02400u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ASRV Wd, Wn, Wm
constexpr u32 asrv_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC02800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ROR Wd, Wn, #shift (alias of EXTR Wd, Wn, Wn, #shift)
constexpr u32 ror_w_imm(u8 d, u8 n, u64 shift) noexcept {
    return 0x13800000u | (static_cast<u32>(n) << 16)
                       | (uimm_field("ror_w_imm", shift, 31) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
static_assert(ror_w_imm(9, 9, 2) == 0x13890929u);  // ror w9, w9, #2 (llvm-mc)

/// Encode ARM64 RORV Wd, Wn, Wm (variable rotate right)
constexpr u32 rorv_w(u8 d, u8 n, u8 m) noexcept {
    return 0x1AC02C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSLV Xd, Xn, Xm (variable left shift, 64-bit)
constexpr u32 lslv_x(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC02000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSRV Xd, Xn, Xm (variable logical right shift, 64-bit)
constexpr u32 lsrv_x(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC02400u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSR Xd, Xn, #shift (alias of UBFM Xd, Xn, #shift, #63)
constexpr u32 lsr_x_imm(u8 d, u8 n, u64 shift) noexcept {
    return 0xD340FC00u | (uimm_field("lsr_x_imm", shift, 63) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 UBFX Xd, Xn, #lsb, #width (alias of UBFM Xd, Xn, #lsb,
/// #(lsb+width-1)). Extracts `width` bits starting at `lsb`, zero-extended.
constexpr u32 ubfx_x(u8 d, u8 n, u64 lsb, u64 width) noexcept {
    bitfield("ubfx_x", lsb, width, 64);
    const u32 immr = static_cast<u32>(lsb);
    const u32 imms = static_cast<u32>(lsb + width - 1);
    return 0xD3400000u | (immr << 16) | (imms << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 UBFX Wd, Wn, #lsb, #width (32-bit UBFM). Extracts `width`
/// bits starting at `lsb`, zero-extended. Replaces the LSR + MOVZ + AND
/// sequence used to pull x87's 3-bit TOP field out of the status word.
constexpr u32 ubfx_w(u8 d, u8 n, u64 lsb, u64 width) noexcept {
    bitfield("ubfx_w", lsb, width, 32);
    const u32 immr = static_cast<u32>(lsb);
    const u32 imms = static_cast<u32>(lsb + width - 1);
    return 0x53000000u | (immr << 16) | (imms << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 BFI Wd, Wn, #lsb, #width (32-bit BFM). Inserts the low
/// `width` bits of Wn into Wd at `lsb`, leaving the other bits of Wd alone --
/// one instruction in place of the mask/clear/shift/OR sequence used to write
/// x87's TOP field back into the status word.
constexpr u32 bfi_w(u8 d, u8 n, u64 lsb, u64 width) noexcept {
    // BFM Wd, Wn, #((32 - lsb) mod 32), #(width - 1).
    bitfield("bfi_w", lsb, width, 32);
    const u32 immr = static_cast<u32>((32u - lsb) % 32u);
    const u32 imms = static_cast<u32>(width - 1);
    return 0x33000000u | (immr << 16) | (imms << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 UXTB Wd, Wn (alias of UBFM Wd, Wn, #0, #7)
constexpr u32 uxtb_w(u8 d, u8 n) noexcept {
    return 0x53001C00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SXTB Wd, Wn (sign-extend byte, alias of SBFM Wd, Wn, #0, #7)
constexpr u32 sxtb_w(u8 d, u8 n) noexcept {
    return 0x13001C00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 UXTH Wd, Wn (zero-extend halfword, alias of UBFM Wd, Wn, #0, #15)
constexpr u32 uxth_w(u8 d, u8 n) noexcept {
    return 0x53003C00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SXTH Wd, Wn (sign-extend halfword, alias of SBFM Wd, Wn, #0, #15)
constexpr u32 sxth_w(u8 d, u8 n) noexcept {
    return 0x13003C00u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 LSL Xd, Xn, #shift (alias of UBFM Xd, Xn, #(-shift)%64, #(63-shift))
constexpr u32 lsl_x_imm(u8 d, u8 n, u64 shift) noexcept {
    // UBFM Xd, Xn, #((64 - shift) mod 64), #(63 - shift).
    const u32 s = uimm_field("lsl_x_imm", shift, 63);
    const u32 immr = (64u - s) % 64u;
    const u32 imms = 63u - s;
    return 0xD3400000u | (immr << 16)
                       | (imms << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ASR Xd, Xn, #shift (alias of SBFM Xd, Xn, #shift, #63)
constexpr u32 asr_x_imm(u8 d, u8 n, u64 shift) noexcept {
    return 0x9340FC00u | (uimm_field("asr_x_imm", shift, 63) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ASRV Xd, Xn, Xm (variable arithmetic right shift, 64-bit)
constexpr u32 asrv_x(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC02800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 RORV Xd, Xn, Xm (variable rotate right, 64-bit)
constexpr u32 rorv_x(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC02C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ROR Xd, Xn, #shift (alias of EXTR Xd, Xn, Xn, #shift, 64-bit; shift in [1..63])
constexpr u32 ror_x_imm(u8 d, u8 n, u64 shift) noexcept {
    return 0x93C00000u | (static_cast<u32>(n) << 16)
                       | (uimm_field("ror_x_imm", shift, 63) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 EXTR Xd, Xn, Xm, #lsb — Xd = (Xn:Xm) >> lsb, low 64 bits.
/// This is exactly x86 SHRD when Xn=src, Xm=dest, lsb=count, and exactly SHLD
/// when Xn=dest, Xm=src, lsb=64-count.
constexpr u32 extr_x(u8 d, u8 n, u8 m, u64 lsb) noexcept {
    return 0x93C00000u | (static_cast<u32>(m) << 16)
                       | (uimm_field("extr_x", lsb, 63) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 EXTR Wd, Wn, Wm, #lsb — 32-bit form (N=0, lsb in [0..31]).
constexpr u32 extr_w(u8 d, u8 n, u8 m, u64 lsb) noexcept {
    return 0x13800000u | (static_cast<u32>(m) << 16)
                       | (uimm_field("extr_w", lsb, 31) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 BFI Xd, Xn, #lsb, #width (64-bit BFM, N=1). Inserts the low
/// `width` bits of Xn into Xd at `lsb`. The 64-bit form is mandatory wherever
/// the destination is a 64-bit guest register pin: the W form would zero
/// bits 63:32, and FNSTSW AX must leave RAX[63:16] exactly as it was.
constexpr u32 bfi_x(u8 d, u8 n, u64 lsb, u64 width) noexcept {
    // BFM Xd, Xn, #((64 - lsb) mod 64), #(width - 1).
    bitfield("bfi_x", lsb, width, 64);
    const u32 immr = static_cast<u32>((64u - lsb) % 64u);
    const u32 imms = static_cast<u32>(width - 1);
    return 0xB3400000u | (immr << 16) | (imms << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

} // namespace vitreo::cpu::jit::arm64
