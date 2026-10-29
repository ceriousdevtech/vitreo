// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// String and flag instruction decoding (MOVS, STOS, LODS, CMPS, SCAS, CLD, STD, CLC, STC, CMC, SAHF, LAHF)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_string_ops(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;

    switch (byte) {
        // MOVSB (A4)
        case 0xA4:
            out.opcode = Opcode::MOVSB;
            return DecodeError::Success;

        // MOVSW/MOVSD/MOVSQ (A5)
        case 0xA5:
            if (is_64bit_operand) {
                out.opcode = Opcode::MOVSQ;
            } else if (op_size == 2) {
                out.opcode = Opcode::MOVSW;
            } else {
                out.opcode = Opcode::MOVSD_STR;
            }
            return DecodeError::Success;

        // CMPSB (A6)
        case 0xA6:
            out.opcode = Opcode::CMPSB;
            return DecodeError::Success;

        // CMPSW/CMPSD/CMPSQ (A7)
        case 0xA7:
            if (is_64bit_operand) {
                out.opcode = Opcode::CMPSQ;
            } else if (op_size == 2) {
                out.opcode = Opcode::CMPSW;
            } else {
                out.opcode = Opcode::CMPSD_STR;
            }
            return DecodeError::Success;

        // STOSB (AA)
        case 0xAA:
            out.opcode = Opcode::STOSB;
            return DecodeError::Success;

        // STOSW/STOSD/STOSQ (AB)
        case 0xAB:
            if (is_64bit_operand) {
                out.opcode = Opcode::STOSQ;
            } else if (op_size == 2) {
                out.opcode = Opcode::STOSW;
            } else {
                out.opcode = Opcode::STOSD;
            }
            return DecodeError::Success;

        // LODSB (AC)
        case 0xAC:
            out.opcode = Opcode::LODSB;
            return DecodeError::Success;

        // LODSW/LODSD/LODSQ (AD)
        case 0xAD:
            if (is_64bit_operand) {
                out.opcode = Opcode::LODSQ;
            } else if (op_size == 2) {
                out.opcode = Opcode::LODSW;
            } else {
                out.opcode = Opcode::LODSD;
            }
            return DecodeError::Success;

        // SCASB (AE)
        case 0xAE:
            out.opcode = Opcode::SCASB;
            return DecodeError::Success;

        // SCASW/SCASD/SCASQ (AF)
        case 0xAF:
            if (is_64bit_operand) {
                out.opcode = Opcode::SCASQ;
            } else if (op_size == 2) {
                out.opcode = Opcode::SCASW;
            } else {
                out.opcode = Opcode::SCASD;
            }
            return DecodeError::Success;

        default:
            return DecodeError::InvalidOpcode;
    }
}

DecodeError decode_flag_ops(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;

    switch (byte) {
        case 0xF8:
            out.opcode = Opcode::CLC;
            return DecodeError::Success;

        case 0xF9:
            out.opcode = Opcode::STC;
            return DecodeError::Success;

        case 0xF5:
            out.opcode = Opcode::CMC;
            return DecodeError::Success;

        case 0xFC:
            out.opcode = Opcode::CLD;
            return DecodeError::Success;

        case 0xFD:
            out.opcode = Opcode::STD;
            return DecodeError::Success;

        case 0x9E:
            out.opcode = Opcode::SAHF;
            return DecodeError::Success;

        case 0x9F:
            out.opcode = Opcode::LAHF;
            return DecodeError::Success;

        default:
            return DecodeError::InvalidOpcode;
    }
}

} // namespace vitreo::cpu::decode
