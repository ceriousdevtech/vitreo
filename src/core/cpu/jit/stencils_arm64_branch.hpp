// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_branch.hpp
/// @brief ARM64 branch, PC-relative and NZCV system-register encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 RET (return to LR)
constexpr u32 ret() noexcept {
    return 0xD65F03C0u;
}

/// Encode ARM64 NOP
constexpr u32 nop() noexcept {
    return 0xD503201Fu;
}

/// Encode ARM64 ADR Xd, #offset (PC-relative address, +-1 MiB).
constexpr u32 adr_x(u8 d, i64 offset_bytes) noexcept {
    const u32 imm = adr_imm21("adr_x", offset_bytes);  // immhi:immlo
    return 0x10000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) |
           static_cast<u32>(d);
}

/// Encode ARM64 B imm26 (unconditional branch)
constexpr u32 b_imm(i64 offset_bytes) noexcept {
    const u32 imm26 = arm64::imm26("b_imm", offset_bytes);
    return 0x14000000u | imm26;
}

/// Encode ARM64 BL imm26 (branch with link). Same displacement as B.
constexpr u32 bl_imm(i64 offset_bytes) noexcept {
    const u32 imm26 = arm64::imm26("bl_imm", offset_bytes);
    return 0x94000000u | imm26;
}

/// Encode ARM64 B.cond imm19
constexpr u32 b_cond(u64 cond, i64 offset_bytes) noexcept {
    const u32 imm19 = arm64::imm19("b_cond", offset_bytes);
    return 0x54000000u | (imm19 << 5) | uimm_field("b_cond", cond, 15);
}

/// Encode ARM64 MRS Xt, NZCV
constexpr u32 mrs_nzcv(u8 t) noexcept {
    return 0xD53B4200u | static_cast<u32>(t);
}

/// Encode ARM64 MSR NZCV, Xt
constexpr u32 msr_nzcv(u8 t) noexcept {
    return 0xD51B4200u | static_cast<u32>(t);
}

/// Encode ARM64 CFINV -- invert PSTATE.C, leaving N, Z and V untouched
/// (FEAT_FlagM; gate every use on host_supports_flagm()).
/// This is the entire cost of an x86 CF polarity change: ARM's subtract
/// carry is the inverse of x86's borrow, so a chain switching between ADC
/// and SBB needs exactly one of these at the switch point.
/// Verified: cfinv = 0xD500401F.
constexpr u32 cfinv() noexcept {
    return 0xD500401Fu;
}

/// Encode ARM64 RMIF Xn, #shift, #mask -- write ONLY the NZCV bits selected
/// by `mask`, from the low four bits of ROR(Xn, shift):
///     N <- t<3>, Z <- t<2>, C <- t<1>, V <- t<0>
/// Used to inject a 0/1 x86 CF held in a GPR into PSTATE.C without touching
/// N/Z/V: bit 0 must land in t<1>, i.e. rotate left by one, i.e. shift = 63.
/// Verified: rmif x21,#63,#2 = 0xBA1F86A2; rmif x0,#0,#0 = 0xBA000400.
constexpr u32 rmif(u8 n, u64 shift, u64 mask) noexcept {
    return 0xBA000400u | (uimm_field("rmif", shift, 63) << 15)
                       | (static_cast<u32>(n) << 5)
                       | uimm_field("rmif", mask, 15);
}

/// NZCV mask bits for rmif().
constexpr u8 RMIF_N = 0x8;
constexpr u8 RMIF_Z = 0x4;
constexpr u8 RMIF_C = 0x2;
constexpr u8 RMIF_V = 0x1;
/// Rotate that puts bit 0 of the source register into the C position.
constexpr u8 RMIF_SHIFT_BIT0_TO_C = 63;

/// Encode ARM64 CBZ Wt, label (compare and branch on zero)
constexpr u32 cbz_w(u8 t, i64 offset_bytes) noexcept {
    const u32 imm19 = arm64::imm19("cbz_w", offset_bytes);
    return 0x34000000u | (imm19 << 5) | static_cast<u32>(t);
}

/// Encode ARM64 CBZ Xt, label (64-bit compare and branch on zero)
constexpr u32 cbz_x(u8 t, i64 offset_bytes) noexcept {
    const u32 imm19 = arm64::imm19("cbz_x", offset_bytes);
    return 0xB4000000u | (imm19 << 5) | static_cast<u32>(t);
}

/// Encode ARM64 BR Xn (branch to register, no link)
constexpr u32 br(u8 n) noexcept {
    return 0xD61F0000u | (static_cast<u32>(n) << 5);
}

/// Encode ARM64 CBNZ Wt, label (compare and branch on non-zero)
constexpr u32 cbnz_w(u8 t, i64 offset_bytes) noexcept {
    const u32 imm19 = arm64::imm19("cbnz_w", offset_bytes);
    return 0x35000000u | (imm19 << 5) | static_cast<u32>(t);
}

/// Encode ARM64 CBNZ Xt, label (64-bit compare and branch on non-zero)
constexpr u32 cbnz_x(u8 t, i64 offset_bytes) noexcept {
    const u32 imm19 = arm64::imm19("cbnz_x", offset_bytes);
    return 0xB5000000u | (imm19 << 5) | static_cast<u32>(t);
}

/// Encode ARM64 TBZ Xt, #bit, label (skip when bit is 0). `bit` is 0..63.
constexpr u32 tbz(u8 t, u64 bit, i64 offset_bytes) noexcept {
    const u32 b = uimm_field("tbz", bit, 63);  // b5:b40
    const u32 imm14 = arm64::imm14("tbz", offset_bytes);
    return ((b >> 5) << 31)
         | 0x36000000u
         | ((b & 0x1Fu) << 19)
         | (imm14 << 5)
         | static_cast<u32>(t);
}

/// Encode ARM64 TBNZ Xt, #bit, label (taken when bit is 1). `bit` is 0..63.
constexpr u32 tbnz(u8 t, u64 bit, i64 offset_bytes) noexcept {
    const u32 b = uimm_field("tbnz", bit, 63);  // b5:b40
    const u32 imm14 = arm64::imm14("tbnz", offset_bytes);
    return ((b >> 5) << 31)
         | 0x37000000u
         | ((b & 0x1Fu) << 19)
         | (imm14 << 5)
         | static_cast<u32>(t);
}

/// One word that branches when bit `bit` of Xt is set: TBNZ while the target
/// is within TBNZ's imm14 (+-32 KiB), CBNZ Xt (imm19, +-1 MiB) beyond it. The
/// CBNZ branches on ANY set bit, so this is only for a test where "any bit"
/// is a safe, conservative answer (a check byte whose every bit means "take
/// the slow path"). The choice is made when the branch is patched, so the
/// code around it is the same length either way.
constexpr u32 tbnz_or_cbnz_x(u8 t, u64 bit, i64 offset_bytes) noexcept {
    const bool bit_test_reaches = offset_bytes >= -(i64{1} << 15) &&
                                  offset_bytes < (i64{1} << 15);
    return bit_test_reaches ? tbnz(t, bit, offset_bytes) : cbnz_x(t, offset_bytes);
}

} // namespace vitreo::cpu::jit::arm64
