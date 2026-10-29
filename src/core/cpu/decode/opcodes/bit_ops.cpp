// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Bit manipulation instruction decoding (BSF, BSR, BT, BTS, BTR, BTC, BSWAP) - CPU-025

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_popcnt(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
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

    if (is_64bit_operand) {
        out.opcode = Opcode::POPCNT_R64_RM64;
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
    } else if (op_size == 2) {
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
        out.opcode = Opcode::POPCNT_R16_RM16;
        out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
    } else {
        out.opcode = Opcode::POPCNT_R32_RM32;
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
    }
    out.add_operand(rm);
    return DecodeError::Success;
}

DecodeError decode_bsf_bsr(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    // 0F BC = BSF, 0F BD = BSR
    // F3 0F BC = TZCNT, F3 0F BD = LZCNT (BMI1/ABM)
    bool is_bsf = (byte == 0xBC);
    bool is_count = prefix.rep;  // F3 prefix -> TZCNT/LZCNT

    // Read ModR/M: reg = destination, r/m = source
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (is_count) {
        // TZCNT (BC) / LZCNT (BD). REX.W selects r64 in long mode and
        // takes precedence over the legacy 66 operand-size override.
        if (is_64bit_operand) {
            out.opcode = is_bsf ? Opcode::TZCNT_R64_RM64
                                : Opcode::LZCNT_R64_RM64;
            out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
        } else if (op_size == 2) {
            if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
            else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
            out.opcode = is_bsf ? Opcode::TZCNT_R16_RM16 : Opcode::LZCNT_R16_RM16;
            out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
        } else {
            out.opcode = is_bsf ? Opcode::TZCNT_R32_RM32 : Opcode::LZCNT_R32_RM32;
            out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
        }
        out.add_operand(rm);
        prefix.rep = 0;  // consume F3 prefix
        return DecodeError::Success;
    }

    if (is_64bit_operand) {
        out.opcode = is_bsf ? Opcode::BSF_R64_RM64 : Opcode::BSR_R64_RM64;
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
    } else if (op_size == 2) {
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
        out.opcode = is_bsf ? Opcode::BSF_R16_RM16 : Opcode::BSR_R16_RM16;
        out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
    } else {
        out.opcode = is_bsf ? Opcode::BSF_R32_RM32 : Opcode::BSR_R32_RM32;
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
    }
    out.add_operand(rm);
    return DecodeError::Success;
}

DecodeError decode_bt(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    // 0F A3 = BT, 0F AB = BTS, 0F B3 = BTR, 0F BB = BTC
    // r/m = bit base (operand 0), reg = bit index (operand 1)
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    if (is_64bit_operand) {
        switch (byte) {
            case 0xA3: out.opcode = Opcode::BT_RM64_R64;  break;
            case 0xAB: out.opcode = Opcode::BTS_RM64_R64; break;
            case 0xB3: out.opcode = Opcode::BTR_RM64_R64; break;
            case 0xBB: out.opcode = Opcode::BTC_RM64_R64; break;
            default: return DecodeError::InvalidOpcode;
        }
        out.add_operand(rm);
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
    } else if (op_size == 2) {
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
        switch (byte) {
            case 0xA3: out.opcode = Opcode::BT_RM16_R16;  break;
            case 0xAB: out.opcode = Opcode::BTS_RM16_R16; break;
            case 0xB3: out.opcode = Opcode::BTR_RM16_R16; break;
            case 0xBB: out.opcode = Opcode::BTC_RM16_R16; break;
            default: return DecodeError::InvalidOpcode;
        }
        out.add_operand(rm);
        out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
    } else {
        switch (byte) {
            case 0xA3: out.opcode = Opcode::BT_RM32_R32;  break;
            case 0xAB: out.opcode = Opcode::BTS_RM32_R32; break;
            case 0xB3: out.opcode = Opcode::BTR_RM32_R32; break;
            case 0xBB: out.opcode = Opcode::BTC_RM32_R32; break;
            default: return DecodeError::InvalidOpcode;
        }
        out.add_operand(rm);
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
    }
    return DecodeError::Success;
}

DecodeError decode_bt_imm(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    bool is_64bit_operand = ctx.is_64bit_operand;
    u8 op_size = ctx.op_size;
    DecodeMode mode = ctx.mode;

    // 0F BA /4=BT, /5=BTS, /6=BTR, /7=BTC with imm8
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, is_64bit_operand, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Read imm8 bit index
    u8 imm;
    if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;

    if (is_64bit_operand) {
        switch (reg_field) {
            case 4: out.opcode = Opcode::BT_RM64_IMM8;  break;
            case 5: out.opcode = Opcode::BTS_RM64_IMM8; break;
            case 6: out.opcode = Opcode::BTR_RM64_IMM8; break;
            case 7: out.opcode = Opcode::BTC_RM64_IMM8; break;
            default: return DecodeError::InvalidOpcode;
        }
    } else if (op_size == 2) {
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
        switch (reg_field) {
            case 4: out.opcode = Opcode::BT_RM16_IMM8;  break;
            case 5: out.opcode = Opcode::BTS_RM16_IMM8; break;
            case 6: out.opcode = Opcode::BTR_RM16_IMM8; break;
            case 7: out.opcode = Opcode::BTC_RM16_IMM8; break;
            default: return DecodeError::InvalidOpcode;
        }
    } else {
        switch (reg_field) {
            case 4: out.opcode = Opcode::BT_RM32_IMM8;  break;
            case 5: out.opcode = Opcode::BTS_RM32_IMM8; break;
            case 6: out.opcode = Opcode::BTR_RM32_IMM8; break;
            case 7: out.opcode = Opcode::BTC_RM32_IMM8; break;
            default: return DecodeError::InvalidOpcode;
        }
    }

    out.add_operand(rm);
    out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
    return DecodeError::Success;
}

DecodeError decode_bswap(DecodeContext& ctx) noexcept {
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    bool is_64bit_operand = ctx.is_64bit_operand;

    // 0F C8+rd: register encoded in low 3 bits
    u8 reg = (byte - 0xC8) | (prefix.rex_b() ? 8 : 0);

    if (is_64bit_operand) {
        out.opcode = Opcode::BSWAP_R64;
        out.add_operand(Operand::make_reg(OperandType::Reg64, reg));
    } else {
        out.opcode = Opcode::BSWAP_R32;
        out.add_operand(Operand::make_reg(OperandType::Reg32, reg));
    }
    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
