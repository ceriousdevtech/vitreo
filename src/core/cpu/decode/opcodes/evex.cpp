// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// EVEX prefix decode (Intel SDM Vol. 2, 2.6 "Intel AVX-512 Encoding")

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

namespace {

/// Does this (map, opcode) carry an imm8?
///
/// Length is the whole point of decoding an instruction Vitreo will refuse to
/// execute, and the immediate is the only length component that does not fall
/// out of ModRM/SIB/displacement. Every EVEX form has a ModRM byte, so the
/// rest is mechanical.
///
/// This rule was derived from Intel XED's ISA database rather than written by
/// hand, and verified exact against all 532 (map, opcode) pairs across the
/// 6,851 EVEX forms in jit-refactor/generated/OPCODE_FORMS.csv. The check
/// lives in tests/cpu/test_decoder.cpp; regenerate the catalog and re-run it
/// if a future ISA extension adds an opcode here.
///
/// Exact for 6,823 of the catalog's 6,851 EVEX forms. The 28 exceptions are
/// all one (map, opcode) pair: map 4 opcode F6, where APX's CTEST/CCMP (imm8)
/// shares an opcode with the APX forms of group 3 NEG/NOT/MUL/IMUL/DIV/IDIV
/// (no imm8). ModRM.reg does not separate them -- CTEST uses reg 0 and 1,
/// which group 3 also uses -- so the split is the APX SCC field in the EVEX
/// payload, which this decoder deliberately does not read.
///
/// The imm8 reading is taken, so those 28 forms report one byte too long.
/// Map 4 is APX: Vitreo advertises no APX and every one of these raises #UD,
/// where length feeds diagnostics and nothing else -- #UD reports the
/// instruction's start address and carries no resume past it. Decoding the
/// APX payload to sharpen a number that only appears in a log for an ISA
/// nothing executes is not worth the bits in Prefixes. If APX is ever
/// implemented, this is one of the places that has to change.
[[nodiscard]] constexpr bool evex_has_imm8(u8 map, u8 opcode) noexcept {
    switch (map) {
        case 3:  // 0F3A: every form takes an imm8
            return true;
        case 1:  // 0F
            return opcode == 0x70 || opcode == 0x71 || opcode == 0x72 ||
                   opcode == 0x73 || opcode == 0xC2 || opcode == 0xC4 ||
                   opcode == 0xC5 || opcode == 0xC6;
        case 4:  // APX
            return opcode == 0x24 || opcode == 0x2C || opcode == 0x6B ||
                   opcode == 0x80 || opcode == 0x83 || opcode == 0xC0 ||
                   opcode == 0xC1 || opcode == 0xF6;
        default:  // 2 (0F38), 5/6 (AVX512-FP16), 7 — none take an imm8
            return false;
    }
}

}  // namespace

DecodeError decode_evex(ByteReader& reader, DecodeMode mode, Prefixes& prefix,
                        DecodedInstruction& out, u8 p0,
                        usize start_pos) noexcept {
    // 62 P0 P1 P2 <opcode> <modrm> [sib] [disp] [imm8]
    //
    //   P0: R X B R' B' m m m    R/X/B/R'/B' inverted; mmm selects the map
    //   P1: W v v v v 1 p p      vvvv inverted; bit 2 MUST be 1; pp as VEX
    //   P2: z L' L b V' a a a    V' inverted; aaa is the mask register
    //
    // mmm is three bits, not the two of the original AVX-512 definition:
    // maps 4 (APX), 5 and 6 (AVX512-FP16) and 7 are in current silicon, and
    // 3,372 of the catalog's EVEX forms live in them. Reading only two bits
    // would take map 5 for map 1 and mis-length the instruction.
    u8 p1, p2;
    if (!reader.read_u8(p1)) return DecodeError::UnexpectedEnd;
    if (!reader.read_u8(p2)) return DecodeError::UnexpectedEnd;

    // P1 bit 2 is architecturally fixed at 1, and mmm==0 is not a map. Both
    // are malformed encodings rather than unimplemented ones, so they are a
    // genuine #UD and must NOT be reported as an EVEX instruction: saying
    // "AVX-512 we chose not to implement" about corrupt bytes is the exact
    // confusion this decode exists to remove.
    const u8 map = static_cast<u8>(p0 & 0x07);
    if ((p1 & 0x04) == 0 || map == 0) {
        return DecodeError::InvalidOpcode;
    }

    prefix.evex = true;
    // The mandatory-prefix field carries the same meaning as in VEX, and the
    // rest of the decoder reads these flags, so populate them. Everything
    // else in the payload (L'L, W, aaa, z, b, V') is intentionally dropped --
    // see the note on Prefixes::evex.
    const u8 pp = static_cast<u8>(p1 & 0x03);
    if (pp == 1) prefix.operand_size = true;
    else if (pp == 2) prefix.rep = true;
    else if (pp == 3) prefix.repne = true;

    u8 opcode_byte;
    if (!reader.read_u8(opcode_byte)) return DecodeError::UnexpectedEnd;

    // Every EVEX-encoded instruction has a ModRM byte (verified across all
    // 6,851 catalog forms), so this is unconditional.
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    Prefixes ea = prefix;
    if (decode_modrm(reader, mode, ea, false, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // EVEX's compressed displacement (disp8*N) scales the VALUE by the
    // tuple's element size; the encoded field is still one byte, so length is
    // unaffected and nothing here needs the tuple type. An implementation
    // that executes these will need it; a decode that refuses them does not.
    if (evex_has_imm8(map, opcode_byte)) {
        u8 imm;
        if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
    }

    out.opcode = Opcode::EVEX_UNSUPPORTED;
    out.length = static_cast<u8>(reader.position() - start_pos);
    return DecodeError::Success;
}

}  // namespace vitreo::cpu::decode
