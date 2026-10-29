// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Indirect control flow instruction decoding (LEAVE, ENTER)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_leave(DecodeContext& ctx) noexcept {
    auto& out = ctx.out;

    // Opcode C9: LEAVE — no operands
    out.opcode = Opcode::LEAVE;
    return DecodeError::Success;
}

DecodeError decode_enter(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& out = ctx.out;

    // Opcode C8: ENTER imm16, imm8
    // Read frame size (u16)
    u16 frame_size;
    if (!reader.read_u16(frame_size)) return DecodeError::UnexpectedEnd;

    // Read nesting level (u8)
    u8 nesting_level;
    if (!reader.read_u8(nesting_level)) return DecodeError::UnexpectedEnd;

    out.opcode = Opcode::ENTER;
    out.add_operand(Operand::make_imm(OperandType::Imm16, frame_size));
    out.add_operand(Operand::make_imm(OperandType::Imm8, nesting_level));

    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
