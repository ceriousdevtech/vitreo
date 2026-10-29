// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_alu.hpp
/// @brief ARM64 integer data-processing (register) encoders: add/sub/logic, SP forms, multiply/divide, select, bit counts.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 ADD Wd, Wn, Wm (32-bit)
constexpr u32 add_w(u8 d, u8 n, u8 m) noexcept {
    return 0x0B000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADD Wd, Wn, Wm, LSL #shift (32-bit scaled index).
constexpr u32 add_w_lsl(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return 0x0B000000u | (static_cast<u32>(m) << 16)
                       | (uimm_field("add_w_lsl", shift, 31) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADDS Wd, Wn, Wm (32-bit, sets flags)
constexpr u32 adds_w(u8 d, u8 n, u8 m) noexcept {
    return 0x2B000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADCS Wd, Wn, Wm (32-bit add with carry-in, sets flags).
/// Maps exactly onto x86 ADC: value AND carry-out in one instruction, so a
/// carry chain can keep CF live in NZCV instead of recomputing it from the
/// lazy FlagOp on every step. ARM C == x86 CF for addition (no inversion).
constexpr u32 adcs_w(u8 d, u8 n, u8 m) noexcept {
    return 0x3A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SBCS Wd, Wn, Wm (32-bit subtract with borrow, sets flags).
/// ARM computes Rn - Rm - (1 - C) and x86 SBB computes dst - src - CF, so the
/// carry-in must be inverted going in (C = !CF) and the carry-out is likewise
/// inverted coming out (x86 CF = !C) -- the same polarity the existing
/// NzcvState::Sub fast path already assumes (CSET CC).
constexpr u32 sbcs_w(u8 d, u8 n, u8 m) noexcept {
    return 0x7A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUB Wd, Wn, Wm
constexpr u32 sub_w(u8 d, u8 n, u8 m) noexcept {
    return 0x4B000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUBS Wd, Wn, Wm (sets flags)
constexpr u32 subs_w(u8 d, u8 n, u8 m) noexcept {
    return 0x6B000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUBS Xd, Xn, Xm (64-bit, sets flags)
/// Used for bounds checks where addr+N may overflow 32 bits.
constexpr u32 subs_x(u8 d, u8 n, u8 m) noexcept {
    return 0xEB000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 AND Wd, Wn, Wm
constexpr u32 and_w(u8 d, u8 n, u8 m) noexcept {
    return 0x0A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 AND Wd, Wn, #bitmask (logical immediate, 32-bit): the mask
/// must be a bitmask immediate (logical_imm). Flags untouched.
constexpr u32 and_w_imm(u8 d, u8 n, u64 bitmask) noexcept {
    return 0x12000000u | (logical_imm("and_w_imm", bitmask, 32) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ANDS Wd, Wn, Wm (sets N and Z flags)
constexpr u32 ands_w(u8 d, u8 n, u8 m) noexcept {
    return 0x6A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ANDS Wd, Wn, Wm, LSL #shift (shifted register, shift 0-31).
constexpr u32 ands_w_lsl(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return ands_w(d, n, m) | (uimm_field("ands_w_lsl", shift, 31) << 10);
}

/// Encode ARM64 ORR Wd, Wn, Wm
constexpr u32 orr_w(u8 d, u8 n, u8 m) noexcept {
    return 0x2A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ORR Wd, Wn, Wm, LSL #shift (32-bit, shift in [0..31]).
constexpr u32 orr_w_lsl(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return 0x2A000000u | (uimm_field("orr_w_lsl", shift, 31) << 10)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 EOR Wd, Wn, Wm
constexpr u32 eor_w(u8 d, u8 n, u8 m) noexcept {
    return 0x4A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADD Xd, Xn, Xm (64-bit)
constexpr u32 add_x(u8 d, u8 n, u8 m) noexcept {
    return 0x8B000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUB Xd, Xn, Xm (64-bit)
constexpr u32 sub_x(u8 d, u8 n, u8 m) noexcept {
    return 0xCB000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 AND Xd, Xn, Xm (64-bit)
constexpr u32 and_x(u8 d, u8 n, u8 m) noexcept {
    return 0x8A000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 EOR Xd, Xn, Xm (64-bit)
constexpr u32 eor_x(u8 d, u8 n, u8 m) noexcept {
    return 0xCA000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 EOR Xd, Xn, Xm, LSR #shift (64-bit, shift in [0..63]).
///
/// The L1 index mix folds the guest address' high bits down into the table
/// index (see BlockCache::l1_index), which needs exactly this form.
constexpr u32 eor_x_lsr(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return 0xCA400000u | (uimm_field("eor_x_lsr", shift, 63) << 10)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
// Verified against the system assembler: `eor x3, x1, x1, lsr #16` and
// `eor x3, x3, x3, lsr #16`.
static_assert(eor_x_lsr(3, 1, 1, 16) == 0xCA414023u);
static_assert(eor_x_lsr(3, 3, 3, 16) == 0xCA434063u);

/// Encode ARM64 ORR Xd, Xn, Xm (64-bit)
constexpr u32 orr_x(u8 d, u8 n, u8 m) noexcept {
    return 0xAA000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 BIC Wd, Wn, Wm (32-bit) — Wd = Wn AND ~Wm, zero-extended.
constexpr u32 bic_w(u8 d, u8 n, u8 m) noexcept {
    return 0x0A200000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 BIC Xd, Xn, Xm (64-bit) — Xd = Xn AND ~Xm.
constexpr u32 bic_x(u8 d, u8 n, u8 m) noexcept {
    return 0x8A200000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADDS Xd, Xn, Xm (64-bit, sets flags)
constexpr u32 adds_x(u8 d, u8 n, u8 m) noexcept {
    return 0xAB000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADCS Xd, Xn, Xm (64-bit add with carry-in, sets flags).
/// ARM C-in == x86 CF for addition (no inversion), and the resulting
/// N/Z/C/V are exactly x86 SF/ZF/CF/OF for `ADC r64` -- the same
/// relationship ADDS already has for ADD. AF is NOT in NZCV; the caller
/// must still publish a lazy op for it. Verified: adcs x4,x0,x1 = 0xBA010004.
constexpr u32 adcs_x(u8 d, u8 n, u8 m) noexcept {
    return 0xBA000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SBCS Xd, Xn, Xm (64-bit subtract with borrow, sets flags).
/// ARM computes Rn + NOT(Rm) + C. With C == NOT(x86 CF) that is a - b - CF,
/// and the carry-out is NOT(borrow) -- the *inverted* polarity, identical to
/// what SUBS leaves after a SUB. A following ADC therefore needs one cfinv.
/// Verified: sbcs x4,x0,x1 = 0xFA010004.
constexpr u32 sbcs_x(u8 d, u8 n, u8 m) noexcept {
    return 0xFA000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ANDS Xd, Xn, Xm (64-bit, sets N and Z)
constexpr u32 ands_x(u8 d, u8 n, u8 m) noexcept {
    return 0xEA000000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// ADD Xd, Xn, Wm, UXTW — host = base + zext(guest_addr32).
constexpr u32 add_x_uxtw(u8 d, u8 n, u8 m) noexcept {
    return 0x8B204000u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MOV Wd, Wm (alias of ORR Wd, WZR, Wm)
constexpr u32 mov_w(u8 d, u8 m) noexcept {
    return orr_w(d, 31, m);
}

/// Encode ARM64 SUB Xd, SP, Xm (extended register, UXTX #0): the only SUB
/// form whose first source may be SP. Flag-neutral.
constexpr u32 sub_x_sp_reg(u8 d, u8 m) noexcept {
    return 0xCB2063E0u | (static_cast<u32>(m) << 16) | static_cast<u32>(d);
}
static_assert(sub_x_sp_reg(3, 3) == 0xCB2363E3u);  // llvm-mc: sub x3, sp, x3

/// Encode ARM64 MOV SP, Xn (ADD SP, Xn, #0).
constexpr u32 mov_sp_x(u8 n) noexcept {
    return 0x9100001Fu | (static_cast<u32>(n) << 5);
}
static_assert(mov_sp_x(17) == 0x9100023Fu);  // llvm-mc: mov sp, x17

/// Encode ARM64 UMULL Xd, Wn, Wm (unsigned 32x32 -> 64)
constexpr u32 umull(u8 d, u8 n, u8 m) noexcept {
    return 0x9BA07C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SMULL Xd, Wn, Wm (signed 32x32 -> 64)
constexpr u32 smull(u8 d, u8 n, u8 m) noexcept {
    return 0x9B207C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MUL Xd, Xn, Xm (64x64 -> low 64)
constexpr u32 mul_x(u8 d, u8 n, u8 m) noexcept {
    // MUL is alias of MADD Xd, Xn, Xm, XZR
    return 0x9B007C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SMULH Xd, Xn, Xm (signed 64x64 -> high 64)
constexpr u32 smulh(u8 d, u8 n, u8 m) noexcept {
    return 0x9B407C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 UMULH Xd, Xn, Xm (unsigned 64x64 -> high 64)
constexpr u32 umulh(u8 d, u8 n, u8 m) noexcept {
    return 0x9BC07C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 UDIV Xd, Xn, Xm (unsigned 64-bit divide).
constexpr u32 udiv_x(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC00800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SDIV Xd, Xn, Xm (signed 64-bit divide).
constexpr u32 sdiv_x(u8 d, u8 n, u8 m) noexcept {
    return 0x9AC00C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MSUB Xd, Xn, Xm, Xa (Xd = Xa - Xn * Xm).
constexpr u32 msub_x(u8 d, u8 n, u8 m, u8 a) noexcept {
    return 0x9B008000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(a) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ORR Xd, Xn, Xm, LSL #shift (64-bit, shift in [0..63]).
constexpr u32 orr_x_lsl(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return 0xAA000000u | (uimm_field("orr_x_lsl", shift, 63) << 10)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ORR Xd, Xn, Xm, LSR #shift (64-bit, shift in [0..63]).
///
/// Folds the linear64 reservation test (EA >> address_bits) into the same
/// register as the host-page crossing bit, so one CBNZ covers both.
constexpr u32 orr_x_lsr(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return 0xAA400000u | (uimm_field("orr_x_lsr", shift, 63) << 10)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
// Verified against the system assembler: `orr x8, x8, x9, lsr #46` and
// `orr x8, x8, x26, lsr #46`.
static_assert(orr_x_lsr(8, 8, 9, 46) == 0xAA49B908u);
static_assert(orr_x_lsr(8, 8, 26, 46) == 0xAA5AB908u);

/// Encode ARM64 CSEL Xd, Xn, Xm, cond (64-bit; result = cond ? Xn : Xm)
constexpr u32 csel_x(u8 d, u8 n, u8 m, u64 cond) noexcept {
    return 0x9A800000u | (static_cast<u32>(m) << 16)
                       | (uimm_field("csel_x", cond, 15) << 12)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CSEL Wd, Wn, Wm, cond (32-bit; result = cond ? Wn : Wm)
constexpr u32 csel_w(u8 d, u8 n, u8 m, u64 cond) noexcept {
    return 0x1A800000u | (static_cast<u32>(m) << 16)
                       | (uimm_field("csel_w", cond, 15) << 12)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADD Xd, Xn, Xm, LSL #shift (64-bit, shift in [0..63]).
constexpr u32 add_x_lsl(u8 d, u8 n, u8 m, u64 shift) noexcept {
    return 0x8B000000u | (uimm_field("add_x_lsl", shift, 63) << 10)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
// Verified against the system assembler: `add x17, x17, x3, lsl #5`. The L1
// tails use this to scale a slot index by sizeof(BlockCache::L1Entry).
static_assert(add_x_lsl(17, 17, 3, 5) == 0x8B031631u);
static_assert(add_x_lsl(0, 1, 2, 0) == add_x(0, 1, 2));

/// Encode ARM64 NEG Wd, Wm (alias of SUB Wd, WZR, Wm).
constexpr u32 neg_w(u8 d, u8 m) noexcept {
    return sub_w(d, 31, m);
}

/// Encode ARM64 NEGS Xd, Xm (alias of SUBS Xd, XZR, Xm — sets flags).
constexpr u32 negs_x(u8 d, u8 m) noexcept {
    return 0xEB0003E0u | (static_cast<u32>(m) << 16) | static_cast<u32>(d);
}

/// Encode ARM64 MVN Xd, Xm (alias of ORN Xd, XZR, Xm).
constexpr u32 mvn_x(u8 d, u8 m) noexcept {
    return 0xAA2003E0u | (static_cast<u32>(m) << 16) | static_cast<u32>(d);
}

/// Encode ARM64 MVN Wd, Wm  (alias of ORN Wd, WZR, Wm) — 32-bit bitwise NOT.
constexpr u32 mvn_w(u8 d, u8 m) noexcept {
    return 0x2A2003E0u | (static_cast<u32>(m) << 16) | static_cast<u32>(d);
}

/// Encode ARM64 CSET Wd, cond — Wd = (cond ? 1 : 0). Alias of CSINC Wd, WZR, WZR, !cond.
constexpr u32 cset_w(u8 d, u64 cond) noexcept {
    // CSET is CSINC Wd, WZR, WZR, invert(cond), and is defined only for
    // cond < AL: inverting AL gives NV, which CSINC treats as always TRUE,
    // so the word would set Wd to 0 rather than 1.
    const u32 inv_cond = uimm_field("cset_w", cond, 13) ^ 1u;
    return 0x1A9F07E0u | (inv_cond << 12)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MOV Xd, Xm (ORR Xd, XZR, Xm)
constexpr u32 mov_x(u8 d, u8 m) noexcept {
    return 0xAA0003E0u | (static_cast<u32>(m) << 16)
                       | static_cast<u32>(d);
}

/// Encode ARM64 RBIT Wd, Wn (reverse bits, 32-bit)
constexpr u32 rbit_w(u8 d, u8 n) noexcept {
    return 0x5AC00000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 RBIT Xd, Xn (reverse bits, 64-bit)
constexpr u32 rbit_x(u8 d, u8 n) noexcept {
    return 0xDAC00000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CLZ Wd, Wn (count leading zeros, 32-bit)
constexpr u32 clz_w(u8 d, u8 n) noexcept {
    return 0x5AC01000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CLZ Xd, Xn (count leading zeros, 64-bit)
constexpr u32 clz_x(u8 d, u8 n) noexcept {
    return 0xDAC01000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// ARM64 condition codes
enum Cond : u8 {
    EQ = 0x0, NE = 0x1, CS = 0x2, CC = 0x3,
    MI = 0x4, PL = 0x5, VS = 0x6, VC = 0x7,
    HI = 0x8, LS = 0x9, GE = 0xA, LT = 0xB,
    GT = 0xC, LE = 0xD, AL = 0xE,
};

/// Encode REV Wd, Wn — byte reverse (32-bit). Matches x86 BSWAP r32.
constexpr u32 rev_w(u8 d, u8 n) noexcept {
    return 0x5AC00800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode REV Xd, Xn — byte reverse (64-bit). Matches x86 BSWAP r64.
constexpr u32 rev_x(u8 d, u8 n) noexcept {
    return 0xDAC00C00u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

} // namespace vitreo::cpu::jit::arm64
