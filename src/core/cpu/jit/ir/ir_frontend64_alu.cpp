// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_alu.cpp
/// @brief Long-mode integer ALU families: narrow INC/DEC/NEG/NOT, ADC/SBB,
///        CMPXCHG, XCHG/XADD and 8/16-bit ADD..TEST
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_narrow_unary(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Narrow unary: INC/DEC preserve CF and so publish a three-operand
    // merge record (Intel SDM Vol. 2A); NEG publishes a full record; NOT
    // publishes none at all.
    struct NarrowUnary { Op op; FlagOp record; Type width; bool carry_merge; bool flags; };
    NarrowUnary unary{Op::Add, FlagOp::None, Type::Void, false, false};
    switch (insn.opcode) {
    case Opcode::INC_RM8:
        unary = {Op::Add, FlagOp::Inc8, Type::I8, true, true}; break;
    case Opcode::INC_RM16:
        unary = {Op::Add, FlagOp::Inc16, Type::I16, true, true}; break;
    case Opcode::DEC_RM8:
        unary = {Op::Sub, FlagOp::Dec8, Type::I8, true, true}; break;
    case Opcode::DEC_RM16:
        unary = {Op::Sub, FlagOp::Dec16, Type::I16, true, true}; break;
    case Opcode::NEG_RM8:
        unary = {Op::Neg, FlagOp::Neg8, Type::I8, false, true}; break;
    case Opcode::NEG_RM16:
        unary = {Op::Neg, FlagOp::Neg16, Type::I16, false, true}; break;
    case Opcode::NOT_RM8:
        unary = {Op::Not, FlagOp::None, Type::I8, false, false}; break;
    case Opcode::NOT_RM16:
        unary = {Op::Not, FlagOp::None, Type::I16, false, false}; break;
    // The full widths take the same recipe. Intel SDM Vol. 2A INC/DEC
    // preserve CF; NEG sets CF = (source != 0); NOT changes no flag. A 32-bit
    // destination zero-extends (Vol. 1 3.4.1.1), which the I32 GprWrite does.
    case Opcode::INC_RM32:
        unary = {Op::Add, FlagOp::Inc32, Type::I32, true, true}; break;
    case Opcode::INC_RM64:
        unary = {Op::Add, FlagOp::Inc64, Type::I64, true, true}; break;
    case Opcode::DEC_RM32:
        unary = {Op::Sub, FlagOp::Dec32, Type::I32, true, true}; break;
    case Opcode::DEC_RM64:
        unary = {Op::Sub, FlagOp::Dec64, Type::I64, true, true}; break;
    case Opcode::NEG_RM32:
        unary = {Op::Neg, FlagOp::Neg32, Type::I32, false, true}; break;
    case Opcode::NEG_RM64:
        unary = {Op::Neg, FlagOp::Neg64, Type::I64, false, true}; break;
    case Opcode::NOT_RM32:
        unary = {Op::Not, FlagOp::None, Type::I32, false, false}; break;
    case Opcode::NOT_RM64:
        unary = {Op::Not, FlagOp::None, Type::I64, false, false}; break;
    default: break;
    }
    if (unary.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const Type w = unary.width;
        const OperandType reg_type = w == Type::I8  ? OperandType::Reg8
                                   : w == Type::I16 ? OperandType::Reg16
                                   : w == Type::I32 ? OperandType::Reg32
                                                    : OperandType::Reg64;
        const OperandType mem_type = w == Type::I8  ? OperandType::Mem8
                                   : w == Type::I16 ? OperandType::Mem16
                                   : w == Type::I32 ? OperandType::Mem32
                                                    : OperandType::Mem64;
        const bool to_memory = dst.type == mem_type;
        if ((dst.type == reg_type && dst.reg < 16) || to_memory) {
            ValueId address = kInvalidValue;
            ValueId original = kInvalidValue;
            GuestReg reg{0, w};
            if (to_memory) {
                address = build_long_mode_ea(builder, dst.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    original = builder.load(w, address, kMemFaultState);
            } else {
                reg = GuestReg{narrow_reg_index(insn, dst), w};
                original = builder.gpr_read(reg);
            }
            if (original != kInvalidValue) {
                // The flag value whose CF an INC/DEC inherits. With nothing
                // defined yet in this block that value IS the block's
                // incoming flags, which is what `FlagsIn` names.
                const ValueId inherited = unary.carry_merge
                    ? require_flags(builder, live_flags) : kInvalidValue;
                ValueId result;
                if (unary.op == Op::Not || unary.op == Op::Neg) {
                    result = builder.unary(unary.op, w, original);
                } else {
                    // INC/DEC are +1/-1 with CF untouched.
                    result = builder.binary(unary.op, w, original,
                                            builder.constant(w, 1));
                }
                const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                  unary.record, false, false};
                // `Inc8`/`Dec16` and friends take the ORIGINAL value in their
                // source word, not the constant 1.
                if (to_memory) {
                    // A faulting write-back never retires, so the store
                    // precedes the definition exactly as in the ALU forms.
                    builder.store(w, address, result, kMemFaultState);
                    if (unary.flags)
                        live_flags = builder.def_flags(def, original, result, inherited);
                } else {
                    if (unary.flags)
                        live_flags = builder.def_flags(def, original, result, inherited);
                    builder.gpr_write(reg, result);
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_carry_alu(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Narrow ALU. Same shapes as the full-width forms, at I8/I16, with the
    // matching narrow lazy-flag record. The materializer masks both payloads
    // to the record's width, so the lowering may leave the bits above the
    // field undefined — the merge and the store take only `width` bits too.
     // ADC / SBB. These READ the carry and then define every flag from the
    // three-way sum, so the record has to carry the carry-in as well as the
    // source: `flag_src = (cf_in << 32) | src`, packed, which is why both
    // DefFlags sources are I64 here.
    //
    // 64-bit ADC/SBB is absent and cannot be added in this format: a 64-bit
    // source already fills the payload word, leaving nowhere for the carry-in.
    // `Adc64`/`Sbb64` do not exist for exactly that reason.
    struct CarryAlu { bool subtract; FlagOp record; Type width; };
    CarryAlu carry_alu{false, FlagOp::None, Type::Void};
    switch (insn.opcode) {
    case Opcode::ADC_RM8_R8: case Opcode::ADC_R8_RM8:
    case Opcode::ADC_RM8_IMM8: case Opcode::ADC_AL_IMM8:
        carry_alu = {false, FlagOp::Adc8, Type::I8}; break;
    case Opcode::ADC_RM16_R16: case Opcode::ADC_R16_RM16:
    case Opcode::ADC_RM16_IMM16: case Opcode::ADC_RM16_IMM8:
    case Opcode::ADC_AX_IMM16:
        carry_alu = {false, FlagOp::Adc16, Type::I16}; break;
    case Opcode::ADC_RM32_R32: case Opcode::ADC_R32_RM32:
    case Opcode::ADC_RM32_IMM32: case Opcode::ADC_RM32_IMM8:
    case Opcode::ADC_EAX_IMM32:
        carry_alu = {false, FlagOp::Adc32, Type::I32}; break;
    case Opcode::SBB_RM8_R8: case Opcode::SBB_R8_RM8:
    case Opcode::SBB_RM8_IMM8: case Opcode::SBB_AL_IMM8:
        carry_alu = {true, FlagOp::Sbb8, Type::I8}; break;
    case Opcode::SBB_RM16_R16: case Opcode::SBB_R16_RM16:
    case Opcode::SBB_RM16_IMM16: case Opcode::SBB_RM16_IMM8:
    case Opcode::SBB_AX_IMM16:
        carry_alu = {true, FlagOp::Sbb16, Type::I16}; break;
    case Opcode::SBB_RM32_R32: case Opcode::SBB_R32_RM32:
    case Opcode::SBB_RM32_IMM32: case Opcode::SBB_RM32_IMM8:
    case Opcode::SBB_EAX_IMM32:
        carry_alu = {true, FlagOp::Sbb32, Type::I32}; break;
    default: break;
    }
    if (carry_alu.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count == 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const Type w = carry_alu.width;
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                   : w == Type::I16 ? OperandType::Reg16
                                                    : OperandType::Reg32;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                   : w == Type::I16 ? OperandType::Mem16
                                                    : OperandType::Mem32;
        const auto operand_value = [&](const X86Operand& op) -> ValueId {
            if (op.type == reg_type && op.reg < 16)
                return builder.gpr_read(GuestReg{narrow_reg_index(insn, op), w});
            if (is_immediate_operand(op.type)) {
                // Intel SDM Vol. 2: 83 /n sign-extends its imm8 to the width.
                u64 bits = static_cast<u64>(op.imm);
                if (op.type == OperandType::Imm8 && w != Type::I8)
                    bits = static_cast<u64>(static_cast<i64>(
                        static_cast<i8>(op.imm)));
                return builder.constant(w, w == Type::I8
                    ? static_cast<u64>(static_cast<u8>(bits))
                    : w == Type::I16 ? static_cast<u64>(static_cast<u16>(bits))
                                     : static_cast<u64>(static_cast<u32>(bits)));
            }
            return kInvalidValue;
        };
        const bool to_memory = dst.type == mem_type;
        if ((dst.type == reg_type && dst.reg < 16) || to_memory) {
            ValueId address = kInvalidValue;
            ValueId original = kInvalidValue;
            GuestReg reg{0, w};
            if (to_memory) {
                address = build_long_mode_ea(builder, dst.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    original = builder.load(w, address, kMemFaultState);
            } else {
                reg = GuestReg{narrow_reg_index(insn, dst), w};
                original = builder.gpr_read(reg);
            }
            ValueId source = kInvalidValue;
            if (original != kInvalidValue) {
                source = operand_value(src);
                if (source == kInvalidValue && src.type == mem_type) {
                    const ValueId a = build_long_mode_ea(builder, src.mem,
                                                         next_rip, px.address_size);
                    if (a != kInvalidValue)
                        source = builder.load(w, a, kMemFaultState);
                }
            }
            if (source != kInvalidValue) {
                // CF as an integer, before anything redefines the flags, and
                // immediately before its consumer: nothing between them may
                // disturb host C when the backend takes the carry from there.
                const ValueId cf =
                    builder.eval_cc(require_flags(builder, live_flags), 0x2);
                const ValueId result = builder.carry_binary(
                    carry_alu.subtract ? Op::SubBorrow : Op::AddCarry, w,
                    original, source, cf);
                // flag_src = (cf_in << 32) | src, as a single 64-bit value.
                const ValueId packed = builder.binary(Op::Or, Type::I64,
                    builder.shift_imm(Op::Shl, Type::I64,
                                      builder.zext(Type::I64, cf), 32),
                    builder.zext(Type::I64, source));
                const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                  carry_alu.record, false, false};
                if (to_memory) {
                    builder.store(w, address, result, kMemFaultState);
                    live_flags = builder.def_flags(def, packed,
                                                   builder.zext(Type::I64, result));
                } else {
                    live_flags = builder.def_flags(def, packed,
                                                   builder.zext(Type::I64, result));
                    builder.gpr_write(reg, result);
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_cmpxchg(
    IrBuilder& builder, const DecodedInstruction& insn, ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // CMPXCHG at the narrow widths, register destination.
    //
    // Its conditional write is not control flow: both arms are values, so
    // `Select` picks between them. The flags are an ordinary SUB record --
    // `accumulator - DEST`, measured on silicon (see
    // tools/oracle_probe/cmpxchg_flag_probe.c) -- and that record's ZF is the
    // equality the instruction is specified to report, so nothing overrides it.
    //
    // Register destination only: Intel SDM Vol. 2 CMPXCHG writes DEST on BOTH
    // paths, which for memory means a store on the not-equal path too, and the
    // reference additionally probes write permission before doing anything.
    // That is a memory contract, not an arithmetic one.
    Type cmpx = Type::Void;
    switch (insn.opcode) {
    case Opcode::CMPXCHG_RM8_R8:   cmpx = Type::I8;  break;
    case Opcode::CMPXCHG_RM16_R16: cmpx = Type::I16; break;
    default: break;
    }
    if (cmpx != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const Type w = cmpx;
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                                   : OperandType::Reg16;
        if (dst.type == reg_type && dst.reg < 16 &&
            src.type == reg_type && src.reg < 16) {
            const GuestReg dest_reg{narrow_reg_index(insn, dst), w};
            const GuestReg src_reg{narrow_reg_index(insn, src), w};
            const GuestReg acc_reg{0, w};
            const ValueId temp = builder.gpr_read(dest_reg);
            const ValueId acc = builder.gpr_read(acc_reg);
            const ValueId source = builder.gpr_read(src_reg);
            const ValueId result = builder.binary(Op::Sub, w, acc, temp);
            const FlagState::FlagOp record = w == Type::I8
                ? FlagState::FlagOp::Sub8 : FlagState::FlagOp::Sub16;
            const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                              record, 0, false, false};
            live_flags = builder.def_flags(def, temp, result);
            // `Select` is not lowerable below 16 bits, and the difference is
            // the equality test either way, so both arms widen to I64.
            const ValueId differs = builder.zext(Type::I64, result);
            const auto pick = [&](ValueId unequal, ValueId equal) {
                return builder.trunc(w, builder.select(Type::I64, differs,
                                                       builder.zext(Type::I64, unequal),
                                                       builder.zext(Type::I64, equal)));
            };
            // The accumulator is written FIRST and the destination LAST, which
            // is the reference's order and is observable: `cmpxchg al, cl`
            // names the accumulator as its destination, and on the equal path
            // the answer is the SOURCE, not the accumulator's old value.
            builder.gpr_write(acc_reg, pick(temp, acc));
            builder.gpr_write(dest_reg, pick(temp, source));
            return {true, DeclineReason::None};
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_xchg_xadd(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // XCHG and XADD at the narrow widths.
    //
    // Neither is unconditionally atomic. XCHG is, but ONLY with a memory
    // operand -- Intel SDM Vol. 2 XCHG: "the processor's locking protocol is
    // automatically implemented ... regardless of the presence or absence of
    // the LOCK prefix" -- so the register forms are an ordinary swap and the
    // memory form is left to the tier that owns the lock protocol. XADD locks
    // only when the prefix says so, and a LOCK-prefixed instruction already
    // declines above, so what reaches here is a plain read-modify-write.
    struct Swap { bool add; Type width; };
    Swap swap{false, Type::Void};
    switch (insn.opcode) {
    case Opcode::XCHG_AX_R16:    swap = {false, Type::I16}; break;
    case Opcode::XCHG_RM8_R8:    swap = {false, Type::I8};  break;
    case Opcode::XCHG_RM16_R16:  swap = {false, Type::I16}; break;
    case Opcode::XADD_RM8_R8:    swap = {true,  Type::I8};  break;
    case Opcode::XADD_RM16_R16:  swap = {true,  Type::I16}; break;
    default: break;
    }
    if (swap.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const Type w = swap.width;
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                                   : OperandType::Reg16;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                                   : OperandType::Mem16;
        // Register destinations only, for both. XCHG's memory form is
        // implicitly locked. XADD's is not, but the reference publishes its
        // FLAGS and writes its register operand BEFORE storing, so a faulting
        // store leaves both behind -- which contradicts the fault invariant
        // every other memory form here is built on (a faulting write commits
        // nothing). Reproducing one or the other needs an oracle, so the
        // memory form waits for it.
        (void)mem_type;
        constexpr bool to_memory = false;
        if (src.type == reg_type && src.reg < 16 &&
            ((dst.type == reg_type && dst.reg < 16) || to_memory)) {
            const GuestReg source_reg{narrow_reg_index(insn, src), w};
            ValueId address = kInvalidValue;
            ValueId original = kInvalidValue;
            GuestReg reg{0, w};
            if (to_memory) {
                address = build_long_mode_ea(builder, dst.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    original = builder.load(w, address, kMemFaultState);
            } else {
                reg = GuestReg{narrow_reg_index(insn, dst), w};
                original = builder.gpr_read(reg);
            }
            if (original != kInvalidValue) {
                const ValueId source = builder.gpr_read(source_reg);
                if (!swap.add) {
                    // Both reads happen before either write, so a register
                    // exchanged with itself is still a no-op.
                    builder.gpr_write(reg, source);
                    builder.gpr_write(source_reg, original);
                    return {true, DeclineReason::None};
                }
                const ValueId sum = builder.binary(Op::Add, w, original, source);
                const FlagState::FlagOp record = w == Type::I8
                    ? FlagState::FlagOp::Add8 : FlagState::FlagOp::Add16;
                const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                  record, 0, false, false};
                // The register operand receives the destination's OLD value
                // and is written FIRST, exactly as the reference orders it.
                // The order is observable: `xadd cl, cl` names one register
                // twice, and whichever write lands last is what survives.
                builder.gpr_write(source_reg, original);
                builder.gpr_write(reg, sum);
                live_flags = builder.def_flags(def, source, sum);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_narrow_alu(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
   struct NarrowAlu { Op op; FlagOp record; Type width; bool compare; };
    NarrowAlu narrow_alu{Op::Add, FlagOp::None, Type::Void, false};
    const auto set_narrow = [&](Op op, FlagOp r8, FlagOp r16, Type w, bool cmp) {
        narrow_alu = {op, w == Type::I8 ? r8 : r16, w, cmp};
    };
    switch (insn.opcode) {
    case Opcode::ADD_RM8_R8: case Opcode::ADD_R8_RM8: case Opcode::ADD_RM8_IMM8:
    case Opcode::ADD_AL_IMM8:
        set_narrow(Op::Add, FlagOp::Add8, FlagOp::Add16, Type::I8, false); break;
    case Opcode::ADD_RM16_R16: case Opcode::ADD_R16_RM16:
    case Opcode::ADD_RM16_IMM16: case Opcode::ADD_RM16_IMM8: case Opcode::ADD_AX_IMM16:
        set_narrow(Op::Add, FlagOp::Add8, FlagOp::Add16, Type::I16, false); break;
    case Opcode::SUB_RM8_R8: case Opcode::SUB_R8_RM8: case Opcode::SUB_RM8_IMM8:
    case Opcode::SUB_AL_IMM8:
        set_narrow(Op::Sub, FlagOp::Sub8, FlagOp::Sub16, Type::I8, false); break;
    case Opcode::SUB_RM16_R16: case Opcode::SUB_R16_RM16:
    case Opcode::SUB_RM16_IMM16: case Opcode::SUB_RM16_IMM8: case Opcode::SUB_AX_IMM16:
        set_narrow(Op::Sub, FlagOp::Sub8, FlagOp::Sub16, Type::I16, false); break;
    case Opcode::AND_RM8_R8: case Opcode::AND_R8_RM8: case Opcode::AND_RM8_IMM8:
    case Opcode::AND_AL_IMM8:
        set_narrow(Op::And, FlagOp::And8, FlagOp::And16, Type::I8, false); break;
    case Opcode::AND_RM16_R16: case Opcode::AND_R16_RM16:
    case Opcode::AND_RM16_IMM16: case Opcode::AND_RM16_IMM8: case Opcode::AND_AX_IMM16:
        set_narrow(Op::And, FlagOp::And8, FlagOp::And16, Type::I16, false); break;
    case Opcode::OR_RM8_R8: case Opcode::OR_R8_RM8: case Opcode::OR_RM8_IMM8:
    case Opcode::OR_AL_IMM8:
        set_narrow(Op::Or, FlagOp::Or8, FlagOp::Or16, Type::I8, false); break;
    case Opcode::OR_RM16_R16: case Opcode::OR_R16_RM16:
    case Opcode::OR_RM16_IMM16: case Opcode::OR_RM16_IMM8: case Opcode::OR_AX_IMM16:
        set_narrow(Op::Or, FlagOp::Or8, FlagOp::Or16, Type::I16, false); break;
    case Opcode::XOR_RM8_R8: case Opcode::XOR_R8_RM8: case Opcode::XOR_RM8_IMM8:
    case Opcode::XOR_AL_IMM8:
        set_narrow(Op::Xor, FlagOp::Xor8, FlagOp::Xor16, Type::I8, false); break;
    case Opcode::XOR_RM16_R16: case Opcode::XOR_R16_RM16:
    case Opcode::XOR_RM16_IMM16: case Opcode::XOR_RM16_IMM8: case Opcode::XOR_AX_IMM16:
        set_narrow(Op::Xor, FlagOp::Xor8, FlagOp::Xor16, Type::I16, false); break;
    case Opcode::CMP_RM8_R8: case Opcode::CMP_R8_RM8: case Opcode::CMP_RM8_IMM8:
    case Opcode::CMP_AL_IMM8:
        set_narrow(Op::Sub, FlagOp::Sub8, FlagOp::Sub16, Type::I8, true); break;
    case Opcode::CMP_RM16_R16: case Opcode::CMP_R16_RM16:
    case Opcode::CMP_RM16_IMM16: case Opcode::CMP_RM16_IMM8: case Opcode::CMP_AX_IMM16:
        set_narrow(Op::Sub, FlagOp::Sub8, FlagOp::Sub16, Type::I16, true); break;
    case Opcode::TEST_RM8_R8: case Opcode::TEST_RM8_IMM8: case Opcode::TEST_AL_IMM8:
        set_narrow(Op::And, FlagOp::And8, FlagOp::And16, Type::I8, true); break;
    case Opcode::TEST_RM16_R16: case Opcode::TEST_RM16_IMM16: case Opcode::TEST_AX_IMM16:
        set_narrow(Op::And, FlagOp::And8, FlagOp::And16, Type::I16, true); break;
    default: break;
    }
    if (narrow_alu.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count == 2) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const Type w = narrow_alu.width;
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                                   : OperandType::Reg16;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                                   : OperandType::Mem16;
        const auto narrow_source = [&](const X86Operand& op) -> ValueId {
            if (op.type == reg_type && op.reg < 16)
                return builder.gpr_read(GuestReg{narrow_reg_index(insn, op), w});
            if (is_immediate_operand(op.type)) {
                // Intel SDM Vol. 2: 83 /n sign-extends its imm8 to the
                // operand width; the same-width immediates do not.
                const u64 bits = op.type == OperandType::Imm8 && w == Type::I16
                    ? static_cast<u64>(static_cast<u16>(static_cast<i16>(
                          static_cast<i8>(op.imm))))
                    : static_cast<u64>(op.imm);
                return builder.constant(w, w == Type::I8
                    ? static_cast<u64>(static_cast<u8>(bits))
                    : static_cast<u64>(static_cast<u16>(bits)));
            }
            return kInvalidValue;
        };
        const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                          narrow_alu.record, false, false};
        if (dst.type == reg_type && dst.reg < 16) {
            ValueId source = narrow_source(src);
            if (source == kInvalidValue && src.type == mem_type) {
                const ValueId address = build_long_mode_ea(builder, src.mem,
                                                           next_rip, px.address_size);
                if (address != kInvalidValue)
                    source = builder.load(w, address, kMemFaultState);
            }
            if (source != kInvalidValue) {
                const GuestReg reg{narrow_reg_index(insn, dst), w};
                const ValueId original = builder.gpr_read(reg);
                const ValueId result = builder.binary(narrow_alu.op, w, original, source);
                live_flags = builder.def_flags(def, source, result);
                if (!narrow_alu.compare) builder.gpr_write(reg, result);
                return {true, DeclineReason::None};
            }
            return verdict;
        }
        if (dst.type == mem_type) {
            const ValueId source = narrow_source(src);
            if (source == kInvalidValue) return verdict;
            const ValueId address = build_long_mode_ea(builder, dst.mem, next_rip,
                                                       px.address_size);
            if (address == kInvalidValue) return verdict;
            const ValueId loaded = builder.load(w, address, kMemFaultState);
            const ValueId result = builder.binary(narrow_alu.op, w, loaded, source);
            // The store precedes the flag definition: an instruction that
            // faults writing back never retires, so the architectural flags
            // must still be the previous definition's.
            if (!narrow_alu.compare)
                builder.store(w, address, result, kMemFaultState);
            live_flags = builder.def_flags(def, source, result);
            return {true, DeclineReason::None};
        }
        return verdict;
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir
