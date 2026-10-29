// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_rotates.cpp
/// @brief Long-mode rotates: RCL/RCR by immediate, ROL/ROR/RCL/RCR by CL
///        and ROL/ROR by immediate, at 8-32 bits
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_rcl_rcr_imm(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // RCL / RCR: rotate THROUGH the carry. The rotated quantity is width+1
    // bits wide -- CF is its top bit -- so the count is masked to 5 bits and
    // then taken modulo 9 for a byte and 17 for a word (Intel SDM Vol. 2).
    // A 32-bit operand rotates 33 bits, which a 5-bit count never exceeds.
    //
    // Like the plain rotates these write only CF and OF, so they publish no
    // record; unlike them they also READ CF, which `EvalCc` supplies. The
    // whole (width+1)-bit rotate is built out of ordinary shifts and ORs at
    // I64, so no new IR operation is needed.
    struct ThroughCarry { bool right; Type width; };
    ThroughCarry rcl{false, Type::Void};
    switch (insn.opcode) {
    case Opcode::RCL_RM8_IMM8:  case Opcode::RCL_RM8_1:  rcl = {false, Type::I8};  break;
    case Opcode::RCL_RM16_IMM8: case Opcode::RCL_RM16_1: rcl = {false, Type::I16}; break;
    case Opcode::RCL_RM32_IMM8: case Opcode::RCL_RM32_1: rcl = {false, Type::I32}; break;
    case Opcode::RCR_RM8_IMM8:  case Opcode::RCR_RM8_1:  rcl = {true,  Type::I8};  break;
    case Opcode::RCR_RM16_IMM8: case Opcode::RCR_RM16_1: rcl = {true,  Type::I16}; break;
    case Opcode::RCR_RM32_IMM8: case Opcode::RCR_RM32_1: rcl = {true,  Type::I32}; break;
    default: break;
    }
    if (rcl.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const Type w = rcl.width;
        const u32 bits = static_cast<u32>(type_bits(w));
        const u32 span = bits + 1;            // CF rides above the operand
        const u8 masked = (insn.operand_count >= 2 &&
                           is_immediate_operand(insn.operands[1].type))
            ? static_cast<u8>(insn.operands[1].imm & 31u) : 1u;
        const u8 amount = static_cast<u8>(masked % span);
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                   : w == Type::I16 ? OperandType::Reg16
                                                    : OperandType::Reg32;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                   : w == Type::I16 ? OperandType::Mem16
                                                    : OperandType::Mem32;
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
                if (amount == 0) {
                    // Nothing rotates and no flag moves, but the destination
                    // is still written -- and a 32-bit write zero-extends.
                    if (to_memory)
                        builder.store(w, address, original, kMemFaultState);
                    else
                        builder.gpr_write(reg, original);
                    return {true, DeclineReason::None};
                }
                // The (width+1)-bit quantity, CF on top.
                const ValueId carry_in =
                    builder.eval_cc(require_flags(builder, live_flags), 0x2);
                const ValueId orig64 = builder.zext(Type::I64, original);
                const ValueId carry64 = builder.zext(Type::I64, carry_in);
                const ValueId joined = builder.binary(Op::Or, Type::I64, orig64,
                    builder.shift_imm(Op::Shl, Type::I64, carry64, bits));
                const u8 left = rcl.right ? static_cast<u8>(span - amount) : amount;
                const u8 right = rcl.right ? amount : static_cast<u8>(span - amount);
                const ValueId rotated = builder.binary(Op::And, Type::I64,
                    builder.binary(Op::Or, Type::I64,
                        builder.shift_imm(Op::Shl, Type::I64, joined, left),
                        builder.shift_imm(Op::Shr, Type::I64, joined, right)),
                    builder.constant(Type::I64,
                                     span == 64 ? ~0ull : ((1ull << span) - 1ull)));
                // The operand is the low bits; the new carry is the top one.
                const ValueId narrow = builder.binary(Op::And, Type::I64, rotated,
                    builder.constant(Type::I64, bits == 64 ? ~0ull
                                                           : ((1ull << bits) - 1ull)));
                const ValueId carry_out =
                    builder.shift_imm(Op::Shr, Type::I64, rotated, static_cast<u8>(bits));
                const ValueId written = builder.trunc(w, narrow);
                if (to_memory)
                    builder.store(w, address, written, kMemFaultState);
                else
                    builder.gpr_write(reg, written);
                builder.set_flag_overlay(Op::SetCf, carry_out, Type::I64);
                if (masked == 1) {
                    // Intel SDM Vol. 2 RCL/RCR: OF is defined only for a count
                    // of 1, and the two directions read DIFFERENT bits. RCL
                    // computes it AFTER the rotate -- MSB of the result XOR the
                    // carry that fell out. RCR computes it BEFORE -- MSB of the
                    // ORIGINAL operand XOR the carry that rotated in. Using the
                    // RCL rule for RCR is wrong whenever the operand's top bit
                    // differs from the bit the rotate replaces it with, which
                    // is why `exec_rcr` captures its OF before mutating.
                    const ValueId of = rcl.right
                        ? builder.binary(Op::Xor, Type::I64,
                              builder.shift_imm(Op::Shr, Type::I64, orig64,
                                                static_cast<u8>(bits - 1)),
                              carry64)
                        : builder.binary(Op::Xor, Type::I64,
                              builder.shift_imm(Op::Shr, Type::I64, narrow,
                                                static_cast<u8>(bits - 1)),
                              carry_out);
                    builder.set_flag_overlay(Op::SetOf, of, Type::I64);
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_rotate_cl(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // ROL/ROR/RCL/RCR by CL. The count is a RUNTIME value, and a masked count
    // of zero writes NO flag at all -- which reads like control flow inside an
    // instruction, and is why these declined while the by-CL shifts did not.
    // They are not control flow: a rotate's flag write is a single BIT, so
    // "skip the write" is `Select(count != 0, computed, current)`, with the
    // current value read back through `EvalCc` before the overlay replaces it.
    // The by-CL SHIFTS cannot use this -- they replace the whole flag state,
    // so their skip has to wrap a record publication, which stays a lowering
    // concern.
    //
    // A rotate of a SPAN-bit field by a runtime amount is two runtime shifts
    // ORed together and masked back to the span. Deliberately NOT the doubling
    // trick the constant-count lowering uses: doubling a through-carry span
    // needs 2*(W+1) bits, which is 66 for a 32-bit operand and silently drops
    // off the top of an I64. This form only ever shifts by at most the span,
    // so every width below 64 is expressible and none of them can overflow.
    struct RotateCl { bool right; bool through_carry; Type width; };
    RotateCl rot_cl{false, false, Type::Void};
    switch (insn.opcode) {
    case Opcode::ROL_RM8_CL:  rot_cl = {false, false, Type::I8};  break;
    case Opcode::ROL_RM16_CL: rot_cl = {false, false, Type::I16}; break;
    case Opcode::ROL_RM32_CL: rot_cl = {false, false, Type::I32}; break;
    case Opcode::ROR_RM8_CL:  rot_cl = {true,  false, Type::I8};  break;
    case Opcode::ROR_RM16_CL: rot_cl = {true,  false, Type::I16}; break;
    case Opcode::ROR_RM32_CL: rot_cl = {true,  false, Type::I32}; break;
    case Opcode::RCL_RM8_CL:  rot_cl = {false, true,  Type::I8};  break;
    case Opcode::RCL_RM16_CL: rot_cl = {false, true,  Type::I16}; break;
    case Opcode::RCL_RM32_CL: rot_cl = {false, true,  Type::I32}; break;
    case Opcode::RCR_RM8_CL:  rot_cl = {true,  true,  Type::I8};  break;
    case Opcode::RCR_RM16_CL: rot_cl = {true,  true,  Type::I16}; break;
    case Opcode::RCR_RM32_CL: rot_cl = {true,  true,  Type::I32}; break;
    default: break;
    }
    if (rot_cl.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const Type w = rot_cl.width;
        const u32 bits = static_cast<u32>(type_bits(w));
        const u32 span = rot_cl.through_carry ? bits + 1 : bits;
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                   : w == Type::I16 ? OperandType::Reg16
                                                    : OperandType::Reg32;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                   : w == Type::I16 ? OperandType::Mem16
                                                    : OperandType::Mem32;
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
                const ValueId flags_in = require_flags(builder, live_flags);
                const ValueId cf_in = builder.zext(Type::I64,
                    builder.eval_cc(flags_in, 0x2));
                const ValueId of_in = builder.zext(Type::I64,
                    builder.eval_cc(flags_in, 0x0));
                const auto imm = [&](u64 v) {
                    return builder.constant(Type::I64, v);
                };
                const auto shl = [&](ValueId v, u8 n) {
                    return builder.shift_imm(Op::Shl, Type::I64, v, n);
                };
                const auto shr = [&](ValueId v, u8 n) {
                    return builder.shift_imm(Op::Shr, Type::I64, v, n);
                };
                const auto andv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::And, Type::I64, a, b);
                };
                const auto orv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::Or, Type::I64, a, b);
                };
                const auto addv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::Add, Type::I64, a, b);
                };
                const auto subv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::Sub, Type::I64, a, b);
                };
                const auto xorv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::Xor, Type::I64, a, b);
                };
                const auto bit = [&](ValueId v, u8 n) {
                    return andv(shr(v, n), imm(1));
                };
                // Intel SDM Vol. 2: the count is masked to five bits at every
                // operand width below 64.
                const ValueId count = builder.zext(Type::I64,
                    builder.binary(Op::And, Type::I8,
                        builder.gpr_read(GuestReg{1, Type::I8}),
                        builder.constant(Type::I8, 31)));
                // The effective rotation. A plain rotate reduces modulo the
                // width, which is a power of two; a through-carry rotate
                // reduces modulo the (W+1)-bit SPAN, which is 9 or 17. There
                // is no runtime divide here: over the five-bit count range
                // `n / 9` is `(n * 29) >> 8` and `n / 17` is `(n * 241) >> 12`,
                // and both multipliers are shift-and-add constants.
                ValueId effective = kInvalidValue;
                if (!rot_cl.through_carry) {
                    effective = andv(count, imm(bits - 1));
                } else if (bits == 32) {
                    // The span is 33 and the count is masked to five bits, so
                    // it can never reach the span: the modulo is the identity.
                    effective = count;
                } else if (bits == 8) {
                    const ValueId scaled =
                        subv(subv(shl(count, 5), shl(count, 1)), count);
                    const ValueId quotient = shr(scaled, 8);
                    effective = subv(count, addv(shl(quotient, 3), quotient));
                } else {   // 16-bit operand, 17-bit span
                    const ValueId scaled =
                        addv(subv(shl(count, 8), shl(count, 4)), count);
                    const ValueId quotient = shr(scaled, 12);
                    effective = subv(count, addv(shl(quotient, 4), quotient));
                }
                // Right-rotating the doubled field by `amount` is the rotate.
                // A left rotate by `e` is a right rotate by `span - e`; at
                // e == 0 that is `span` itself, which lands the upper copy
                // untouched, so a full-cycle count needs no special case.
                const ValueId amount = rot_cl.right
                    ? effective
                    : (rot_cl.through_carry
                           ? subv(imm(span), effective)
                           : andv(subv(imm(span), effective), imm(span - 1)));
                ValueId field = builder.zext(Type::I64, original);
                if (rot_cl.through_carry)
                    field = orv(field, shl(cf_in, static_cast<u8>(bits)));
                // `(field >> amount) | (field << (span - amount))`, masked
                // back to the span. At amount == 0 the left shift is by the
                // whole span, which lands entirely above the mask -- so a
                // zero or full-cycle count needs no special case.
                const ValueId low = builder.shift_reg(Op::Shr, Type::I64, field,
                    builder.trunc(Type::I8, amount));
                const ValueId high = builder.shift_reg(Op::Shl, Type::I64, field,
                    builder.trunc(Type::I8, subv(imm(span), amount)));
                const ValueId rotated =
                    andv(orv(low, high), imm((1ull << span) - 1ull));
                const ValueId narrow =
                    andv(rotated, imm((1ull << bits) - 1ull));
                const ValueId written = builder.trunc(w, narrow);
                if (to_memory)
                    builder.store(w, address, written, kMemFaultState);
                else
                    builder.gpr_write(reg, written);
                // CF: the bit that rotated out. For the through-carry forms
                // that is the span's top bit; for a plain rotate it is the
                // result bit that wrapped.
                const ValueId cf_out = rot_cl.through_carry
                    ? bit(rotated, static_cast<u8>(bits))
                    : (rot_cl.right ? bit(narrow, static_cast<u8>(bits - 1))
                                    : bit(narrow, 0));
                // OF is defined only for a count of one, and each form reads
                // different bits (SDM Vol. 2). RCR alone computes it from the
                // state BEFORE the rotate.
                ValueId of_out = kInvalidValue;
                if (rot_cl.through_carry && rot_cl.right)
                    of_out = xorv(bit(field, static_cast<u8>(bits - 1)), cf_in);
                else if (rot_cl.right)
                    of_out = xorv(bit(narrow, static_cast<u8>(bits - 1)),
                                  bit(narrow, static_cast<u8>(bits - 2)));
                else
                    of_out = xorv(bit(narrow, static_cast<u8>(bits - 1)), cf_out);
                // The zero-count skip, as data rather than control flow.
                builder.set_flag_overlay(Op::SetCf,
                    builder.select(Type::I64, count, cf_out, cf_in), Type::I64);
                builder.set_flag_overlay(Op::SetOf,
                    builder.select(Type::I64, xorv(count, imm(1)),
                                   of_in, of_out), Type::I64);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_rotate_imm(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Rotates by a compile-time count. Intel SDM Vol. 2 ROL/ROR: the count is
    // masked to 5 bits and the rotation is by that MODULO the operand width.
    //
    // A rotate moves only CF and OF. It publishes no record for exactly that
    // reason -- a record replaces the whole flag state, and ZF/SF/PF/AF must
    // survive from whatever the previous instruction left. `SetCf`/`SetOf`
    // force the two bits through the same overlay `CpuState::write_cf_only`
    // uses, which is what the stencil compiler does for these too.
    //
    // RCL/RCR are absent: they rotate THROUGH the carry, so they need CF as an
    // input value, which this IR cannot yet express.
    struct Rotate { Op op; Type width; };
    Rotate rot{Op::Rol, Type::Void};
    switch (insn.opcode) {
    case Opcode::ROL_RM8_IMM8: case Opcode::ROL_RM8_1:
        rot = {Op::Rol, Type::I8}; break;
    case Opcode::ROL_RM16_IMM8: case Opcode::ROL_RM16_1:
        rot = {Op::Rol, Type::I16}; break;
    case Opcode::ROR_RM8_IMM8: case Opcode::ROR_RM8_1:
        rot = {Op::Ror, Type::I8}; break;
    case Opcode::ROR_RM16_IMM8: case Opcode::ROR_RM16_1:
        rot = {Op::Ror, Type::I16}; break;
    case Opcode::ROL_RM32_IMM8: case Opcode::ROL_RM32_1:
        rot = {Op::Rol, Type::I32}; break;
    case Opcode::ROR_RM32_IMM8: case Opcode::ROR_RM32_1:
        rot = {Op::Ror, Type::I32}; break;
    // The by-CL rotates are absent: a rotate writes only CF and OF, so a
    // masked count of zero must skip the OVERLAY writes, and a runtime skip
    // around them is control flow the IR cannot express within an
    // instruction. The by-CL SHIFTS can do it because their skip is around a
    // single record publication, which the lowering owns.
    default: break;
    }
    if (rot.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const Type w = rot.width;
        const u32 bits = static_cast<u32>(type_bits(w));
        const u8 masked = (insn.operand_count >= 2 &&
                           is_immediate_operand(insn.operands[1].type))
            ? static_cast<u8>(insn.operands[1].imm & 31u) : 1u;
        const OperandType reg_type = w == Type::I8 ? OperandType::Reg8
                                   : w == Type::I16 ? OperandType::Reg16
                                                    : OperandType::Reg32;
        const OperandType mem_type = w == Type::I8 ? OperandType::Mem8
                                   : w == Type::I16 ? OperandType::Mem16
                                                    : OperandType::Mem32;
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
                // A masked count of zero moves no FLAGS -- not even CF -- but
                // still WRITES the destination, and a 32-bit write
                // zero-extends to 64.
                const u8 amount = static_cast<u8>(masked % bits);
                const ValueId result = masked == 0 ? original
                    : builder.shift_imm(rot.op, w, original, amount);
                if (to_memory)
                    builder.store(w, address, result, kMemFaultState);
                else
                    builder.gpr_write(reg, result);
                if (masked == 0) return {true, DeclineReason::None};
                // CF is the bit that wrapped: the result's LSB for ROL, its
                // MSB for ROR. The overlay writer isolates bit 0 itself.
                const ValueId carry = rot.op == Op::Rol
                    ? result
                    : builder.shift_imm(Op::Shr, w, result,
                                        static_cast<u8>(bits - 1));
                builder.set_flag_overlay(Op::SetCf, carry, w);
                // OF is defined ONLY for a count of one -- and by the count
                // being one, not by the distance moved, so `rol al, 9` is not
                // a by-1 rotate even though it rotates by one.
                if (masked == 1) {
                    const ValueId msb = builder.shift_imm(
                        Op::Shr, w, result, static_cast<u8>(bits - 1));
                    const ValueId other = rot.op == Op::Rol
                        ? result
                        : builder.shift_imm(Op::Shr, w, result,
                                            static_cast<u8>(bits - 2));
                    builder.set_flag_overlay(
                        Op::SetOf, builder.binary(Op::Xor, w, msb, other), w);
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir
