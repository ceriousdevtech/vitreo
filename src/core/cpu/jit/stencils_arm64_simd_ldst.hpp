// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_simd_ldst.hpp
/// @brief ARM64 NEON element-size vocabulary and SIMD&FP load/store encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

// ============================================================================
// ARM64 NEON (ASIMD) encoding helpers — 128-bit quad form (Q=1)
// Used for native SSE emission (copy-and-patch path).
// ============================================================================

/// SIMD element size encoding used by NEON three-same-element form.
enum VSize : u8 { VS_B = 0, VS_H = 1, VS_S = 2, VS_D = 3 };

/// Packed FP element size (0 = single/4S, 1 = double/2D).
enum FSize : u8 { FS_S = 0, FS_D = 1 };

/// Encode ARM64 LDP Qt, Qt2, [Xn, #imm7*16] (SIMD&FP pair, signed scaled offset).
constexpr u32 ldp_q_off(u8 t, u8 t2, u8 n, i64 imm7) noexcept {
    return 0xAD400000u
         | (simm7("ldp_q_off", imm7) << 15)
         | (static_cast<u32>(t2) << 10)
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(t);
}
static_assert(ldp_q_off(0, 1, 17, 0) == 0xAD400620u);   // ldp q0, q1, [x17]

/// Encode ARM64 STP Qt, Qt2, [Xn, #imm7*16] (SIMD&FP pair, signed scaled offset).
constexpr u32 stp_q_off(u8 t, u8 t2, u8 n, i64 imm7) noexcept {
    return 0xAD000000u
         | (simm7("stp_q_off", imm7) << 15)
         | (static_cast<u32>(t2) << 10)
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(t);
}
static_assert(stp_q_off(0, 1, 17, 0) == 0xAD000620u);   // stp q0, q1, [x17]

/// Encode ARM64 LDR Qt, [Xn, #uimm12] (128-bit quad load, pimm scaled by 16)
constexpr u32 ldr_q_uoff(u8 t, u8 n, u64 pimm12) noexcept {
    return 0x3DC00000u | (uimm12("ldr_q_uoff", pimm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR St, [Xn, #uimm12] (32-bit SIMD&FP load, pimm scaled by 4).
/// Loads xmm lane 0 straight into the FP register file: the scalar SS
/// lowerings need no GPR round trip, and the upper lanes of Vt are zeroed
/// (which keeps a following FCMEQ NaN test on the whole vector honest). It
/// also keeps the multi-cycle GPR->SIMD cross-domain move out of a compare's
/// dependency chain (COMISS/UCOMISS).
constexpr u32 ldr_s_uoff(u8 t, u8 n, u64 pimm12) noexcept {
    return 0xBD400000u | (uimm12("ldr_s_uoff", pimm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STR St, [Xn, #uimm12] (32-bit SIMD&FP store, pimm scaled by 4).
/// Writes only lane 0, so the destination xmm's upper 96 bits are preserved
/// (Intel SDM Vol. 2 ADDSS: DEST[127:32] unchanged).
constexpr u32 str_s_uoff(u8 t, u8 n, u64 pimm12) noexcept {
    return 0xBD000000u | (uimm12("str_s_uoff", pimm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR Dt, [Xn, #uimm12] (64-bit SIMD&FP load, pimm scaled by 8).
constexpr u32 ldr_d_uoff(u8 t, u8 n, u64 pimm12) noexcept {
    return 0xFD400000u | (uimm12("ldr_d_uoff", pimm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STR Dt, [Xn, #uimm12] (64-bit SIMD&FP store, pimm scaled by 8).
constexpr u32 str_d_uoff(u8 t, u8 n, u64 pimm12) noexcept {
    return 0xFD000000u | (uimm12("str_d_uoff", pimm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STR Qt, [Xn, #uimm12] (128-bit quad store, pimm scaled by 16)
constexpr u32 str_q_uoff(u8 t, u8 n, u64 pimm12) noexcept {
    return 0x3D800000u | (uimm12("str_q_uoff", pimm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR Qt, [Xn, Xm] (128-bit register offset, LSL #0).
/// Same size/V/opc/option/S layout as ldr_x_reg above, with size=00, V=1,
/// opc=11 selecting the 128-bit SIMD&FP form.
constexpr u32 ldr_q_reg(u8 t, u8 n, u8 m) noexcept {
    return 0x3CE06800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STR Qt, [Xn, Xm] (128-bit register offset, LSL #0).
constexpr u32 str_q_reg(u8 t, u8 n, u8 m) noexcept {
    return 0x3CA06800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

} // namespace vitreo::cpu::jit::arm64
