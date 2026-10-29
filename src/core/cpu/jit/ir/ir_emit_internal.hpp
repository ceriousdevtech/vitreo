// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file ir_emit_internal.hpp
/// @brief Tier 1 ARM64 emission from canonical IR (ADR-0002).
///
/// This replaces the stencil compiler's per-guest-instruction loop body for a
/// block the frontend accepted, and nothing else. Prologue, pin ABI selection,
/// cold exits, epilogue, the fault side table and finalisation stay Tier 0's
/// (ADR-0002), so Tier 1 inherits their correctness properties instead of
/// re-earning them.
///
/// ## Emission cannot fail
///
/// Once the first ARM word of the block body is emitted there is no way back:
/// Tier 0 has already written a prologue that assumes it will be followed by a
/// body and an epilogue. So every reason this emitter could give up is checked
/// by `ir_emit_feasible()` **before** Seam A returns its verdict, and the
/// emitter itself has no failure path. A `return false` discovered mid-emission
/// would be a half-written block, which is worse than either tier alone.
///
/// ## Register discipline
///
/// Guest GPRs keep the fixed pin ABI as their only home (plan §3.2 Option A):
/// for a 32-bit block EAX/ECX/EDX/EBX/ESI/EDI are X23-X28 and ESP/EBP are
/// X12/X13. A guest register living anywhere else at a fault is invisible to
/// the handler — it harvests X19, X20 and the block's `JitPinMap`, and nothing
/// else — and is silently lost.
///
/// Temporaries come from `REG_SCRATCH0..2` only. Deliberately not W3 or W4:
/// W3 is assumed dead at every diagnostic and direct-link probe site, and W4
/// is the NZCV kind-byte temp and the out-of-range pin-spill temp. Neither is
/// reachable from this op set today, and that is exactly why relying on it
/// would be a trap for whoever extends this emitter into ops that do call
/// helpers. `REG_EA_TMP` (W5) is left alone for the same reason.
///
/// The three-register pool supports block-wide I32/I64 lifetimes for non-faulting
/// scalar blocks without helper-dependent flag merges. Long-mode memory
/// blocks and scalar blocks exceeding the pool use invocation-local spill
/// slots. PE32 helper blocks still allocate within each guest instruction.
/// Architectural pins carry PE32 unchanged reads and committed results.
/// Feasibility rejects stale pins, excess slot pressure and unsupported forms.
///
/// Flag-demand, recovery and NZCV-residency passes already operate across
/// instructions. Long-mode scalar ALU, direct branches and simple MOV memory
/// forms reuse the architectural pin ABI and memory primitives. Unsupported
/// addressing forms and scratch lifetimes fail feasibility before emission.

#include "ir_frontend.hpp"
#include "ir_passes.hpp"
#include "ir_spill_frame.hpp"
#include "../jit_compiler.hpp"
#include "../jit_coverage.hpp"
#include "../jit_helpers.hpp"
#include "../jit_stats.hpp"
#include "../stencils.hpp"
#include <vitreo/diagnostics.hpp>

namespace vitreo::cpu::jit {

namespace {

/// Temporaries available to the IR emitter. See the register-discipline note
/// above for why the pool stops at three and excludes W3/W4/W5.
constexpr u8 kIrScratch[] = {REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH2};

/// Where the long-mode snapshot emitter reloads a rematerialized operand (a
/// constant, or a guest register read from CpuState), by operand position.
/// Outside the pool on purpose: such a value is recomputed at EVERY use, so
/// it needs a register only at that instant -- giving it a pool register for
/// its whole interval is what overflowed the three-register pool on long
/// blocks of CpuState-resident registers and spilled every value. Nothing in
/// the snapshot lowerings touches X6/X7; X5 is used only inside the one-
/// operand SetCf/SetOf lowering, so it is safe as the third operand's home.
constexpr u8 kIrRematAt[] = {6, 7, 5};

/// The one register a value can be parked in ACROSS a guest memory access.
///
/// `REG_CALL_TMP` (X21) is callee-saved, so a helper call preserves it, and
/// the cold memory stubs are documented as never touching it — precisely
/// because callers park values there across an access. The stencil compiler's
/// own POP lowering does exactly this ("REG_CALL_TMP (callee-saved) holds the
/// old RSP across the BLR that emit_guest_load32 may emit").
///
/// One register, not a pool. A second simultaneous survivor is a real register
/// allocator, and declining is the honest alternative to half-building one.
/// It is enough for every shape the frontend currently produces: a
/// read-modify-write needs its RESULT to outlive the store, and its address
/// and source operand are both rematerializable.
constexpr u8 kIrSurvivor = REG_CALL_TMP;

/// A live CF-preserving record (INC/DEC): its third operand is the flag value
/// it inherits CF from. A by-CL shift's third operand is its I8 count, which
/// needs no capture.
[[nodiscard]] inline bool ir_is_cf_merge(const ir::Inst& in) noexcept {
    return in.op == ir::Op::DefFlags && in.operand_count == 3 &&
           in.flags.repr != ir::FlagRepr::Dead &&
           in.operands[2].kind == ir::OperandKind::Value &&
           in.operands[2].type == ir::Type::Flags;
}

/// The carry formula for the record a CF merge inherits from, when that
/// record is published in this block.
///
/// `FlagDef::record` is the carry producer (jit_ir.hpp): the formulas in
/// `emit_lazy_cf_to_call_tmp` read back the record's fields, whose meaning is
/// fixed by the `FlagOp` rather than by the tier that wrote it. Unknown when
/// the producer is outside the block, not published, a CF-preserving record,
/// or separated from the merge by an op that may fault; the capture then
/// keeps its runtime dispatch.
[[nodiscard]] inline CarrySource in_block_carry_source(const ir::IrBlock& block,
                                                usize merge_at) {
    const ir::Inst& merge = block.insts[merge_at];
    if (merge.operand_count != 3 ||
        merge.operands[2].kind != ir::OperandKind::Value ||
        merge.operands[2].type != ir::Type::Flags) {
        return CarrySource::Unknown;
    }
    const ir::ValueId incoming = merge.operands[2].as_value();
    for (usize i = merge_at; i-- > 0;) {
        const ir::Inst& in = block.insts[i];
        if (in.result != incoming) {
            if (in.may_fault) return CarrySource::Unknown;
            continue;
        }
        if (in.op != ir::Op::DefFlags ||
            in.flags.repr != ir::FlagRepr::InCpuStateRecord) {
            return CarrySource::Unknown;
        }
        // Only the producers IrEmit.CfMergeTakesAnInBlockProducersCarryFormula
        // and IrEmit64NarrowAlu.FullWidthIncDecNegNotAtOverflowBoundaries
        // reach. ADC/SBB keep the dispatch until a test drives them into a
        // merge. The 64-bit sources are the formulas the stencil x64 path
        // names (emit_set_lazy_flags64), read back from the same record.
        using F = FlagState::FlagOp;
        switch (in.flags.record) {
            case F::Add32: return CarrySource::Add32;
            case F::Sub32: return CarrySource::Sub32;
            case F::And32: case F::Or32: case F::Xor32:
                return CarrySource::Logic32;
            case F::Add64: return CarrySource::Add64;
            case F::Sub64: return CarrySource::Sub64;
            case F::And64: case F::Or64: case F::Xor64:
                return CarrySource::Logic64;
            default: return CarrySource::Unknown;
        }
    }
    return CarrySource::Unknown;
}
constexpr u32 kIrScratchCount =
    static_cast<u32>(sizeof(kIrScratch) / sizeof(kIrScratch[0]));

/// Can value `v` be recomputed from scratch at any point in its guest
/// instruction, cheaply and with no side effects?
///
/// Only two shapes qualify, and both are deliberate rather than a starting
/// point: a `Const` (one or two MOVs) and a `Lea` whose base and index live in
/// guest PINS (an ADD or two off registers a memory access cannot disturb).
/// Anything reading another scratch value would need that value rematerialized
/// too, and a transitive remat is a register allocator wearing a disguise —
/// that is M6's remaining work, not this.
[[nodiscard]] inline bool rematerializable(const ir::IrBlock& block, ir::ValueId v,
                                    const std::vector<u8>& pin_home) {
    for (const ir::Inst& def : block.insts) {
        if (!def.defines_value() || def.result != v) continue;
        if (def.op == ir::Op::Const) return true;
        if (def.op != ir::Op::Lea) return false;
        for (u32 k = 0; k < def.operand_count; ++k) {
            const ir::Operand& o = def.operands[k];
            if (o.kind != ir::OperandKind::Value) continue;
            if (o.as_value() >= pin_home.size()) return false;
            if (pin_home[o.as_value()] == 0xFF) return false;
        }
        return true;
    }
    return false;
}


/// Assign pool registers over an instruction or a pure scalar block using
/// def-to-last-scratch-use intervals.
///
/// What this replaces: a running count of values DEFINED in the instruction,
/// which never decremented. That rejected `sub [mem], imm` — four defined
/// values, never more than three simultaneously live — and every shape like
/// it. Counting definitions is not measuring pressure; a value whose last use
/// has passed is not occupying anything.
///
/// Memory/CF-helper blocks use instruction ranges. Pure scalar blocks
/// use the complete block, allowing uncommitted values across instructions.
/// Architectural commits end scratch residency; pins then hold those values.
///
/// Deterministic: called from both the feasibility check and emission, and
/// they must agree. It ignores in-place destinations, which emission resolves
/// to a pin without consuming a slot, so emission may use fewer registers
/// than planned but never more.
///
/// Returns false when the pool genuinely cannot hold the interval set; fills
/// `assign` (value -> ARM register, 0xFF for none) when it can.
[[nodiscard]] inline bool plan_pool_registers(const ir::IrBlock& block, usize begin,
                                       usize end,
                                       const std::vector<u8>& pin_home,
                                       const std::vector<u8>& is_survivor,
                                       std::vector<u8>* assign,
                                       u16 commit_pin_mask = 0,
                                       const std::vector<u32>* interval_end = nullptr,
                                       bool allow_spill = false,
                                       const std::vector<u8>* remat_kind = nullptr) {
    const auto needs_slot = [&](const ir::Inst& in) {
        // Narrow values occupy a full W register (zero-extended), so they
        // need a pool slot exactly like an I32 does.
        if (!in.defines_value()) return false;
        if (in.type != ir::Type::I32 && in.type != ir::Type::I8 &&
            in.type != ir::Type::I16 && in.type != ir::Type::I64) {
            return false;
        }
        if (in.result >= pin_home.size()) return false;
        if (pin_home[in.result] != 0xFF) return false;      // lives in a pin
        if (is_survivor[in.result]) return false;           // lives in X21
        return true;
    };

    // Last use of each value defined in this range, defaulting to its own
    // definition so a value nobody reads still gets a register to be written
    // into.
    std::vector<usize> last_use(block.next_value, 0);
    for (usize d = begin; d < end; ++d) {
        if (needs_slot(block.insts[d])) last_use[block.insts[d].result] = d;
    }
    for (usize j = begin; j < end; ++j) {
        for (u32 k = 0; k < block.insts[j].operand_count; ++k) {
            const ir::Operand& o = block.insts[j].operands[k];
            if (o.kind != ir::OperandKind::Value) continue;
            if (o.as_value() >= last_use.size()) continue;
            if (j > last_use[o.as_value()]) last_use[o.as_value()] = j;
        }
    }

    // A value the caller proved resident in a pin after its commit stops
    // occupying scratch there (snapshot plan: commit_home / commit_at).
    if (interval_end != nullptr) {
        for (usize v = 0; v < last_use.size() && v < interval_end->size(); ++v) {
            const u32 end_at = (*interval_end)[v];
            if (end_at != 0xFFFFFFFFu && last_use[v] > end_at) last_use[v] = end_at;
        }
    }
    // A full architectural commit migrates a computed value out of scratch
    // and into its pin. Later uses no longer extend the scratch interval.
    // Feasibility separately checks that no overwritten pin alias is used.
    if (commit_pin_mask != 0) {
        for (usize j = begin; j < end; ++j) {
            const auto& in = block.insts[j];
            if (in.op != ir::Op::GprWrite || in.operands[0].type != ir::Type::I32 ||
                in.operands[0].reg_index >= 16 ||
                !(commit_pin_mask & (1u << in.operands[0].reg_index))) continue;
            const auto value = in.operands[1].as_value();
            if (value < last_use.size() && last_use[value] > j)
                last_use[value] = j;
        }
    }

    u8 held_by[kIrScratchCount];
    for (u32 i = 0; i < kIrScratchCount; ++i) held_by[i] = 0xFF;
    ir::ValueId held_value[kIrScratchCount];
    for (u32 i = 0; i < kIrScratchCount; ++i) held_value[i] = ir::kInvalidValue;

    for (usize d = begin; d < end; ++d) {
        // Retire every interval that ended before this op.
        for (u32 i = 0; i < kIrScratchCount; ++i) {
            if (held_by[i] == 0xFF) continue;
            if (last_use[held_value[i]] < d) {
                held_by[i] = 0xFF;
                held_value[i] = ir::kInvalidValue;
            }
        }
        if (!needs_slot(block.insts[d])) continue;

        u32 slot = kIrScratchCount;
        for (u32 i = 0; i < kIrScratchCount; ++i) {
            if (held_by[i] == 0xFF) { slot = i; break; }
        }
        if (slot == kIrScratchCount) {
            if (!allow_spill) return false;  // genuine pressure
            // Linear-scan eviction: of the held values and this one, the one
            // used furthest away goes to its spill slot for its whole life
            // (it never occupied the register in emitted code, so the register
            // is free for the rest). Only the snapshot planner allows this; it
            // backs such values and reloads them at each use (kIrRematAt).
            // A rematerializable value (a constant, a CpuState read) is the
            // cheapest to evict -- it reloads at each use with no store --
            // so it goes first; otherwise the furthest-used value does.
            const auto remat = [&](ir::ValueId v) {
                return remat_kind != nullptr && v < remat_kind->size() &&
                       (*remat_kind)[v] != 0;
            };
            const ir::ValueId incoming = block.insts[d].result;
            u32 victim = kIrScratchCount;
            bool victim_remat = remat(incoming);
            usize furthest = last_use[incoming];
            for (u32 i = 0; i < kIrScratchCount; ++i) {
                const ir::ValueId held = held_value[i];
                const bool r = remat(held);
                if ((r && !victim_remat) ||
                    (r == victim_remat && last_use[held] > furthest)) {
                    furthest = last_use[held];
                    victim_remat = r;
                    victim = i;
                }
            }
            if (victim == kIrScratchCount) continue;  // this one spills
            if (assign != nullptr) (*assign)[held_value[victim]] = 0xFF;
            slot = victim;
        }
        held_by[slot] = 1;
        held_value[slot] = block.insts[d].result;
        if (assign != nullptr) (*assign)[block.insts[d].result] =
                                   kIrScratch[slot];
    }
    return true;
}

/// log2 of an x86 SIB scale, for the ARM shifted-register form.
[[nodiscard]] inline u8 scale_shift(u64 scale) noexcept {
    switch (scale) {
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: return 0;
    }
}

}  // namespace

// Host helpers the snapshot emitter calls by BLR; defined in
// ir_runtime_helpers.cpp.
extern "C" int jit_ir_divide(CpuState* cpu, u64 divisor_bits, u32 kind);
extern "C" u64 jit_ir_flags_image(CpuState* cpu);
extern "C" void jit_ir_flags_install(CpuState* cpu, u64 value);

}  // namespace vitreo::cpu::jit
