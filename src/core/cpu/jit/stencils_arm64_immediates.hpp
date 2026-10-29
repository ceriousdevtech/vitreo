// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_immediates.hpp
/// @brief ARM64 move-wide (MOVZ/MOVN/MOVK) and add/sub (immediate) encoders.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <core/cpu/jit/stencils_arm64_fields.hpp>
#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// Encode ARM64 MOVZ Wd, #imm16
constexpr u32 movz_w(u8 d, u64 imm16) noexcept {
    return 0x52800000u | (uimm16("movz_w", imm16) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MOVZ Wd, #imm16, LSL #shift (shift: 0 or 16).
constexpr u32 movz_w_shift(u8 d, u64 imm16, u64 shift) noexcept {
    return 0x52800000u | (mov_hw("movz_w_shift", shift, 16) << 21)
                       | (uimm16("movz_w_shift", imm16) << 5)
                       | static_cast<u32>(d);
}
/// Encode ARM64 MOVN Wd, #imm16, LSL #shift (shift: 0 or 16): Wd =
/// ~(imm16 << shift), upper 32 bits of Xd zeroed. Like MOVZ/MOVK it leaves
/// NZCV alone.
constexpr u32 movn_w(u8 d, u64 imm16, u64 shift) noexcept {
    return 0x12800000u | (mov_hw("movn_w", shift, 16) << 21)
                       | (uimm16("movn_w", imm16) << 5)
                       | static_cast<u32>(d);
}
// llvm-mc: mov w1,#-100; mov w23,#0x7fffffff; mov w23,#0x10000; mov w0,#-1;
// mov w5,#-65536
static_assert(movn_w(1, 0x63, 0) == 0x12800C61u);
static_assert(movn_w(23, 0x8000, 16) == 0x12B00017u);
static_assert(movz_w_shift(23, 1, 16) == 0x52A00037u);
static_assert(movn_w(0, 0, 0) == 0x12800000u);
static_assert(movz_w_shift(5, 0xFFFF, 16) == 0x52BFFFE5u);

/// Encode ARM64 MOVK Wd, #imm16, LSL #shift (shift: 0 or 16)
constexpr u32 movk_w(u8 d, u64 imm16, u64 shift) noexcept {
    const u32 hw = mov_hw("movk_w", shift, 16);
    return 0x72800000u | (hw << 21)
                       | (uimm16("movk_w", imm16) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADD Xd, Xn, #imm12
constexpr u32 add_x_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0x91000000u | (uimm12("add_x_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADD Xd, Xn, #imm12, LSL #12
constexpr u32 add_x_imm_lsl12(u8 d, u8 n, u64 imm12) noexcept {
    return 0x91400000u | (uimm12("add_x_imm_lsl12", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Xd = Xn + `value` for any value below 2^24, as at most two ADD (immediate)
/// words: `ADD Xd, Xn, #hi, LSL #12` then `ADD Xd, Xd, #lo`, each dropped when
/// its half is zero (so Xd = Xd + 0 is no word at all). Flags untouched, no
/// scratch; Xn is read before Xd is written, so d == n is fine. Register 31 is
/// SP in these forms and is not a valid d or n. A value of 2^24 or more stops
/// emission like any other overflowing field.
struct AddUimm24 {
    u32 word[2];
    u32 count;
};
constexpr AddUimm24 add_x_uimm24(u8 d, u8 n, u64 value) noexcept {
    if (value > 0xFF'FFFFu) imm_field_overflow("add_x_uimm24", value, 0xFF'FFFFu);
    AddUimm24 seq{{0u, 0u}, 0u};
    const u64 low = value & 0xFFFu;
    const u64 high = value >> 12;
    if (high == 0) {
        if (low != 0 || d != n) seq.word[seq.count++] = add_x_imm(d, n, low);
        return seq;
    }
    seq.word[seq.count++] = add_x_imm_lsl12(d, n, high);
    if (low != 0) seq.word[seq.count++] = add_x_imm(d, d, low);
    return seq;
}

/// Encode ARM64 SUB Xd, Xn, #imm12
constexpr u32 sub_x_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0xD1000000u | (uimm12("sub_x_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUB Xd, Xn, #imm12, LSL #12
constexpr u32 sub_x_imm_lsl12(u8 d, u8 n, u64 imm12) noexcept {
    return 0xD1400000u | (uimm12("sub_x_imm_lsl12", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADDS Xd, Xn, #imm12 (64-bit, sets flags)
constexpr u32 adds_x_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0xB1000000u | (uimm12("adds_x_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUBS Xd, Xn, #imm12 (64-bit, sets flags)
constexpr u32 subs_x_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0xF1000000u | (uimm12("subs_x_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CMP Xn, #imm12 (alias of SUBS XZR, Xn, #imm12)
constexpr u32 cmp_x_imm(u8 n, u64 imm12) noexcept {
    return subs_x_imm(31, n, imm12);
}

/// Encode ARM64 MOVZ Xd, #imm16, LSL #shift (shift: 0,16,32,48).
constexpr u32 movz_x(u8 d, u64 imm16, u64 shift) noexcept {
    const u32 hw = mov_hw("movz_x", shift, 48);
    return 0xD2800000u | (hw << 21)
                       | (uimm16("movz_x", imm16) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MOVK Xd, #imm16, LSL #shift (shift: 0,16,32,48).
constexpr u32 movk_x(u8 d, u64 imm16, u64 shift) noexcept {
    const u32 hw = mov_hw("movk_x", shift, 48);
    return 0xF2800000u | (hw << 21)
                       | (uimm16("movk_x", imm16) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 MOVN Xd, #imm16, LSL #shift — Xd = ~(imm16 << shift).
constexpr u32 movn_x(u8 d, u64 imm16, u64 shift) noexcept {
    const u32 hw = mov_hw("movn_x", shift, 48);
    return 0x92800000u | (hw << 21)
                       | (uimm16("movn_x", imm16) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADD Wd, Wn, #imm12
constexpr u32 add_w_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0x11000000u | (uimm12("add_w_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUB Wd, Wn, #imm12
constexpr u32 sub_w_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0x51000000u | (uimm12("sub_w_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 ADDS Wd, Wn, #imm12 (32-bit, sets flags)
constexpr u32 adds_w_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0x31000000u | (uimm12("adds_w_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 SUBS Wd, Wn, #imm12 (32-bit, sets flags)
constexpr u32 subs_w_imm(u8 d, u8 n, u64 imm12) noexcept {
    return 0x71000000u | (uimm12("subs_w_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5)
                       | static_cast<u32>(d);
}

/// Encode ARM64 CMP Wn, #imm12 (SUBS WZR, Wn, #imm12)
constexpr u32 cmp_w_imm(u8 n, u64 imm12) noexcept {
    return 0x7100001Fu | (uimm12("cmp_w_imm", imm12) << 10)
                       | (static_cast<u32>(n) << 5);
}

} // namespace vitreo::cpu::jit::arm64
