// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_passes.cpp
/// @brief IR optimization passes, tested as pure IR transforms.
///
/// Hand-built IR isolates dataflow and fault-state obligations from decoding.
/// In particular, a faulting operation between a flag definition and a later
/// overwrite must still expose the preceding instruction's architectural flags.

#include <gtest/gtest.h>

#include <string>

#include "core/cpu/jit/ir/ir_passes.hpp"
#include "core/cpu/jit/ir/jit_ir.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit::ir;

namespace {

constexpr u64 kRip0 = 0x00401000;
constexpr u64 kRip1 = 0x00401003;
constexpr u64 kRip2 = 0x00401006;

constexpr GuestReg kEax{0, Type::I32};
constexpr GuestReg kEcx{1, Type::I32};

FlagDef published(FlagState::FlagOp record) {
    return FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None, record, false,
                   false};
}

/// `add eax, ecx` at `rip`, publishing `record`. Returns the DefFlags index.
u32 emit_alu(IrBuilder& b, u64 rip, FlagState::FlagOp record) {
    b.set_rip(rip);
    const ValueId a = b.gpr_read(kEax);
    const ValueId s = b.gpr_read(kEcx);
    const ValueId r = b.binary(Op::Add, Type::I32, a, s);
    b.def_flags(published(record), s, r);
    const u32 flags_at = b.last_id();
    b.gpr_write(kEax, r);
    return flags_at;
}

}  // namespace

// Two ALU instructions: the first's flags are overwritten by the second and
// nothing can observe them in between, so the first record need not be stored.
TEST(IrPasses, KillsAFlagDefinitionOverwrittenByALaterOne) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    emit_alu(b, kRip1, FlagState::FlagOp::Add32);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 1u);
    EXPECT_EQ(stats.flags_killed, 1u);
    EXPECT_EQ(block.insts[first].flags.repr, FlagRepr::Dead);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// The LAST flag definition in a block is live-out: a successor block may read
// it, and this IR has no cross-block liveness yet. It must survive.
TEST(IrPasses, NeverKillsTheLastFlagDefinition) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 only = emit_alu(b, kRip0, FlagState::FlagOp::Add32);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u);
    EXPECT_EQ(block.insts[only].flags.repr, FlagRepr::InCpuStateRecord);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// THE case the stencil path gets wrong. A faulting op between a definition and
// its killer means a fault would observe the flags, so the record must be
// stored even though it is overwritten later.
TEST(IrPasses, NeverKillsAcrossAFaultingOp) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);

    // Stand in for `MOV [mem], eax` — the exact shape `classify()` calls
    // Neutral and the stencil scan walks straight past.
    b.set_rip(kRip1);
    const ValueId v = b.gpr_read(kEax);
    b.gpr_write(kEcx, v);
    b.mark_may_fault(FaultState::RipAndFlags);

    emit_alu(b, kRip2, FlagState::FlagOp::Add32);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u)
        << "a fault between definition and killer can observe the flags";
    EXPECT_EQ(block.insts[first].flags.repr, FlagRepr::InCpuStateRecord);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// A faulting op AFTER the killer is irrelevant to the earlier definition.
TEST(IrPasses, KillsWhenTheFaultingOpIsAfterTheKiller) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    emit_alu(b, kRip1, FlagState::FlagOp::Add32);

    b.set_rip(kRip2);
    const ValueId v = b.gpr_read(kEax);
    b.gpr_write(kEcx, v);
    b.mark_may_fault(FaultState::RipAndFlags);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 1u);
    EXPECT_EQ(block.insts[first].flags.repr, FlagRepr::Dead);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// Three definitions in a row: the first two die, the last is live-out.
TEST(IrPasses, KillsEveryDefinitionExceptTheLast) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 a = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    const u32 c = emit_alu(b, kRip1, FlagState::FlagOp::Sub32);
    const u32 d = emit_alu(b, kRip2, FlagState::FlagOp::Xor32);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 2u);
    EXPECT_EQ(block.insts[a].flags.repr, FlagRepr::Dead);
    EXPECT_EQ(block.insts[c].flags.repr, FlagRepr::Dead);
    EXPECT_EQ(block.insts[d].flags.repr, FlagRepr::InCpuStateRecord);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// A killed definition must drop its record and its NZCV annotation: the IR
// verifier rejects a dead definition that still claims a home, and a stale
// `record` would tell the backend to publish the very store the pass removed.
TEST(IrPasses, KilledDefinitionClaimsNoHome) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    emit_alu(b, kRip1, FlagState::FlagOp::Add32);

    PassStats stats;
    ASSERT_EQ(eliminate_dead_flags(block, stats, false), 1u);
    EXPECT_EQ(block.insts[first].flags.record, FlagState::FlagOp::None);
    EXPECT_EQ(block.insts[first].flags.nzcv, NzcvKind::None);
}

// Running the pass twice must be a no-op the second time: an idempotent pass
// is what makes pipeline order a performance question rather than a
// correctness one.
TEST(IrPasses, IsIdempotent) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    emit_alu(b, kRip1, FlagState::FlagOp::Add32);

    PassStats first;
    ASSERT_EQ(eliminate_dead_flags(block, first, false), 1u);
    const std::string after_once = print(block);

    PassStats second;
    EXPECT_EQ(eliminate_dead_flags(block, second, false), 0u);
    EXPECT_EQ(print(block), after_once);
}

// An empty block, and a block with no flag definitions at all.
TEST(IrPasses, HandlesBlocksWithNoFlagDefinitions) {
    IrBlock block;
    block.entry_rip = kRip0;
    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u);

    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId v = b.gpr_read(kEax);
    b.gpr_write(kEcx, v);
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// The pass must leave a definition alone if it is already dead — including one
// the frontend marked dead itself — rather than double-counting it.
TEST(IrPasses, DoesNotRecountAnAlreadyDeadDefinition) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    emit_alu(b, kRip1, FlagState::FlagOp::Add32);
    block.insts[first].flags = FlagDef{FlagRepr::Dead, NzcvKind::None,
                                      FlagState::FlagOp::None, false, false};

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u);
    EXPECT_EQ(stats.flags_killed, 0u);
}

// =============================================================================
// Control flow and flag demand
// =============================================================================

// A Jcc USES the flags, so its producer must survive. This is the pass's
// composition claim made concrete: nothing in `eliminate_dead_flags` mentions
// branches, and it keeps the definition alive purely because the Jcc names the
// value as an operand.
TEST(IrPasses, KeepsAFlagDefinitionConsumedByAJcc) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 def = emit_alu(b, kRip0, FlagState::FlagOp::Sub32);
    const ValueId flags = block.insts[def].result;
    b.set_rip(kRip1);
    b.jcc(0x4 /* JZ */, flags, 0x500, kRip2);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u)
        << "the branch reads these flags";
    EXPECT_EQ(block.insts[def].flags.repr, FlagRepr::InCpuStateRecord);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// An EARLIER definition, killed by a later one that the branch actually reads,
// is still dead. Two definitions, one consumer: only the consumed one lives.
TEST(IrPasses, KillsAnEarlierDefinitionEvenWhenTheBlockBranches) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    const u32 second = emit_alu(b, kRip1, FlagState::FlagOp::Sub32);
    b.set_rip(kRip2);
    b.jcc(0x4, block.insts[second].result, 0x500, 0x600);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 1u);
    EXPECT_EQ(block.insts[first].flags.repr, FlagRepr::Dead);
    EXPECT_EQ(block.insts[second].flags.repr, FlagRepr::InCpuStateRecord);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// =============================================================================
// CF merge edges
// =============================================================================

// THE hole the merge edge exposed. An ordinary `DefFlags` never reads the
// definition it overwrites, so scanning strictly BETWEEN a definition and its
// killer was correct — until CF-preserving records arrived. `INC` overwrites
// the flags AND inherits their CF, so the killer is itself a use.
//
// With an exclusive scan, `ADD ; INC` marked the ADD's record dead and the INC
// then merged a carry that was never published.
TEST(IrPasses, KeepsADefinitionThatItsOwnKillerMergesFrom) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    const u32 first = emit_alu(b, kRip0, FlagState::FlagOp::Add32);
    const ValueId inherited = block.insts[first].result;

    // An INC: overwrites the flags, but inherits CF from the ADD.
    b.set_rip(kRip1);
    const ValueId a = b.gpr_read(kEax);
    const ValueId one = b.constant(Type::I32, 1);
    const ValueId r = b.binary(Op::Add, Type::I32, a, one);
    b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                        FlagState::FlagOp::Inc32, false, false},
                a, r, inherited);
    b.gpr_write(kEax, r);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 0u)
        << "the killer inherits this definition's CF, so it is live";
    EXPECT_EQ(block.insts[first].flags.repr, FlagRepr::InCpuStateRecord);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// A CF-preserving record with no merge edge is malformed: it would invent a
// carry. The verifier rejects it rather than the backend guessing.
TEST(IrPasses, VerifierRejectsCfPreservingRecordWithNoMergeEdge) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId a = b.gpr_read(kEax);
    const ValueId one = b.constant(Type::I32, 1);
    const ValueId r = b.binary(Op::Add, Type::I32, a, one);
    b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                        FlagState::FlagOp::Inc32, false, false}, a, r);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("preserves CF"), std::string::npos) << why;
}

// ...and the other direction: a record that DEFINES CF must not claim to
// inherit one. Both directions are checked so the pairing cannot drift.
TEST(IrPasses, VerifierRejectsMergeEdgeOnACfDefiningRecord) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId f0 = b.flags_in();
    const ValueId a = b.gpr_read(kEax);
    const ValueId c = b.gpr_read(kEcx);
    const ValueId r = b.binary(Op::Add, Type::I32, a, c);
    b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                        FlagState::FlagOp::Add32, false, false}, c, r, f0);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("defines CF"), std::string::npos) << why;
}

// One `flags.in` per block: two would be two names for the same architectural
// state, which is the ambiguity SSA exists to remove.
TEST(IrPasses, VerifierRejectsASecondFlagsIn) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    (void)b.flags_in();
    (void)b.flags_in();

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("at most one"), std::string::npos) << why;
}

// Killing a CF-preserving definition must drop its merge edge with the record.
// A merge edge on a `None` record claims an inheritance that does not exist,
// and the verifier rejects it — which is how this was found: `inc eax ;
// add eax,ecx` declined as `verifier-rejected` on a real guest, i.e. the pass
// bug cost coverage rather than producing a wrong carry.
TEST(IrPasses, KillingACfPreservingDefinitionDropsItsMergeEdge) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);

    b.set_rip(kRip0);
    const ValueId f0 = b.flags_in();
    const ValueId a = b.gpr_read(kEax);
    const ValueId one = b.constant(Type::I32, 1);
    const ValueId r = b.binary(Op::Add, Type::I32, a, one);
    const ValueId inc_flags =
        b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                            FlagState::FlagOp::Inc32, false, false},
                    a, r, f0);
    const u32 inc_at = b.last_id();
    b.gpr_write(kEax, r);
    (void)inc_flags;

    // A later full-flag writer kills it.
    emit_alu(b, kRip1, FlagState::FlagOp::Add32);

    PassStats stats;
    EXPECT_EQ(eliminate_dead_flags(block, stats, false), 1u);
    EXPECT_EQ(block.insts[inc_at].flags.repr, FlagRepr::Dead);
    EXPECT_EQ(block.insts[inc_at].operand_count, 2u)
        << "the merge edge must go with the record";

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

TEST(IrPasses, ReusesUnchangedRegisterReadsAcrossInstructionsAndMemory) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    const auto first = b.gpr_read(kEcx);
    b.gpr_write(kEax, first);
    b.set_rip(kRip1);
    const auto address = b.constant(Type::I32, 0x2000);
    b.store(Type::I32, address, first, FaultState::RipAndFlags);
    b.set_rip(kRip2);
    const auto again = b.gpr_read(kEcx);
    b.gpr_write(kEax, again);
    const usize before = block.insts.size();
    PassStats stats;
    EXPECT_EQ(reuse_register_reads(block, stats), 1u);
    EXPECT_EQ(stats.register_reads_reused, 1u);
    EXPECT_EQ(block.insts.size(), before - 1);
    EXPECT_EQ(block.insts.back().operands[1].as_value(), first);
    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

TEST(IrPasses, PartialRegisterWriteInvalidatesFullRegisterRead) {
    // CL, CH and CX. CH lives in the high-byte index space: index 5 at I8 is
    // BPL, a different architectural register entirely.
    for (const auto reg : {GuestReg{1, Type::I8},
                           GuestReg{kHighByteBase + 1, Type::I8},
                           GuestReg{1, Type::I16}}) {
        IrBlock block;
        block.entry_rip = kRip0;
        IrBuilder b(block);
        b.set_rip(kRip0);
        b.gpr_write(kEax, b.gpr_read(kEcx));
        b.set_rip(kRip1);
        b.gpr_write(reg, b.constant(reg.width, 7));
        b.set_rip(kRip2);
        const auto after = b.gpr_read(kEcx);
        b.gpr_write(kEax, after);
        PassStats stats;
        EXPECT_EQ(reuse_register_reads(block, stats), 0u);
        EXPECT_EQ(block.insts.back().operands[1].as_value(), after);
        std::string why;
        EXPECT_TRUE(verify(block, &why)) << why;
    }
}

TEST(IrPasses, ReusesCommittedComputedValueAcrossInstructions) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    const auto result = b.binary(Op::Add, Type::I32,
        b.gpr_read(kEax), b.gpr_read(kEcx));
    b.gpr_write(kEax, result);
    b.set_rip(kRip1);
    b.gpr_write(GuestReg{2, Type::I32}, b.gpr_read(kEax));
    PassStats stats;
    EXPECT_EQ(reuse_register_reads(block, stats), 1u);
    EXPECT_EQ(block.insts.back().operands[1].as_value(), result);
    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

TEST(IrPasses, CopiedValueDoesNotMigrateItsOriginalPinHome) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.gpr_write(kEax, b.constant(Type::I32, 7));
    b.set_rip(kRip1);
    b.gpr_write(kEcx, b.gpr_read(kEax));
    b.set_rip(kRip2);
    b.gpr_write(kEax, b.constant(Type::I32, 11));
    const auto copy = b.gpr_read(kEcx);
    b.gpr_write(GuestReg{2, Type::I32}, copy);
    PassStats stats;
    EXPECT_EQ(reuse_register_reads(block, stats), 1u);
    EXPECT_EQ(block.insts.back().operands[1].as_value(), copy);
    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// A constant shared across guest instructions is a block-long interval in the
// three-register pool, so sharing stops at the instruction boundary and a
// repeated immediate in the next instruction keeps its own definition.
TEST(IrPasses, CommonScalarConstantsStopAtGuestInstructionBoundary) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    for (u8 reg = 0; reg < 3; ++reg) {
        b.set_rip(kRip0 + reg * 3);
        const auto value = b.binary(Op::Add, Type::I32,
            b.gpr_read(GuestReg{reg, Type::I32}), b.constant(Type::I32, 5));
        b.gpr_write(GuestReg{reg, Type::I32}, value);
    }
    const usize before = block.insts.size();
    PassStats stats;
    ASSERT_TRUE(scalar_block_allocatable(block));
    EXPECT_EQ(common_scalar_constants(block, stats), 0u);
    EXPECT_EQ(stats.scalar_constants_reused, 0u);
    EXPECT_EQ(block.insts.size(), before);
    EXPECT_FALSE(requires_snapshot_backing(block));
    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// Memory and CF-merge blocks are outside the three-register pool, but the
// pass still shares a repeated constant WITHIN one instruction of them, and
// still refuses to share it with the next instruction.
TEST(IrPasses, ScalarConstantCommoningCoversSnapshotBackedMemoryAndCarryMerge) {
    for (bool memory : {false, true}) {
        IrBlock block;
        block.entry_rip = kRip0;
        IrBuilder b(block);
        b.set_rip(kRip0);
        const auto initial = b.gpr_read(kEax);
        const auto one = b.constant(Type::I32, 1);
        const auto result = b.binary(Op::Add, Type::I32, initial, one);
        if (memory) {
            b.load(Type::I32, initial, FaultState::RipAndFlags);
        } else {
            const auto incoming = b.flags_in();
            b.def_flags(published(FlagState::FlagOp::Inc32), initial, result, incoming);
        }
        b.gpr_write(kEax, result);
        const GuestReg edx{2, Type::I32};
        b.gpr_write(edx, b.binary(Op::Add, Type::I32,
            b.gpr_read(edx), b.constant(Type::I32, 1)));
        b.set_rip(kRip1);
        const auto next_one = b.constant(Type::I32, 1);
        b.gpr_write(kEcx, b.binary(Op::Add, Type::I32,
            b.gpr_read(kEcx), next_one));
        const usize before = block.insts.size();
        PassStats stats;
        EXPECT_FALSE(scalar_block_allocatable(block));
        EXPECT_TRUE(snapshot_block_allocatable(block));
        EXPECT_EQ(common_scalar_constants(block, stats), 1u);
        EXPECT_EQ(stats.scalar_constants_reused, 1u);
        ASSERT_EQ(block.insts.size(), before - 1);
        // Both Adds of the first instruction read its one constant; the next
        // instruction's Add keeps its own.
        std::vector<ValueId> used;
        for (const auto& in : block.insts)
            if (in.op == Op::Add) used.push_back(in.operands[1].as_value());
        ASSERT_EQ(used.size(), 3u);
        EXPECT_EQ(used[0], one);
        EXPECT_EQ(used[1], one);
        EXPECT_EQ(used[2], next_one);
        std::string why;
        EXPECT_TRUE(verify(block, &why)) << why;
    }
}

// The boundary that still has no allocator: neither the pool nor the snapshot
// backend lowers narrow memory or an overlay-bearing flag definition, so the
// constant must stay instruction-local in those blocks.
TEST(IrPasses, ScalarConstantCommoningRejectsFormsNoAllocatorBacks) {
    for (bool narrow_memory : {false, true}) {
        IrBlock block;
        block.entry_rip = kRip0;
        IrBuilder b(block);
        b.set_rip(kRip0);
        const auto initial = b.gpr_read(kEax);
        const auto one = b.constant(Type::I32, 1);
        const auto result = b.binary(Op::Add, Type::I32, initial, one);
        if (narrow_memory) {
            // 8-bit access: no width-aware spill lowering exists for it.
            b.load(Type::I8, initial, FaultState::RipAndFlags);
        } else {
            FlagDef def = published(FlagState::FlagOp::Add32);
            def.overlay_live = true;
            b.def_flags(def, initial, result);
        }
        b.gpr_write(kEax, result);
        b.set_rip(kRip1);
        b.gpr_write(kEcx, b.binary(Op::Add, Type::I32,
            b.gpr_read(kEcx), b.constant(Type::I32, 1)));
        const usize before = block.insts.size();
        PassStats stats;
        EXPECT_FALSE(scalar_block_allocatable(block));
        EXPECT_FALSE(snapshot_block_allocatable(block));
        EXPECT_EQ(common_scalar_constants(block, stats), 0u);
        EXPECT_EQ(block.insts.size(), before);
        std::string why;
        EXPECT_TRUE(verify(block, &why)) << why;
    }
}

TEST(IrPasses, ScalarConstantsCommittedByMovAreNeverCommoned) {
    IrBlock block;
    block.entry_rip = kRip0;
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.gpr_write(kEax, b.constant(Type::I32, 5));
    b.set_rip(kRip1);
    b.gpr_write(kEcx, b.binary(Op::Add, Type::I32,
        b.gpr_read(kEcx), b.constant(Type::I32, 5)));
    PassStats stats;
    EXPECT_EQ(common_scalar_constants(block, stats), 0u);
}

// ---------------------------------------------------------------------------
// Dead CF/OF overlay elimination (long mode).
// ---------------------------------------------------------------------------

#include "core/cpu/state/flag_state.hpp"
#include "core/cpu/state/flags.hpp"

namespace {

constexpr GuestReg kRax{0, Type::I64};
constexpr GuestReg kRcx{1, Type::I64};

/// `shl rax, 5` as the frontend builds it: a NzOnly Shl64 record, then CF from
/// the overlay because the record has no room for the shifted-out bit.
void emit_shl5(IrBuilder& b, u64 rip) {
    b.set_rip(rip);
    const ValueId a = b.gpr_read(kRax);
    const ValueId one = b.constant(Type::I64, 1);
    const ValueId out = b.binary(Op::And, Type::I64,
                                 b.shift_imm(Op::Shr, Type::I64, a, 59), one);
    const ValueId r = b.shift_imm(Op::Shl, Type::I64, a, 5);
    b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::NzOnly,
                        FlagState::FlagOp::Shl64, false, false},
                a, r);
    b.set_flag_overlay(Op::SetCf, out, Type::I64);
    b.gpr_write(kRax, r);
}

/// `add rax, rcx`. Returns its flag value.
ValueId emit_add64(IrBuilder& b, u64 rip) {
    b.set_rip(rip);
    const ValueId a = b.gpr_read(kRax);
    const ValueId s = b.gpr_read(kRcx);
    const ValueId r = b.binary(Op::Add, Type::I64, a, s);
    const ValueId f = b.def_flags(published(FlagState::FlagOp::Add64), s, r);
    b.gpr_write(kRax, r);
    return f;
}

ValueId last_flags(const IrBlock& block) {
    for (usize i = block.insts.size(); i-- > 0;)
        if (block.insts[i].op == Op::DefFlags) return block.insts[i].result;
    return kInvalidValue;
}

u32 count_op(const IrBlock& block, Op op) {
    u32 n = 0;
    for (const auto& in : block.insts) n += in.op == op ? 1u : 0u;
    return n;
}

}  // namespace

// The whitelist is the pass's whole soundness argument: a record listed there
// must set CF and OF from itself alone. Pinned against the materializer rather
// than restated, so a record whose materialize case changes fails here.
TEST(IrPasses, RecordsThatRetireOverlaysOwnCfAndOfInTheMaterializer) {
    using F = FlagState::FlagOp;
    const u64 samples[] = {0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
                           0x7FFF'FFFF, 0x8000'0000, 0xFFFF'FFFF,
                           0x1'0000'0001ull, 0x7FFF'FFFF'FFFF'FFFFull,
                           0x8000'0000'0000'0000ull, 0xFFFF'FFFF'FFFF'FFFFull,
                           0x0123'4567'89AB'CDEFull, 0xFEDC'BA98'7654'3210ull};
    u32 owned = 0;
    for (u32 raw = 1; raw <= static_cast<u32>(F::Ror64_1); ++raw) {
        const F record = static_cast<F>(raw);
        if (!record_owns_cf_and_of(record)) continue;
        ++owned;
        for (u64 src : samples) {
            for (u64 dst : samples) {
                Flags clear(0);
                Flags set(FLAG_CF | FLAG_OF);
                FlagState a{};
                a.flag_op = record; a.flag_src = src; a.flag_dst = dst;
                FlagState b = a;
                a.materialize(clear);
                b.materialize(set);
                ASSERT_EQ(clear.cf(), set.cf())
                    << "record " << raw << " leaves CF to RFLAGS, src=" << src
                    << " dst=" << dst;
                ASSERT_EQ(clear.of(), set.of())
                    << "record " << raw << " leaves OF to RFLAGS, src=" << src
                    << " dst=" << dst;
            }
        }
    }
    EXPECT_GT(owned, 20u);
    // The records the frontend pairs with an overlay must NOT be listed.
    EXPECT_FALSE(record_owns_cf_and_of(F::Shl64));
    EXPECT_FALSE(record_owns_cf_and_of(F::Shr64));
    EXPECT_FALSE(record_owns_cf_and_of(F::Sar64));
    EXPECT_FALSE(record_owns_cf_and_of(F::Rol64));
    EXPECT_FALSE(record_owns_cf_and_of(F::Inc64));
}

// `shl rax,5 ; add rax,rcx`: the ADD's record owns CF and OF, so the SHL's
// overlay is dead, and so is the bit extraction that fed it.
TEST(IrPasses, OverlayRetiredByAFullFlagWriterIsRemovedWithItsInputs) {
    IrBlock block;
    block.entry_rip = kRip0;
    block.is_64bit = true;
    IrBuilder b(block);
    emit_shl5(b, kRip0);
    emit_add64(b, kRip1);
    const usize before = block.insts.size();

    PassStats stats;
    eliminate_dead_flags(block, stats, false);
    EXPECT_EQ(eliminate_dead_overlays(block, stats), 1u);
    EXPECT_EQ(count_op(block, Op::SetCf), 0u);
    // Shr, And and the constant 1 fed only the overlay.
    EXPECT_EQ(stats.values_dce, 3u);
    EXPECT_EQ(block.insts.size(), before - 4);
    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// Anything that can observe CF in between keeps the overlay.
TEST(IrPasses, OverlayObservedBeforeItsKillerIsKept) {
    // A flag reader: `shl rax,5 ; setc` style EvalCc on the live flags.
    {
        IrBlock block;
        block.entry_rip = kRip0;
        block.is_64bit = true;
        IrBuilder b(block);
        emit_shl5(b, kRip0);
        b.set_rip(kRip1);
        const ValueId flags = last_flags(block);
        const ValueId c = b.eval_cc(flags, 0x2);
        b.gpr_write(GuestReg{2, Type::I8}, c);
        emit_add64(b, kRip2);
        PassStats stats;
        eliminate_dead_flags(block, stats, false);
        EXPECT_EQ(eliminate_dead_overlays(block, stats), 0u);
        EXPECT_EQ(count_op(block, Op::SetCf), 1u);
    }
    // A fault: the handler rebuilds RFLAGS from CpuState, overlay included.
    {
        IrBlock block;
        block.entry_rip = kRip0;
        block.is_64bit = true;
        IrBuilder b(block);
        emit_shl5(b, kRip0);
        b.set_rip(kRip1);
        const ValueId p = b.gpr_read(kRcx);
        (void)b.load(Type::I64, p, FaultState::RipAndFlags);
        emit_add64(b, kRip2);
        PassStats stats;
        eliminate_dead_flags(block, stats, false);
        EXPECT_EQ(eliminate_dead_overlays(block, stats), 0u);
    }
    // No later writer: the overlay is live out of the block.
    {
        IrBlock block;
        block.entry_rip = kRip0;
        block.is_64bit = true;
        IrBuilder b(block);
        emit_shl5(b, kRip0);
        PassStats stats;
        EXPECT_EQ(eliminate_dead_overlays(block, stats), 0u);
    }
    // A later SHL's record does not own CF: its own overlay retires it, but
    // the record alone never does, so the first overlay is only dead because
    // the second exists -- and this pass, deliberately, does not reason that
    // far. Both stay.
    {
        IrBlock block;
        block.entry_rip = kRip0;
        block.is_64bit = true;
        IrBuilder b(block);
        emit_shl5(b, kRip0);
        emit_shl5(b, kRip1);
        PassStats stats;
        eliminate_dead_flags(block, stats, false);
        EXPECT_EQ(eliminate_dead_overlays(block, stats), 0u);
        EXPECT_EQ(count_op(block, Op::SetCf), 2u);
    }
    // 32-bit blocks are left to their own emitters.
    {
        IrBlock block;
        block.entry_rip = kRip0;
        IrBuilder b(block);
        emit_shl5(b, kRip0);
        emit_add64(b, kRip1);
        PassStats stats;
        EXPECT_EQ(eliminate_dead_overlays(block, stats), 0u);
    }
}

// INC preserves CF, so it READS the overlay through its merge edge.
TEST(IrPasses, OverlayBeforeACarryPreservingWriterIsKept) {
    IrBlock block;
    block.entry_rip = kRip0;
    block.is_64bit = true;
    IrBuilder b(block);
    emit_shl5(b, kRip0);
    const ValueId shl_flags = last_flags(block);
    b.set_rip(kRip1);
    const ValueId a = b.gpr_read(kRax);
    const ValueId r = b.binary(Op::Add, Type::I64, a, b.constant(Type::I64, 1));
    b.def_flags(published(FlagState::FlagOp::Inc64), a, r, shl_flags);
    b.gpr_write(kRax, r);
    PassStats stats;
    eliminate_dead_flags(block, stats, false);
    EXPECT_EQ(eliminate_dead_overlays(block, stats), 0u);
}
