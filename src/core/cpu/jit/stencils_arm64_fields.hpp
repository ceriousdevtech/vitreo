// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file stencils_arm64_fields.hpp
/// @brief Checked immediate-field helpers every arm64:: encoder routes through.
///
/// Part of the arm64:: encoder set; include core/cpu/jit/stencils.hpp.

#include <vitreo/common.hpp>

namespace vitreo::cpu::jit::arm64 {

/// A value too wide for the fixed-width immediate field of the instruction
/// being encoded. Fatal, by design: the encoders used to MASK the value, and a
/// masked immediate is a different instruction, not a failure -- x64
/// `RET imm16` with imm16 >= 4088 lowered as ADD RSP, RSP, #((8 + imm16) mod
/// 4096). A caller whose value can exceed the field tests it first
/// (fits_uimm12, fits_ldr_*_offset) or uses an emitter that splits or
/// materializes it (JitCompiler::emit_add_x_uimm24).
[[noreturn]] void imm_field_overflow(const char* encoder, u64 value,
                                     u64 field_max) noexcept;
/// The signed twin: `value` lies outside [field_min, field_max].
[[noreturn]] void simm_field_overflow(const char* encoder, i64 value,
                                      i64 field_min, i64 field_max) noexcept;
/// A value the field can only hold in multiples of `granule` (a PC-relative
/// branch's byte offset is a whole number of 4-byte words; MOVZ/MOVK/MOVN's
/// shift is hw * 16) is not one.
[[noreturn]] void imm_field_misaligned(const char* encoder, i64 value,
                                       u64 granule) noexcept;
/// A logical (immediate) operand that is not a bitmask immediate: no
/// N:immr:imms describes it (A64 DecodeBitMasks), so no AND/ORR/EOR/ANDS
/// (immediate) instruction computes it.
[[noreturn]] void logical_imm_unencodable(const char* encoder, u64 value,
                                          unsigned datasize) noexcept;

// Checked field helpers. Every arm64:: encoder routes each of its immediate,
// shift, bit-position, lane and condition fields through one of these, which
// returns the field's bits for an in-range value and stops emission for
// anything else -- never masks. In a constant expression an out-of-range
// value does not compile. Each takes the caller's value at full 64-bit width
// so the check runs BEFORE any narrowing (a u32 parameter would turn a 64-bit
// 2^32 + 5 into #5 before the check ran). Ranges are the Arm ARM A64
// encodings of the instructions named.

/// An unsigned field whose largest value is `field_max`: shift amounts,
/// immr/imms, bit numbers, lane indices, condition codes, MOVZ's imm16.
constexpr u32 uimm_field(const char* encoder, u64 value, u64 field_max) noexcept {
    if (value > field_max) imm_field_overflow(encoder, value, field_max);
    return static_cast<u32>(value);
}

/// The 12-bit unsigned immediate of ADD/SUB (immediate) -- `imm12`, shifted by
/// `sh` = LSL #0 or #12 -- and the scaled 12-bit offset of LDR/STR (immediate,
/// unsigned offset).
constexpr u32 uimm12(const char* encoder, u64 value) noexcept {
    return uimm_field(encoder, value, 0xFFFu);
}

/// MOVZ/MOVK/MOVN `imm16`.
constexpr u32 uimm16(const char* encoder, u64 value) noexcept {
    return uimm_field(encoder, value, 0xFFFFu);
}

/// A signed two's-complement field `bits` wide: [-2^(bits-1), 2^(bits-1) - 1].
/// Returns the field's low `bits` bits, ready to shift into place.
constexpr u32 simm_field(const char* encoder, i64 value, unsigned bits) noexcept {
    const i64 lo = -(i64{1} << (bits - 1));
    const i64 hi = (i64{1} << (bits - 1)) - 1;
    if (value < lo || value > hi) simm_field_overflow(encoder, value, lo, hi);
    return static_cast<u32>(static_cast<u64>(value) & ((u64{1} << bits) - 1u));
}

/// LDP/STP (signed offset) `imm7`, already divided by the access size
/// (the encoders' callers pass the scaled index): [-64, 63].
constexpr u32 simm7(const char* encoder, i64 scaled) noexcept {
    return simm_field(encoder, scaled, 7);
}

/// LDUR/STUR and the pre/post-index forms' unscaled `imm9`: [-256, 255].
constexpr u32 simm9(const char* encoder, i64 value) noexcept {
    return simm_field(encoder, value, 9);
}

/// A PC-relative branch's word displacement from its BYTE offset: a whole
/// number of words that fits `bits`. B/BL imm26 reaches +-128 MiB, B.cond,
/// CBZ/CBNZ and LDR (literal) imm19 +-1 MiB, and TBZ/TBNZ imm14 only +-32 KiB
/// -- the field a large block can actually outgrow (hot_to_stub_branch_reach).
constexpr u32 branch_disp(const char* encoder, i64 offset_bytes,
                          unsigned bits) noexcept {
    if ((offset_bytes & 3) != 0) imm_field_misaligned(encoder, offset_bytes, 4);
    return simm_field(encoder, offset_bytes / 4, bits);
}
constexpr u32 imm26(const char* encoder, i64 offset_bytes) noexcept {
    return branch_disp(encoder, offset_bytes, 26);
}
constexpr u32 imm19(const char* encoder, i64 offset_bytes) noexcept {
    return branch_disp(encoder, offset_bytes, 19);
}
constexpr u32 imm14(const char* encoder, i64 offset_bytes) noexcept {
    return branch_disp(encoder, offset_bytes, 14);
}

/// ADR's 21-bit signed BYTE offset (+-1 MiB), as immhi:immlo.
constexpr u32 adr_imm21(const char* encoder, i64 offset_bytes) noexcept {
    return simm_field(encoder, offset_bytes, 21);
}

/// MOVZ/MOVK/MOVN `hw` from the LSL amount: a multiple of 16 no larger than
/// 16 for the W forms and 48 for the X forms.
constexpr u32 mov_hw(const char* encoder, u64 shift, u64 max_shift) noexcept {
    if ((shift & 15u) != 0) imm_field_misaligned(encoder, static_cast<i64>(shift), 16);
    return uimm_field(encoder, shift, max_shift) >> 4;
}

/// A bitfield's (lsb, width) for UBFX/BFI and their SBFM/UBFM/BFM bases:
/// lsb in [0, datasize - 1] and width in [1, datasize - lsb].
constexpr void bitfield(const char* encoder, u64 lsb, u64 width,
                        u64 datasize) noexcept {
    (void)uimm_field(encoder, lsb, datasize - 1);
    if (width < 1 || width > datasize - lsb) {
        simm_field_overflow(encoder, static_cast<i64>(width), 1,
                            static_cast<i64>(datasize - lsb));
    }
}

/// A logical (immediate) operand's N:immr:imms (A64 DecodeBitMasks, inverted):
/// `value` must be, within `datasize` (32 or 64) bits, a run of ones rotated
/// right and replicated across an element of 2, 4, 8, 16, 32 or 64 bits. Zero
/// and all-ones are not encodable. Returns N << 12 | immr << 6 | imms, the
/// 13-bit field at bits [22:10].
constexpr u32 logical_imm(const char* encoder, u64 value, unsigned datasize) noexcept {
    const u64 mask = datasize == 64 ? ~u64{0} : (u64{1} << datasize) - 1u;
    if ((datasize != 32 && datasize != 64) || (value & ~mask) != 0 ||
        value == 0 || value == mask) {
        logical_imm_unencodable(encoder, value, datasize);
    }
    // The smallest element size the value replicates at.
    unsigned esize = datasize;
    while (esize > 2) {
        const unsigned half = esize / 2;
        const u64 hmask = (u64{1} << half) - 1u;
        if ((value & hmask) != ((value >> half) & hmask)) break;
        esize = half;
    }
    const u64 emask = esize == 64 ? ~u64{0} : (u64{1} << esize) - 1u;
    const u64 elem = value & emask;
    // DecodeBitMasks builds the element as ROR(Ones(imms + 1), immr), so immr
    // is the LEFT rotation that brings the element's run of ones down to
    // bit 0 -- a value whose successor is a power of two.
    unsigned rot = 0;
    u64 e = elem;
    while (rot < esize && (e & (e + 1)) != 0) {
        e = ((e << 1) | (e >> (esize - 1))) & emask;
        ++rot;
    }
    // No rotation lines it up: more than one run of ones.
    if (rot == esize) logical_imm_unencodable(encoder, value, datasize);
    unsigned ones = 0;
    while (ones < esize && ((e >> ones) & 1u) != 0) ++ones;
    // imms: the element size's prefix (NOT(esize*2 - 1) in its high bits) and
    // ones - 1 in the rest; N = 1 only for a 64-bit element.
    const u32 n = esize == 64 ? 1u : 0u;
    const u32 imms = static_cast<u32>((~(esize * 2u - 1u) & 0x3Fu) | (ones - 1u));
    const u32 immr = static_cast<u32>(rot);
    return (n << 12) | (immr << 6) | (imms & 0x3Fu);
}

} // namespace vitreo::cpu::jit::arm64
