// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_simd_int.hpp
/// @brief ARM64 NEON integer and bitwise vector encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_simd_ldst.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Base pattern for SIMD three-same form (Q=1):
///   <31:24> = 0 1 Q U 0 1 1 1 0 → Q=1, U selectable
///   <23:22> = size
///   <21>    = 1
///   <20:16> = Rm
///   <15:10> = opcode (5 bits) + bit10 (set for element ops)
///   <9:5>   = Rn
///   <4:0>   = Rd

/// Encode AND Vd.16B, Vn.16B, Vm.16B (bitwise AND).
constexpr u32 and_v16b(u8 d, u8 n, u8 m) noexcept {
    return 0x4E201C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode BIC Vd.16B, Vn.16B, Vm.16B (Vd = Vn AND NOT Vm).
constexpr u32 bic_v16b(u8 d, u8 n, u8 m) noexcept {
    return 0x4E601C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ORR Vd.16B, Vn.16B, Vm.16B (bitwise OR).
constexpr u32 orr_v16b(u8 d, u8 n, u8 m) noexcept {
    return 0x4EA01C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode EOR Vd.16B, Vn.16B, Vm.16B (bitwise XOR).
constexpr u32 eor_v16b(u8 d, u8 n, u8 m) noexcept {
    return 0x6E201C00u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode NOT Vd.16B, Vn.16B (bitwise NOT, alias MVN).
constexpr u32 not_v16b(u8 d, u8 n) noexcept {
    return 0x6E205800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode ADD Vd.T, Vn.T, Vm.T — element-wise add, T in {16B,8H,4S,2D}.
constexpr u32 add_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E208400u | (uimm_field("add_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode SUB Vd.T, Vn.T, Vm.T — element-wise sub.
constexpr u32 sub_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E208400u | (uimm_field("sub_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode CMEQ Vd.T, Vn.T, Vm.T — element-wise equality (sets all-ones on match).
constexpr u32 cmeq_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E208C00u | (uimm_field("cmeq_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode CMGT Vd.T, Vn.T, Vm.T — signed element-wise greater-than.
constexpr u32 cmgt_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E203400u | (uimm_field("cmgt_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode MUL Vd.T, Vn.T, Vm.T — per-lane integer multiply (B/H/S).
/// For PMULLD use VS_S; the S/B/H forms keep only the low half of the product.
constexpr u32 mul_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E209C00u | (uimm_field("mul_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode SMIN Vd.T, Vn.T, Vm.T — signed min.
constexpr u32 smin_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E206C00u | (uimm_field("smin_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode SMAX Vd.T, Vn.T, Vm.T — signed max.
constexpr u32 smax_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E206400u | (uimm_field("smax_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode UMIN Vd.T, Vn.T, Vm.T — unsigned min.
constexpr u32 umin_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E206C00u | (uimm_field("umin_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode CMHI Vd.T, Vn.T, Vm.T — unsigned "higher than" compare, all-ones
/// per element where Vn > Vm. Unlike UMIN/UMAX this DOES have a 2D form,
/// which is why the variable-shift clamp uses CMHI+BSL rather than UMIN.
constexpr u32 cmhi_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E203400u | (uimm_field("cmhi_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode UMAX Vd.T, Vn.T, Vm.T — unsigned max.
constexpr u32 umax_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E206400u | (uimm_field("umax_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode URHADD Vd.T, Vn.T, Vm.T — unsigned rounding average.
constexpr u32 urhadd_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E201400u | (uimm_field("urhadd_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode UQADD Vd.T, Vn.T, Vm.T — unsigned saturating add.
constexpr u32 uqadd_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E200C00u | (uimm_field("uqadd_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode UQSUB Vd.T, Vn.T, Vm.T — unsigned saturating subtract.
constexpr u32 uqsub_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E202C00u | (uimm_field("uqsub_v", sz, 3) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode SHL Vd.T, Vn.T, #shift (vector, immediate). Shift in [0, esize-1].
constexpr u32 shl_v_imm(u8 d, u8 n, u64 shift, VSize sz) noexcept {
    const u32 esize = 8u << uimm_field("shl_v_imm", sz, 3);
    const u32 imm7 = esize + uimm_field("shl_v_imm", shift, esize - 1u);
    return 0x4F005400u | (imm7 << 16)
                       | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode USHR Vd.T, Vn.T, #shift (vector, immediate). Shift in [1, esize].
constexpr u32 ushr_v_imm(u8 d, u8 n, u64 shift, VSize sz) noexcept {
    const u32 esize = 8u << uimm_field("ushr_v_imm", sz, 3);
    if (shift == 0) simm_field_overflow("ushr_v_imm", 0, 1, esize);  // range [1, esize]
    const u32 imm7 = 2u * esize - uimm_field("ushr_v_imm", shift, esize);
    return 0x6F000400u | (imm7 << 16)
                       | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode SSHR Vd.T, Vn.T, #shift (vector, immediate). Shift in [1, esize].
constexpr u32 sshr_v_imm(u8 d, u8 n, u64 shift, VSize sz) noexcept {
    const u32 esize = 8u << uimm_field("sshr_v_imm", sz, 3);
    if (shift == 0) simm_field_overflow("sshr_v_imm", 0, 1, esize);  // range [1, esize]
    const u32 imm7 = 2u * esize - uimm_field("sshr_v_imm", shift, esize);
    return 0x4F000400u | (imm7 << 16)
                       | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode MOV Vd.16B, Vn.16B — alias of ORR Vd.16B, Vn.16B, Vn.16B.
constexpr u32 mov_v16b(u8 d, u8 n) noexcept {
    return orr_v16b(d, n, n);
}

/// Encode MOVI Vd.16B, #imm8 — broadcast imm8 to all 16 bytes.
constexpr u32 movi_v16b(u8 d, u64 imm8) noexcept {
    const u32 imm = uimm_field("movi_v16b", imm8, 0xFF);  // a:b:c:d:e:f:g:h
    const u32 abc = imm >> 5;
    const u32 defgh = imm & 0x1Fu;
    return 0x4F00E400u | (abc << 16) | (defgh << 5) | static_cast<u32>(d);
}

/// Encode MOVI Vd.4S, #0xFF, MSL #8. Each 32-bit lane becomes 0x0000FFFF,
/// i.e. alternating 16-bit all-ones/zero lanes (PBLENDW mask 0x55).
constexpr u32 movi_v4s_ff_msl8(u8 d) noexcept {
    return 0x4F07C7E0u | static_cast<u32>(d);
}
static_assert(movi_v4s_ff_msl8(2) == 0x4F07C7E2u);

/// Encode MVNI Vd.4S, #0xFF, MSL #8. Each 32-bit lane becomes 0xFFFF0000,
/// the complementary alternating-word pattern (PBLENDW mask 0xAA).
constexpr u32 mvni_v4s_ff_msl8(u8 d) noexcept {
    return 0x6F07C7E0u | static_cast<u32>(d);
}
static_assert(mvni_v4s_ff_msl8(3) == 0x6F07C7E3u);

/// Encode MVNI Vd.4S, #1, LSL #24. This broadcasts 0xFEFFFFFF, the strict
/// shifted-magnitude threshold between max finite and Inf/NaN.
constexpr u32 mvni_v4s_finite_threshold(u8 d) noexcept {
    return 0x6F006420u | static_cast<u32>(d);
}
static_assert(mvni_v4s_finite_threshold(4) == 0x6F006424u);

// ---- Additional NEON integer/FP encoders for SSE3/SSSE3 stencils ----

/// Encode ABS Vd.T, Vn.T — element-wise absolute value (signed).
/// INT_MIN maps to INT_MIN (NEON ABS does NOT saturate), matching
/// x86 PABSB/W/D semantics per Intel SDM.
constexpr u32 abs_v(u8 d, u8 n, VSize sz) noexcept {
    return 0x4E20B800u | (uimm_field("abs_v", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode SQADD Vd.T, Vn.T, Vm.T — signed saturating add (PADDSB/W/D).
constexpr u32 sqadd_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E200C00u | (uimm_field("sqadd_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode SQSUB Vd.T, Vn.T, Vm.T — signed saturating sub (PSUBSB/W/D).
constexpr u32 sqsub_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E202C00u | (uimm_field("sqsub_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// Encode ADDP Vd.T, Vn.T, Vm.T — pairwise integer add (PHADDW/D).
/// Result lanes: [n0+n1, n2+n3, ..., m0+m1, m2+m3, ...]
constexpr u32 addp_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E20BC00u | (uimm_field("addp_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

// ---- Compare-against-zero (vector) ----

/// CMGT Vd.T, Vn.T, #0 — element-wise > 0 (signed).
constexpr u32 cmgt_v_zero(u8 d, u8 n, VSize sz) noexcept {
    return 0x4E208800u | (uimm_field("cmgt_v_zero", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// CMEQ Vd.T, Vn.T, #0 — element-wise == 0.
constexpr u32 cmeq_v_zero(u8 d, u8 n, VSize sz) noexcept {
    return 0x4E209800u | (uimm_field("cmeq_v_zero", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// CMLT Vd.T, Vn.T, #0 — element-wise < 0.
constexpr u32 cmlt_v_zero(u8 d, u8 n, VSize sz) noexcept {
    return 0x4E20A800u | (uimm_field("cmlt_v_zero", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// NEG Vd.T, Vn.T — element-wise negate.
constexpr u32 neg_v(u8 d, u8 n, VSize sz) noexcept {
    return 0x6E20B800u | (uimm_field("neg_v", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// BIT Vd.16B, Vn.16B, Vm.16B — Vd = (Vd & ~Vm) | (Vn & Vm): insert the bits
/// of Vn where Vm is set. (clang: `bit.16b v3, v4, v5` = 0x6EA51C83.)
constexpr u32 bit_v16b(u8 d, u8 n, u8 m) noexcept {
    return 0x6EA01C00u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
static_assert(bit_v16b(3, 4, 5) == 0x6EA51C83u);

/// BSL Vd.16B, Vn.16B, Vm.16B — Vd = (Vd & Vn) | (~Vd & Vm).
constexpr u32 bsl_v16b(u8 d, u8 n, u8 m) noexcept {
    return 0x6E601C00u | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SQRDMULH Vd.T, Vn.T, Vm.T — signed saturating rounding doubling
/// multiply returning high half.  PMULHRSW maps to .8H form.
constexpr u32 sqrdmulh_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E20B400u | (uimm_field("sqrdmulh_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SMULL Vd.<2T>, Vn.<T>, Vm.<T> — vector signed widening multiply (Q=0).
/// src_sz is SOURCE element size (B, H, S → dst is H, S, D).
constexpr u32 smull_v(u8 d, u8 n, u8 m, VSize src_sz) noexcept {
    return 0x0E20C000u | (uimm_field("smull_v", src_sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UMULL Vd.<2T>, Vn.<T>, Vm.<T> — vector unsigned widening multiply (Q=0).
constexpr u32 umull_v(u8 d, u8 n, u8 m, VSize src_sz) noexcept {
    return 0x2E20C000u | (uimm_field("umull_v", src_sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SMULL2 Vd.<2T>, Vn.<2T>, Vm.<2T> — widening multiply of the upper half (Q=1).
constexpr u32 smull2_v(u8 d, u8 n, u8 m, VSize src_sz) noexcept {
    return 0x4E20C000u | (uimm_field("smull2_v", src_sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UMULL2 Vd.<2T>, Vn.<2T>, Vm.<2T> — unsigned widening multiply of the upper
/// half (Q=1). Pair with `umull_v` + `uzp2_v` for PMULHUW.
constexpr u32 umull2_v(u8 d, u8 n, u8 m, VSize src_sz) noexcept {
    return 0x6E20C000u | (uimm_field("umull2_v", src_sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// USHL Vd.T, Vn.T, Vm.T — per-lane shift by signed amounts in Vm (Q=1).
constexpr u32 ushl_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E204400u | (uimm_field("ushl_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SSHL Vd.T, Vn.T, Vm.T — per-lane arithmetic shift by signed amounts (Q=1).
constexpr u32 sshl_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x4E204400u | (uimm_field("sshl_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UABD Vd.T, Vn.T, Vm.T — unsigned absolute difference (Q=1).
constexpr u32 uabd_v(u8 d, u8 n, u8 m, VSize sz) noexcept {
    return 0x6E207400u | (uimm_field("uabd_v", sz, 3) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UADDLV Hd, Vn.16B — unsigned sum of bytes into a halfword (high bits cleared).
constexpr u32 uaddlv_16b(u8 d, u8 n) noexcept {
    return 0x6E303800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// SADDLP Vd.<2T>, Vn.<T> — pairwise signed add long (Q=1; e.g. 8H→4S).
constexpr u32 saddlp_v(u8 d, u8 n, VSize src_sz) noexcept {
    return 0x4E202800u | (uimm_field("saddlp_v", src_sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UADDLP Vd.<2T>, Vn.<T> — pairwise UNSIGNED add long (Q=1; 16B→8H, 8H→4S,
/// 4S→2D). The unsigned form matters: the signed twin above would read a byte
/// of 200 as -56, and the inputs here are absolute differences in 0..255.
constexpr u32 uaddlp_v(u8 d, u8 n, VSize src_sz) noexcept {
    return 0x6E202800u | (uimm_field("uaddlp_v", src_sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
// UADDLP V2.8H, V2.16B
static_assert(uaddlp_v(2, 2, VS_B) == 0x6E202842u);

/// UMINV Sd, Vn.T — across-vector unsigned min reduction (writes lane0 scalar).
constexpr u32 uminv_v(u8 d, u8 n, VSize sz) noexcept {
    return 0x6E31A800u | (uimm_field("uminv_v", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// UMAXV Sd, Vn.T — across-vector unsigned max reduction (writes lane0 scalar).
constexpr u32 umaxv_v(u8 d, u8 n, VSize sz) noexcept {
    return 0x6E30A800u | (uimm_field("umaxv_v", sz, 3) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

} // namespace vitreo::cpu::jit::arm64
