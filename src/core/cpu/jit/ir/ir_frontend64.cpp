// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64.cpp
/// @brief The long-mode frontend entry point: `build_long_mode_insn`, which
///        tries each instruction family in order, then the full-width
///        MOV/ALU/Jcc/JMP forms; and `build_long_mode_ea`
///
/// Split out of ir_frontend.cpp by pure code motion. The families live in
/// the ir_frontend64_*.cpp files.

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

/// Build an effective address as an IR value, or return `kInvalidValue` if the
/// addressing form is not expressible.
///
/// Shared by `LEA` and by every memory access, which is the whole point of
/// plan §C: the address is a value produced by a node the EA-commoning pass
/// can CSE, not a `MemoryOperand` re-decoded at each use site.
// Intel SDM Vol. 1, 3.3.7 and 3.7.5: address-size arithmetic wraps before
// zero extension; only FS/GS contribute a segment base in 64-bit mode.
[[nodiscard]] ValueId build_long_mode_ea(IrBuilder& b,
                                            const MemoryOperand& mem,
                                            u64 next_rip, bool address32) {
    if (mem.vsib || (mem.has_base() && mem.base >= 16) ||
        (mem.has_index() && (mem.index >= 16 ||
         (mem.scale != 1 && mem.scale != 2 && mem.scale != 4 && mem.scale != 8))))
        return kInvalidValue;
    const Type width = address32 ? Type::I32 : Type::I64;
    const auto constant = [&](u64 value) {
        return b.constant(width, address32 ? static_cast<u32>(value) : value);
    };
    ValueId address;
    if (mem.has_absolute) {
        address = constant(mem.absolute);
    } else if (mem.is_rip_relative()) {
        address = constant(next_rip + static_cast<u64>(static_cast<i64>(mem.disp)));
    } else if (!mem.has_base() && !mem.has_index()) {
        address = constant(static_cast<u64>(static_cast<i64>(mem.disp)));
    } else {
        const ValueId base = mem.has_base()
            ? b.gpr_read(GuestReg{mem.base, width}) : constant(0);
        const ValueId index = mem.has_index()
            ? b.gpr_read(GuestReg{mem.index, width}) : kInvalidValue;
        address = !mem.has_index() && mem.disp == 0 ? base
            : b.lea(width, base, index, mem.has_index() ? mem.scale : 1, mem.disp);
    }
    if (address32) address = b.zext(Type::I64, address);
    if (mem.segment == SegReg::FS || mem.segment == SegReg::GS) {
        address = b.binary(Op::Add, Type::I64, address,
            b.seg_base(static_cast<u8>(mem.segment), Type::I64));
    }
    return address;
}

// Long-mode forms share the canonical builder, pass pipeline and verifier.
// Unsupported width/address/fault contracts decline before emission.
[[nodiscard]] InsnVerdict build_long_mode_insn(IrBuilder& builder,
                                              const DecodedInstruction& insn,
                                              u64 next_rip,
                                              ValueId& live_flags) {
    if (const auto v = try_nop(insn)) return *v;
    if (const auto v = try_long_mode_widen(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_acc_sign_extend(builder, insn))
        return *v;
    if (const auto v = try_long_mode_narrow_unary(builder, insn, next_rip,
                                                  live_flags))
        return *v;
    if (const auto v = try_long_mode_carry_alu(builder, insn, next_rip,
                                               live_flags))
        return *v;
    if (const auto v = try_long_mode_cmov(builder, insn, next_rip, live_flags))
        return *v;
    if (const auto v = try_long_mode_setcc(builder, insn, next_rip, live_flags))
        return *v;
    if (const auto v = try_long_mode_rcl_rcr_imm(builder, insn, next_rip,
                                                 live_flags))
        return *v;
    if (const auto v = try_long_mode_sse(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_stack(builder, insn, next_rip, live_flags))
        return *v;
    if (const auto v = try_long_mode_lea(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_cmpxchg(builder, insn, live_flags))
        return *v;
    if (const auto v = try_long_mode_xchg_xadd(builder, insn, next_rip,
                                               live_flags))
        return *v;
    if (const auto v = try_long_mode_divide(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_mul_accumulator(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_imul(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_double_shift(builder, insn, next_rip,
                                                  live_flags))
        return *v;
    if (const auto v = try_long_mode_shift_rotate64(builder, insn, next_rip,
                                                    live_flags))
        return *v;
    if (const auto v = try_long_mode_rotate_cl(builder, insn, next_rip,
                                               live_flags))
        return *v;
    if (const auto v = try_long_mode_rotate_imm(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_shift_cl(builder, insn, next_rip,
                                              live_flags))
        return *v;
    if (const auto v = try_long_mode_shift_imm(builder, insn, next_rip,
                                               live_flags))
        return *v;
    if (const auto v = try_long_mode_narrow_alu(builder, insn, next_rip,
                                                live_flags))
        return *v;
    if (const auto v = try_long_mode_moffs(builder, insn, next_rip))
        return *v;
    if (const auto v = try_long_mode_mov_narrow(builder, insn, next_rip))
        return *v;

    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    const bool mov = insn.opcode == Opcode::MOV_RAX_MOFFS64 ||
        insn.opcode == Opcode::MOV_MOFFS64_RAX ||
        insn.opcode == Opcode::MOV_EAX_MOFFS32 ||
        insn.opcode == Opcode::MOV_MOFFS32_EAX ||
        insn.opcode == Opcode::MOV_R64_IMM64 ||
        insn.opcode == Opcode::MOV_RM64_IMM32 ||
        insn.opcode == Opcode::MOV_RM64_R64 ||
        insn.opcode == Opcode::MOV_R64_RM64 ||
        insn.opcode == Opcode::MOV_R32_IMM32 ||
        insn.opcode == Opcode::MOV_RM32_IMM32 ||
        insn.opcode == Opcode::MOV_RM32_R32 ||
        insn.opcode == Opcode::MOV_R32_RM32;
    Recipe scalar = classify(insn.opcode);
    switch (insn.opcode) {
    case Opcode::ADD_RAX_IMM32:
    case Opcode::ADD_RM64_IMM32: case Opcode::ADD_RM64_IMM8:
    case Opcode::ADD_RM64_R64: case Opcode::ADD_R64_RM64:
        scalar = {Form::AluRmw, Op::Add, FlagOp::Add64, Type::I64};
        break;
    case Opcode::SUB_RAX_IMM32:
    case Opcode::SUB_RM64_IMM32: case Opcode::SUB_RM64_IMM8:
    case Opcode::SUB_RM64_R64: case Opcode::SUB_R64_RM64:
        scalar = {Form::AluRmw, Op::Sub, FlagOp::Sub64, Type::I64};
        break;
    case Opcode::AND_RAX_IMM32:
    case Opcode::AND_RM64_IMM32: case Opcode::AND_RM64_IMM8:
    case Opcode::AND_RM64_R64: case Opcode::AND_R64_RM64:
        scalar = {Form::AluRmw, Op::And, FlagOp::And64, Type::I64};
        break;
    case Opcode::OR_RAX_IMM32:
    case Opcode::OR_RM64_IMM32: case Opcode::OR_RM64_IMM8:
    case Opcode::OR_RM64_R64: case Opcode::OR_R64_RM64:
        scalar = {Form::AluRmw, Op::Or, FlagOp::Or64, Type::I64};
        break;
    case Opcode::XOR_RAX_IMM32:
    case Opcode::XOR_RM64_IMM32: case Opcode::XOR_RM64_IMM8:
    case Opcode::XOR_RM64_R64: case Opcode::XOR_R64_RM64:
        scalar = {Form::AluRmw, Op::Xor, FlagOp::Xor64, Type::I64};
        break;
    case Opcode::CMP_RAX_IMM32:
    case Opcode::CMP_RM64_IMM32: case Opcode::CMP_RM64_IMM8:
    case Opcode::CMP_RM64_R64: case Opcode::CMP_R64_RM64:
        scalar = {Form::AluCompare, Op::Sub, FlagOp::Sub64, Type::I64};
        break;
    case Opcode::TEST_RAX_IMM32: case Opcode::TEST_RM64_IMM32:
    case Opcode::TEST_RM64_R64:
        scalar = {Form::AluCompare, Op::And, FlagOp::And64, Type::I64};
        break;
    default: break;
    }

    const bool alu = scalar.form == Form::AluRmw || scalar.form == Form::AluCompare;
    if (mov || alu || scalar.form == Form::Jcc || scalar.form == Form::Jmp) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        // Name the prefix when one is why a recognized form cannot be built.
        // LOCK in particular is an atomic read-modify-write with ordering this
        // IR does not model, and a silently dropped LOCK is a correctness bug
        // no register-only test would see.
        if (px.lock || px.rep || px.repne || px.vex || px.evex || px.operand_size)
            verdict.reason = DeclineReason::UnsupportedPrefix;
    }
    if ((scalar.form == Form::Jcc || scalar.form == Form::Jmp) &&
        !px.lock && !px.rep && !px.repne && !px.vex && !px.evex &&
        !px.operand_size && !px.address_size && px.segment == SegReg::Default &&
        insn.operand_count == 1 &&
        (insn.operands[0].type == OperandType::Rel8 ||
         insn.operands[0].type == OperandType::Rel32)) {
        // Intel SDM Vol. 2A Jcc/JMP: sign-extended relative offset,
        // full RIP width. Noncanonical destinations require #GP and
        // stay on the fallback path rather than publishing a bad RIP.
        const i64 displacement = insn.operands[0].type == OperandType::Rel8
            ? static_cast<i8>(insn.operands[0].imm)
            : static_cast<i32>(insn.operands[0].imm);
        const u64 target = next_rip + static_cast<u64>(displacement);
        const auto canonical = [](u64 address) {
            return address >> 47 == 0 || address >> 47 == 0x1FFFFu;
        };
        if (canonical(target) && canonical(next_rip)) {
            if (scalar.form == Form::Jcc)
                builder.jcc(jcc_condition(insn.opcode),
                    require_flags(builder, live_flags), target, next_rip);
            else builder.jmp(target);
            verdict = {true, DeclineReason::None};
        }
    }
    // A memory form carries its segment and address-size overrides into the
    // shared effective-address builder; a register-only ALU form has no
    // address to override, so those prefixes stay declined there.
    const bool has_memory_operand = insn.operand_count == 2 &&
        (insn.operands[0].type == OperandType::Mem32 ||
         insn.operands[0].type == OperandType::Mem64 ||
         insn.operands[1].type == OperandType::Mem32 ||
         insn.operands[1].type == OperandType::Mem64);
    if ((mov || alu) && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && !px.operand_size &&
        (mov || has_memory_operand ||
         (!px.address_size && px.segment == SegReg::Default)) &&
        insn.operand_count == 2) {
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const bool load = mov &&
            ((dst.type == OperandType::Reg64 && src.type == OperandType::Mem64) ||
             (dst.type == OperandType::Reg32 && src.type == OperandType::Mem32));
        const bool store = mov &&
            ((dst.type == OperandType::Mem64 &&
              (src.type == OperandType::Reg64 || is_immediate_operand(src.type))) ||
             (dst.type == OperandType::Mem32 &&
              (src.type == OperandType::Reg32 || is_immediate_operand(src.type))));
        if (load || store) {
            const Type width = (load ? dst.type == OperandType::Reg64
                                     : dst.type == OperandType::Mem64)
                ? Type::I64 : Type::I32;
            const ValueId address = build_long_mode_ea(builder,
                load ? src.mem : dst.mem, next_rip, px.address_size);
            if (address != kInvalidValue) {
                if (load) {
                    const auto value = builder.load(width, address, kMemFaultState);
                    builder.gpr_write(GuestReg{dst.reg, width}, value);
                } else {
                    u64 bits = is_immediate_operand(src.type) ? static_cast<u64>(src.imm) : 0;
                    if (insn.opcode == Opcode::MOV_RM64_IMM32)
                        bits = static_cast<u64>(static_cast<i64>(static_cast<i32>(src.imm)));
                    const auto value = is_immediate_operand(src.type)
                        ? builder.constant(width, width == Type::I32 ? static_cast<u32>(bits) : bits)
                        : builder.gpr_read(GuestReg{src.reg, width});
                    builder.store(width, address, value, kMemFaultState);
                }
                verdict = {true, DeclineReason::None};
            }
        } else if (alu && has_memory_operand) {
            const bool mem_dst = dst.type == OperandType::Mem32 ||
                                 dst.type == OperandType::Mem64;
            const bool wide =
                (mem_dst ? dst.type : src.type) == OperandType::Mem64;
            const Type width = wide ? Type::I64 : Type::I32;
            const OperandType reg = wide ? OperandType::Reg64 : OperandType::Reg32;
            const bool immediate = is_immediate_operand(src.type);
            // One memory operand, matching widths, encodable register index.
            const bool shape = mem_dst
                ? (immediate || (src.type == reg && src.reg < 16))
                : (dst.type == reg && dst.reg < 16 &&
                   src.type == (wide ? OperandType::Mem64 : OperandType::Mem32));
            const ValueId address = shape
                ? build_long_mode_ea(builder, mem_dst ? dst.mem : src.mem,
                                     next_rip, px.address_size)
                : kInvalidValue;
            if (address != kInvalidValue) {
                const ValueId loaded = builder.load(width, address, kMemFaultState);
                // Intel SDM Vol. 2 ADD/SUB/AND/OR/XOR/CMP/TEST: immediate
                // operands sign-extend to the destination width.
                u64 bits = immediate ? static_cast<u64>(src.imm) : 0;
                if (immediate && src.type == OperandType::Imm8)
                    bits = static_cast<u64>(static_cast<i64>(static_cast<i8>(src.imm)));
                else if (immediate && wide && src.type == OperandType::Imm32)
                    bits = static_cast<u64>(static_cast<i64>(static_cast<i32>(src.imm)));
                const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                  scalar.record, false, false};
                if (mem_dst) {
                    const ValueId value = immediate
                        ? builder.constant(width, wide ? bits : static_cast<u32>(bits))
                        : builder.gpr_read(GuestReg{src.reg, width});
                    const ValueId result =
                        builder.binary(scalar.op, width, loaded, value);
                    // The flag definition follows the store on purpose: an
                    // instruction that faults writing back never retires, so
                    // the architectural flags must still be the PREVIOUS
                    // definition's, not this attempted ALU's.
                    if (scalar.form == Form::AluRmw)
                        builder.store(width, address, result, kMemFaultState);
                    builder.def_flags(def, value, result);
                } else {
                    const ValueId original =
                        builder.gpr_read(GuestReg{dst.reg, width});
                    const ValueId result =
                        builder.binary(scalar.op, width, original, loaded);
                    builder.def_flags(def, loaded, result);
                    if (scalar.form == Form::AluRmw)
                        builder.gpr_write(GuestReg{dst.reg, width}, result);
                }
                verdict = {true, DeclineReason::None};
            }
        } else if (!px.address_size && px.segment == SegReg::Default &&
            (dst.type == OperandType::Reg64 ||
             dst.type == OperandType::Reg32) && dst.reg < 16 &&
            ((src.type == dst.type && src.reg < 16) || is_immediate_operand(src.type))) {
            const Type width = dst.type == OperandType::Reg64
                ? Type::I64 : Type::I32;
            // Intel SDM Vol. 2B, MOV: C7 /0 sign-extends imm32.
            // Intel SDM Vol. 2 ADD/SUB/AND/OR/XOR/CMP/TEST:
            // immediate ALU operands sign-extend to destination width.
            u64 bits = is_immediate_operand(src.type) ? static_cast<u64>(src.imm) : 0;
            if (alu && src.type == OperandType::Imm8)
                bits = static_cast<u64>(static_cast<i64>(static_cast<i8>(src.imm)));
            else if ((alu && width == Type::I64 && src.type == OperandType::Imm32) ||
                     insn.opcode == Opcode::MOV_RM64_IMM32)
                bits = static_cast<u64>(static_cast<i64>(static_cast<i32>(src.imm)));
            const ValueId original = alu
                ? builder.gpr_read(GuestReg{dst.reg, width}) : kInvalidValue;
            const ValueId value = is_immediate_operand(src.type)
                ? builder.constant(width, width == Type::I32 ? static_cast<u32>(bits) : bits)
                : builder.gpr_read(GuestReg{src.reg, width});
            if (alu) {
                const ValueId result = builder.binary(scalar.op, width, original, value);
                builder.def_flags(FlagDef{FlagRepr::InCpuStateRecord,
                    NzcvKind::None, scalar.record, false, false}, value, result);
                if (scalar.form == Form::AluRmw)
                    builder.gpr_write(GuestReg{dst.reg, width}, result);
            } else {
                builder.gpr_write(GuestReg{dst.reg, width}, value);
            }
            verdict = {true, DeclineReason::None};
        }
    }
    return verdict;
}

}  // namespace vitreo::cpu::jit::ir
