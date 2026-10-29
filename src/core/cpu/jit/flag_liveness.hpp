// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file flag_liveness.hpp
/// @brief Within-block full-flag liveness for JIT lazy-flag stores.
///
/// Flag materialization dominates hot loops. Many ALU ops record a lazy
/// FlagOp that is overwritten by a later full-flag writer in the same block
/// before any consumer. Omitting those dead stores is pure win: the ALU still
/// runs, but flag_op/flag_src/flag_dst are left alone so a still-live prior
/// pending op (or the later writer) remains authoritative.
///
/// v2 scope:
///   - Full-flag writers: ADD/SUB/AND/OR/XOR/CMP/TEST, plus proven-nonzero
///     imm / by-1 SHL/SHR/SAR (CL-count shifts stay barriers — count 0
///     preserves flags)
///   - Within a single BasicBlock
///   - Never write FlagOp::None to "kill" — just skip the store
///   - Last writer before a reader or block end stays live-out
///   - INC/DEC/ADC/SBB/BT*/ROL/ROR and *_CL shifts are barriers
///

#pragma once

#include "../decode/decoder.hpp"
#include "block_cache.hpp"

#include <vector>

namespace vitreo::cpu::jit {

/// Bit 2 of an omit entry, set on a *Jcc* index rather than a publisher.
/// It marks the one forward in-block edge whose predecessor's deferred
/// lazy-flag record was retired by the internal-join proof (a non-faulting
/// full-flag writer at or past the branch target, reached on both paths).
/// Only such an edge may be emitted as a real intra-block branch: every other
/// shape leaves through a cold exit that re-emits the skipped store, and
/// compiling it as an internal edge drops that store on the taken path.
inline constexpr u8 kInternalJoinJcc = 4;

/// Flag effect of one instruction, as the within-block analysis classifies it.
///
/// - `Neutral`   — leaves the flags alone.
/// - `FullWriter`— redefines every arithmetic flag and never reads one.
/// - `Barrier`   — anything else: reads flags, preserves some (INC/DEC),
///                 has a runtime-dependent effect (CL-count shifts), or is
///                 simply unrecognised. Conservative by construction.
///
/// Exposed so the CROSS-block scan asks the same question of a successor's
/// instructions that the within-block pass asks of its own. Two analyses of
/// "does this kill the flags" that could disagree would be a silent
/// correctness divergence, so there is one classifier.
enum class FlagEffect : u8 { Neutral = 0, FullWriter, Barrier };

[[nodiscard]] FlagEffect classify_flag_effect(
    const DecodedInstruction& instr) noexcept;

/// True for every Jcc form (rel8/rel32, all 16 conditions). Jcc reads flags
/// and writes none, so a lazy-record prediction survives it.
[[nodiscard]] bool is_jcc_opcode(Opcode op) noexcept;

/// True when `instr` reads or writes memory, and so may fault part-way.
[[nodiscard]] bool instruction_touches_memory(
    const DecodedInstruction& instr) noexcept;


/// Guest-level recipe for rebuilding an elided lazy-flag record after a fault
/// on the Tier-0 stencil path. Mirrors `ir::FlagRecoveryPlan` field-for-field
/// so the compiler can publish the same `JitFlagRecovery` / cold-stub replay
/// the IR path already uses, without pulling the IR headers into liveness.
struct StencilFlagRecovery {
    u8 flag_op{0};
    bool recompute_dst{false};
    bool src_is_imm{false};
    u8 src_reg{0xFF};
    u64 src_imm{0};
    bool dst_is_imm{false};
    u8 dst_reg{0xFF};
    u64 dst_imm{0};
    u64 def_rip{0};
    u64 first_rip{0};
    u64 last_rip{0};
    /// Operand width of the record: 32, or 64 for a REX.W publisher. A 32-bit
    /// operation in a 64-bit block is still 32 (its source and result are
    /// bits 31:0 of the named registers).
    u8 width{32};
    /// INC/DEC carry-merge record (JitFlagRecovery::MergeCf): `dst_reg` is
    /// the result, the inherited CF is read from CpuState at the fault.
    bool merge_cf{false};

    [[nodiscard]] bool active() const noexcept { return flag_op != 0; }
};

/// Compute per-instruction omit bits for `block`.
/// `omit[i] & 3` is the publisher's omit state: 1 means instruction i's
/// emit_set_lazy_flags(_64) should be a no-op, 2 additionally keeps the CF
/// overlay. `omit[i] & kInternalJoinJcc` is the separate Jcc-edge bit above.
/// Vector size equals block.instruction_count.
///
/// `elided_span`, when given, gets one entry per instruction: publisher index
/// + 1 where that omitted publisher's record is absent from CpuState -- after
/// the publisher, through its killer -- and 0 elsewhere. Code emitted there
/// must not read the lazy record (the emitter recompiles the block without
/// omission if it would).
///
/// `cross_nzcv_readers` false refuses to cross SETcc / CMOVcc (the rule
/// before 2026-10-09); the emitter's retry uses it.
void compute_omit_lazy_flags(
    const BasicBlock& block, std::vector<u8>& omit,
    std::vector<StencilFlagRecovery>* recovery = nullptr,
    std::vector<u32>* elided_span = nullptr,
    bool cross_nzcv_readers = true) noexcept;

/// True when a pending lazy FlagOp is dead across `instr`, so the delegate
/// boundary can drop it instead of computing RFLAGS nobody will read.
///
/// Requires all three, which together make the pending flags unobservable:
///   - `instr` redefines every flag (the FullWriter class above: ADD/SUB/AND/
///     OR/XOR/CMP/TEST and proven-nonzero shifts), so nothing survives it;
///   - that class never *reads* flags -- ADC/SBB/RCL/RCR/Jcc/SETcc are
///     Barriers, not FullWriters;
///   - `instr` touches no memory, so it cannot fault part-way and hand an
///     exception record the stale RFLAGS. Register-only forms are the hot
///     case anyway.
[[nodiscard]] bool pending_flags_dead_across(
    const DecodedInstruction& instr) noexcept;

}  // namespace vitreo::cpu::jit
