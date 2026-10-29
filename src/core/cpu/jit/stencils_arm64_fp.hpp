// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_fp.hpp
/// @brief ARM64 floating-point encoders: vector and scalar arithmetic, compare, convert, round.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_simd_ldst.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 FCVTL Vd.4S, Vn.4H — widen 4 halves to 4 singles.
constexpr u32 fcvtl_4s_4h(u8 d, u8 n) noexcept {
    return 0x0E217800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode ARM64 FCVTL2 Vd.4S, Vn.8H — widen the UPPER 4 halves.
constexpr u32 fcvtl2_4s_8h(u8 d, u8 n) noexcept {
    return 0x4E217800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode ARM64 FCVTL Vd.2D, Vn.2S — widen the low 2 singles to 2 doubles.
/// Same instruction as the 4S/4H form above with sz=1 (bit 22).
constexpr u32 fcvtl_2d_2s(u8 d, u8 n) noexcept {
    return 0x0E617800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
static_assert(fcvtl_2d_2s(0, 1) == 0x0E617820u);

/// Encode ARM64 FCVTN Vd.2S, Vn.2D — narrow 2 doubles into the low 2 singles
/// and zero Vd[127:64]. Same instruction as the 4H/4S form below with sz=1.
constexpr u32 fcvtn_2s_2d(u8 d, u8 n) noexcept {
    return 0x0E616800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
static_assert(fcvtn_2s_2d(1, 0) == 0x0E616801u);  // llvm-mc: fcvtn v1.2s, v0.2d

/// Encode ARM64 FCVTN2 Vd.4S, Vn.2D — narrow 2 doubles into the UPPER 2
/// singles, keeping Vd[63:0].
constexpr u32 fcvtn2_4s_2d(u8 d, u8 n) noexcept {
    return 0x4E616800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
static_assert(fcvtn2_4s_2d(1, 5) == 0x4E6168A1u);  // llvm-mc: fcvtn2 v1.4s, v5.2d

/// Encode ARM64 FCVTL2 Vd.2D, Vn.4S — widen the UPPER 2 singles to 2 doubles.
constexpr u32 fcvtl2_2d_4s(u8 d, u8 n) noexcept {
    return 0x4E617800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
static_assert(fcvtl2_2d_4s(6, 1) == 0x4E617826u);  // llvm-mc: fcvtl2 v6.2d, v1.4s

/// Encode ARM64 FCVTN Vd.4H, Vn.4S — narrow 4 singles into the low 4 halves.
constexpr u32 fcvtn_4h_4s(u8 d, u8 n) noexcept {
    return 0x0E216800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode ARM64 FCVTN2 Vd.8H, Vn.4S — narrow into the UPPER 4 halves.
constexpr u32 fcvtn2_8h_4s(u8 d, u8 n) noexcept {
    return 0x4E216800u | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode FADD Vd.T, Vn.T, Vm.T — packed FP add.
constexpr u32 fadd_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x4E20D400u | (uimm_field("fadd_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode FSUB Vd.T, Vn.T, Vm.T — packed FP sub.
constexpr u32 fsub_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x4EA0D400u | (uimm_field("fsub_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode FMUL Vd.T, Vn.T, Vm.T — packed FP multiply.
constexpr u32 fmul_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x6E20DC00u | (uimm_field("fmul_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode FDIV Vd.T, Vn.T, Vm.T — packed FP divide.
constexpr u32 fdiv_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x6E20FC00u | (uimm_field("fdiv_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// FMLA Vd.T, Vn.T, Vm.T — Vd += Vn * Vm (Q=1). Residual a + (−r)*b.
constexpr u32 fmla_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x4E20CC00u | (uimm_field("fmla_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// FMLS Vd.T, Vn.T, Vm.T — Vd -= Vn * Vm (Q=1). Same encoding as FMLA with
/// the op bit (23) set. Fused, like FMLA: one rounding, which is what makes
/// these an exact match for x86 FMA.
constexpr u32 fmls_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x4EA0CC00u | (uimm_field("fmls_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// FCMEQ Vd.<T>, Vn.<T>, Vm.<T> — packed FP equal compare.
constexpr u32 fcmeq_v(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x4E20E400u | (uimm_field("fcmeq_v", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FCMGE Vd.<T>, Vn.<T>, Vm.<T> — packed FP greater-or-equal compare.
constexpr u32 fcmge_v(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x6E20E400u | (uimm_field("fcmge_v", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FCMGT Vd.<T>, Vn.<T>, Vm.<T> — packed FP greater-than compare.
constexpr u32 fcmgt_v(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x6EA0E400u | (uimm_field("fcmgt_v", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FCMEQ <V>d, <V>n, <V>m (scalar, V = S or D) — compares element 0 only and
/// zeroes the rest of Vd (Arm ARM, Advanced SIMD scalar three same: the
/// vector encoding with bit 28 set and Q fixed at 1).
constexpr u32 fcmeq_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x5E20E400u | (uimm_field("fcmeq_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FCMGE <V>d, <V>n, <V>m (scalar) — element 0 only.
constexpr u32 fcmge_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x7E20E400u | (uimm_field("fcmge_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FCMGT <V>d, <V>n, <V>m (scalar) — element 0 only.
constexpr u32 fcmgt_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x7EA0E400u | (uimm_field("fcmgt_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode FMAX Vd.T, Vn.T, Vm.T — packed FP max.
constexpr u32 fmax_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x4E20F400u | (uimm_field("fmax_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode FMIN Vd.T, Vn.T, Vm.T — packed FP min.
constexpr u32 fmin_v(u8 d, u8 n, u8 m, FSize sz) noexcept {
    return 0x4EA0F400u | (uimm_field("fmin_v", sz, 1) << 22)
                       | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode FSQRT Vd.T, Vn.T — packed FP square root (two-reg misc form).
constexpr u32 fsqrt_v(u8 d, u8 n, FSize sz) noexcept {
    return 0x6EA1F800u | (uimm_field("fsqrt_v", sz, 1) << 22)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

// ---- Scalar FP ops (FSize: FS_S=single, FS_D=double). ----
// 2-source: 0x1E200800 | (fs<<22) | (opc<<12) | (Rm<<16) | (Rn<<5) | Rd
//   opc: FMUL=0, FDIV=1, FADD=2, FSUB=3, FMAX=4, FMIN=5.
constexpr u32 fadd_s_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x1E200800u | (uimm_field("fadd_s_scalar", fs, 1) << 22) | (2u << 12)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 fsub_s_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x1E200800u | (uimm_field("fsub_s_scalar", fs, 1) << 22) | (3u << 12)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 fmul_s_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x1E200800u | (uimm_field("fmul_s_scalar", fs, 1) << 22) | (0u << 12)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 fdiv_s_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x1E200800u | (uimm_field("fdiv_s_scalar", fs, 1) << 22) | (1u << 12)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FMSUB Sd, Sn, Sm, Sa = Sa − Sn*Sm. Residual a − r*b for MXCSR.PE.
constexpr u32 fmsub_s_scalar(u8 d, u8 n, u8 m, u8 a, FSize fs) noexcept {
    return 0x1F008000u | (uimm_field("fmsub_s_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(a) << 10)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FMADD Sd, Sn, Sm, Sa = Sa + Sn*Sm (scalar, fused).
constexpr u32 fmadd_s_scalar(u8 d, u8 n, u8 m, u8 a, FSize fs) noexcept {
    return 0x1F000000u | (uimm_field("fmadd_s_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(a) << 10)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FNMADD Sd, Sn, Sm, Sa = -Sa - Sn*Sm (scalar, fused).
constexpr u32 fnmadd_s_scalar(u8 d, u8 n, u8 m, u8 a, FSize fs) noexcept {
    return 0x1F200000u | (uimm_field("fnmadd_s_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(a) << 10)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FNMSUB Sd, Sn, Sm, Sa = -Sa + Sn*Sm (scalar, fused).
constexpr u32 fnmsub_s_scalar(u8 d, u8 n, u8 m, u8 a, FSize fs) noexcept {
    return 0x1F208000u | (uimm_field("fnmsub_s_scalar", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(a) << 10)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode FMOV Sd, Wn / FMOV Wd, Sn / FMOV Xd, Dn / FMOV Dd, Xn.
constexpr u32 fmov_s_from_w(u8 d, u8 n) noexcept {
    return 0x1E270000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
constexpr u32 fmov_w_from_s(u8 d, u8 n) noexcept {
    return 0x1E260000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
constexpr u32 fmov_x_from_d(u8 d, u8 n) noexcept {
    return 0x9E660000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
constexpr u32 fmov_d_from_x(u8 d, u8 n) noexcept {
    return 0x9E670000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
/// FCMP Sd, Sm — sets NZCV (V=unordered, Z=equal, N=less).
constexpr u32 fcmp_s(u8 n, u8 m) noexcept {
    return 0x1E202000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5);
}
/// FCMP Dd, Dm — sets NZCV (V=unordered, Z=equal, N=less).
constexpr u32 fcmp_d(u8 n, u8 m) noexcept {
    return 0x1E602000u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5);
}
/// SCVTF Dd, Wn / SCVTF Dd, Xn.
constexpr u32 scvtf_d_from_w(u8 d, u8 n) noexcept {
    return 0x1E620000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
constexpr u32 scvtf_d_from_x(u8 d, u8 n) noexcept {
    return 0x9E620000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
/// Encode FCVT Dd, Sn (binary32 to binary64) / FCVT Sd, Dn (binary64 to binary32).
constexpr u32 fcvt_d_from_s(u8 d, u8 n) noexcept {
    return 0x1E22C000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
constexpr u32 fcvt_s_from_d(u8 d, u8 n) noexcept {
    return 0x1E624000u | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}
constexpr u32 fmax_s_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x1E200800u | (uimm_field("fmax_s_scalar", fs, 1) << 22) | (4u << 12)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 fmin_s_scalar(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x1E200800u | (uimm_field("fmin_s_scalar", fs, 1) << 22) | (5u << 12)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FSQRT Sd, Sn / FSQRT Dd, Dn (scalar, 1-source).
constexpr u32 fsqrt_s_scalar(u8 d, u8 n, FSize fs) noexcept {
    return 0x1E21C000u | (uimm_field("fsqrt_s_scalar", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// Encode FADDP Vd.T, Vn.T, Vm.T — pairwise FP add (HADDPS/HADDPD).
/// FS_S → Vd.4S, FS_D → Vd.2D. Result lanes:
/// S: [n0+n1, n2+n3, m0+m1, m2+m3].  D: [n0+n1, m0+m1].
constexpr u32 faddp_v(u8 d, u8 n, u8 m, FSize fs) noexcept {
    return 0x6E20D400u | (uimm_field("faddp_v", fs, 1) << 22)
                      | (static_cast<u32>(m) << 16)
                      | (static_cast<u32>(n) << 5)
                      | static_cast<u32>(d);
}

/// FNEG Vd.T, Vn.T — packed FP negate. fs=FS_S→4S, FS_D→2D.
constexpr u32 fneg_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x6EA0F800u | (uimm_field("fneg_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// FRECPE Vd.T, Vn.T — packed FP reciprocal estimate.
constexpr u32 frecpe_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x4EA1D800u | (uimm_field("frecpe_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// FRSQRTE Vd.T, Vn.T — packed FP reciprocal sqrt estimate.
constexpr u32 frsqrte_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x6EA1D800u | (uimm_field("frsqrte_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

// ---- FP rounding (vector) ----
/// FRINTN Vd.T, Vn.T — round to nearest, ties to even (x86 ROUND mode 0).
constexpr u32 frintn_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x4E218800u | (uimm_field("frintn_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FRINTM Vd.T, Vn.T — round toward -inf (floor) (x86 ROUND mode 1).
constexpr u32 frintm_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x4E219800u | (uimm_field("frintm_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FRINTP Vd.T, Vn.T — round toward +inf (ceil) (x86 ROUND mode 2).
constexpr u32 frintp_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x4EA18800u | (uimm_field("frintp_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
/// FRINTZ Vd.T, Vn.T — round toward zero (trunc) (x86 ROUND mode 3).
constexpr u32 frintz_v(u8 d, u8 n, FSize fs) noexcept {
    return 0x4EA19800u | (uimm_field("frintz_v", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

// ---- FP rounding (scalar) ----
// Format: 0_0_0_11110_TYPE:2_1_OPCODE:6_10000_Rn_Rd
// OPCODE: FRINTN=0x08, FRINTP=0x09, FRINTM=0x0A, FRINTZ=0x0B, FRINTA=0x0C.
constexpr u32 frintn_s(u8 d, u8 n, FSize fs) noexcept {
    return 0x1E244000u | (uimm_field("frintn_s", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 frintp_s(u8 d, u8 n, FSize fs) noexcept {
    return 0x1E24C000u | (uimm_field("frintp_s", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 frintm_s(u8 d, u8 n, FSize fs) noexcept {
    return 0x1E254000u | (uimm_field("frintm_s", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}
constexpr u32 frintz_s(u8 d, u8 n, FSize fs) noexcept {
    return 0x1E25C000u | (uimm_field("frintz_s", fs, 1) << 22)
                      | (static_cast<u32>(n) << 5) | static_cast<u32>(d);
}

/// FMOV Vd.4S, #1.0 — broadcast 1.0f to all 4 lanes.
constexpr u32 fmov_v4s_one(u8 d) noexcept {
    return 0x4F03F600u | static_cast<u32>(d);
}
/// FMOV Vd.2D, #1.0 — broadcast 1.0 to both lanes.
constexpr u32 fmov_v2d_one(u8 d) noexcept {
    return 0x6F03F600u | static_cast<u32>(d);
}

} // namespace vitreo::cpu::jit::arm64
