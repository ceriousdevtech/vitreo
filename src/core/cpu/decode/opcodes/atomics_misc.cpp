// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Decode: CMPXCHG, XADD, XCHG r/m, MOVSXD

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

// ============================================================================
// CMPXCHG — 0F B0 (8-bit), 0F B1 (16/32/64-bit)
// ============================================================================

DecodeError decode_cmpxchg(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (byte == 0xB0) {
        // 8-bit form
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
        out.opcode = Opcode::CMPXCHG_RM8_R8;
        out.add_operand(rm);
        out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
    } else {
        // 16/32/64-bit form
        if (is_64bit_operand) {
            out.opcode = Opcode::CMPXCHG_RM64_R64;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
        } else if (op_size == 2) {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
            out.opcode = Opcode::CMPXCHG_RM16_R16;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
        } else {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
            out.opcode = Opcode::CMPXCHG_RM32_R32;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
        }
    }

    return DecodeError::Success;
}

// ============================================================================
// XADD — 0F C0 (8-bit), 0F C1 (16/32/64-bit)
// ============================================================================

DecodeError decode_xadd(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (byte == 0xC0) {
        // 8-bit form
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
        out.opcode = Opcode::XADD_RM8_R8;
        out.add_operand(rm);
        out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
    } else {
        // 16/32/64-bit form
        if (is_64bit_operand) {
            out.opcode = Opcode::XADD_RM64_R64;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
        } else if (op_size == 2) {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
            out.opcode = Opcode::XADD_RM16_R16;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
        } else {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
            out.opcode = Opcode::XADD_RM32_R32;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
        }
    }

    return DecodeError::Success;
}

// ============================================================================
// XCHG r/m, r — opcodes 86 (8-bit), 87 (16/32/64-bit)
// ============================================================================

DecodeError decode_xchg_rm(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (byte == 0x86) {
        // 8-bit form
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;
        out.opcode = Opcode::XCHG_RM8_R8;
        out.add_operand(rm);
        out.add_operand(Operand::make_reg(OperandType::Reg8, reg_field));
    } else {
        // 16/32/64-bit form
        if (is_64bit_operand) {
            out.opcode = Opcode::XCHG_RM64_R64;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
        } else if (op_size == 2) {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
            out.opcode = Opcode::XCHG_RM16_R16;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
        } else {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
            out.opcode = Opcode::XCHG_RM32_R32;
            out.add_operand(rm);
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
        }
    }

    return DecodeError::Success;
}

// ============================================================================
// MOVSXD — opcode 63 (REX.W for 64-bit sign-extend of 32-bit source)
// ============================================================================

DecodeError decode_movsxd(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    bool is_64bit_operand = ctx.is_64bit_operand;
    DecodeMode mode = ctx.mode;

    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    // Opcode 63 is TWO instructions, selected by mode (Intel SDM Vol. 2):
    //   63 /r  ARPL r/m16, r16   -- valid in legacy/compatibility mode,
    //                               "N.E." (not encodable) in 64-bit mode
    //   63 /r  MOVSXD r64, r/m32 -- 64-bit mode ONLY
    // Decoding MOVSXD in 32-bit mode is a wrong-result bug, not a missing
    // feature: a 32-bit guest's ARPL silently performs a sign-extending move
    // and writes a full 32-bit destination instead of adjusting an RPL field
    // and setting ZF.
    if (mode != DecodeMode::Mode64) {
        Operand arpl_rm;
        u8 arpl_reg;
        if (decode_modrm(reader, mode, prefix, false, modrm, arpl_rm,
                         arpl_reg) < 0) {
            return DecodeError::InvalidModRM;
        }
        // Both operands are 16-bit regardless of the operand-size prefix.
        if (is_register_operand(arpl_rm.type)) {
            arpl_rm.type = OperandType::Reg16;
        } else if (is_memory_operand(arpl_rm.type)) {
            arpl_rm.type = OperandType::Mem16;
        }
        out.opcode = Opcode::ARPL_RM16_R16;
        out.add_operand(arpl_rm);
        out.add_operand(Operand::make_reg(OperandType::Reg16, arpl_reg));
        return DecodeError::Success;
    }

    // Decode modrm with 32-bit operand size (source is always 32-bit)
    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Force source to 32-bit
    if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
    else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;

    out.opcode = Opcode::MOVSXD_R64_RM32;
    // Destination is 64-bit if REX.W, otherwise 32-bit (acts as MOV)
    if (is_64bit_operand) {
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
    } else {
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
    }
    out.add_operand(rm);

    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
