// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CMOVcc/SETcc decode (CPU-021)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

// ============================================================================
// CMOVcc opcode tables indexed by condition code (0x0-0xF)
// ============================================================================

namespace {

struct CmovOpcodes {
    Opcode r16;
    Opcode r32;
    Opcode r64;
};

constexpr CmovOpcodes cmov_table[16] = {
    { Opcode::CMOVO_R16_RM16,   Opcode::CMOVO_R32_RM32,   Opcode::CMOVO_R64_RM64   }, // 0 O
    { Opcode::CMOVNO_R16_RM16,  Opcode::CMOVNO_R32_RM32,  Opcode::CMOVNO_R64_RM64  }, // 1 NO
    { Opcode::CMOVB_R16_RM16,   Opcode::CMOVB_R32_RM32,   Opcode::CMOVB_R64_RM64   }, // 2 B
    { Opcode::CMOVNB_R16_RM16,  Opcode::CMOVNB_R32_RM32,  Opcode::CMOVNB_R64_RM64  }, // 3 NB
    { Opcode::CMOVZ_R16_RM16,   Opcode::CMOVZ_R32_RM32,   Opcode::CMOVZ_R64_RM64   }, // 4 Z
    { Opcode::CMOVNZ_R16_RM16,  Opcode::CMOVNZ_R32_RM32,  Opcode::CMOVNZ_R64_RM64  }, // 5 NZ
    { Opcode::CMOVBE_R16_RM16,  Opcode::CMOVBE_R32_RM32,  Opcode::CMOVBE_R64_RM64  }, // 6 BE
    { Opcode::CMOVNBE_R16_RM16, Opcode::CMOVNBE_R32_RM32, Opcode::CMOVNBE_R64_RM64 }, // 7 NBE
    { Opcode::CMOVS_R16_RM16,   Opcode::CMOVS_R32_RM32,   Opcode::CMOVS_R64_RM64   }, // 8 S
    { Opcode::CMOVNS_R16_RM16,  Opcode::CMOVNS_R32_RM32,  Opcode::CMOVNS_R64_RM64  }, // 9 NS
    { Opcode::CMOVP_R16_RM16,   Opcode::CMOVP_R32_RM32,   Opcode::CMOVP_R64_RM64   }, // A P
    { Opcode::CMOVNP_R16_RM16,  Opcode::CMOVNP_R32_RM32,  Opcode::CMOVNP_R64_RM64  }, // B NP
    { Opcode::CMOVL_R16_RM16,   Opcode::CMOVL_R32_RM32,   Opcode::CMOVL_R64_RM64   }, // C L
    { Opcode::CMOVNL_R16_RM16,  Opcode::CMOVNL_R32_RM32,  Opcode::CMOVNL_R64_RM64  }, // D NL
    { Opcode::CMOVLE_R16_RM16,  Opcode::CMOVLE_R32_RM32,  Opcode::CMOVLE_R64_RM64  }, // E LE
    { Opcode::CMOVNLE_R16_RM16, Opcode::CMOVNLE_R32_RM32, Opcode::CMOVNLE_R64_RM64 }, // F NLE
};

constexpr Opcode setcc_table[16] = {
    Opcode::SETO_RM8,   // 0 O
    Opcode::SETNO_RM8,  // 1 NO
    Opcode::SETB_RM8,   // 2 B
    Opcode::SETNB_RM8,  // 3 NB
    Opcode::SETZ_RM8,   // 4 Z
    Opcode::SETNZ_RM8,  // 5 NZ
    Opcode::SETBE_RM8,  // 6 BE
    Opcode::SETNBE_RM8, // 7 NBE
    Opcode::SETS_RM8,   // 8 S
    Opcode::SETNS_RM8,  // 9 NS
    Opcode::SETP_RM8,   // A P
    Opcode::SETNP_RM8,  // B NP
    Opcode::SETL_RM8,   // C L
    Opcode::SETNL_RM8,  // D NL
    Opcode::SETLE_RM8,  // E LE
    Opcode::SETNLE_RM8, // F NLE
};

} // anonymous namespace

// ============================================================================
// CMOVcc decode (0F 40-4F)
// ============================================================================

DecodeError decode_cmov(DecodeContext& ctx) noexcept {
    u8 cc = ctx.opcode_byte & 0x0F;

    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, ctx.is_64bit_operand,
                     modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    const auto& entry = cmov_table[cc];

    if (ctx.is_64bit_operand) {
        ctx.out.opcode = entry.r64;
        ctx.out.add_operand(Operand::make_reg(OperandType::Reg64, reg_field));
        // Fix rm type for register operands
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg64;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem64;
    } else if (ctx.op_size == 2) {
        ctx.out.opcode = entry.r16;
        ctx.out.add_operand(Operand::make_reg(OperandType::Reg16, reg_field));
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg16;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem16;
    } else {
        ctx.out.opcode = entry.r32;
        ctx.out.add_operand(Operand::make_reg(OperandType::Reg32, reg_field));
        if (is_register_operand(rm.type)) rm.type = OperandType::Reg32;
        else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem32;
    }

    ctx.out.add_operand(rm);
    return DecodeError::Success;
}

// ============================================================================
// SETcc decode (0F 90-9F)
// ============================================================================

DecodeError decode_setcc(DecodeContext& ctx) noexcept {
    u8 cc = ctx.opcode_byte & 0x0F;

    u8 modrm;
    if (!ctx.reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    // SETcc is always 8-bit, so pass false for is_64bit_operand
    if (decode_modrm(ctx.reader, ctx.mode, ctx.prefix, false,
                     modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Force 8-bit operand size
    if (is_register_operand(rm.type)) rm.type = OperandType::Reg8;
    else if (is_memory_operand(rm.type)) rm.type = OperandType::Mem8;

    ctx.out.opcode = setcc_table[cc];
    ctx.out.add_operand(rm);
    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
