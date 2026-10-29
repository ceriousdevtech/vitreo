// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_load_store.hpp
/// @brief ARM64 general-register load/store encoders: unsigned, register, UXTW, unscaled and pair forms.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 LDR Wt, [Xn, #imm12] (unsigned offset, scaled by 4)
constexpr u32 ldr_w_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0xB9400000u | (uimm12("ldr_w_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STR Wt, [Xn, #imm12] (unsigned offset, scaled by 4)
constexpr u32 str_w_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0xB9000000u | (uimm12("str_w_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR Xt, [Xn, #imm12] (unsigned 64-bit, scaled by 8)
/// LDRSW Xt, [Xn, #imm12*4] — load 32 bits and SIGN-extend to 64. Gather
/// index elements are sign-extended before scaling, so a plain LDR W would
/// turn a negative index into a huge positive offset.
constexpr u32 ldrsw_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0xB9800000u | (uimm12("ldrsw_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

constexpr u32 ldr_x_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0xF9400000u | (uimm12("ldr_x_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDP Xt, Xt2, [Xn, #imm7*8] (signed scaled offset).
constexpr u32 ldp_x_off(u8 t, u8 t2, u8 n, i64 imm7) noexcept {
    return 0xA9400000u
         | (simm7("ldp_x_off", imm7) << 15)
         | (static_cast<u32>(t2) << 10)
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(t);
}
static_assert(ldp_x_off(23, 24, 19, 0) == 0xA9406277u);
static_assert(ldp_x_off(25, 26, 19, 2) == 0xA9416A79u);
static_assert(ldp_x_off(12, 13, 19, 4) == 0xA942366Cu);
static_assert(ldp_x_off(27, 28, 19, 6) == 0xA943727Bu);

/// Encode ARM64 STP Xt, Xt2, [Xn, #imm7*8] (signed scaled offset).
constexpr u32 stp_x_off(u8 t, u8 t2, u8 n, i64 imm7) noexcept {
    return 0xA9000000u
         | (simm7("stp_x_off", imm7) << 15)
         | (static_cast<u32>(t2) << 10)
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(t);
}
static_assert(stp_x_off(4, 0, 3, 0) == 0xA9000064u);

/// Encode ARM64 STP Wt, Wt2, [Xn, #imm7*4] (32-bit pair, signed scaled
/// offset). With Wt2 = WZR it stores a zero-extended 32-bit value into a
/// 64-bit slot in one instruction.
constexpr u32 stp_w_off(u8 t, u8 t2, u8 n, i64 imm7) noexcept {
    return 0x29000000u
         | (simm7("stp_w_off", imm7) << 15)
         | (static_cast<u32>(t2) << 10)
         | (static_cast<u32>(n) << 5)
         | static_cast<u32>(t);
}
// llvm-mc: stp w25, wzr, [x19, #64]; stp w0, wzr, [x19]; stp w3, wzr, [x19, #252]
static_assert(stp_w_off(25, 31, 19, 16) == 0x29087E79u);
static_assert(stp_w_off(0, 31, 19, 0) == 0x29007E60u);
static_assert(stp_w_off(3, 31, 19, 63) == 0x291FFE63u);

/// Encode ARM64 STR Xt, [Xn, #imm12] (unsigned 64-bit, scaled by 8)
constexpr u32 str_x_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0xF9000000u | (uimm12("str_x_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR Xt, [Xn, Xm] (64-bit register offset)
constexpr u32 ldr_x_reg(u8 t, u8 n, u8 m) noexcept {
    return 0xF8606800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR Xt, [Xn, Xm, LSL #3] (64-bit scaled register offset).
constexpr u32 ldr_x_reg_lsl3(u8 t, u8 n, u8 m) noexcept {
    return 0xF8607800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
static_assert(ldr_x_reg_lsl3(17, 16, 3) == 0xF8637A11u);

/// Encode ARM64 STR Xt, [Xn, Xm] (64-bit register offset)
constexpr u32 str_x_reg(u8 t, u8 n, u8 m) noexcept {
    return 0xF8206800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDR Wt, [Xn, Xm] (register offset)
constexpr u32 ldr_w_reg(u8 t, u8 n, u8 m) noexcept {
    return 0xB8606800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STR Wt, [Xn, Xm] (register offset)
constexpr u32 str_w_reg(u8 t, u8 n, u8 m) noexcept {
    return 0xB8206800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDRB Wt, [Xn, Xm] (byte load, register offset)
constexpr u32 ldrb_reg(u8 t, u8 n, u8 m) noexcept {
    return 0x38606800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STRB Wt, [Xn, Xm] (byte store, register offset)
constexpr u32 strb_reg(u8 t, u8 n, u8 m) noexcept {
    return 0x38206800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 LDRH Wt, [Xn, Xm] (halfword load, register offset)
constexpr u32 ldrh_reg(u8 t, u8 n, u8 m) noexcept {
    return 0x78606800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STRH Wt, [Xn, Xm] (halfword store, register offset)
constexpr u32 strh_reg(u8 t, u8 n, u8 m) noexcept {
    return 0x78206800u | (static_cast<u32>(m) << 16)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STRH Wt, [Xn, #imm12] (halfword store, unsigned offset scaled by 2)
constexpr u32 strh_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0x79000000u | (uimm12("strh_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

// ---------------------------------------------------------------------------
// UXTW register-offset load/store: access [Xn + zero_extend32(Wm)]. Used by the
// linear (guest-base) memory path — one instruction does host_base +
// zext(guest_addr32) + access. Encodings verified against the assembler.
// ---------------------------------------------------------------------------
constexpr u32 ldr_w_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0xB8604800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 str_w_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0xB8204800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 ldr_x_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0xF8604800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 str_x_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0xF8204800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 ldrb_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0x38604800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 strb_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0x38204800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 ldrh_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0x78604800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}
constexpr u32 strh_uxtw(u8 t, u8 n, u8 m) noexcept {
    return 0x78204800u | (static_cast<u32>(m) << 16) | (static_cast<u32>(n) << 5) | t;
}

/// Encode ARM64 LDUR Xt, [Xn, #simm9] (unscaled 64-bit load, signed 9-bit offset).
constexpr u32 ldur_x(u8 t, u8 n, i64 simm9) noexcept {
    const u32 off = arm64::simm9("ldur_x", simm9);
    return 0xF8400000u | (off << 12)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

/// Encode ARM64 STUR Xt, [Xn, #simm9] (unscaled 64-bit store).
constexpr u32 stur_x(u8 t, u8 n, i64 simm9) noexcept {
    const u32 off = arm64::simm9("stur_x", simm9);
    return 0xF8000000u | (off << 12)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}
constexpr u32 strb_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0x39000000u | (uimm12("strb_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

constexpr u32 ldrb_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0x39400000u | (uimm12("ldrb_uoff", imm12) << 10)
         | (static_cast<u32>(n) << 5) | t;
}

/// Encode ARM64 LDRH Wt, [Xn, #imm12] (halfword load, unsigned offset scaled
/// by 2). The register-offset form ldrh_reg needs the address materialised in
/// a scratch first; every x87 status-word read pays that ADD for nothing.
constexpr u32 ldrh_uoff(u8 t, u8 n, u64 imm12) noexcept {
    return 0x79400000u | (uimm12("ldrh_uoff", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(t);
}

} // namespace vitreo::cpu::jit::arm64
