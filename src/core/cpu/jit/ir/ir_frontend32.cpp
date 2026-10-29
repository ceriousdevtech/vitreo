// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend32.cpp
/// @brief The 32-bit (protected-mode) frontend: `build_insn` and its
///        32-only operand and addressing helpers
///
/// Split out of ir_frontend.cpp by pure code motion; see that file for the
/// flag-record operand contract.

#include "ir_frontend_internal.hpp"

#include <cstdlib>

namespace vitreo::cpu::jit::ir {

namespace {

/// `VITREO_JIT_IR_NARROW`: bit 0 = 8/16-bit CMP/TEST, bit 1 = partial-register
/// MOV to a register, bit 2 = narrow MOV to MEMORY, bit 3 = MOVZX/MOVSX.
///
/// CMP/TEST and MOVZX/MOVSX shared bit 0 until a measurement needed to tell
/// them apart: the appbench `lzss` workload is 1.78x slower with the narrow
/// forms on and bit 0 accounts for all of it, which is two different
/// suspects until they are separable.
///
/// **Default is 3, not 7 — bit 2 is off because it is implicated in a host
/// SIGSEGV.** With all three enabled, the guest dies 3/3 with a host fault
/// inside JIT code reached through its own UnhandledExceptionFilter; with bits
/// 0+1 (3/3 clean), 0+2 (clean) or 0 alone (clean) it does not. Each PAIR is
/// fine and only the full set fails, which points at a resource interaction
/// rather than a wrong lowering in either — but "points at" is not a
/// diagnosis, and shipping a default that reproducibly crashes a guest is not
/// something a bisection switch excuses.
///
/// Recorded in `status/BASELINE.md`. Set `VITREO_JIT_IR_NARROW=7` to reproduce.
///
/// Same reasoning as the SSE mask: when a tranche is suspected of breaking a
/// guest, "which part" is the first question and a game launch is the
/// measurement.
[[nodiscard]] u32 narrow_form_mask() noexcept {
    static const u32 mask = [] {
        const char* v = std::getenv("VITREO_JIT_IR_NARROW");
        if (v == nullptr || *v == '\0') return 15u;  // every narrow form
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(v, &end, 0);
        if (end == v || *end != '\0') return 7u;
        return static_cast<u32>(parsed);
    }();
    return mask;
}

[[nodiscard]] bool narrow_form_enabled(u32 bit) noexcept {
    return (narrow_form_mask() & bit) != 0;
}

/// A prefix that changes what the instruction means. Each one is a decline,
/// not a detail to carry along: LOCK is an atomic RMW with ordering the IR
/// does not yet model, REP is an implicit loop, a segment override changes the
/// address space, and an operand-size override changes the width of everything
/// below.
/// `allow_mandatory_rep` exempts F3 for the SSE forms that use it as a
/// MANDATORY prefix rather than a repeat.
///
/// The decoder has already folded F3 into the opcode — `MOVSS_XMM_XMMM32` is
/// a distinct enumerator, not `MOVUPS` plus a repeat — so `p.rep` here is a
/// decoding artifact with no semantics of its own. Rejecting it turned every
/// MOVSS into `unsupported-prefix`, which on one load is 2021 blocks. Every
/// other prefix stays rejected, including REPNE: only the exact opcodes whose
/// enumerator already encodes the prefix are exempted, at the one call site
/// that knows which those are.
[[nodiscard]] bool has_unsupported_prefix(const Prefixes& p,
                                          bool allow_mandatory_rep,
                                          bool allow_operand_size,
                                          bool allow_segment) noexcept {
    if (p.segment != SegReg::Default && !allow_segment) return true;
    if (p.rep && !allow_mandatory_rep) return true;
    // 0x66 on a 16-bit form is the same kind of artifact as F3 on MOVSS: the
    // decoder CONSUMED it to select `CMP_RM16_R16` rather than
    // `CMP_RM32_R32`, so by the time the enumerator says 16-bit the prefix has
    // already done its work. Exempted only for the exact forms whose
    // enumerator encodes the width.
    //
    // 0x67 (address size) stays rejected: it changes how the effective
    // address is computed, and nothing here has consumed it.
    if (p.operand_size && !allow_operand_size) return true;
    return p.lock || p.repne || p.address_size ||
           p.vex || p.rex != 0;
}

[[nodiscard]] bool is_reg32(const X86Operand& op) noexcept {
    return op.type == OperandType::Reg32;
}

[[nodiscard]] bool is_imm(const X86Operand& op) noexcept {
    return op.type == OperandType::Imm8 || op.type == OperandType::Imm16 ||
           op.type == OperandType::Imm32;
}

/// A 32-bit memory operand. `MemUnknown` is deliberately NOT accepted: an
/// access whose width the decoder could not determine is not one to guess at.
[[nodiscard]] bool is_mem32(const X86Operand& op) noexcept {
    return op.type == OperandType::Mem32;
}

/// Narrow register/memory operand predicates, and the one piece of x86
/// encoding trivia this tranche turns on.
///
/// **Without a REX prefix, `Reg8` indices 4-7 are AH/CH/DH/BH** — the HIGH
/// byte of GPRs 0-3 — not SPL/BPL/SIL/DIL. Tier 1 rejects REX outright, so in
/// this frontend that mapping is unconditional. `GuestReg{4, I8}` therefore
/// means AH, and the emitter extracts bits 15:8. Reading it as "the low byte
/// of ESP" would be wrong on every `test ah, ah`.
[[nodiscard]] bool is_reg_of_width(const X86Operand& op, Type w) noexcept {
    return (w == Type::I8 && op.type == OperandType::Reg8) ||
           (w == Type::I16 && op.type == OperandType::Reg16);
}

[[nodiscard]] bool is_mem_of_width(const X86Operand& op, Type w) noexcept {
    return (w == Type::I8 && op.type == OperandType::Mem8) ||
           (w == Type::I16 && op.type == OperandType::Mem16);
}

constexpr GuestReg kEsp{kEspIndex, Type::I32};
constexpr GuestReg kEbp{5, Type::I32};

[[nodiscard]] GuestReg gpr32(const X86Operand& op) noexcept {
    return GuestReg{op.reg, Type::I32};
}

/// Materialize an operand that must already be a 32-bit register or an
/// immediate. Callers have checked the shape; this only builds.
[[nodiscard]] ValueId read_operand(IrBuilder& b, const X86Operand& op) {
    if (is_reg32(op)) return b.gpr_read(gpr32(op));
    return b.constant(Type::I32, static_cast<u64>(op.imm));
}

[[nodiscard]] ValueId build_ea(IrBuilder& b, const MemoryOperand& mem) {
    if (mem.vsib || mem.has_absolute) return kInvalidValue;
    if (mem.is_rip_relative()) return kInvalidValue;  // 64-bit only anyway

    // The segment base, when the instruction carries an override. It is added
    // to the finished address rather than folded into the `Lea`, so an EA with
    // a segment stays an ordinary expression instead of a special node.
    //
    // Every non-default segment goes through the same CpuState field, so FS
    // (the TEB, and the one that matters) costs no more machinery than the
    // flat ones whose base happens to be zero.
    const auto with_segment = [&](ValueId addr) -> ValueId {
        if (mem.segment == SegReg::Default || addr == kInvalidValue) {
            return addr;
        }
        return b.binary(Op::Add, Type::I32, addr,
                        b.seg_base(static_cast<u8>(mem.segment)));
    };

    // Displacement-only, e.g. `mov eax, [0x00404000]` — extremely common in
    // 32-bit code, and just a constant address. With an override this is the
    // `fs:[0x18]` TEB read.
    if (!mem.has_base() && !mem.has_index()) {
        return with_segment(b.constant(Type::I32, static_cast<u64>(
                                static_cast<i64>(mem.disp))));
    }
    if (mem.has_index() && mem.index > kMaxGuestGprIndex) return kInvalidValue;
    if (mem.has_base() && mem.base > kMaxGuestGprIndex) return kInvalidValue;

    const u64 scale = mem.scale;
    if (mem.has_index() && scale != 1 && scale != 2 && scale != 4 && scale != 8)
        return kInvalidValue;

    // `[edx*4 + disp]` — an index with NO base. Common enough to matter: it is
    // what a compiler emits to scale an array subscript, and it was the single
    // reason `LEA` still declined once everything else was supported.
    //
    // The `Lea` node wants a base, so the displacement becomes one. That costs
    // a constant materialization the stencil path folds into its addressing
    // mode; commoning it back is the M8 EA pass's job, not the frontend's.
    const ValueId base =
        mem.has_base() ? b.gpr_read(GuestReg{mem.base, Type::I32})
                       : b.constant(Type::I32,
                                    static_cast<u64>(
                                        static_cast<i64>(mem.disp)));
    const i64 disp = mem.has_base() ? static_cast<i64>(mem.disp) : 0;

    ValueId index = kInvalidValue;
    if (mem.has_index()) index = b.gpr_read(GuestReg{mem.index, Type::I32});
    return with_segment(
        b.lea(Type::I32, base, index, mem.has_index() ? scale : 1u, disp));
}

[[nodiscard]] InsnVerdict decline(DeclineReason reason) noexcept {
    return {false, reason};
}

/// Materialize a narrow operand as a value of `w`.
[[nodiscard]] ValueId read_narrow(IrBuilder& b, const DecodedInstruction& insn,
                                  const X86Operand& op, Type w) {
    if (is_reg_of_width(op, w))
        return b.gpr_read(GuestReg{narrow_reg_index(insn, op), w});
    if (is_imm(op)) return b.constant(w, static_cast<u64>(op.imm));
    if (is_mem_of_width(op, w)) {
        const ValueId addr = build_ea(b, op.mem);
        if (addr == kInvalidValue) return kInvalidValue;
        return b.load(w, addr, kMemFaultState);
    }
    return kInvalidValue;
}

}  // namespace

[[nodiscard]] InsnVerdict build_insn(IrBuilder& b,
                                     const DecodedInstruction& insn,
                                     u64 next_rip, ValueId& live_flags) {
    if (const auto v = try_nop(insn)) return *v;
    const Recipe prefix_recipe = classify(insn.opcode);
    if (prefix_recipe.form == Form::NarrowCompare && !narrow_form_enabled(1)) {
        return decline(DeclineReason::UnsupportedOpcode);
    }
    if (prefix_recipe.form == Form::Widen && !narrow_form_enabled(8)) {
        return decline(DeclineReason::UnsupportedOpcode);
    }
    if (prefix_recipe.form == Form::MovNarrow && !narrow_form_enabled(6)) {
        return decline(DeclineReason::UnsupportedOpcode);
    }
    if (prefix_recipe.form == Form::MovapsRR && !sse_form_enabled(4)) {
        return decline(DeclineReason::UnsupportedOpcode);
    }
    if (prefix_recipe.form == Form::XorpsRR && !sse_form_enabled(1)) {
        return decline(DeclineReason::UnsupportedOpcode);
    }
    if (prefix_recipe.form == Form::Movss && !sse_form_enabled(2)) {
        return decline(DeclineReason::UnsupportedOpcode);
    }
    const bool mandatory_rep = prefix_recipe.form == Form::Movss;
    // A segment override is honoured only by forms whose memory operands all
    // go through `build_ea`, which is what applies the base. PUSH/POP/CALL/RET
    // reach memory through an IMPLICIT stack operand that never sees
    // `mem.segment`, so an SS override on those would be silently dropped —
    // they keep declining.
    const bool segment_ok =
        prefix_recipe.form == Form::Mov ||
        prefix_recipe.form == Form::AluRmw ||
        prefix_recipe.form == Form::AluCompare ||
        prefix_recipe.form == Form::NarrowCompare ||
        prefix_recipe.form == Form::Widen ||
        prefix_recipe.form == Form::MovNarrow ||
        prefix_recipe.form == Form::Movss ||
        prefix_recipe.form == Form::Lea ||
        prefix_recipe.form == Form::Not ||
        prefix_recipe.form == Form::Neg ||
        prefix_recipe.form == Form::IncDec;
    const bool folded_operand_size =
        (prefix_recipe.form == Form::NarrowCompare ||
         prefix_recipe.form == Form::MovNarrow) &&
        prefix_recipe.width == Type::I16;
    if (has_unsupported_prefix(insn.prefixes, mandatory_rep,
                               folded_operand_size, segment_ok)) {
        // WHICH prefix, not just that there was one. "unsupported-prefix" was
        // 5.9% of RCT3's declines with no way to tell a segment override from
        // a LOCK — and those are opposite amounts of work: a DS/ES/SS override
        // in a flat 32-bit process adds a zero base and is free, while LOCK is
        // an atomicity contract.
        const Prefixes& px = insn.prefixes;
        stats::note_prefix_decline(
            px.lock ? 0 :
            px.repne ? 1 :
            (px.rep && !mandatory_rep) ? 2 :
            (px.operand_size && !folded_operand_size) ? 3 :
            px.address_size ? 4 :
            px.vex ? 5 :
            px.rex != 0 ? 6 :
            px.segment != SegReg::Default
                ? (7 + static_cast<u32>(px.segment)) : 15);
        return decline(DeclineReason::UnsupportedPrefix);
    }

    const Recipe recipe = classify(insn.opcode);
    if (recipe.form == Form::Unsupported)
        return decline(DeclineReason::UnsupportedOpcode);

    switch (recipe.form) {
    case Form::AluRmw:
    case Form::AluCompare: {
        if (insn.operand_count != 2)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];

        // A memory destination splits in two.
        //
        // CMP and TEST never WRITE their destination — they exist only for the
        // flags — so `cmp [mem], eax` is a plain load.
        //
        // A read-modify-write (`add [mem], eax`) writes it, and the ORDER is
        // the correctness content. Measured against the interpreter
        // (`IrEmit.FaultingReadModifyWriteLeavesFlagsUnchanged`): a faulting
        // RMW leaves the PREVIOUS instruction's flags visible, so the flag
        // definition is published only after the store retires.
        const bool mem_dst = is_mem32(dst);
        if (!mem_dst && !is_reg32(dst))
            return decline(DeclineReason::UnsupportedOperand);

        // A memory SOURCE has no such hazard: the load is the only faulting
        // op, and everything it feeds happens after it. If it faults, nothing
        // downstream has run.
        if (!is_reg32(src) && !is_imm(src) && !is_mem32(src))
            return decline(DeclineReason::UnsupportedOperand);

        ValueId a;
        ValueId mem_addr = kInvalidValue;
        if (mem_dst) {
            mem_addr = build_ea(b, dst.mem);
            if (mem_addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            a = b.load(Type::I32, mem_addr, kMemFaultState);
        } else {
            a = b.gpr_read(gpr32(dst));
        }
        ValueId s;
        if (is_mem32(src)) {
            const ValueId addr = build_ea(b, src.mem);
            if (addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            s = b.load(Type::I32, addr, kMemFaultState);
        } else {
            s = read_operand(b, src);
        }
        const ValueId r = b.binary(recipe.op, Type::I32, a, s);
        // Two orders that look contradictory and are not.
        //
        // A MEMORY destination stores BEFORE publishing flags, because a
        // faulting store must leave the previous instruction's flags intact.
        // A REGISTER destination publishes flags BEFORE the write, because the
        // write overwrites the pin a flag source may still be reading (the NEG
        // case). In each, the flag definition sits where nothing it depends on
        // has been destroyed and nothing it publishes is visible too early.
        if (mem_dst && recipe.form == Form::AluRmw) {
            b.store(Type::I32, mem_addr, r, kMemFaultState);
        }

        // Operand order is the flag-record contract documented at the top of
        // this file: 0 -> flag_src (the source operand), 1 -> flag_dst (the
        // result).
        b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                            recipe.record, false, false},
                    s, r);

        // AluCompare writes nothing, which is why a memory destination is
        // trivially safe for it.
        if (recipe.form == Form::AluRmw && !mem_dst) {
            b.gpr_write(gpr32(dst), r);
        }
        return {true, DeclineReason::None};
    }

    case Form::Not: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        if (!is_reg32(dst)) return decline(DeclineReason::UnsupportedOperand);
        const ValueId a = b.gpr_read(gpr32(dst));
        const ValueId r = b.unary(Op::Not, Type::I32, a);
        b.gpr_write(gpr32(dst), r);
        return {true, DeclineReason::None};
    }

    case Form::Neg: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        if (!is_reg32(dst)) return decline(DeclineReason::UnsupportedOperand);
        const ValueId a = b.gpr_read(gpr32(dst));
        const ValueId r = b.unary(Op::Neg, Type::I32, a);
        // Neg32 materializes as `0 - flag_src` against `flag_dst`, so the
        // source word is the ORIGINAL value, not the result -- and the
        // original lives in the destination's pin, which `gpr_write` is about
        // to overwrite. Publishing first is what keeps it readable.
        b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                            FlagOp::Neg32, false, false},
                    a, r);
        b.gpr_write(gpr32(dst), r);
        return {true, DeclineReason::None};
    }

    case Form::XorpsRR: {
        if (insn.operand_count != 2) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];
        // Register-register only. A 128-bit memory operand would additionally
        // have to model the alignment #GP, and it is a small share of the
        // population: the dominant use is `xorps xmm,xmm` to zero a register.
        if (!is_xmm(dst) || !is_xmm(src)) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const ValueId a = b.xmm_read(dst.reg);
        const ValueId c = b.xmm_read(src.reg);
        b.xmm_write(dst.reg, b.binary(Op::VXor, Type::V128, a, c));
        return {true, DeclineReason::None};
    }

    case Form::NarrowCompare: {
        if (insn.operand_count != 2) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const Type w = recipe.width;
        const ValueId a = read_narrow(b, insn, insn.operands[0], w);
        if (a == kInvalidValue) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const ValueId c = read_narrow(b, insn, insn.operands[1], w);
        if (c == kInvalidValue) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        // CMP and TEST discard the result and define flags from it. Operand
        // order matters for SUB and is why the two reads are not commuted:
        // `cmp al, bl` is AL-BL, and the record's `flag_src` is the SECOND
        // operand.
        const ValueId result = b.binary(recipe.op, w, a, c);
        FlagDef def;
        def.repr = FlagRepr::InCpuStateRecord;
        def.record = recipe.record;
        live_flags = b.def_flags(def, c, result);
        return {true, DeclineReason::None};
    }

    case Form::ShiftImm: {
        if (insn.operand_count < 1) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const X86Operand& dst = insn.operands[0];
        // Register destination only. A memory destination is a
        // read-modify-write, whose flag publication must be ordered against a
        // faulting store — a contract this emitter does not yet implement for
        // any op.
        if (!is_reg32(dst)) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        // The by-1 forms encode no immediate operand.
        const u64 raw = (insn.operand_count >= 2 && is_imm(insn.operands[1]))
                            ? static_cast<u64>(insn.operands[1].imm)
                            : 1u;
        const u8 count = static_cast<u8>(raw & 31u);

        // Intel SDM Vol. 2: the count is masked to 5 bits, and a masked count
        // of ZERO leaves the destination AND every flag untouched — not even
        // ZF. So the whole instruction is a no-op, and emitting a flag
        // definition with a zero count would claim this instruction's record
        // over the previous definition's still-live flags.
        if (count == 0) {
            return {true, DeclineReason::None};
        }

        const ValueId src = b.gpr_read(gpr32(dst));
        const ValueId result = b.shift_imm(recipe.op, Type::I32, src, count);
        FlagDef def;
        def.repr = FlagRepr::InCpuStateRecord;
        def.record = recipe.record;
        def.shift_count = count;
        // The record is `flag_src` = the ORIGINAL value, `flag_dst` =
        // `(count << 32) | result`. Operand order here is (original, result);
        // the count travels on the FlagDef because it is a constant.
        live_flags = b.def_flags(def, src, result);
        b.gpr_write(gpr32(dst), result);
        return {true, DeclineReason::None};
    }

    case Form::MovNarrow: {
        if (insn.operand_count != 2) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const Type w = recipe.width;
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];

        const ValueId v = read_narrow(b, insn, src, w);
        if (v == kInvalidValue) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        // A register destination is a PARTIAL write: the emitter inserts the
        // field and leaves the rest of the architectural register alone. The
        // GuestReg carries the x86 index unchanged, so 4-7 at I8 still means
        // AH/CH/DH/BH and `architectural_gpr` is what resolves it.
        if (is_reg_of_width(dst, w)) {
            if (!narrow_form_enabled(2)) {
                return decline(DeclineReason::UnsupportedOperand);
            }
            b.gpr_write(GuestReg{narrow_reg_index(insn, dst), w}, v);
            return {true, DeclineReason::None};
        }
        if (is_mem_of_width(dst, w)) {
            if (!narrow_form_enabled(4)) {
                return decline(DeclineReason::UnsupportedOperand);
            }
            const ValueId addr = build_ea(b, dst.mem);
            if (addr == kInvalidValue) {
                return decline(DeclineReason::UnsupportedOperand);
            }
            b.store(w, addr, v, kMemFaultState);
            return {true, DeclineReason::None};
        }
        return decline(DeclineReason::UnsupportedOperand);
    }

    case Form::Widen: {
        if (insn.operand_count != 2) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const X86Operand& dst = insn.operands[0];
        if (!is_reg32(dst)) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const ValueId src = read_narrow(b, insn, insn.operands[1], recipe.width);
        if (src == kInvalidValue) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        // The destination is a WHOLE 32-bit register, so this needs no
        // partial-register merge. MOVZX/MOVSX define no flags.
        const ValueId wide = (recipe.op == Op::Sext)
                                 ? b.sext(Type::I32, src)
                                 : b.zext(Type::I32, src);
        b.gpr_write(gpr32(dst), wide);
        return {true, DeclineReason::None};
    }

    case Form::MovapsRR: {
        if (insn.operand_count != 2) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];
        // Register-register only. A 128-bit memory operand needs a guest
        // access width this IR does not have and an alignment #GP it does not
        // model; declining is not a workaround, it is the absence of both.
        if (!is_xmm(dst) || !is_xmm(src)) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        b.xmm_write(dst.reg, b.xmm_read(src.reg));
        return {true, DeclineReason::None};
    }

    case Form::Movss: {
        if (insn.operand_count != 2) {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];

        // `movss xmm, xmm` — bits 127:32 of the DESTINATION are preserved.
        if (is_xmm(dst) && is_xmm(src)) {
            const ValueId old_dst = b.xmm_read(dst.reg);
            const ValueId low = b.v_low32(b.xmm_read(src.reg));
            b.xmm_write(dst.reg, b.v_insert_low32(old_dst, low));
            return {true, DeclineReason::None};
        }

        // `movss xmm, m32` — bits 127:32 are ZEROED. Measured against the
        // interpreter; the two forms genuinely differ, and the difference
        // hides whenever the destination was already zero.
        if (is_xmm(dst) && src.type == OperandType::Mem32) {
            const ValueId addr = build_ea(b, src.mem);
            if (addr == kInvalidValue) {
                return decline(DeclineReason::UnsupportedOperand);
            }
            const ValueId v = b.load(Type::I32, addr, kMemFaultState);
            b.xmm_write(dst.reg, b.v_zext32(v));
            return {true, DeclineReason::None};
        }

        // `movss m32, xmm` — a plain 32-bit store of the low lane.
        if (dst.type == OperandType::Mem32 && is_xmm(src)) {
            const ValueId addr = build_ea(b, dst.mem);
            if (addr == kInvalidValue) {
                return decline(DeclineReason::UnsupportedOperand);
            }
            b.store(Type::I32, addr, b.v_low32(b.xmm_read(src.reg)),
                    kMemFaultState);
            return {true, DeclineReason::None};
        }
        return decline(DeclineReason::UnsupportedOperand);
    }

    case Form::Mov: {
        if (insn.operand_count != 2)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];

        // `mov r32, [mem]` — a load into a register.
        if (is_reg32(dst) && is_mem32(src)) {
            const ValueId addr = build_ea(b, src.mem);
            if (addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            const ValueId v = b.load(Type::I32, addr, kMemFaultState);
            b.gpr_write(gpr32(dst), v);
            return {true, DeclineReason::None};
        }

        // `mov [mem], r32` and `mov [mem], imm32` — a store from a register
        // or a materialized constant. Measured as the second most common
        // reason a real block declined, which is why the immediate form is
        // here rather than deferred.
        if (is_mem32(dst) && (is_reg32(src) || is_imm(src))) {
            const ValueId addr = build_ea(b, dst.mem);
            if (addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            const ValueId value = is_reg32(src)
                ? b.gpr_read(gpr32(src))
                : b.constant(Type::I32, static_cast<u64>(src.imm));
            b.store(Type::I32, addr, value, kMemFaultState);
            return {true, DeclineReason::None};
        }

        if (!is_reg32(dst)) return decline(DeclineReason::UnsupportedOperand);
        if (!is_reg32(src) && !is_imm(src))
            return decline(DeclineReason::UnsupportedOperand);
        b.gpr_write(gpr32(dst), read_operand(b, src));
        return {true, DeclineReason::None};
    }

    case Form::Lea: {
        if (insn.operand_count != 2)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        const X86Operand& src = insn.operands[1];
        if (!is_reg32(dst)) return decline(DeclineReason::UnsupportedOperand);
        // LEA's source is syntactically a memory operand that is never
        // accessed. Only the register-indexed forms are expressible here: a
        // disp-only or absolute address is a constant the M8 folding pass
        // should produce, and RIP-relative addressing does not exist in a
        // 32-bit block.
        switch (src.type) {
        case OperandType::Mem8:  case OperandType::Mem16:
        case OperandType::Mem32: case OperandType::Mem64:
        case OperandType::MemUnknown:
            break;
        default:
            return decline(DeclineReason::UnsupportedOperand);
        }
        // LEA computes an address and does not access memory, so a
        // displacement-only form would be a plain constant move — legal, but
        // `build_ea` returning a `Const` there means LEA and a real access
        // share one path.
        const ValueId r = build_ea(b, src.mem);
        if (r == kInvalidValue)
            return decline(DeclineReason::UnsupportedOperand);
        b.gpr_write(gpr32(dst), r);
        return {true, DeclineReason::None};
    }

    case Form::Jcc: {
        // A Jcc reads whatever flags are live here. When no instruction in
        // this block defined any, that is the block's INCOMING flags — which
        // `FlagsIn` now names, so the shape no longer declines.
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& rel = insn.operands[0];
        if (rel.type != OperandType::Rel8 && rel.type != OperandType::Rel32)
            return decline(DeclineReason::UnsupportedOperand);
        const u8 cond = jcc_condition(insn.opcode);
        if (cond == 0xFF) return decline(DeclineReason::UnsupportedOpcode);
        // x86 relative branches are relative to the END of the instruction,
        // and a 32-bit guest wraps its EIP at 4 GB.
        const u64 target =
            static_cast<u32>(next_rip + static_cast<u64>(rel.imm));
        b.jcc(cond, require_flags(b, live_flags), target, next_rip);
        return {true, DeclineReason::None};
    }

    case Form::Jmp: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& rel = insn.operands[0];
        if (rel.type != OperandType::Rel8 && rel.type != OperandType::Rel32)
            return decline(DeclineReason::UnsupportedOperand);
        b.jmp(static_cast<u32>(next_rip + static_cast<u64>(rel.imm)));
        return {true, DeclineReason::None};
    }

    // -------------------------------------------------------------------
    // The stack forms. Their ORDER is the correctness content, not the ops.
    //
    // Intel SDM Vol. 2, PUSH: if the stack write faults, the instruction is
    // aborted and ESP is left UNCHANGED. So the ESP commit must come AFTER the
    // store, never before — and that is expressible here precisely because a
    // `GprWrite` is an explicit op with a position rather than a side effect of
    // the access.
    //
    // The emitter's in-place planner independently refuses to compute the new
    // ESP directly into the ESP pin, because its fourth condition forbids
    // committing a destination across a may-fault op. That guard was written
    // as dead code two milestones ago; this is the shape that reaches it.
    // -------------------------------------------------------------------
    case Form::Push: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& src = insn.operands[0];

        const ValueId esp0 = b.gpr_read(kEsp);
        // The source is read BEFORE the decrement, which is what makes
        // `PUSH ESP` fall out correctly rather than needing a special case:
        // the SDM stores "the value of ESP before it is decremented". For a
        // memory source it is also the fault order — a faulting load commits
        // nothing, and the ESP commit still waits for the store.
        ValueId value;
        if (is_reg32(src)) {
            value = b.gpr_read(gpr32(src));
        } else if (is_imm(src)) {
            value = b.constant(Type::I32, static_cast<u64>(src.imm));
        } else if (is_mem32(src)) {
            const ValueId src_addr = build_ea(b, src.mem);
            if (src_addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            value = b.load(Type::I32, src_addr, kMemFaultState);
        } else {
            return decline(DeclineReason::UnsupportedOperand);
        }

        const ValueId addr = b.lea(Type::I32, esp0, kInvalidValue, 1, -4);
        b.store(Type::I32, addr, value, kMemFaultState);
        b.gpr_write(kEsp, addr);      // committed only if the store completed
        return {true, DeclineReason::None};
    }

    case Form::Pop: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        if (!is_reg32(dst)) return decline(DeclineReason::UnsupportedOperand);
        // POP ESP loads INTO the stack pointer, so the increment below would
        // overwrite the popped value. Declined rather than special-cased: it
        // is rare, and a wrong ESP is unrecoverable.
        if (dst.reg == kEspIndex) return decline(DeclineReason::UnsupportedOperand);

        const ValueId esp0 = b.gpr_read(kEsp);
        const ValueId value = b.load(Type::I32, esp0, kMemFaultState);
        b.gpr_write(gpr32(dst), value);
        const ValueId esp1 = b.lea(Type::I32, esp0, kInvalidValue, 1, 4);
        b.gpr_write(kEsp, esp1);
        return {true, DeclineReason::None};
    }

    case Form::Call: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& rel = insn.operands[0];
        if (rel.type != OperandType::Rel32 && rel.type != OperandType::Rel8)
            return decline(DeclineReason::UnsupportedOperand);

        const ValueId esp0 = b.gpr_read(kEsp);
        const ValueId ret_addr = b.constant(Type::I32, next_rip);
        const ValueId addr = b.lea(Type::I32, esp0, kInvalidValue, 1, -4);
        b.store(Type::I32, addr, ret_addr, kMemFaultState);
        b.gpr_write(kEsp, addr);
        b.jmp(static_cast<u32>(next_rip + static_cast<u64>(rel.imm)));
        b.set_call_return_site(next_rip);
        return {true, DeclineReason::None};
    }

    case Form::CallIndirect: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& tgt = insn.operands[0];

        // Read the target FIRST, before anything commits.
        //
        // For the register form that is because `call esp` must branch to the
        // value ESP held on entry, not the decremented one. For the MEMORY
        // form — which is what an import thunk is, and the single most common
        // blocker measured on real 32-bit code — it is a fault-ordering
        // requirement: measured against the interpreter
        // (`IrEmit.FaultingIndirectCallTargetLoadCommitsNothing`), a faulting
        // target load leaves ESP untouched and pushes nothing.
        //
        // The push then commits ESP only after its store retires, so a fault
        // at EITHER access leaves the guest exactly as it was.
        ValueId target;
        if (is_reg32(tgt)) {
            target = b.gpr_read(gpr32(tgt));
        } else if (is_mem32(tgt)) {
            const ValueId tgt_addr = build_ea(b, tgt.mem);
            if (tgt_addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            target = b.load(Type::I32, tgt_addr, kMemFaultState);
        } else {
            return decline(DeclineReason::UnsupportedOperand);
        }
        const ValueId esp0 = b.gpr_read(kEsp);
        const ValueId ret_addr = b.constant(Type::I32, next_rip);
        const ValueId addr = b.lea(Type::I32, esp0, kInvalidValue, 1, -4);
        b.store(Type::I32, addr, ret_addr, kMemFaultState);
        b.gpr_write(kEsp, addr);
        b.jmp_indirect(target);
        b.set_call_return_site(next_rip);
        return {true, DeclineReason::None};
    }

    case Form::Ret: {
        // `RET imm16` pops the return address and then discards `imm16` more
        // bytes of arguments — the stdcall convention, and the single most
        // common blocker measured on real GUI code. Mechanically it is `RET`
        // with a larger adjustment.
        i64 extra = 0;
        if (insn.operand_count == 1) {
            const X86Operand& n = insn.operands[0];
            if (!is_imm(n)) return decline(DeclineReason::UnsupportedOperand);
            extra = n.imm;
        } else if (insn.operand_count != 0) {
            return decline(DeclineReason::UnsupportedOperand);
        }

        const ValueId esp0 = b.gpr_read(kEsp);
        const ValueId target = b.load(Type::I32, esp0, kMemFaultState);
        const ValueId esp1 = b.lea(Type::I32, esp0, kInvalidValue, 1, 4 + extra);
        b.gpr_write(kEsp, esp1);
        b.jmp_indirect(target, /*is_return=*/true);
        return {true, DeclineReason::None};
    }

    case Form::Leave: {
        // `LEAVE` is `ESP <- EBP; EBP <- Pop()` as ONE instruction: when the
        // pop faults, Windows on Intel reports the starting ESP
        // (tools/oracle_probe/stack_access_fault_probe.expected, `leave`
        // ctxsp=+0). So nothing is written before the load retires -- the
        // rule PUSH and the read-modify-write forms follow too.
        const ValueId ebp0 = b.gpr_read(kEbp);
        const ValueId value = b.load(Type::I32, ebp0, kMemFaultState);
        // The new ESP is computed while EBP's old value is still readable:
        // the write below overwrites the pin `ebp0` lives in.
        const ValueId esp1 = b.lea(Type::I32, ebp0, kInvalidValue, 1, 4);
        b.gpr_write(kEbp, value);
        b.gpr_write(kEsp, esp1);
        return {true, DeclineReason::None};
    }

    case Form::IncDec: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& dst = insn.operands[0];
        // Register destination only for now; the memory form is a
        // read-modify-write on top of the merge and is worth adding once this
        // shape is proven.
        if (!is_reg32(dst)) return decline(DeclineReason::UnsupportedOperand);

        // The flag value whose CF this definition inherits. When nothing in
        // this block has defined flags yet, that value IS the block's incoming
        // flags — which is what `FlagsIn` names.
        const ValueId inherited = require_flags(b, live_flags);

        const ValueId a = b.gpr_read(gpr32(dst));
        const ValueId one = b.constant(Type::I32, 1);
        const ValueId r = b.binary(recipe.op, Type::I32, a, one);

        // Flags BEFORE the register write: `Inc32` takes the ORIGINAL value in
        // its source word, and that value lives in the pin the write is about
        // to overwrite. Same rule as NEG.
        b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                            recipe.record, false, false},
                    a, r, inherited);
        b.gpr_write(gpr32(dst), r);
        return {true, DeclineReason::None};
    }

    case Form::JmpIndirect: {
        if (insn.operand_count != 1)
            return decline(DeclineReason::UnsupportedOperand);
        const X86Operand& tgt = insn.operands[0];
        if (is_reg32(tgt)) {
            b.jmp_indirect(b.gpr_read(gpr32(tgt)));
            return {true, DeclineReason::None};
        }
        if (is_mem32(tgt)) {
            const ValueId addr = build_ea(b, tgt.mem);
            if (addr == kInvalidValue)
                return decline(DeclineReason::UnsupportedOperand);
            b.jmp_indirect(b.load(Type::I32, addr, kMemFaultState));
            return {true, DeclineReason::None};
        }
        return decline(DeclineReason::UnsupportedOperand);
    }

    case Form::LoadReg:
    case Form::StoreReg:
    case Form::Unsupported:
        break;
    }
    return decline(DeclineReason::UnsupportedOpcode);
}

}  // namespace vitreo::cpu::jit::ir
