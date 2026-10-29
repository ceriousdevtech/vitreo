// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE arithmetic instruction decoding (CPU-032)

#include "decode_ops.hpp"

namespace vitreo::cpu::decode {

DecodeError decode_sse_arith(DecodeContext& ctx) noexcept {
    auto& reader = ctx.reader;
    auto& prefix = ctx.prefix;
    auto& out = ctx.out;
    u8 byte = ctx.opcode_byte;
    DecodeMode mode = ctx.mode;

    // Read ModR/M byte
    u8 modrm;
    if (!reader.read_u8(modrm)) return DecodeError::UnexpectedEnd;

    Operand rm;
    u8 reg_field;
    if (decode_modrm(reader, mode, prefix, false, modrm, rm, reg_field) < 0) {
        return DecodeError::InvalidModRM;
    }

    // Apply REX.R to reg field for XMM8-XMM15
    if (prefix.rex_r()) reg_field |= 8;

    // Determine mandatory prefix: F3 = rep, F2 = repne, 66 = operand_size, NP = none
    bool has_f3 = prefix.rep;
    bool has_f2 = prefix.repne;
    bool has_66 = prefix.operand_size;

    // Helper: set rm operand type based on whether it's register or memory
    // For scalar SS: reg → Reg128, mem → Mem32
    // For scalar SD: reg → Reg128, mem → Mem64
    // For packed:    reg → Reg128, mem → Mem128
    auto set_rm_scalar_ss = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            // Apply REX.B for XMM register in r/m
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem32;
        }
    };

    auto set_rm_scalar_sd = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem64;
        }
    };

    auto set_rm_packed = [&]() {
        if (is_register_operand(rm.type)) {
            rm.type = OperandType::Reg128;
            if (prefix.rex_b()) rm.reg |= 8;
        } else {
            rm.type = OperandType::Mem128;
        }
    };

    switch (byte) {
        // 0x58: ADD
        case 0x58:
            if (has_f3)      { out.opcode = Opcode::ADDSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::ADDSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::ADDPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::ADDPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0x5C: SUB
        case 0x5C:
            if (has_f3)      { out.opcode = Opcode::SUBSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::SUBSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::SUBPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::SUBPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0x59: MUL
        case 0x59:
            if (has_f3)      { out.opcode = Opcode::MULSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::MULSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::MULPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::MULPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0x5E: DIV
        case 0x5E:
            if (has_f3)      { out.opcode = Opcode::DIVSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::DIVSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::DIVPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::DIVPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0x51: SQRT
        case 0x51:
            if (has_f3)      { out.opcode = Opcode::SQRTSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::SQRTSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::SQRTPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::SQRTPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0x5F: MAX
        case 0x5F:
            if (has_f3)      { out.opcode = Opcode::MAXSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::MAXSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::MAXPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::MAXPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0x5D: MIN
        case 0x5D:
            if (has_f3)      { out.opcode = Opcode::MINSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else if (has_f2) { out.opcode = Opcode::MINSD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else if (has_66) { out.opcode = Opcode::MINPD_XMM_XMMM128; set_rm_packed(); }
            else             { out.opcode = Opcode::MINPS_XMM_XMMM128; set_rm_packed(); }
            break;

        // 0xD0: ADDSUBPD (66) / ADDSUBPS (F2)  [SSE3]
        case 0xD0:
            if (has_66)      { out.opcode = Opcode::ADDSUBPD_XMM_XMMM128; set_rm_packed(); }
            else if (has_f2) { out.opcode = Opcode::ADDSUBPS_XMM_XMMM128; set_rm_packed(); }
            else             { return DecodeError::InvalidOpcode; }
            break;

        // 0x7C: HADDPD (66) / HADDPS (F2)  [SSE3]
        case 0x7C:
            if (has_66)      { out.opcode = Opcode::HADDPD_XMM_XMMM128; set_rm_packed(); }
            else if (has_f2) { out.opcode = Opcode::HADDPS_XMM_XMMM128; set_rm_packed(); }
            else             { return DecodeError::InvalidOpcode; }
            break;

        // 0x7D: HSUBPD (66) / HSUBPS (F2)  [SSE3]
        case 0x7D:
            if (has_66)      { out.opcode = Opcode::HSUBPD_XMM_XMMM128; set_rm_packed(); }
            else if (has_f2) { out.opcode = Opcode::HSUBPS_XMM_XMMM128; set_rm_packed(); }
            else             { return DecodeError::InvalidOpcode; }
            break;

        // 0x2F: COMISS / COMISD
        case 0x2F:
            if (has_66) { out.opcode = Opcode::COMISD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else        { out.opcode = Opcode::COMISS_XMM_XMMM32;  set_rm_scalar_ss(); }
            break;

        // 0x2E: UCOMISS / UCOMISD
        case 0x2E:
            if (has_66) { out.opcode = Opcode::UCOMISD_XMM_XMMM64;  set_rm_scalar_sd(); }
            else        { out.opcode = Opcode::UCOMISS_XMM_XMMM32;  set_rm_scalar_ss(); }
            break;

        // 0x53: RCPSS / RCPPS
        case 0x53:
            if (has_f3) { out.opcode = Opcode::RCPSS_XMM_XMMM32;   set_rm_scalar_ss(); }
            else        { out.opcode = Opcode::RCPPS_XMM_XMMM128;  set_rm_packed(); }
            break;

        // 0x52: RSQRTSS / RSQRTPS
        case 0x52:
            if (has_f3) { out.opcode = Opcode::RSQRTSS_XMM_XMMM32;  set_rm_scalar_ss(); }
            else        { out.opcode = Opcode::RSQRTPS_XMM_XMMM128; set_rm_packed(); }
            break;

        default:
            return DecodeError::InvalidOpcode;
    }

    // Operand 0: destination XMM register (from reg field)
    out.add_operand(Operand::make_reg(OperandType::Reg128, reg_field));
    // Operand 1: source XMM/memory (from r/m field)
    out.add_operand(rm);

    return DecodeError::Success;
}

} // namespace vitreo::cpu::decode
