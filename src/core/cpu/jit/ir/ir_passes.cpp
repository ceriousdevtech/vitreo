// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_passes.cpp
/// @brief IR optimization passes. See `ir_passes.hpp` for the rules every pass
///        here obeys.

#include "ir_passes.hpp"
#include "ir_spill_frame.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace vitreo::cpu::jit::ir {

namespace {

constexpr u32 kAllPasses = static_cast<u32>(Pass::FlagDemand) |
                           static_cast<u32>(Pass::NzcvResidency) |
                           static_cast<u32>(Pass::FlagRecovery) |
                           static_cast<u32>(Pass::RegisterReadReuse) |
                           static_cast<u32>(Pass::ScalarConstants) |
                           static_cast<u32>(Pass::OverlayDemand) |
                           static_cast<u32>(Pass::PstateCarry) |
                           static_cast<u32>(Pass::ExitRecord);

/// True for ops whose lowering may destroy host NZCV.
///
/// Conservative on purpose. A memory access takes the region-cache path's
/// bounds-check SUBS when the linear fast path is unavailable, and which one
/// applies is a runtime configuration the IR cannot see. A CF-preserving
/// definition captures the incoming carry through `emit_lazy_cf_to_call_tmp`,
/// which may make a helper call. Marking both as clobbering costs a fusion
/// opportunity that `cmp ; jcc` never has anyway — nothing sits between them.
[[nodiscard]] bool clobbers_nzcv(const Inst& in) noexcept {
    if (in.op == Op::Load || in.op == Op::Store) return true;
    // Its condition is evaluated from the lazy record, destroying NZCV.
    if (in.op == Op::CondExit) return true;
    // Helper calls, and Select's compare of its condition value against zero.
    if (in.op == Op::Select || in.op == Op::EvalCc || in.op == Op::Divide ||
        in.op == Op::FlagsImage || in.op == Op::FlagsInstall ||
        in.op == Op::SetCf || in.op == Op::SetOf) {
        return true;
    }
    if (in.op == Op::DefFlags && in.operand_count == 3) return true;
    // ADCS/SBCS, and the carry-in setup when C is not already the carry.
    if (in.op == Op::AddCarry || in.op == Op::SubBorrow) return true;
    return false;
}

std::atomic<u32> g_pass_mask{0xFFFF'FFFFu};  ///< sentinel: not yet parsed
std::atomic<bool> g_pass_mask_ready{false};

[[nodiscard]] u32 pass_mask() noexcept {
    if (g_pass_mask_ready.load(std::memory_order_relaxed))
        return g_pass_mask.load(std::memory_order_relaxed);
    u32 mask = kAllPasses;
    const char* env = std::getenv("VITREO_JIT_IR_PASSES");
    if (env != nullptr && *env != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(env, &end, 0);
        // Fail OPEN, unlike the Tier 1 gate itself. A typo that silently
        // disabled every pass would look like "the optimization does nothing"
        // and be chased in the pass instead of in the environment; a typo that
        // leaves them on costs nothing but the default behaviour.
        if (end != env && *end == '\0') mask = static_cast<u32>(parsed);
    }
    g_pass_mask.store(mask, std::memory_order_relaxed);
    g_pass_mask_ready.store(true, std::memory_order_relaxed);
    return mask;
}

}  // namespace

bool pass_enabled(Pass pass) noexcept {
    return (pass_mask() & static_cast<u32>(pass)) != 0;
}

namespace {

/// Index of the op defining `v`, or `kNoDef`.
constexpr u32 kNoDef = 0xFFFF'FFFFu;

u32 defining_op(const IrBlock& block, ValueId v, u32 before) noexcept {
    for (u32 j = before; j-- > 0;) {
        const Inst& in = block.insts[j];
        if (in.type != Type::Void && in.result == v) return j;
    }
    return kNoDef;
}

/// Resolve one x86 operand to something a fault handler can find: a guest
/// register whose pin still holds it, or a literal.
///
/// Anything else — a value computed into a scratch, a load result, a
/// zero-extension — is deliberately NOT resolvable. The emitter resets its
/// value-to-register map at every guest instruction boundary, so a temporary
/// simply does not exist at the later instruction where the fault happens.
/// That is the whole reason this analysis is restricted to register and
/// immediate forms.
bool resolve_operand(const IrBlock& block, const Operand& op, u32 before,
                     bool& is_imm, u8& reg, u64& imm) noexcept {
    if (op.kind == OperandKind::Imm) {
        is_imm = true;
        imm = op.payload;
        return true;
    }
    if (op.kind != OperandKind::Value) return false;
    const u32 def = defining_op(block, op.as_value(), before);
    if (def == kNoDef) return false;
    const Inst& d = block.insts[def];
    if (d.op == Op::Const) {
        is_imm = true;
        imm = d.operands[0].payload;
        return true;
    }
    if (d.op == Op::GprRead) {
        is_imm = false;
        reg = d.operands[0].reg_index;
        return true;
    }
    return false;
}

/// The records whose x86 operation the fault handler can redo, at 32 and 64
/// bits. Kept in lockstep with `jit_rebuild_flag_record` and with publication
/// validation — three places, deliberately, because a record the handler
/// cannot rebuild must never be elided in the first place.
bool recoverable_record(FlagState::FlagOp op) noexcept {
    switch (op) {
    case FlagState::FlagOp::Add32: case FlagState::FlagOp::Add64:
    case FlagState::FlagOp::Sub32: case FlagState::FlagOp::Sub64:
    case FlagState::FlagOp::And32: case FlagState::FlagOp::And64:
    case FlagState::FlagOp::Or32:  case FlagState::FlagOp::Or64:
    case FlagState::FlagOp::Xor32: case FlagState::FlagOp::Xor64:
        return true;
    default:
        return false;
    }
}

bool record_is_64bit(FlagState::FlagOp op) noexcept {
    return op == FlagState::FlagOp::Add64 || op == FlagState::FlagOp::Sub64 ||
           op == FlagState::FlagOp::And64 || op == FlagState::FlagOp::Or64 ||
           op == FlagState::FlagOp::Xor64;
}

}  // namespace

namespace {

/// True for an INC/DEC carry-merge definition at 32 or 64 bits in a long-mode
/// block: three operands, the third the `Flags` value its CF is inherited
/// from (not a by-CL shift's `I8` count).
bool is_recoverable_merge(const IrBlock& block, const Inst& def) noexcept {
    if (!block.is_64bit || def.op != Op::DefFlags || def.operand_count != 3 ||
        def.operands[2].kind != OperandKind::Value ||
        def.operands[2].type != Type::Flags || def.flags.shift_count != 0) {
        return false;
    }
    switch (def.flags.record) {
    case FlagState::FlagOp::Inc32: case FlagState::FlagOp::Dec32:
    case FlagState::FlagOp::Inc64: case FlagState::FlagOp::Dec64:
        return true;
    default:
        return false;
    }
}

/// Ops whose lowering neither writes the architectural flag state in
/// CpuState (record, overlays, RFLAGS) nor calls a helper that might.
bool leaves_cpu_flag_state_alone(Op op) noexcept {
    switch (op) {
    case Op::FlagsIn:  // names the entry state; emits nothing
    case Op::Const: case Op::GprRead: case Op::GprWrite:
    case Op::Lea: case Op::SegBase:
    case Op::Trunc: case Op::Zext: case Op::Sext:
    case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor:
    case Op::Not: case Op::Neg:
    case Op::Shl: case Op::Shr: case Op::Sar: case Op::Rol: case Op::Ror:
    case Op::Mul: case Op::SMulH:
    case Op::Load: case Op::Store:
    case Op::XmmRead: case Op::XmmWrite:
        return true;
    default:
        return false;
    }
}

/// The carry an elided INC/DEC record inherited is still CpuState's at every
/// fault the plan covers, so the handler can read it there (`lazy_cf`).
///
/// The merge captures CF at the start of its instruction from the flags it
/// names: the block's entry state (`FlagsIn`, which CpuState holds on entry)
/// or the definition just before it, which must be PUBLISHED so its record is
/// what CpuState holds. From that capture to the last covered fault nothing
/// may change the CpuState flag state: the merge itself publishes nothing
/// (that is the elision), no other definition intervenes (the killer is the
/// next one), and every op in between is one whose lowering leaves the
/// record, the overlays and RFLAGS alone. The caller pins that definition's
/// record (FlagDef::record_pinned) so no later pass elides it.
bool merge_carry_source_live(const IrBlock& block, u32 def_index,
                             u32 last_fault_index) noexcept {
    const Inst& def = block.insts[def_index];
    const ValueId src = def.operands[2].as_value();
    const u32 at = defining_op(block, src, def_index);
    if (at == kNoDef) return false;
    const Inst& m = block.insts[at];
    if (m.op == Op::DefFlags) {
        if (m.flags.repr != FlagRepr::InCpuStateRecord ||
            m.flags.record == FlagState::FlagOp::None) {
            return false;
        }
    } else if (m.op != Op::FlagsIn) {
        return false;
    }
    // From the merge's own instruction start through the last covered fault.
    u32 start = def_index;
    while (start > 0 && block.insts[start - 1].guest_rip == def.guest_rip) --start;
    for (u32 j = start; j <= last_fault_index && j < block.insts.size(); ++j) {
        if (j == def_index) continue;
        if (!leaves_cpu_flag_state_alone(block.insts[j].op)) return false;
    }
    return true;
}

}  // namespace

const char* recovery_decline_name(RecoveryDecline r) noexcept {
    switch (r) {
        case RecoveryDecline::None:                  return "none";
        case RecoveryDecline::CarryMerge:            return "carry-merge";
        case RecoveryDecline::OverlayLive:           return "overlay-live";
        case RecoveryDecline::UnsupportedRecord:     return "unsupported-record";
        case RecoveryDecline::FaultInsideEndpoint:   return "fault-in-endpoint";
        case RecoveryDecline::OperandNotRecoverable: return "operand-not-recoverable";
        case RecoveryDecline::OperandClobbered:      return "operand-clobbered";
        case RecoveryDecline::StoreRaisesInHelper:   return "store-raises-in-helper";
        case RecoveryDecline::Structural:            return "structural";
        case RecoveryDecline::_Count:                break;
    }
    return "?";
}

bool try_plan_flag_recovery(const IrBlock& block, u32 def_index,
                            u32 killer_index,
                            FlagRecoveryPlan& plan,
                            RecoveryDecline* why) noexcept {
    const auto refuse = [&](RecoveryDecline r) {
        if (why != nullptr) *why = r;
        return false;
    };
    if (!pass_enabled(Pass::FlagRecovery)) return refuse(RecoveryDecline::Structural);
    if (def_index >= block.insts.size()) return refuse(RecoveryDecline::Structural);
    if (killer_index > block.insts.size()) return refuse(RecoveryDecline::Structural);

    const Inst& def = block.insts[def_index];
    if (def.op != Op::DefFlags) return refuse(RecoveryDecline::Structural);
    // A CF-preserving record (three operands) needs the OLD carry, which no
    // register holds by then -- but CpuState may: see merge_carry_source_live.
    // An overlay live over this definition must survive, but the handler's
    // reconstruction clears overlays — correctly, for a full-flag writer and
    // for the merge, whose captured CF already includes any overlay.
    const bool merge = is_recoverable_merge(block, def);
    if (def.operand_count != 2 && !merge) {
        return refuse(RecoveryDecline::CarryMerge);
    }
    if (def.flags.overlay_live) return refuse(RecoveryDecline::OverlayLive);
    if (!merge && !recoverable_record(def.flags.record)) {
        return refuse(RecoveryDecline::UnsupportedRecord);
    }

    // The covered range is the guest instructions STRICTLY between the
    // definition's own instruction and its killer's. Any may-fault op inside
    // either endpoint's instruction is therefore uncovered, and the shape is
    // refused rather than covered approximately: within one guest instruction
    // the record's publication point relative to a faulting access is a
    // property of that lowering, not of this analysis.
    const u64 def_rip = def.guest_rip;
    const u64 killer_rip = (killer_index < block.insts.size())
                               ? block.insts[killer_index].guest_rip
                               : kInvalidRip;
    u64 first_rip = kInvalidRip;
    u64 last_rip = kInvalidRip;
    u32 last_fault_index = 0;
    for (u32 j = def_index + 1; j < killer_index; ++j) {
        const Inst& mid = block.insts[j];
        if (!mid.may_fault) continue;
        // A may-fault op inside the DEFINITION's own instruction sits after
        // this definition's `DefFlags` — everything before it has a lower
        // index and is not scanned. That is the aborting read-modify-write
        // shape, where a faulting store leaves the flags UNCHANGED, so the
        // record that must be visible is the PREVIOUS one, not this one.
        // Covering the interval would install the wrong record; refusing
        // outright is the conservative reading and costs nothing today,
        // because Tier 1 does not yet emit read-modify-write.
        if (mid.guest_rip == def_rip) {
            return refuse(RecoveryDecline::FaultInsideEndpoint);
        }
        // Stores were refused here for one increment, because only a HARDWARE
        // fault rebuilds an elided record and a store can raise in SOFTWARE:
        // its check byte sends mixed-protection, code-holding and
        // write-watched pages to a cold stub whose write helper raises through
        // `emit_exit_if_pending_raise`, which the recovery map never reaches.
        // Measured before the fix: EFLAGS 0x2 against the interpreter's 0x57.
        //
        // They are allowed again now that the cold stub REPUBLISHES the record
        // before calling that helper (`ColdLinearStoreStub::replay`), which is
        // the same shape the stencil path uses for a Jcc's taken edge. It
        // costs nothing on the hot path because the stub is cold, and it is
        // possible at all because every operand a plan names is a pin or an
        // immediate, and pins are live at the stub.
        //
        // The load path needs no such thing: it is a bare LDR with no helper,
        // so a bad address is always a hardware fault.
        if (first_rip == kInvalidRip || mid.guest_rip < first_rip) {
            first_rip = mid.guest_rip;
        }
        if (last_rip == kInvalidRip || mid.guest_rip > last_rip) {
            last_rip = mid.guest_rip;
        }
        if (j > last_fault_index) last_fault_index = j;
    }
    if (first_rip == kInvalidRip) return refuse(RecoveryDecline::Structural);

    // A may-fault op in the KILLER's own instruction is the opposite case, and
    // it is the single largest reason this producer used to refuse (35 of 57
    // on Solitaire). Such an op runs BEFORE the killer's `DefFlags` — the
    // frontend emits the access, then the ALU, then the record — so at a fault
    // there the killer has defined nothing and the architectural flags are
    // still this definition's. That interval therefore MUST be covered rather
    // than refused: refusing it was not conservative, it was just smaller.
    //
    // The guard is for a may-fault op AFTER the killer's record, which would
    // fault with the killer's own flags already committed and must not be
    // handed this definition's. Unreachable while Tier 1 has no
    // read-modify-write, and checked rather than assumed.
    if (killer_rip != kInvalidRip) {
        for (u32 j = killer_index + 1; j < block.insts.size(); ++j) {
            const Inst& after = block.insts[j];
            if (after.guest_rip != killer_rip) break;
            if (after.may_fault) {
                return refuse(RecoveryDecline::FaultInsideEndpoint);
            }
        }
        if (last_rip == kInvalidRip || killer_rip > last_rip) {
            // Only extend when the killer's instruction actually contains a
            // covered fault; otherwise the range would claim intervals with
            // nothing to recover.
            for (u32 j = def_index + 1; j < killer_index; ++j) {
                if (block.insts[j].may_fault &&
                    block.insts[j].guest_rip == killer_rip) {
                    last_rip = killer_rip;
                    break;
                }
            }
        }
    }

    // flag_src is x86's SOURCE operand; flag_dst is the RESULT.
    FlagRecoveryPlan built{};
    built.flag_op = static_cast<u8>(def.flags.record);
    built.width = (record_is_64bit(def.flags.record) ||
                   def.flags.record == FlagState::FlagOp::Inc64 ||
                   def.flags.record == FlagState::FlagOp::Dec64) ? 64 : 32;
    built.def_rip = def_rip;
    built.first_rip = first_rip;
    built.last_rip = last_rip;
    if (merge) {
        // The handler derives the original from the result and reads the
        // inherited CF from CpuState; no source register is involved.
        built.merge_cf = true;
        built.src_is_imm = true;
        built.src_imm = 0;
        if (!merge_carry_source_live(block, def_index, last_fault_index)) {
            return refuse(RecoveryDecline::CarryMerge);
        }
    } else if (!resolve_operand(block, def.operands[0], def_index,
                                built.src_is_imm, built.src_reg,
                                built.src_imm)) {
        return refuse(RecoveryDecline::OperandNotRecoverable);
    }

    // Where does the result live at fault time? If the instruction wrote it to
    // a guest register, that register's pin holds it. If nothing wrote it —
    // CMP and TEST — then both x86 inputs are still live precisely BECAUSE
    // neither was written, and the handler can redo the operation.
    const Operand& dst_op = def.operands[1];
    u32 result_write = kNoDef;
    if (dst_op.kind == OperandKind::Value) {
        for (u32 j = def_index + 1; j < killer_index; ++j) {
            const Inst& in = block.insts[j];
            if (in.op == Op::GprWrite && in.operand_count >= 2 &&
                in.operands[1].kind == OperandKind::Value &&
                in.operands[1].as_value() == dst_op.as_value()) {
                result_write = j;
                break;
            }
        }
    }
    if (result_write != kNoDef) {
        built.recompute_dst = false;
        built.dst_is_imm = false;
        built.dst_reg = block.insts[result_write].operands[0].reg_index;
        // The handler reads the register at the record's width; a narrow
        // write would hold the result in a field, not in bits width-1:0.
        const Type wt = block.insts[result_write].operands[0].type;
        if (merge && wt != (built.width == 64 ? Type::I64 : Type::I32)) {
            return refuse(RecoveryDecline::OperandNotRecoverable);
        }
    } else if (merge) {
        // INC/DEC always write their result; a merge whose result reaches no
        // register before the faults has nothing to rebuild from.
        return refuse(RecoveryDecline::OperandNotRecoverable);
    } else {
        // Recompute needs the FIRST x86 operand, which lives on the ALU op
        // that produced the result.
        if (dst_op.kind != OperandKind::Value) {
            return refuse(RecoveryDecline::OperandNotRecoverable);
        }
        const u32 alu = defining_op(block, dst_op.as_value(), def_index);
        if (alu == kNoDef) return refuse(RecoveryDecline::OperandNotRecoverable);
        const Inst& a = block.insts[alu];
        switch (a.op) {
        case Op::Add: case Op::Sub: case Op::And:
        case Op::Or:  case Op::Xor:
            break;
        default:
            return refuse(RecoveryDecline::OperandNotRecoverable);
        }
        if (a.operand_count < 2) {
            return refuse(RecoveryDecline::OperandNotRecoverable);
        }
        if (!resolve_operand(block, a.operands[0], alu, built.dst_is_imm,
                             built.dst_reg, built.dst_imm)) {
            return refuse(RecoveryDecline::OperandNotRecoverable);
        }
        built.recompute_dst = true;
    }

    // Every named guest register must still hold the same value at every
    // covered instruction. The definition's OWN write of the result is what
    // establishes `dst_reg` in DirectDst mode, so it is not a violation; any
    // other write to a named register is.
    // Clobber analysis is by OP INDEX, not by guest RIP.
    //
    // A write that happens after every fault this plan covers is harmless —
    // the fault already happened, and the register still held the right value
    // when it did. Comparing RIPs instead would refuse the whole
    // `cmp eax,ecx ; mov [edx],ebx ; add eax,[mem]` shape, because the killer
    // writes EAX; but that write is emitted after the killer's load, which is
    // the fault site, so it cannot have run.
    //
    // `result_write` is exempt for the opposite reason: in DirectDst mode it
    // is what PUTS the result in the pin, so it must precede the faults. That
    // exemption belongs to the RESULT register. For the source it holds only
    // when the record's source feeds no flag -- AND/OR/XOR derive every flag
    // from the result -- because `add eax, eax` leaves the one register
    // holding the result, and a record rebuilt from it loses CF/OF/AF.
    // A narrow write names its register through `architectural_gpr` (`mov
    // ah, 1` is index 4 and clobbers RAX), and a `Divide` helper writes the
    // accumulator pair without any `GprWrite`, so it counts as writing all.
    const auto written_before_a_covered_fault = [&](u8 reg, bool exempt_result) {
        for (u32 j = def_index + 1; j < last_fault_index; ++j) {
            if (exempt_result && j == result_write) continue;
            const Inst& in = block.insts[j];
            if (in.op == Op::Divide) return true;
            if (in.op != Op::GprWrite) continue;
            if (architectural_gpr(in.operands[0].type,
                                  in.operands[0].reg_index) == reg) return true;
        }
        return false;
    };
    const bool source_feeds_no_flag =
        def.flags.record == FlagState::FlagOp::And32 ||
        def.flags.record == FlagState::FlagOp::Or32 ||
        def.flags.record == FlagState::FlagOp::Xor32 ||
        def.flags.record == FlagState::FlagOp::And64 ||
        def.flags.record == FlagState::FlagOp::Or64 ||
        def.flags.record == FlagState::FlagOp::Xor64;
    if (!built.src_is_imm && !merge &&
        written_before_a_covered_fault(built.src_reg, source_feeds_no_flag)) {
        return refuse(RecoveryDecline::OperandClobbered);
    }
    if (!built.dst_is_imm && written_before_a_covered_fault(built.dst_reg, true)) {
        return refuse(RecoveryDecline::OperandClobbered);
    }

    // The handler reads the interrupted register file, so every named guest
    // register must be pinned in this block (`IrBlock::recovery_gprs`).
    const auto pinned = [&](u8 reg) {
        return reg < 16 && ((block.recovery_gprs >> reg) & 1u) != 0;
    };
    if (!built.src_is_imm && !pinned(built.src_reg)) {
        return refuse(RecoveryDecline::OperandNotRecoverable);
    }
    if (!built.dst_is_imm && !pinned(built.dst_reg)) {
        return refuse(RecoveryDecline::OperandNotRecoverable);
    }

    plan = built;
    return true;
}

u32 eliminate_dead_flags(IrBlock& block, PassStats& stats,
                         bool recovery_available) {
    const u32 n = static_cast<u32>(block.insts.size());
    u32 killed = 0;

    for (u32 i = 0; i < n; ++i) {
        Inst& def = block.insts[i];
        if (def.op != Op::DefFlags) continue;
        if (def.flags.repr == FlagRepr::Dead) continue;  // already killed

        // The killer: the next definition of the architectural flags. Without
        // one this definition is live-out of the block — a successor may read
        // it, and this IR has no cross-block flag liveness yet.
        u32 killer = n;
        for (u32 j = i + 1; j < n; ++j) {
            // A by-CL shift publishes its record only when the masked count
            // is NONZERO -- the lowering skips it at runtime otherwise, per
            // Intel SDM Vol. 2 -- so it is a CONDITIONAL writer and cannot
            // kill anything. Treating it as a killer eliminated the previous
            // definition, and at CL==0 the flags then came from stale RFLAGS.
            const Inst& candidate = block.insts[j];
            if (candidate.op == Op::DefFlags && candidate.operand_count == 3 &&
                candidate.operands[2].kind == OperandKind::Value &&
                candidate.operands[2].type == Type::I8) {
                killer = n;
                break;
            }
            if (candidate.op == Op::DefFlags) { killer = j; break; }
        }
        if (killer == n) { ++stats.flags_live_out; continue; }

        // Between definition and killer, two things keep it alive.
        //
        // The scan is INCLUSIVE of the killer, and that is not a detail. An
        // ordinary `DefFlags` never reads the definition it overwrites, so a
        // strictly-exclusive scan was correct right up until CF-preserving
        // records arrived: `INC` overwrites the flags AND inherits their CF,
        // so the killer is itself a use. With an exclusive scan,
        // `ADD; INC` marked the ADD's record dead and the INC then merged a
        // carry that was never published.
        //
        // Found by reading the printed IR rather than by a failing test, which
        // is the argument for the printer being deterministic.
        bool observable = false;
        bool fault_blocked = false;
        bool used = false;
        // A fault does not end the scan: a use AFTER it (`cmp ; adc [m], r`
        // reads CF past its own load) must still be seen, or recovery would
        // kill a record that is read.
        for (u32 j = i + 1; j <= killer && !used; ++j) {
            const Inst& mid = block.insts[j];

            // 1. A real USE of the value. This is the "demand" in flag-demand
            //    analysis, and it is why the pass composes: today no op in
            //    this IR consumes a Flags value, so this never fires. The
            //    moment M3 adds Jcc / ADC / SETcc / CMOV, they show up as
            //    ordinary operands and this loop keeps their producer alive
            //    without anyone editing this pass. That is the difference
            //    between a dataflow analysis and the opcode-name-based
            //    `classify()` scan it replaces.
            for (u32 k = 0; k < mid.operand_count; ++k) {
                if (mid.operands[k].kind == OperandKind::Value &&
                    mid.operands[k].as_value() == def.result) {
                    observable = true;
                    used = true;
                    break;
                }
            }
            if (used) break;

            // 2. A FAULT, strictly BEFORE the killer. A faulting op at the
            //    killer's own index is a different question — the killer has
            //    not redefined anything yet at that point, so the definition
            //    is still architecturally live and must have been published.
            //    Handled by the `j < killer` guard here rather than by the
            //    loop bound, which the merge edge above needs inclusive.
            //
            //    The signal handler reconstructs architectural state from the
            //    lazy record, so a fault here would read whatever the elided
            //    store left behind — the pre-definition flags.
            //
            //    This single line is the correctness hole in the stencil
            //    equivalent: `compute_omit_lazy_flags` crosses
            //    `Neutral`-classified instructions, and `classify()` calls
            //    `MOV [mem], eax` Neutral, so `CMP eax,imm ; MOV [mem],eax ;
            //    ADD eax,1` omits the CMP's record and a fault on the MOV
            //    exposes pre-CMP RFLAGS to the exception record,
            //    `GetThreadContext` or `PUSHF`.
            if (j < killer && mid.may_fault) {
                observable = true;
                fault_blocked = true;
            }
        }
        bool recovered = false;
        if (observable) {
            // A definition blocked ONLY by faults is the population recovery
            // metadata exists for: if the record can be rebuilt at every
            // covered instruction, the fault stops being an observation and
            // the definition is dead after all.
            FlagRecoveryPlan plan{};
            RecoveryDecline why = RecoveryDecline::None;
            if (fault_blocked && !used && recovery_available &&
                try_plan_flag_recovery(block, i, killer, plan, &why)) {
                block.flag_recovery.push_back(plan);
                ++stats.flags_killed_by_recovery;
                recovered = true;
                if (plan.merge_cf) {
                    // The plan reads the inherited carry from the record the
                    // merge names: that record must stay where it is.
                    const u32 src = defining_op(
                        block, def.operands[2].as_value(), i);
                    if (src != kNoDef &&
                        block.insts[src].op == Op::DefFlags) {
                        block.insts[src].flags.record_pinned = true;
                    }
                }
            } else {
                if (fault_blocked && !used) {
                    ++stats.flags_kept_by_fault;
                    ++stats.recovery_declined;
                    if (static_cast<usize>(why) <
                        static_cast<usize>(RecoveryDecline::_Count)) {
                        ++stats.recovery_decline_reason[
                            static_cast<usize>(why)];
                    }
                } else {
                    ++stats.flags_kept_by_use;
                }
                continue;
            }
        }
        (void)recovered;

        // Kill it. A dead definition must claim no home at all: the verifier
        // rejects `Dead` carrying an NZCV kind or a record, and a leftover
        // `record` would tell the backend to publish the very store this pass
        // just removed.
        //
        // `keep_overlay` stays false. That is not an oversight, and it is not
        // a general claim either — it is the same distinction
        // `flag_liveness.cpp` draws between marking an omission 1 and marking
        // it 2.
        //
        // Publishing a record retires the CF/OF overlay: every published
        // record goes through `emit_lazy_flag_store_raw`, which ends in
        // `emit_clear_cf_overlay()` (jit_compiler.cpp:1944). So for a killed
        // definition whose KILLER publishes, nothing is owed — the killer's
        // own store performs the clear. That is the stencil path's "marked 1"
        // case, and it needs nothing extra either.
        //
        // "Marked 2" is a narrower shape: an adjacent ADC/SBB retires the
        // publisher's record but reads CF from PSTATE rather than from the
        // record, so it never clears the overlay, and a stale CLC/STC would
        // win over the real carry (`flag_liveness.cpp:344-347`). This IR has
        // no ADC/SBB, and more to the point the pass never kills the LAST
        // definition — so every block still ends with a published record.
        //
        // The load-bearing precondition is therefore the use-scan above, not
        // an absence of overlays: the overlay has THREE readers that consult
        // it before any record fast path (`emit_eval_lazy_condition`'s gate,
        // `emit_lazy_cf_to_call_tmp`, and `CpuState::try_lazy_condition`), so
        // "a live record implies no overlay" is false by design. Because the
        // scan keeps a definition alive whenever anything can observe it,
        // those readers can never see an elided one.
        //
        // CONTROL FLOW, and why the linear killer scan survived it.
        //
        // The hazard was real: with a branch between a definition and its
        // killer, the taken edge escapes the block with the elided record
        // still owed — which is precisely why the stencil path replays it from
        // `m_pending_stub_store`. A linear scan cannot see that edge.
        //
        // It cannot arise here because the verifier requires a TERMINATOR TO
        // BE THE LAST OP IN ITS BLOCK (`is_terminator`). Nothing can redefine
        // flags after a branch, so no definition is ever killed across one,
        // and every branch's flag consumer is caught by the use-scan above as
        // an ordinary operand.
        //
        // If a later milestone admits a NON-TERMINAL branch — an in-block
        // forward Jcc, which the stencil path does support — this reasoning
        // lapses and the scan has to become a CFG query. The verifier rule is
        // the thing holding it up; do not relax that rule without revisiting
        // this pass.
        def.flags.repr = FlagRepr::Dead;
        // A dead definition claims no home AT ALL — including the shift count,

        // which the verifier rejects on a record that is no longer a shift.

        def.flags.shift_count = 0;
        def.flags.record = FlagState::FlagOp::None;
        def.flags.nzcv = NzcvKind::None;
        // Drop the CF merge edge with the record it belonged to.
        //
        // The edge exists so a CF-PRESERVING record can name the flag value it
        // inherits its carry from. With the record gone there is nothing to
        // inherit into, and the verifier rightly rejects a merge edge on a
        // `None` record as claiming an inheritance that does not exist.
        //
        // Leaving it attached is what `inc eax ; add eax,ecx` did: the pass
        // killed the INC's dead record, the dangling edge failed verification,
        // and the whole block declined. Note the shape of that failure — the
        // verifier turned a pass bug into LOST COVERAGE, not a miscompiled
        // carry. That is the reason passes run before it.
        if (def.operand_count == 3) def.operand_count = 2;
        ++killed;
    }

    stats.flags_killed += killed;
    return killed;
}

bool record_owns_cf_and_of(FlagState::FlagOp record) noexcept {
    using F = FlagState::FlagOp;
    switch (record) {
    case F::Add8: case F::Add16: case F::Add32: case F::Add64:
    case F::Sub8: case F::Sub16: case F::Sub32: case F::Sub64:
    case F::And8: case F::And16: case F::And32: case F::And64:
    case F::Or8:  case F::Or16:  case F::Or32:  case F::Or64:
    case F::Xor8: case F::Xor16: case F::Xor32: case F::Xor64:
    case F::Neg32: case F::Neg64:
    case F::Shl64_1: case F::Shr64_1: case F::Sar64_1:
        return true;
    default:
        // Everything else either leaves CF or OF to the stored RFLAGS bit
        // (Shl64/Shr64/Sar64, Rol64/Ror64, Neg8/Neg16), depends on a runtime
        // count that may be zero (the 8/16/32-bit shift records), or inherits
        // CF (INC/DEC, ADC/SBB). None of them retires an overlay's bit.
        return false;
    }
}

namespace {

/// Pure integer value ops: no side effect, cannot fault, read no flags. Only
/// these are removed by the dead-code sweep that follows overlay removal.
[[nodiscard]] bool pure_value_op(Op op) noexcept {
    switch (op) {
    case Op::Const: case Op::GprRead:
    case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor:
    case Op::Not: case Op::Neg:
    case Op::Shl: case Op::Shr: case Op::Sar: case Op::Rol: case Op::Ror:
    case Op::Mul: case Op::SMulH:
    case Op::Trunc: case Op::Zext: case Op::Sext: case Op::Lea:
    case Op::AddCarry: case Op::SubBorrow:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool reads_flags_operand(const Inst& in) noexcept {
    for (u32 k = 0; k < in.operand_count; ++k) {
        if (in.operands[k].kind == OperandKind::Value &&
            in.operands[k].type == Type::Flags) {
            return true;
        }
    }
    return false;
}

}  // namespace

u32 eliminate_dead_overlays(IrBlock& block, PassStats& stats) {
    if (!block.is_64bit) return 0;
    const u32 n = static_cast<u32>(block.insts.size());
    std::vector<u8> drop(n, 0);
    u32 killed = 0;
    for (u32 i = 0; i < n; ++i) {
        const Inst& overlay = block.insts[i];
        if (overlay.op != Op::SetCf && overlay.op != Op::SetOf) continue;
        bool dead = false;
        // A killed full writer whose record a recovery plan rebuilds at the
        // faults it covers: the rebuild clears both overlays
        // (jit_rebuild_flag_record), exactly as publishing would have, so
        // such a fault does not observe this overlay either.
        u64 rebuilt_def = kInvalidRip;
        const auto fault_rebuilds_a_full_writer = [&](const Inst& fault) {
            if (rebuilt_def == kInvalidRip) return false;
            for (const FlagRecoveryPlan& plan : block.flag_recovery) {
                if (!plan.merge_cf && plan.covers(rebuilt_def, fault.guest_rip))
                    return true;
            }
            return false;
        };
        for (u32 j = i + 1; j < n; ++j) {
            const Inst& mid = block.insts[j];
            // Every flag reader names a Flags value -- EvalCc, CondExit, a
            // terminal Jcc, FlagsImage, a CF-merge edge -- and sees the
            // overlay through it even though the overlay defines no value.
            // A fault reconstructs RFLAGS from CpuState, overlay included.
            // POPF replaces RFLAGS without publishing a record.
            if (reads_flags_operand(mid) ||
                (mid.may_fault && !fault_rebuilds_a_full_writer(mid)) ||
                is_terminator(mid.op) || mid.op == Op::CondExit ||
                mid.op == Op::FlagsInstall || mid.op == Op::FlagsImage) {
                break;
            }
            if (mid.op != Op::DefFlags) continue;
            // A definition the demand pass killed emits nothing; its own
            // killer is further on.
            if (mid.flags.repr == FlagRepr::Dead) {
                rebuilt_def = mid.guest_rip;
                continue;
            }
            // The by-CL shift is a conditional writer (count 0 publishes
            // nothing) and a merge edge already stopped the scan above.
            dead = mid.operand_count == 2 && mid.flags.shift_count == 0 &&
                   record_owns_cf_and_of(mid.flags.record);
            break;
        }
        if (!dead) continue;
        drop[i] = 1;
        ++killed;
    }
    if (killed == 0) return 0;

    // Dead-code sweep. Uses always follow definitions in a straight-line
    // block, so one backward walk removes whole chains.
    std::vector<u32> uses(block.next_value, 0);
    for (u32 i = 0; i < n; ++i) {
        if (drop[i]) continue;
        const Inst& in = block.insts[i];
        for (u32 k = 0; k < in.operand_count; ++k) {
            if (in.operands[k].kind == OperandKind::Value &&
                in.operands[k].as_value() < uses.size()) {
                ++uses[in.operands[k].as_value()];
            }
        }
    }
    u32 swept = 0;
    for (u32 i = n; i-- > 0;) {
        if (drop[i]) continue;
        const Inst& in = block.insts[i];
        if (!pure_value_op(in.op) || in.may_fault || !in.defines_value() ||
            in.result >= uses.size() || uses[in.result] != 0) {
            continue;
        }
        drop[i] = 1;
        ++swept;
        for (u32 k = 0; k < in.operand_count; ++k) {
            if (in.operands[k].kind == OperandKind::Value &&
                in.operands[k].as_value() < uses.size()) {
                --uses[in.operands[k].as_value()];
            }
        }
    }
    usize output = 0;
    for (u32 i = 0; i < n; ++i) {
        if (drop[i]) continue;
        if (output != i) block.insts[output] = block.insts[i];
        ++output;
    }
    block.insts.resize(output);
    stats.overlays_killed += killed;
    stats.values_dce += swept;
    return killed;
}

u32 assign_nzcv_residency(IrBlock& block, PassStats& stats) {
    // Only a branch in this same block can read host PSTATE: the epilogue
    // clears the runtime NZCV kind byte, so nothing is published to a
    // successor. That makes the analysis local -- for each branch (the
    // terminal Jcc, or a long-mode CondExit), find the definition it reads
    // and check nothing between them destroys the flags.
    u32 claimed = 0;
    const u32 n = static_cast<u32>(block.insts.size());
    for (u32 at = 0; at < n; ++at) {
        const Inst& branch = block.insts[at];
        const bool terminal_jcc = branch.op == Op::Jcc && at + 1 == n;
        // A long-mode EvalCc (CMOVcc / SETcc) reads NZCV the same way: its
        // emitter takes a CSET or a CSEL straight off the claim.
        const bool eval_cc = branch.op == Op::EvalCc && block.is_64bit;
        // So does a long-mode CF merge (INC/DEC): the snapshot emitter
        // captures CF at the START of the merging instruction, with one CSET
        // when a claim is live there (emit_lazy_cf_to_call_tmp).
        const bool merge = block.is_64bit && branch.op == Op::DefFlags &&
            branch.operand_count == 3 &&
            branch.operands[2].kind == OperandKind::Value &&
            branch.operands[2].type == Type::Flags;
        if (!terminal_jcc && branch.op != Op::CondExit && !eval_cc && !merge)
            continue;
        if (!merge && (branch.operand_count != (eval_cc ? 2u : 1u) ||
                       branch.operands[0].kind != OperandKind::Value)) {
            continue;
        }
        const ValueId cond = merge ? branch.operands[2].as_value()
                                   : branch.operands[0].as_value();
        // The point the reader reads host flags.
        u32 read_at = at;
        if (merge) {
            while (read_at > 0 &&
                   block.insts[read_at - 1].guest_rip == branch.guest_rip)
                --read_at;
        }

        u32 def_at = at;
        for (u32 i = 0; i < at; ++i) {
            if (block.insts[i].op == Op::DefFlags &&
                block.insts[i].result == cond) {
                def_at = i;
                break;
            }
        }
        // A `FlagsIn` condition has no definition in this block, so there is
        // nothing to keep resident — the flags were computed before entry and
        // host PSTATE says nothing about them.
        if (def_at == at) continue;

        Inst& def = block.insts[def_at];
        // A definition the demand pass proved dead computes nothing to be
        // resident, and one already marked stays as it is.
        if (def.flags.repr != FlagRepr::InCpuStateRecord) continue;
        if (def.flags.nzcv != NzcvKind::None) continue;

        NzcvKind kind = nzcv_kind_for(def.flags.record);
        // Long mode: a narrow AND/TEST claims the full And kind, because the
        // snapshot emitter sets its flags with a width-shifted ANDS -- N is
        // the operand's own sign bit, Z its result, C and V clear, exactly
        // x86's SF/ZF/CF/OF for AND (Intel SDM Vol. 2A). A plain 32-bit ANDS
        // of the zero-extended bytes would leave N clear, which is why
        // nzcv_kind_for itself does not map these.
        if (kind == NzcvKind::None && block.is_64bit &&
            (def.flags.record == FlagState::FlagOp::And8 ||
             def.flags.record == FlagState::FlagOp::And16)) {
            kind = NzcvKind::And;
        }
        if (kind == NzcvKind::None) continue;

        bool clobbered = def_at >= read_at;
        for (u32 j = def_at + 1; j < read_at && !clobbered; ++j) {
            const Inst& mid = block.insts[j];
            // A long-mode scalar Store keeps a live claim: the snapshot
            // emitter emits it with the claim held, and every store
            // primitive then preserves NZCV itself (should_hold_nzcv: a cold
            // stub spill, or a bracket where the check is not NZCV-clean).
            // Loads have no such contract, and a 128-bit store is not
            // covered.
            if (block.is_64bit && mid.op == Op::Store &&
                mid.mem.width != Type::V128) {
                continue;
            }
            clobbered = clobbers_nzcv(mid);
        }
        if (clobbered) continue;

        // Two homes at once, which the lattice allows by design: the record
        // stays the architectural home — a fault or a successor still needs
        // it — and the kind is the additional fast-path fact the branch reads.
        def.flags.nzcv = kind;
        ++stats.nzcv_resident;
        ++claimed;
    }
    return claimed;
}

namespace {

/// Ops the snapshot emitter lowers without touching host NZCV, a helper, a
/// fault or the lazy record -- the ones its "keep the claim" list names --
/// plus a fused EvalCc. Only these may sit inside a PSTATE-only window.
[[nodiscard]] bool pstate_window_op(Op op) noexcept {
    switch (op) {
    case Op::Const: case Op::GprRead: case Op::GprWrite:
    case Op::Lea: case Op::SegBase:
    case Op::Trunc: case Op::Zext: case Op::Sext:
    case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor:
    case Op::Not: case Op::Neg:
    case Op::Shl: case Op::Shr: case Op::Sar: case Op::Rol: case Op::Ror:
    case Op::Mul: case Op::SMulH:
    case Op::AddCarry: case Op::SubBorrow:
    case Op::EvalCc:
        return true;
    default:
        return false;
    }
}

/// Ops whose lowering may itself claim (and so replace) host NZCV.
[[nodiscard]] bool may_claim_nzcv(Op op) noexcept {
    return op == Op::Add || op == Op::Sub || op == Op::And ||
           op == Op::AddCarry || op == Op::SubBorrow;
}

}  // namespace

u32 keep_carry_in_pstate(IrBlock& block, PassStats& stats) {
    if (!block.is_64bit) return 0;
    const u32 n = static_cast<u32>(block.insts.size());
    std::vector<u32> def_index(block.next_value, 0xFFFFFFFFu);
    for (u32 i = 0; i < n; ++i) {
        const Inst& in = block.insts[i];
        if (in.defines_value() && in.result < def_index.size())
            def_index[in.result] = i;
    }
    const auto defined_at = [&](const Operand& o) -> u32 {
        if (o.kind != OperandKind::Value || o.as_value() >= def_index.size())
            return 0xFFFFFFFFu;
        return def_index[o.as_value()];
    };

    u32 demoted = 0;
    for (u32 i = 0; i < n; ++i) {
        Inst& def = block.insts[i];
        if (def.op != Op::DefFlags || def.operand_count != 2) continue;
        if (def.flags.repr != FlagRepr::InCpuStateRecord) continue;
        if (def.flags.shift_count != 0 || def.flags.overlay_live ||
            def.flags.keep_overlay || def.flags.record_pinned) continue;
        const NzcvKind kind = def.flags.nzcv;
        if (kind != NzcvKind::Add && kind != NzcvKind::Sub) continue;

        // The producer whose S-form leaves `kind`: the record's result
        // operand, through ADC/SBB's widening for the record.
        u32 p = defined_at(def.operands[1]);
        if (p == 0xFFFFFFFFu) continue;
        if (block.insts[p].op == Op::Zext) {
            const u32 inner = defined_at(block.insts[p].operands[0]);
            if (inner == 0xFFFFFFFFu) continue;
            const Op op = block.insts[inner].op;
            if (op != Op::AddCarry && op != Op::SubBorrow) continue;
            p = inner;
        }
        const Inst& producer = block.insts[p];
        const bool adds = producer.op == Op::Add || producer.op == Op::AddCarry;
        const bool subs = producer.op == Op::Sub || producer.op == Op::SubBorrow;
        if (!(kind == NzcvKind::Add ? adds : subs)) continue;
        if (producer.type != Type::I32 && producer.type != Type::I64) continue;
        // Producer to definition: nothing that could take the claim away.
        bool ok = true;
        for (u32 j = p + 1; j < i && ok; ++j) {
            const Inst& mid = block.insts[j];
            ok = !mid.may_fault && pstate_window_op(mid.op) &&
                 !may_claim_nzcv(mid.op) && !clobbers_nzcv(mid);
        }
        if (!ok) continue;

        // Definition to killer: every reader is a carry condition taken while
        // the claim provably holds, and nothing observes the record.
        u32 killer = n;
        bool claim_may_change = false;
        for (u32 j = i + 1; j < n && ok; ++j) {
            const Inst& mid = block.insts[j];
            bool reads = false;
            for (u32 k = 0; k < mid.operand_count; ++k) {
                if (mid.operands[k].kind == OperandKind::Value &&
                    mid.operands[k].as_value() == def.result) reads = true;
            }
            if (mid.op == Op::DefFlags) {
                // A CF merge or a by-CL shift reads or may keep this one.
                if (reads || mid.operand_count != 2 || mid.flags.shift_count != 0) {
                    ok = false;
                    break;
                }
                killer = j;
                break;
            }
            // ADC/SBB clobber NZCV only after they read their carry, and
            // `claim_may_change` stops any later read.
            // This definition's own condition reads are CSETs off the claim.
            const bool carry_op = mid.op == Op::AddCarry || mid.op == Op::SubBorrow;
            const bool own_cc = mid.op == Op::EvalCc && reads;
            // CMOVcc/SETcc's Select on one of those conditions, in the same
            // guest instruction: the snapshot emitter CSELs on the claim
            // itself (fused_select_cc), which leaves NZCV alone.
            bool own_select = false;
            if (mid.op == Op::Select) {
                const u32 c = defined_at(mid.operands[0]);
                own_select = !claim_may_change && c != 0xFFFFFFFFu && c > i &&
                    block.insts[c].op == Op::EvalCc &&
                    block.insts[c].guest_rip == mid.guest_rip &&
                    defined_at(block.insts[c].operands[0]) == i;
            }
            if (mid.may_fault || (!pstate_window_op(mid.op) && !own_select) ||
                (clobbers_nzcv(mid) && !carry_op && !own_cc && !own_select)) {
                ok = false;
                break;
            }
            // Any other condition read here would be a lazy evaluation,
            // which calls a helper.
            if (mid.op == Op::EvalCc && !reads) {
                ok = false;
                break;
            }
            if (reads) {
                ok = mid.op == Op::EvalCc && !claim_may_change &&
                     mid.operand_count == 2 &&
                     (mid.operands[1].payload == 0x2 || mid.operands[1].payload == 0x3);
            }
            if (may_claim_nzcv(mid.op)) claim_may_change = true;
        }
        if (!ok || killer == n) continue;
        // Flags values are SSA, but nothing may name this one past its killer.
        for (u32 j = killer + 1; j < n && ok; ++j) {
            const Inst& later = block.insts[j];
            for (u32 k = 0; k < later.operand_count; ++k) {
                if (later.operands[k].kind == OperandKind::Value &&
                    later.operands[k].as_value() == def.result) ok = false;
            }
        }
        if (!ok) continue;

        def.flags.repr = FlagRepr::InPstate;
        def.flags.record = FlagState::FlagOp::None;
        def.operands[0] = Operand::value(producer.result, producer.type);
        def.operands[1] = Operand{};
        def.operand_count = 1;
        ++demoted;
    }
    // A dead ADC/SBB definition publishes nothing, but its operands still
    // name the packed carry and widened result built only for the record.
    // Naming the carry op's own result instead keeps it well-formed and lets
    // the sweep drop that work, and with it the CF materialization.
    u32 retargeted = 0;
    for (Inst& def : block.insts) {
        if (def.op != Op::DefFlags || def.flags.repr != FlagRepr::Dead ||
            def.operand_count != 2) continue;
        const u32 w = defined_at(def.operands[1]);
        if (w == 0xFFFFFFFFu || block.insts[w].op != Op::Zext) continue;
        const u32 c = defined_at(block.insts[w].operands[0]);
        if (c == 0xFFFFFFFFu) continue;
        const Inst& carry = block.insts[c];
        if (carry.op != Op::AddCarry && carry.op != Op::SubBorrow) continue;
        def.operands[0] = Operand::value(carry.result, carry.type);
        def.operands[1] = Operand::value(carry.result, carry.type);
        ++retargeted;
    }
    // Any other dead definition: its operands are equally unread, but they
    // keep their values live to it -- SHLD's original destination, through
    // the pool and into a spill slot. Name one operand twice, preferring one
    // something else reads, so the other's chain can go.
    {
        std::vector<u32> live_uses(block.next_value, 0);
        for (const Inst& in : block.insts) {
            if (in.op == Op::DefFlags && in.flags.repr == FlagRepr::Dead) continue;
            for (u32 k = 0; k < in.operand_count; ++k) {
                if (in.operands[k].kind == OperandKind::Value &&
                    in.operands[k].as_value() < live_uses.size()) {
                    ++live_uses[in.operands[k].as_value()];
                }
            }
        }
        for (Inst& def : block.insts) {
            if (def.op != Op::DefFlags || def.flags.repr != FlagRepr::Dead ||
                def.operand_count != 2 ||
                def.operands[0].kind != OperandKind::Value ||
                def.operands[1].kind != OperandKind::Value ||
                def.operands[0].as_value() == def.operands[1].as_value()) continue;
            const ValueId a = def.operands[0].as_value();
            const ValueId b = def.operands[1].as_value();
            const bool keep_a = b >= live_uses.size() || (live_uses[b] == 0 &&
                a < live_uses.size() && live_uses[a] != 0);
            def.operands[keep_a ? 1 : 0] = def.operands[keep_a ? 0 : 1];
            ++retargeted;
        }
    }

    // The record's operands (ADC/SBB's packed carry and widened result, a
    // dead definition's other source) are now unread, and the frontend can
    // leave a value nothing reads (SHLD's sign change).
    std::vector<u32> uses(block.next_value, 0);
    for (const Inst& in : block.insts) {
        for (u32 k = 0; k < in.operand_count; ++k) {
            if (in.operands[k].kind == OperandKind::Value &&
                in.operands[k].as_value() < uses.size()) {
                ++uses[in.operands[k].as_value()];
            }
        }
    }
    std::vector<u8> drop(n, 0);
    u32 swept = 0;
    for (u32 i = n; i-- > 0;) {
        const Inst& in = block.insts[i];
        if (!pure_value_op(in.op) || in.may_fault || !in.defines_value() ||
            in.result >= uses.size() || uses[in.result] != 0) {
            continue;
        }
        drop[i] = 1;
        ++swept;
        for (u32 k = 0; k < in.operand_count; ++k) {
            if (in.operands[k].kind == OperandKind::Value &&
                in.operands[k].as_value() < uses.size()) {
                --uses[in.operands[k].as_value()];
            }
        }
    }
    if (swept != 0) {
        usize output = 0;
        for (u32 i = 0; i < n; ++i) {
            if (drop[i]) continue;
            if (output != i) block.insts[output] = block.insts[i];
            ++output;
        }
        block.insts.resize(output);
    }
    stats.flags_pstate_only += demoted;
    stats.values_dce += swept;
    return demoted;
}

void measure_cross_instruction_opportunity(const IrBlock& block,
                                           PassStats& stats) noexcept {
    const u32 n = static_cast<u32>(block.insts.size());

    // Which guest instruction defined each value.
    std::vector<u64> owner(block.next_value, kInvalidRip);
    for (u32 i = 0; i < n; ++i) {
        const Inst& in = block.insts[i];
        if (in.type != Type::Void) owner[in.result] = in.guest_rip;
    }

    for (u32 i = 0; i < n; ++i) {
        const Inst& in = block.insts[i];
        for (u8 k = 0; k < in.operand_count; ++k) {
            const Operand& op = in.operands[k];
            if (op.kind != OperandKind::Value) continue;
            // A Flags value's home is the lazy record in CpuState, which is
            // architectural and outlives the instruction by design — it is not
            // something a register allocator would hold.
            if (op.type == Type::Flags) continue;
            const ValueId v = op.as_value();
            if (v < owner.size() && owner[v] != in.guest_rip) {
                ++stats.cross_instruction_values;
            }
        }
    }

    // Equivalent `Lea` nodes: same ADDRESS, not the same ValueIds.
    //
    // Comparing ValueIds is the wrong test and reports zero on every workload.
    // Each guest instruction emits its own `GprRead`, so two LEAs for
    // `[ebp-8]` in consecutive instructions reference DIFFERENT values that
    // hold the same thing. The comparison has to resolve each operand through
    // its definition to a guest register or a literal.
    //
    // A pair only counts if nothing writes any guest register they name in
    // between — otherwise the two LEAs compute different addresses and folding
    // them would be wrong, so counting them would overstate the opportunity.
    struct Canon { bool is_reg; u8 reg; u64 imm; bool ok; };
    const auto canon = [&](const Operand& op, u32 before) -> Canon {
        if (op.kind == OperandKind::None) return {false, 0, 0, true};
        if (op.kind == OperandKind::Imm) return {false, 0, op.payload, true};
        if (op.kind == OperandKind::GuestReg) {
            return {true, op.reg_index, 0, true};
        }
        if (op.kind != OperandKind::Value) return {false, 0, 0, false};
        const u32 d = defining_op(block, op.as_value(), before);
        if (d == kNoDef) return {false, 0, 0, false};
        const Inst& def = block.insts[d];
        if (def.op == Op::GprRead) return {true, def.operands[0].reg_index, 0, true};
        if (def.op == Op::Const)   return {false, 0, def.operands[0].payload, true};
        return {false, 0, 0, false};
    };
    const auto same_lea = [&](u32 ia, u32 ib) {
        const Inst& a = block.insts[ia];
        const Inst& b = block.insts[ib];
        if (a.operand_count != b.operand_count) return false;
        u8 named[4]{}; u8 named_count = 0;
        for (u8 k = 0; k < a.operand_count; ++k) {
            const Canon ca = canon(a.operands[k], ia);
            const Canon cb = canon(b.operands[k], ib);
            if (!ca.ok || !cb.ok) return false;
            if (ca.is_reg != cb.is_reg) return false;
            if (ca.is_reg) {
                if (ca.reg != cb.reg) return false;
                if (named_count < 4) named[named_count++] = ca.reg;
            } else if (ca.imm != cb.imm) {
                return false;
            }
        }
        // No write to a named guest register between the two.
        for (u32 j = ib + 1; j < ia; ++j) {
            const Inst& mid = block.insts[j];
            if (mid.op != Op::GprWrite) continue;
            for (u8 k = 0; k < named_count; ++k) {
                if (mid.operands[0].reg_index == named[k]) return false;
            }
        }
        return true;
    };
    for (u32 i = 0; i < n; ++i) {
        if (block.insts[i].op != Op::Lea) continue;
        ++stats.total_leas;
        for (u32 j = 0; j < i; ++j) {
            if (block.insts[j].op != Op::Lea) continue;
            if (!same_lea(i, j)) continue;
            ++stats.redundant_leas;
            if (block.insts[i].guest_rip != block.insts[j].guest_rip) {
                ++stats.redundant_leas_cross_insn;
            }
            break;
        }
    }
}

// Spill slots are immutable SSA snapshots, reused only after their last use.
// Slot allocation is shared by feasibility and emission just like registers.
bool plan_spill_slots(const IrBlock& block, std::vector<u8>* assignment) {
    std::vector<usize> last(block.next_value, 0);
    for (usize i = 0; i < block.insts.size(); ++i) {
        const auto& in = block.insts[i];
        if (in.defines_value()) last[in.result] = i;
        for (u32 k = 0; k < in.operand_count; ++k)
            if (in.operands[k].kind == OperandKind::Value)
                last[in.operands[k].as_value()] = i;
    }
    ValueId held[SpillFrame::kSlotCount];
    for (auto& value : held) value = kInvalidValue;
    for (usize i = 0; i < block.insts.size(); ++i) {
        for (auto& value : held)
            if (value != kInvalidValue && last[value] < i) value = kInvalidValue;
        const auto& in = block.insts[i];
        if (!in.defines_value() || in.type == Type::Flags) continue;
        usize slot = 0;
        while (slot < SpillFrame::kSlotCount && held[slot] != kInvalidValue) ++slot;
        if (slot == SpillFrame::kSlotCount) return false;
        held[slot] = in.result;
        if (assignment) (*assignment)[in.result] = static_cast<u8>(slot);
    }
    return true;
}

static bool scalar_allocatable(const IrBlock& block, bool memory) noexcept {
    for (usize i = 0; i < block.insts.size(); ++i) {
        const auto& in = block.insts[i];
        if (in.may_fault && !memory) return false;
        switch (in.op) {
        case Op::Load:
            // A narrow load feeds a widening op; it never merges into a
            // partial register, so it needs no read-modify-write of the
            // destination. A V128 load fills a vector register out of the
            // vector pool and costs this allocator nothing.
            if (!memory) return false;
            if (in.mem.width != Type::I32 && in.mem.width != Type::I64 &&
                !(block.is_64bit && (in.mem.width == Type::I8 ||
                                     in.mem.width == Type::I16 ||
                                     in.mem.width == Type::V128))) return false;
            break;
        case Op::Store:
            if (!memory) return false;
            if (in.mem.width != Type::I32 && in.mem.width != Type::I64 &&
                !(block.is_64bit && (in.mem.width == Type::I8 ||
                                     in.mem.width == Type::I16 ||
                                     in.mem.width == Type::V128))) return false;
            break;
        case Op::Lea:
            if (!memory || (in.type != Type::I32 && in.type != Type::I64)) return false;
            break;
        case Op::SegBase:
            if (!memory || in.type != (block.is_64bit ? Type::I64 : Type::I32)) return false;
            break;
        case Op::Trunc:
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::Zext: case Op::Sext:
            // Widening a narrow SOURCE into a whole register. The narrow
            // partial WRITE that would merge back into one is not admitted.
            if (!memory) return false;
            if (in.type != Type::I32 && !(block.is_64bit &&
                (in.type == Type::I64 || in.type == Type::I16))) return false;
            if (in.operands[0].type != Type::I8 && in.operands[0].type != Type::I16 &&
                in.operands[0].type != Type::I32) return false;
            if (type_bits(in.operands[0].type) >= type_bits(in.type)) return false;
            break;
        case Op::GprRead:
            // A sub-register read lowers to an extract, so it is allocatable
            // at I8/I16 as well. Only long-mode blocks reach the narrow forms.
            if (in.type == Type::I8 || in.type == Type::I16) {
                if (!memory || !block.is_64bit) return false;
                break;
            }
            if (in.type != Type::I32 && !(block.is_64bit && in.type == Type::I64)) return false;
            break;
        case Op::Const:
        case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor:
        case Op::Not: case Op::Neg:
            // Long mode admits the narrow widths; the lazy record masks to
            // its own width and every consumer takes only the field.
            if (block.is_64bit && (in.type == Type::I8 || in.type == Type::I16)) {
                if (!memory) return false;
                break;
            }
            if (in.type != Type::I32 && !(block.is_64bit && in.type == Type::I64)) return false;
            break;
        case Op::GprWrite:
            // A narrow write is a bitfield merge into the architectural
            // register, which the shared primitive owns.
            if (block.is_64bit && (in.operands[0].type == Type::I8 ||
                                   in.operands[0].type == Type::I16)) {
                if (!memory) return false;
                break;
            }
            if (in.operands[0].type != Type::I32 &&
                !(block.is_64bit && in.operands[0].type == Type::I64)) return false;
            break;
        case Op::AddCarry: case Op::SubBorrow:
            if (!memory || !block.is_64bit) return false;
            if (in.type != Type::I8 && in.type != Type::I16 &&
                in.type != Type::I32) return false;
            break;
        case Op::DefFlags:
            // A PSTATE-resident definition names only its producer's result
            // (keep_carry_in_pstate) and emits nothing.
            if (in.flags.repr == FlagRepr::InPstate) {
                if (!memory || !block.is_64bit || in.operand_count != 1)
                    return false;
                break;
            }
            // A three-operand DefFlags is either a CF merge (INC/DEC) or a
            // by-CL shift carrying its runtime count; both are lowerable.
            if ((in.operand_count != 2 && !(memory && in.operand_count == 3 &&
                 (in.flags.record == FlagState::FlagOp::Inc32 ||
                  in.flags.record == FlagState::FlagOp::Dec32 ||
                  // Long mode only: the snapshot emitter publishes these
                  // with the X-width record (CF at bit 32, full result).
                  (block.is_64bit &&
                   (in.flags.record == FlagState::FlagOp::Inc64 ||
                    in.flags.record == FlagState::FlagOp::Dec64)) ||
                  in.flags.record == FlagState::FlagOp::Inc16 ||
                  in.flags.record == FlagState::FlagOp::Dec16 ||
                  in.flags.record == FlagState::FlagOp::Inc8 ||
                  in.flags.record == FlagState::FlagOp::Dec8 ||
                  in.operands[2].type == Type::I8))) ||
                in.flags.overlay_live || in.flags.keep_overlay) return false;
            // A shift record packs its count and needs the three-input
            // publisher, which takes exactly two IR operands.
            if (in.flags.shift_count != 0 && in.operand_count != 2) return false;
            break;
        case Op::EvalCc:
            if (!memory || !block.is_64bit || in.type != Type::I8) return false;
            break;
        case Op::CondExit:
            // Long-mode snapshot emitter only: it evaluates the condition
            // with the same helper-capable sequence as EvalCc.
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::Select:
            if (!memory || !block.is_64bit) return false;
            if (in.type != Type::I16 && in.type != Type::I32 &&
                in.type != Type::I64) return false;
            break;
        case Op::Divide:
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::SegSelector:
            if (!memory || !block.is_64bit) return false;
            if (in.type != Type::I64) return false;
            break;
        case Op::FlagsImage: case Op::FlagsInstall:
            // Helper calls that always return. They read and write the
            // architectural flags directly and need no pool register beyond
            // their own result.
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::SetCf: case Op::SetOf:
            // A partial flag writer: it forces one bit and deliberately does
            // NOT replace the pending record, so it is not a DefFlags.
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::Mul: case Op::SMulH:
            if (!memory || !block.is_64bit) return false;
            if (in.type != Type::I64) return false;
            break;
        case Op::XmmRead: case Op::XmmWrite:
        case Op::VXor: case Op::VAnd: case Op::VOr:
        case Op::VAdd: case Op::VSub: case Op::VCmpEq: case Op::VCmpGt:
        case Op::VMinU: case Op::VMaxU: case Op::VMinS: case Op::VMaxS:
        case Op::VMulLo: case Op::VAvgU: case Op::VZipLo: case Op::VZipHi:
        case Op::VQAddS: case Op::VQAddU: case Op::VQSubS: case Op::VQSubU:
        case Op::VAbs:
        case Op::VPackS: case Op::VPackU:
        case Op::VShlImm: case Op::VShrImm: case Op::VSarImm:
        case Op::VInsertLow32: case Op::VZext32: case Op::VLow32:
            // Vector values get their OWN register pool and never compete for
            // the GPR one, so they cost this allocator nothing. What they do
            // need is that no may-fault op sits between a definition and its
            // last use -- V0-V7 are caller-saved and the fault handler saves
            // no vector state at all -- and that is checked in
            // `ir_emit_feasible`, which is the pass that knows the emitter.
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::Rol: case Op::Ror:
            // Any integer width: 32 and 64 are one ARM64 ROR, and the narrow
            // ones double the field inside a 32-bit register.
            if (!memory || !block.is_64bit) return false;
            if (!is_integer_type(in.type)) return false;
            break;
        case Op::Shl: case Op::Shr: case Op::Sar:
            // Any integer width: the width limit is on the shift RECORD, not
            // on the arithmetic (ADC packs its carry-in with an I64 shift).
            if (!memory || !block.is_64bit) return false;
            break;
        case Op::FlagsIn: break;
        case Op::Jmp: case Op::Jcc: case Op::JmpIndirect:
            if (i + 1 != block.insts.size()) return false;
            break;
        default: return false;
        }
    }
    return true;
}

bool scalar_block_allocatable(const IrBlock& block) noexcept {
    return scalar_allocatable(block, false);
}

bool snapshot_block_allocatable(const IrBlock& block) noexcept {
    // Explicit clobber contract: RIP publication and memory helpers may
    // destroy X0-X4. Backing every scalar definition before the next op
    // preserves snapshots. Simple DefFlags clobbers X3/V31, while carry
    // merge helpers remain excluded. Branch helpers occur only at block end.
    // A failing access exits with architectural commits, never spill state.
    // Only the long-mode snapshot emitter publishes recovery plans; a 32-bit
    // block with one stays on the scalar emitter, which does.
    return (block.is_64bit || block.flag_recovery.empty()) &&
           scalar_allocatable(block, true);
}

bool requires_snapshot_backing(const IrBlock& block) noexcept {
    if (!snapshot_block_allocatable(block)) return false;
    std::vector<u64> rip(block.next_value, kInvalidRip);
    std::vector<u8> architectural(block.next_value, 0);
    for (const auto& in : block.insts) {
        if (in.defines_value()) {
            rip[in.result] = in.guest_rip;
            architectural[in.result] = in.op == Op::GprRead;
        }
        if (in.op == Op::GprWrite) architectural[in.operands[1].as_value()] = 1;
    }
    for (const auto& in : block.insts)
        for (u32 k = 0; k < in.operand_count; ++k) {
            const auto& operand = in.operands[k];
            if (operand.kind == OperandKind::Value && operand.type != Type::Flags &&
                !architectural[operand.as_value()] && rip[operand.as_value()] != in.guest_rip)
                return true;
        }
    return false;
}

u32 common_scalar_constants(IrBlock& block, PassStats& stats) {
    if (!scalar_block_allocatable(block) && !snapshot_block_allocatable(block)) return 0;
    std::vector<u8> eligible(block.next_value, 1);
    for (const auto& in : block.insts) {
        // Constants committed by MOV have an architectural home whose later
        // overwrite invalidates residency. Keep those definitions distinct.
        const bool scalar_use = in.op == Op::Add || in.op == Op::Sub ||
            in.op == Op::And || in.op == Op::Or || in.op == Op::Xor ||
            in.op == Op::DefFlags;
        if (scalar_use) continue;
        for (u32 k = 0; k < in.operand_count; ++k) {
            const auto& operand = in.operands[k];
            if (operand.kind == OperandKind::Value && operand.as_value() < eligible.size())
                eligible[operand.as_value()] = 0;
        }
    }
    std::vector<ValueId> alias(block.next_value, kInvalidValue);
    struct Constant { u64 bits; Type type; ValueId value; };
    std::vector<Constant> constants;
    usize output = 0;
    u32 removed = 0;
    // Rewrite in place and compact: `for (auto in : ...)` copied every
    // instruction by value, and an `Inst` is large enough that doing so twice
    // per compile (this pass and the read-reuse pass) was a measurable share
    // of long-mode compile time. The output index never exceeds the input
    // index, so moving down is safe.
    //
    // Sharing stops at the guest instruction boundary. A constant is one or
    // two MOVs to recreate, but one shared ACROSS instructions is a
    // block-long interval in a three-register pool: two of them overflow the
    // pool plan, and the snapshot emitter's fallback then spills every value
    // in the block. A cross-instruction use also makes the block
    // `requires_snapshot_backing`, moving it off the scalar emitter.
    const usize count = block.insts.size();
    u64 constants_rip = kInvalidRip;
    for (usize i = 0; i < count; ++i) {
        Inst& in = block.insts[i];
        if (in.guest_rip != constants_rip) {
            constants_rip = in.guest_rip;
            constants.clear();
        }
        for (u32 k = 0; k < in.operand_count; ++k) {
            auto& operand = in.operands[k];
            if (operand.kind == OperandKind::Value && operand.as_value() < alias.size() &&
                alias[operand.as_value()] != kInvalidValue)
                operand = Operand::value(alias[operand.as_value()], operand.type);
        }
        if (in.op == Op::Const && (in.type == Type::I32 || in.type == Type::I64) && eligible[in.result]) {
            ValueId prior = kInvalidValue;
            for (const auto& [bits, type, value] : constants)
                if (bits == in.operands[0].payload && type == in.type) { prior = value; break; }
            if (prior != kInvalidValue) {
                alias[in.result] = prior;
                ++removed;
                continue;
            }
            constants.push_back({in.operands[0].payload, in.type, in.result});
        }
        if (output != i) block.insts[output] = in;
        ++output;
    }
    block.insts.resize(output);
    stats.scalar_constants_reused += removed;
    return removed;
}

u32 reuse_register_reads(IrBlock& block, PassStats& stats) {
    if (block.is_64bit) {
        if (!scalar_block_allocatable(block) && !snapshot_block_allocatable(block)) return 0;
        ValueId current[16][2];
        for (auto& reg : current) for (auto& value : reg) value = kInvalidValue;
        std::vector<ValueId> alias(block.next_value, kInvalidValue);
        usize output = 0;
        u32 removed = 0;
        const usize count = block.insts.size();
        for (usize i = 0; i < count; ++i) {
            Inst& in = block.insts[i];
            for (u32 k = 0; k < in.operand_count; ++k) {
                auto& operand = in.operands[k];
                if (operand.kind == OperandKind::Value && alias[operand.as_value()] != kInvalidValue)
                    operand = Operand::value(alias[operand.as_value()], operand.type);
            }
            if (in.op == Op::GprRead) {
                // Only FULL-width reads are reusable. A sub-register read is an
                // extract of a different field, and the two-slot cache below
                // has one bit of width, so admitting I8/I16 here would alias
                // `movzx ecx, al` to a later `mov edx, eax`.
                if (in.type != Type::I32 && in.type != Type::I64) {
                    if (output != i) block.insts[output] = in;
                    ++output;
                    continue;
                }
                auto& prior = current[in.operands[0].reg_index][in.type == Type::I64];
                if (prior != kInvalidValue) {
                    alias[in.result] = prior;
                    ++removed;
                    continue;
                }
                prior = in.result;
            }
            if (in.op == Op::GprWrite) {
                // Intel SDM Vol. 1 3.4.1.1: writing r32 also changes r64, and a
                // narrow write names its register through `architectural_gpr`.
                auto& reg = current[architectural_gpr(in.operands[0].type,
                                                      in.operands[0].reg_index)];
                reg[0] = reg[1] = kInvalidValue;
            }
            if (in.op == Op::Divide) {
                // The helper writes the accumulator pair without a GprWrite:
                // AX at 8 bits, DX:AX / EDX:EAX / RDX:RAX above (Intel SDM
                // Vol. 2A DIV/IDIV). A read of either before the divide is
                // the dividend, not the quotient or remainder.
                current[0][0] = current[0][1] = kInvalidValue;
                if ((in.operands[1].payload >> 1) != 0)
                    current[2][0] = current[2][1] = kInvalidValue;
            }
            if (output != i) block.insts[output] = in;
            ++output;
        }
        block.insts.resize(output);
        stats.register_reads_reused += removed;
        return removed;
    }
    ValueId current[8];
    for (auto& value : current) value = kInvalidValue;
    std::vector<ValueId> alias(block.next_value, kInvalidValue);
    // One chosen architectural home per propagated value. Copying a value to
    // another register does not migrate its home or promise a second one.
    std::vector<u8> home(block.next_value, 0xFF);
    usize output = 0;
    u32 removed = 0;
    for (usize i = 0; i < block.insts.size(); ++i) {
        Inst in = block.insts[i];
        for (u32 k = 0; k < in.operand_count; ++k) {
            auto& operand = in.operands[k];
            if (operand.kind != OperandKind::Value) continue;
            const auto value = operand.as_value();
            if (value < alias.size() && alias[value] != kInvalidValue)
                operand = Operand::value(alias[value], operand.type);
        }
        if (in.op == Op::GprRead && in.type == Type::I32 &&
            in.operands[0].reg_index < 8) {
            auto& prior = current[in.operands[0].reg_index];
            if (prior != kInvalidValue) {
                alias[in.result] = prior;
                ++removed;
                continue;
            }
            prior = in.result;
            home[in.result] = in.operands[0].reg_index;
        }
        if (in.op == Op::GprWrite) {
            // AL/AH/AX all invalidate the EAX snapshot, likewise CH etc.
            const u8 reg = architectural_gpr(in.operands[0].type,
                                             in.operands[0].reg_index);
            if (reg < 8) {
                current[reg] = kInvalidValue;
                const auto source = in.operands[1].as_value();
                // A computed value becomes architectural at the full-width
                // commit. Its pin then preserves it across helpers/faults.
                // Existing read aliases retain their original homes.
                if (in.operands[0].type == Type::I32 && source < home.size() &&
                    home[source] == 0xFF) {
                    home[source] = reg;
                    current[reg] = source;
                }
            }
        }
        if (in.op == Op::Divide) {
            current[0] = kInvalidValue;
            if ((in.operands[1].payload >> 1) != 0) current[2] = kInvalidValue;
        }
        block.insts[output++] = in;
    }
    block.insts.resize(output);
    stats.register_reads_reused += removed;
    return removed;
}

/// Move a record whose only reader is a mid-block exit onto that exit.
///
/// `cmp ; jcc` with the branch fused to the compare's host NZCV still
/// published the compare's record inline, because `CondExit` names the
/// definition (a use: "the exit edge leaves with every flag live"). The
/// record is needed only on the TAKEN edge, where the successor (or a fault
/// there) may read it. On the fall-through the definition is dead when a
/// later definition kills it and every fault before that killer is covered
/// by a recovery plan -- the same proof eliminate_dead_flags uses. Such a
/// definition is marked `exit_deferred`: the emitter keeps its operands and
/// publishes the record in the exit's cold stub, or inline at the exit when
/// the branch cannot be fused (the evaluator reads the record).
///
/// The shape is kept narrow so the operands provably survive to the exit:
/// between the definition and its exit only full-width guest register
/// commits (GprWrite I32/I64), which define no value and so cannot take a
/// register the record names. The emitter re-checks the one hazard left --
/// a commit into the very register holding an operand -- and publishes at
/// the definition when it occurs.
u32 defer_records_to_exits(IrBlock& block, PassStats& stats,
                           bool recovery_available) {
    if (!block.is_64bit) return 0;
    const u32 n = static_cast<u32>(block.insts.size());
    u32 deferred = 0;
    for (u32 i = 0; i < n; ++i) {
        Inst& def = block.insts[i];
        if (def.op != Op::DefFlags || def.flags.repr != FlagRepr::InCpuStateRecord ||
            def.operand_count != 2 || def.flags.shift_count != 0 ||
            def.flags.overlay_live || def.flags.keep_overlay ||
            def.flags.record_pinned || def.flags.exit_deferred ||
            def.flags.record == FlagState::FlagOp::None) {
            continue;
        }
        const auto reads_def = [&](const Inst& in) {
            for (u32 k = 0; k < in.operand_count; ++k) {
                if (in.operands[k].kind == OperandKind::Value &&
                    in.operands[k].type == Type::Flags &&
                    in.operands[k].as_value() == def.result) return true;
            }
            return false;
        };
        // The exit: after only full-width register commits.
        u32 exit_at = n;
        for (u32 j = i + 1; j < n; ++j) {
            const Inst& mid = block.insts[j];
            if (mid.op == Op::CondExit) {
                if (reads_def(mid) && mid.operand_count == 1) exit_at = j;
                break;
            }
            if (mid.op != Op::GprWrite || mid.may_fault ||
                (mid.operands[0].type != Type::I32 &&
                 mid.operands[0].type != Type::I64)) {
                break;
            }
        }
        if (exit_at == n) continue;
        // Exit to killer: nothing else may read the definition.
        u32 killer = n;
        bool ok = true;
        bool faults = false;
        for (u32 j = exit_at + 1; j < n && ok; ++j) {
            const Inst& mid = block.insts[j];
            if (mid.op == Op::DefFlags) {
                // A CF merge or a by-CL shift (a conditional writer) reads or
                // may keep this definition.
                if (reads_def(mid) || mid.operand_count != 2) ok = false;
                killer = j;
                break;
            }
            if (reads_def(mid)) ok = false;
            if (mid.may_fault) faults = true;
        }
        if (!ok || killer == n) continue;
        if (faults) {
            FlagRecoveryPlan plan{};
            if (!recovery_available ||
                !try_plan_flag_recovery(block, i, killer, plan, nullptr)) {
                continue;
            }
            block.flag_recovery.push_back(plan);
        }
        def.flags.exit_deferred = true;
        ++deferred;
    }
    stats.flags_exit_deferred += deferred;
    return deferred;
}

PassStats run_passes(IrBlock& block, bool recovery_available) {
    PassStats stats;
    if (pass_enabled(Pass::RegisterReadReuse)) reuse_register_reads(block, stats);
    if (pass_enabled(Pass::ScalarConstants)) common_scalar_constants(block, stats);
    // One classification for both decisions: it is a full-block scan, and
    // demand elimination only removes uses, so re-deriving it afterwards can
    // only ever turn it off. Deciding once is the conservative direction and
    // halves the analysis cost of every long-mode compile.
    //
    // EVERY long-mode block takes the snapshot emitter, so asking
    // `requires_snapshot_backing` about one is a full-block scan whose answer
    // cannot matter. The bitness short-circuit skips it on exactly the path
    // that pays for it most.
    //
    // The long-mode snapshot emitter publishes recovery plans through the
    // block's descriptors (ir_emit_snapshot.cpp); a 32-bit snapshot-backed
    // block's does not, so only that one keeps every record.
    const bool snapshot_backed = block.is_64bit || requires_snapshot_backing(block);
    if (pass_enabled(Pass::FlagDemand)) {
        eliminate_dead_flags(block, stats,
                             recovery_available &&
                                 (block.is_64bit || !snapshot_backed));
    }
    // After demand elimination, whose Dead verdicts it reads: a killed
    // definition publishes nothing and so cannot retire an overlay.
    if (pass_enabled(Pass::OverlayDemand)) eliminate_dead_overlays(block, stats);
    // After demand elimination: a definition proved dead has no result to keep
    // resident, and running this first would mark one that is about to be
    // downgraded.
    // A long-mode block's snapshot emitter honours the claim for the S-form
    // ALU ops it can emit (ir_emit_block); a 32-bit snapshot-backed block's
    // does not, so that one stays unannotated.
    if (pass_enabled(Pass::NzcvResidency) && (!snapshot_backed || block.is_64bit))
        assign_nzcv_residency(block, stats);
    // Reads the claims assign_nzcv_residency just made.
    if (pass_enabled(Pass::NzcvResidency) && pass_enabled(Pass::PstateCarry))
        keep_carry_in_pstate(block, stats);
    // Last: it reads the residency claims, and the definitions every other
    // pass left published are the ones it may move.
    if (pass_enabled(Pass::ExitRecord))
        defer_records_to_exits(block, stats, recovery_available);
    // `measure_cross_instruction_opportunity` is NOT called here. It changes
    // no code — it counts opportunities for `VITREO_JIT_IR_STATS` — and its
    // equivalent-Lea search is quadratic in block length. Gating the counter
    // inside the reporting function would still have paid for the whole
    // computation here, which is the same mistake as a diagnostic whose
    // ARGUMENTS are evaluated when the gate is off. The caller runs it only
    // when the telemetry is actually enabled.
    return stats;
}

}  // namespace vitreo::cpu::jit::ir
