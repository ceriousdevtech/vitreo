// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_shifts.cpp
/// @brief Long-mode shifts: SHLD/SHRD by immediate, 64-bit shifts and
///        rotates by immediate, and narrow/32-bit shifts by CL and by
///        immediate
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_double_shift(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // SHLD / SHRD by a compile-time count: a double-precision shift, where the
    // bits shifted in come from a second register rather than from zero.
    //
    // The three operands are not the obstacle they looked like. The flags are
    // a SHIFT's flags, and at 16 and 32 bits the existing `Shl`/`Shr` record
    // already computes them from exactly the two things it stores: CF is bit
    // `width - count` (SHLD) or `count - 1` (SHRD) of the ORIGINAL DESTINATION,
    // which is what `flag_src` holds, and ZF/SF/PF come from the result. The
    // second source never enters the flag rules at all. Only SHRD's OF differs
    // from the plain shift's -- it is a sign CHANGE, not the original's sign --
    // and that is one overlay write on top of the record.
    //
    // A masked count at or above the operand width is left to the interpreter.
    // Intel SDM Vol. 2 SHLD/SHRD: the result is UNDEFINED there, the reference
    // implementation shifts by a negative amount computing it, and reproducing
    // that is not a contract worth encoding.
    struct Dbl { bool right; Type width; };
    Dbl dbl{false, Type::Void};
    switch (insn.opcode) {
    case Opcode::SHLD_RM16_R16_IMM8: dbl = {false, Type::I16}; break;
    case Opcode::SHLD_RM32_R32_IMM8: dbl = {false, Type::I32}; break;
    case Opcode::SHLD_RM64_R64_IMM8: dbl = {false, Type::I64}; break;
    case Opcode::SHRD_RM16_R16_IMM8: dbl = {true,  Type::I16}; break;
    case Opcode::SHRD_RM32_R32_IMM8: dbl = {true,  Type::I32}; break;
    case Opcode::SHRD_RM64_R64_IMM8: dbl = {true,  Type::I64}; break;
    default: break;
    }
    if (dbl.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 3) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const auto& src = insn.operands[1];
        const Type w = dbl.width;
        const u32 bits = static_cast<u32>(type_bits(w));
        const OperandType reg_type = w == Type::I16 ? OperandType::Reg16
                                   : w == Type::I32 ? OperandType::Reg32
                                                    : OperandType::Reg64;
        const OperandType mem_type = w == Type::I16 ? OperandType::Mem16
                                   : w == Type::I32 ? OperandType::Mem32
                                                    : OperandType::Mem64;
        const u8 amount = is_immediate_operand(insn.operands[2].type)
            ? static_cast<u8>(insn.operands[2].imm & (bits == 64 ? 63u : 31u))
            : 0xFFu;
        const bool to_memory = dst.type == mem_type;
        // A masked count of ZERO is declined rather than admitted as an empty
        // lowering. Intel SDM Vol. 2 SHLD/SHRD make it "no operation" -- no
        // flag and, unlike a zero-count plain shift, no destination write
        // either -- so there is literally nothing to emit, and an instruction
        // that emits nothing contributes no RIP site. A block that is only
        // that instruction then has an empty RIP map, which the linear-fault
        // path refuses to publish, and refuses the native entry with it.
        if (amount < bits && amount != 0 && src.type == reg_type && src.reg < 16 &&
            ((dst.type == reg_type && dst.reg < 16) || to_memory)) {
            ValueId address = kInvalidValue;
            ValueId original = kInvalidValue;
            GuestReg reg{0, w};
            if (to_memory) {
                address = build_long_mode_ea(builder, dst.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    original = builder.load(w, address, kMemFaultState);
            } else {
                reg = GuestReg{static_cast<u8>(dst.reg), w};
                original = builder.gpr_read(reg);
            }
            if (original != kInvalidValue) {
                const ValueId feed =
                    builder.gpr_read(GuestReg{static_cast<u8>(src.reg), w});
                const auto shl = [&](ValueId v, u8 n) {
                    return builder.shift_imm(Op::Shl, Type::I64, v, n);
                };
                const auto shr = [&](ValueId v, u8 n) {
                    return builder.shift_imm(Op::Shr, Type::I64, v, n);
                };
                const ValueId dest64 = w == Type::I64
                    ? original : builder.zext(Type::I64, original);
                const ValueId feed64 = w == Type::I64
                    ? feed : builder.zext(Type::I64, feed);
                const u8 other = static_cast<u8>(bits - amount);
                ValueId wide_result = dbl.right
                    ? builder.binary(Op::Or, Type::I64,
                                     shr(dest64, amount), shl(feed64, other))
                    : builder.binary(Op::Or, Type::I64,
                                     shl(dest64, amount), shr(feed64, other));
                const ValueId result = w == Type::I64
                    ? wide_result : builder.trunc(w, wide_result);
                if (to_memory)
                    builder.store(w, address, result, kMemFaultState);
                const auto bit64 = [&](ValueId v, u8 n) {
                    return builder.binary(Op::And, Type::I64, shr(v, n),
                                          builder.constant(Type::I64, 1));
                };
                // OF is a sign CHANGE of the destination, for both directions.
                const ValueId sign_change = builder.binary(Op::Xor, Type::I64,
                    bit64(dest64, static_cast<u8>(bits - 1)),
                    bit64(w == Type::I64 ? result : builder.zext(Type::I64, result),
                          static_cast<u8>(bits - 1)));
                if (w == Type::I64) {
                    // No 64-bit record holds a count, so CF comes through the
                    // overlay -- except at a count of one, where the `_1`
                    // record's own CF rule already IS this instruction's.
                    const bool one = amount == 1;
                    FlagState::FlagOp record = dbl.right
                        ? (one ? FlagState::FlagOp::Shr64_1 : FlagState::FlagOp::Shr64)
                        : (one ? FlagState::FlagOp::Shl64_1 : FlagState::FlagOp::Shl64);
                    const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                      record, 0, false, false};
                    live_flags = builder.def_flags(def, original, result);
                    if (!one) {
                        const u8 index = dbl.right
                            ? static_cast<u8>(amount - 1)
                            : static_cast<u8>(64 - amount);
                        builder.set_flag_overlay(Op::SetCf, bit64(dest64, index),
                                                 Type::I64);
                    } else {
                        // `Shl64_1`'s OF is already the sign change; `Shr64_1`'s
                        // is the original's sign, which is a plain shift's rule,
                        // not a double shift's.
                        if (dbl.right)
                            builder.set_flag_overlay(Op::SetOf, sign_change,
                                                     Type::I64);
                    }
                } else {
                    const FlagState::FlagOp record = dbl.right
                        ? (w == Type::I16 ? FlagState::FlagOp::Shr16
                                          : FlagState::FlagOp::Shr32)
                        : (w == Type::I16 ? FlagState::FlagOp::Shl16
                                          : FlagState::FlagOp::Shl32);
                    const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                      record, amount, false, false};
                    live_flags = builder.def_flags(def, original, result);
                    if (dbl.right && amount == 1)
                        builder.set_flag_overlay(Op::SetOf, sign_change, Type::I64);
                }
                if (!to_memory)
                    builder.gpr_write(reg, result);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_shift_rotate64(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // 64-bit shifts and rotates by a compile-time count.
    //
    // The shifts are FULL flag writers and need a record, but `Shl64`/`Shr64`/
    // `Sar64` have both of their slots taken by the 64-bit original and result
    // -- there is nowhere to pack the count the narrow records pack, and so no
    // way for the materializer to recover CF. That is not a missing carry: the
    // stencil compiler publishes the record and then writes CF through the
    // OVERLAY, exactly as `emit_shift64_cf_imm` does, and this does the same
    // with `SetCf`. (The comment on `FlagOp::Shl64` in flag_state.hpp claiming
    // CF is cleared describes an older fallback; the materializer leaves CF
    // and OF alone and the overlay supplies CF.)
    //
    // A count of one publishes `Shl64_1`/`Shr64_1`/`Sar64_1` instead, which
    // record original and result and so define OF as well.
    //
    // The rotates are PARTIAL writers and publish no record at all, like their
    // narrow siblings; the through-carry pair rotates a 65-bit span, which no
    // I64 value holds, so they are assembled from the three pieces that span
    // decomposes into rather than from a doubled field.
    enum class Wide64 { None, Shl, Shr, Sar, Rol, Ror, Rcl, Rcr };
    Wide64 wide64 = Wide64::None;
    bool wide64_by_one = false;
    switch (insn.opcode) {
    case Opcode::SHL_RM64_1:   wide64 = Wide64::Shl; wide64_by_one = true; break;
    case Opcode::SHR_RM64_1:   wide64 = Wide64::Shr; wide64_by_one = true; break;
    case Opcode::SAR_RM64_1:   wide64 = Wide64::Sar; wide64_by_one = true; break;
    case Opcode::ROL_RM64_1:   wide64 = Wide64::Rol; wide64_by_one = true; break;
    case Opcode::ROR_RM64_1:   wide64 = Wide64::Ror; wide64_by_one = true; break;
    case Opcode::RCL_RM64_1:   wide64 = Wide64::Rcl; wide64_by_one = true; break;
    case Opcode::RCR_RM64_1:   wide64 = Wide64::Rcr; wide64_by_one = true; break;
    case Opcode::SHL_RM64_IMM8: wide64 = Wide64::Shl; break;
    case Opcode::SHR_RM64_IMM8: wide64 = Wide64::Shr; break;
    case Opcode::SAR_RM64_IMM8: wide64 = Wide64::Sar; break;
    case Opcode::ROL_RM64_IMM8: wide64 = Wide64::Rol; break;
    case Opcode::ROR_RM64_IMM8: wide64 = Wide64::Ror; break;
    case Opcode::RCL_RM64_IMM8: wide64 = Wide64::Rcl; break;
    case Opcode::RCR_RM64_IMM8: wide64 = Wide64::Rcr; break;
    default: break;
    }
    if (wide64 != Wide64::None && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const bool to_memory = dst.type == OperandType::Mem64;
        // Intel SDM Vol. 2: a 64-bit operand masks its count to SIX bits.
        const u8 amount = wide64_by_one ? 1u
            : (insn.operand_count >= 2 && is_immediate_operand(insn.operands[1].type)
                   ? static_cast<u8>(insn.operands[1].imm & 63u) : 0u);
        if ((dst.type == OperandType::Reg64 && dst.reg < 16) || to_memory) {
            ValueId address = kInvalidValue;
            ValueId original = kInvalidValue;
            GuestReg reg{0, Type::I64};
            if (to_memory) {
                address = build_long_mode_ea(builder, dst.mem, next_rip, px.address_size);
                if (address != kInvalidValue)
                    original = builder.load(Type::I64, address, kMemFaultState);
            } else {
                reg = GuestReg{static_cast<u8>(dst.reg), Type::I64};
                original = builder.gpr_read(reg);
            }
            if (original != kInvalidValue) {
                if (amount == 0) {
                    // A masked count of zero modifies no flag and leaves a
                    // 64-bit destination bit for bit as it was -- there is no
                    // zero-extension to perform at this width, so the whole
                    // instruction is a no-op and emits nothing.
                    return {true, DeclineReason::None};
                }
                const auto imm = [&](u64 v) {
                    return builder.constant(Type::I64, v);
                };
                const auto shl = [&](ValueId v, u8 n) {
                    return builder.shift_imm(Op::Shl, Type::I64, v, n);
                };
                const auto shr = [&](ValueId v, u8 n) {
                    return builder.shift_imm(Op::Shr, Type::I64, v, n);
                };
                const auto orv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::Or, Type::I64, a, b);
                };
                const auto xorv = [&](ValueId a, ValueId b) {
                    return builder.binary(Op::Xor, Type::I64, a, b);
                };
                const auto bit = [&](ValueId v, u8 n) {
                    return builder.binary(Op::And, Type::I64, shr(v, n), imm(1));
                };
                const bool through_carry =
                    wide64 == Wide64::Rcl || wide64 == Wide64::Rcr;
                ValueId carry_in = kInvalidValue;
                if (through_carry)
                    carry_in = builder.zext(Type::I64,
                        builder.eval_cc(require_flags(builder, live_flags), 0x2));
                ValueId result = kInvalidValue;
                switch (wide64) {
                case Wide64::Shl:
                    result = shl(original, amount); break;
                case Wide64::Shr:
                    result = shr(original, amount); break;
                case Wide64::Sar:
                    result = builder.shift_imm(Op::Sar, Type::I64, original, amount);
                    break;
                case Wide64::Rol:
                    result = builder.shift_imm(Op::Rol, Type::I64, original, amount);
                    break;
                case Wide64::Ror:
                    result = builder.shift_imm(Op::Ror, Type::I64, original, amount);
                    break;
                case Wide64::Rcl:
                    // Rotating a 65-bit span left by k puts the carry at bit
                    // k-1, the operand's low bits above it, and the bits that
                    // wrapped past bit 64 at the bottom. The wrap term is
                    // empty at k == 1, where its shift would also be by 64.
                    result = orv(shl(original, amount),
                                 shl(carry_in, static_cast<u8>(amount - 1)));
                    if (amount >= 2)
                        result = orv(result,
                            shr(original, static_cast<u8>(65 - amount)));
                    break;
                default:
                    // RCR: the mirror image. The carry lands at bit 64-k.
                    result = orv(shr(original, amount),
                                 shl(carry_in, static_cast<u8>(64 - amount)));
                    if (amount >= 2)
                        result = orv(result,
                            shl(original, static_cast<u8>(65 - amount)));
                    break;
                }
                // The store is what can fault, so for a memory destination it
                // happens BEFORE any flag is published: a faulting write must
                // leave the previous instruction's flags standing.
                if (to_memory)
                    builder.store(Type::I64, address, result, kMemFaultState);
                switch (wide64) {
                case Wide64::Shl: case Wide64::Shr: case Wide64::Sar: {
                    const bool one = amount == 1;
                    FlagState::FlagOp record = FlagState::FlagOp::Shl64;
                    if (wide64 == Wide64::Shl)
                        record = one ? FlagState::FlagOp::Shl64_1
                                     : FlagState::FlagOp::Shl64;
                    else if (wide64 == Wide64::Shr)
                        record = one ? FlagState::FlagOp::Shr64_1
                                     : FlagState::FlagOp::Shr64;
                    else
                        record = one ? FlagState::FlagOp::Sar64_1
                                     : FlagState::FlagOp::Sar64;
                    // N and Z of the result ARE the guest's SF and ZF at a
                    // nonzero count, so the definition says so: it lets a
                    // following JZ/JNZ/JS/JNS be one `B.cond` instead of a
                    // condition-helper call. It claims nothing about C or V --
                    // x86 CF is the shifted-out bit and lives only in the
                    // record -- which is exactly what `NzOnly` means.
                    // CF is the last bit shifted out, and the record has no
                    // room for it, so it goes through the overlay. The overlay
                    // WRITE must follow the record -- publishing one clears the
                    // overlays -- but the bit is computed BEFORE it: the ops
                    // that extract it would otherwise sit between the flag
                    // definition and the branch that reads its residency, and
                    // an op there costs the fused `B.cond`.
                    ValueId carry_bit = kInvalidValue;
                    if (!one) {
                        const u8 index = wide64 == Wide64::Shl
                            ? static_cast<u8>(64 - amount)
                            : static_cast<u8>(amount - 1);
                        carry_bit = bit(original, index);
                    }
                    const FlagDef def{FlagRepr::InCpuStateRecord,
                                      NzcvKind::NzOnly, record, 0, false, false};
                    live_flags = builder.def_flags(def, original, result);
                    if (!one)
                        builder.set_flag_overlay(Op::SetCf, carry_bit, Type::I64);
                    break;
                }
                default: {
                    // Rotates publish no record: they move CF and OF and leave
                    // ZF/SF/PF/AF exactly as the previous instruction left them.
                    ValueId cf_out = kInvalidValue;
                    if (wide64 == Wide64::Rcl)
                        cf_out = bit(original, static_cast<u8>(64 - amount));
                    else if (wide64 == Wide64::Rcr)
                        cf_out = bit(original, static_cast<u8>(amount - 1));
                    else if (wide64 == Wide64::Ror)
                        cf_out = bit(result, 63);
                    else
                        cf_out = bit(result, 0);
                    builder.set_flag_overlay(Op::SetCf, cf_out, Type::I64);
                    if (amount == 1) {
                        ValueId of_out = kInvalidValue;
                        if (wide64 == Wide64::Rcr)
                            of_out = xorv(bit(original, 63), carry_in);
                        else if (wide64 == Wide64::Ror)
                            of_out = xorv(bit(result, 63), bit(result, 62));
                        else
                            of_out = xorv(bit(result, 63), cf_out);
                        builder.set_flag_overlay(Op::SetOf, of_out, Type::I64);
                    }
                    break;
                }
                }
                if (!to_memory)
                    builder.gpr_write(reg, result);
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_shift_cl(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Shifts by CL. The count is a RUNTIME value, so `FlagDef::shift_count`
    // cannot carry it; the record takes it as a third operand instead, which
    // is the shape the publisher has always packed (it takes a count
    // register). Intel SDM Vol. 2: the count is masked to 5 bits, and a masked
    // count of ZERO modifies no flags at all -- a runtime decision, which the
    // lowering makes with a runtime skip.
    struct ShiftCl { Op op; FlagOp record; Type width; };
    ShiftCl by_cl{Op::Shl, FlagOp::None, Type::Void};
    switch (insn.opcode) {
    case Opcode::SHL_RM8_CL:  by_cl = {Op::Shl, FlagOp::Shl8,  Type::I8};  break;
    case Opcode::SHL_RM16_CL: by_cl = {Op::Shl, FlagOp::Shl16, Type::I16}; break;
    case Opcode::SHL_RM32_CL: by_cl = {Op::Shl, FlagOp::Shl32, Type::I32}; break;
    case Opcode::SHR_RM8_CL:  by_cl = {Op::Shr, FlagOp::Shr8,  Type::I8};  break;
    case Opcode::SHR_RM16_CL: by_cl = {Op::Shr, FlagOp::Shr16, Type::I16}; break;
    case Opcode::SHR_RM32_CL: by_cl = {Op::Shr, FlagOp::Shr32, Type::I32}; break;
    case Opcode::SAR_RM8_CL:  by_cl = {Op::Sar, FlagOp::Sar8,  Type::I8};  break;
    case Opcode::SAR_RM16_CL: by_cl = {Op::Sar, FlagOp::Sar16, Type::I16}; break;
    case Opcode::SAR_RM32_CL: by_cl = {Op::Sar, FlagOp::Sar32, Type::I32}; break;
    default: break;
    }
    if (by_cl.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const Type w = by_cl.width;
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
                // CL masked to five bits, at every operand width.
                const ValueId count = builder.binary(Op::And, Type::I8,
                    builder.gpr_read(GuestReg{1, Type::I8}),
                    builder.constant(Type::I8, 31));
                const ValueId result = builder.shift_reg(by_cl.op, w, original, count);
                const FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                                  by_cl.record, false, false};
                if (to_memory) {
                    builder.store(w, address, result, kMemFaultState);
                    live_flags = builder.def_flags_shift_reg(def, original, result,
                                                             count, w);
                } else {
                    live_flags = builder.def_flags_shift_reg(def, original, result,
                                                             count, w);
                    builder.gpr_write(reg, result);
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

FamilyVerdict try_long_mode_shift_imm(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Narrow shifts by a compile-time count. Intel SDM Vol. 2 SAL/SAR/SHL/SHR:
    // the count is masked to 5 bits at every operand width, and a masked count
    // of ZERO modifies no flags at all -- not even ZF -- so it emits nothing
    // rather than a record that would claim the previous definition's flags.
    //
    // ROL/ROR/RCL/RCR are absent on purpose: `Rol8/16/32` and `Ror8/16/32`
    // have no materializer case, and the carry-through rotates have no record
    // at all, so publishing one would leave the flags unchanged.
    struct NarrowShift { Op op; FlagOp record; Type width; };
    NarrowShift shift{Op::Shl, FlagOp::None, Type::Void};
    switch (insn.opcode) {
    case Opcode::SHL_RM8_IMM8: case Opcode::SHL_RM8_1:
        shift = {Op::Shl, FlagOp::Shl8, Type::I8}; break;
    case Opcode::SHL_RM16_IMM8: case Opcode::SHL_RM16_1:
        shift = {Op::Shl, FlagOp::Shl16, Type::I16}; break;
    case Opcode::SHR_RM8_IMM8: case Opcode::SHR_RM8_1:
        shift = {Op::Shr, FlagOp::Shr8, Type::I8}; break;
    case Opcode::SHR_RM16_IMM8: case Opcode::SHR_RM16_1:
        shift = {Op::Shr, FlagOp::Shr16, Type::I16}; break;
    case Opcode::SAR_RM8_IMM8: case Opcode::SAR_RM8_1:
        shift = {Op::Sar, FlagOp::Sar8, Type::I8}; break;
    case Opcode::SAR_RM16_IMM8: case Opcode::SAR_RM16_1:
        shift = {Op::Sar, FlagOp::Sar16, Type::I16}; break;
    case Opcode::SHL_RM32_IMM8: case Opcode::SHL_RM32_1:
        shift = {Op::Shl, FlagOp::Shl32, Type::I32}; break;
    case Opcode::SHR_RM32_IMM8: case Opcode::SHR_RM32_1:
        shift = {Op::Shr, FlagOp::Shr32, Type::I32}; break;
    case Opcode::SAR_RM32_IMM8: case Opcode::SAR_RM32_1:
        shift = {Op::Sar, FlagOp::Sar32, Type::I32}; break;
    default: break;
    }
    if (shift.width != Type::Void && !px.lock && !px.rep && !px.repne &&
        !px.vex && !px.evex && insn.operand_count >= 1) {
        verdict.reason = DeclineReason::UnsupportedOperand;
        const auto& dst = insn.operands[0];
        const Type w = shift.width;
        const u8 count = (insn.operand_count >= 2 &&
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
                // x86 masks the count to 5 bits at EVERY operand width, so a
                // byte shift by 9 is a real instruction -- but `Op::Shl` is
                // defined only below its type's width, deliberately, so that
                // the guest's masking rule lives in the lowering that knows
                // it. Past the width the result is already settled: SHL and
                // SHR have shifted every bit out, and SAR has replicated the
                // sign, which is what a shift by width-1 leaves. The RECORD
                // still carries the real count, so CF and OF stay right.
                //
                // A masked count of ZERO modifies no FLAGS, but it still
                // writes the destination -- and a 32-bit write zero-extends to
                // 64. Skipping the write there is a recorded bug that JIT and
                // interpreter once had identically, so parity stayed green
                // while `shl ecx,0` left RCX[63:32] alone.
                const u32 width = static_cast<u32>(type_bits(w));
                const ValueId result = count == 0 ? original
                    : (count < width
                        ? builder.shift_imm(shift.op, w, original, count)
                        : (shift.op == Op::Sar
                            ? builder.shift_imm(Op::Sar, w, original, width - 1)
                            : builder.constant(w, 0)));
                FlagDef def{FlagRepr::InCpuStateRecord, NzcvKind::None,
                            shift.record, false, false};
                def.shift_count = count;
                // The record reads `flag_src` = original and packs the count
                // into `flag_dst`, so the definition may follow the write.
                if (to_memory) {
                    builder.store(w, address, result, kMemFaultState);
                    if (count != 0)
                        live_flags = builder.def_flags(def, original, result);
                } else {
                    if (count != 0)
                        live_flags = builder.def_flags(def, original, result);
                    builder.gpr_write(reg, result);
                }
                return {true, DeclineReason::None};
            }
        }
        return verdict;
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir
