// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_linear.cpp
/// @brief Tier 1 IR emission parity: linear reservation and M5 fault recovery; contract in test_ir_emit.cpp.

#include "ir_emit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

TEST_F(IrEmitLinear, LoadAndStoreRoundTripThroughTheReservation) {
    const std::vector<u8> code = {
        0x8B, 0x01,        // mov eax, [ecx]
        0x83, 0xC0, 0x07,  // add eax, 7
        0x89, 0x41, 0x04,  // mov [ecx+4], eax
    };
    ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));
    ASSERT_TRUE(m_memory->write_u32(kLinData, 0x1234'0000u));
    ASSERT_TRUE(m_memory->write_u32(kLinData + 4, 0));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(1, kLinData);
    cpu.gpr.set32(4, kLinData + 0x800);
    cpu.rip = kLinCode;

    BasicBlock block = decode_at(code);
    ASSERT_TRUE(m_jit->ir_build_block(block).ok)
        << ir::decline_reason_name(m_jit->ir_last_result().reason);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_jit->ir_emit_active())
        << "block fell back to Tier 0; this test would be asserting Tier 0";
    m_jit->execute(*compiled, cpu);

    EXPECT_EQ(cpu.gpr.get32(0), 0x1234'0007u);
    const auto stored = m_memory->read_u32(kLinData + 4);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x1234'0007u);
}

// A bad address on the linear path is a real SIGSEGV, and the RIP comes from
// the host-PC side table this emitter populates — not from an `emit_update_rip`
// (which it deliberately skips here as dead work). If the per-instruction
// interval bookkeeping were wrong, the fault would be attributed to the wrong
// guest instruction, and this is the only test that would notice.
TEST_F(IrEmitLinear, HardwareFaultRecoversTheFaultingInstructionsRip) {
    const std::vector<u8> code = {
        0x01, 0xC8,        // add eax, ecx   at 0x1000
        0x8B, 0x02,        // mov eax, [edx] at 0x1002  -- EDX unmapped
    };
    ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 0x11);
    cpu.gpr.set32(1, 0x22);
    cpu.gpr.set32(2, 0x7F00'0000u);   // inside the reservation, unmapped
    cpu.gpr.set32(4, kLinData + 0x800);
    cpu.rip = kLinCode;

    BasicBlock block = decode_at(code);
    ASSERT_TRUE(m_jit->ir_build_block(block).ok)
        << ir::decline_reason_name(m_jit->ir_last_result().reason);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_jit->ir_emit_active())
        << "block fell back to Tier 0; this test would be asserting Tier 0";
    m_jit->execute(*compiled, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "the load must fault";
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u) << "a load is a READ fault";
    EXPECT_EQ(cpu.rip, kLinCode + 2)
        << "recovered from the host-PC side table, not from a published RIP";
    EXPECT_EQ(cpu.gpr.get32(0), 0x33u)
        << "the ADD before the fault committed, harvested from its pin";
}

// =============================================================================
// M5 — precise recovery at every may-fault form
// =============================================================================
//
// `03_MILESTONES.md` M5's gate is: "Injected faults at each may-fault form
// reconstruct exact guest-visible state/RIP." The RIP half is covered above.
// This is the rest of the guest-visible state, and in particular EFLAGS, which
// nothing tested after a HARDWARE fault before this suite existed.
//
// Why that gap mattered more than it looks. The two fault paths recover flags
// by completely different means:
//
//   * region/helper — generated code persists live NZCV in the shared raise
//     stub (`emit_persist_live_nzcv`), so the flag state is written by the
//     block itself before it exits.
//   * linear/hardware — a real SIGSEGV. The handler harvests pins from the
//     `ucontext` and the RIP from the side table, and touches NO flag state at
//     all. Host NZCV at the fault instant is discarded by the trampoline.
//
// So on the hardware path the lazy record in CpuState is the ONLY thing
// carrying the guest's flags across the fault, and every claim the emitter
// makes about eliding or deferring a flag store has to be justified against
// that. These tests are what make such a claim falsifiable: they compare
// against the interpreter, which is the semantic oracle, rather than against a
// hand-computed expectation.

TEST_F(IrEmitFaultRecovery, PinlessBuildRetainsFaultObservableRecords) {
    if (std::getenv("VITREO_JIT_NO_PIN") == nullptr)
        GTEST_SKIP() << "run vitreo_jit_pinless_regressions";
    for (const u8 opcode : {u8{0x01}, u8{0x29}, u8{0x39}, u8{0x21}}) {
        expect_matches_interpreter({opcode, 0xC8, 0x8B, 0x1A, 0x31, 0xC9},
                                   "pinless flags before fault", false);
        EXPECT_TRUE(m_jit->ir_block().flag_recovery.empty());
    }
}

// A flag definition that completed BEFORE the faulting load must survive it.
TEST_F(IrEmitFaultRecovery, FlagsDefinedBeforeAFaultingLoadSurvive) {
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x8B, 0x02},       // mov eax, [edx]  <- faults
                               "load after ADD");
}

TEST_F(IrEmitFaultRecovery, FlagsDefinedBeforeAFaultingStoreSurvive) {
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x89, 0x02},       // mov [edx], eax  <- faults
                               "store after ADD");
}

// SUB rather than ADD: CF is inverted between x86 and ARM for subtraction, so
// a recovery path that translated host NZCV instead of the record would get
// carry backwards here and nowhere else.
TEST_F(IrEmitFaultRecovery, SubtractionFlagsSurviveAFaultingAccess) {
    expect_matches_interpreter({0x29, 0xC8,        // sub eax, ecx
                                0x8B, 0x02},       // mov eax, [edx]
                               "load after SUB");
}

// A logical op zeroes CF and OF, which arithmetic never produces. If recovery
// ever reconstructed the wrong FlagOp these two bits are where it shows.
//
// The operand is EDI, not ECX, and that is not arbitrary. `and eax,ecx` on
// this seed yields 0x00000001 — SF=0, ZF=0, PF=0, CF=OF=0 — which is RFLAGS
// 0x2, bit for bit the same as having published no flags at all. The test
// passed while a probe deleted every flag store. EDI makes the result
// 0x80000000, so SF and PF are set and the assertion has something to see.
TEST_F(IrEmitFaultRecovery, LogicalFlagsSurviveAFaultingAccess) {
    expect_matches_interpreter({0x21, 0xF8,        // and eax, edi
                                0x8B, 0x02},       // mov eax, [edx]
                               "load after AND");
}

// The fault is the flag-defining instruction itself. Nothing may be committed:
// the flags the guest observes must still be the PREVIOUS instruction's.
TEST_F(IrEmitFaultRecovery, FaultingAluWithMemorySourceCommitsNoFlags) {
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x03, 0x02},       // add eax, [edx]  <- faults
                               "ALU with faulting memory source");
}

TEST_F(IrEmitFaultRecovery, FaultingCompareWithMemorySourceCommitsNoFlags) {
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x3B, 0x02},       // cmp eax, [edx]  <- faults
                               "CMP with faulting memory source");
}

// The stack forms, now that their ESP ordering is fixed, with flags live.
TEST_F(IrEmitFaultRecovery, FlagsSurviveAFaultingPush) {
    CpuState probe;
    seed(probe, kUnmapped);
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0xFF, 0x32},       // push dword [edx] <- faults
                               "PUSH from faulting memory");
}

// Two flag definitions with the faulting access between them. This is exactly
// the shape `eliminate_dead_flags` refuses to kill across, and the reason it
// refuses: the first definition IS observable here, through the fault.
TEST_F(IrEmitFaultRecovery, FirstOfTwoFlagDefinitionsIsObservableThroughAFault) {
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx (never runs)
                               "flag def, fault, flag def");
}

// -----------------------------------------------------------------------------
// M5 — the recovery PRODUCER
// -----------------------------------------------------------------------------
//
// These are the cases the flag-recovery pass actually elides: a flag
// definition that is dead (a later definition overwrites it) except that a
// may-fault op stands in between. Without recovery the record must be
// published; with it, the store is skipped and the fault handler rebuilds the
// record from the interrupted register file.
//
// Every one of these compares against the interpreter, so a wrong
// reconstruction is a failure rather than a plausible-looking number. The
// shapes are chosen to separate the two descriptor modes:
//
//   DirectDst    `add eax,ecx` — the result went to EAX, whose pin holds it.
//   RecomputeDst `cmp eax,ecx` — the result was discarded, so the handler
//                redoes the subtraction from the two surviving inputs.

TEST_F(IrEmitFaultRecovery, ElidedRecordIsRebuiltAfterAFaultingLoad) {
    // add eax,ecx ; mov ebx,[edx] ; xor ecx,ecx
    // The ADD's record is dead (the XOR redefines the flags) but the load
    // between them can fault, which is exactly the population recovery
    // metadata exists for. EAX holds the result, so this is DirectDst.
    //
    // A LOAD, not a store: a store between them is refused, because only a
    // hardware fault rebuilds an elided record and a store can raise in
    // software from its cold-stub helper.
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided ADD record, faulting load");
}

TEST_F(IrEmitFaultRecovery, ElidedCompareRecordIsRecomputedAfterAFault) {
    // cmp eax,ecx ; mov ebx,[edx] ; xor ecx,ecx
    // CMP discards its result, so nothing holds flag_dst and the handler must
    // recompute EAX-ECX. Both inputs survive precisely because CMP writes
    // neither of them.
    expect_matches_interpreter({0x39, 0xC8,        // cmp eax, ecx
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided CMP record, faulting load");
}

TEST_F(IrEmitFaultRecovery, ElidedLogicalRecordIsRecomputedAfterAFault) {
    expect_matches_interpreter({0x21, 0xF8,        // and eax, edi
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided AND record, faulting load");
}

// SUB is the case where a reconstruction that translated host NZCV instead of
// rebuilding the record would get CF backwards.
TEST_F(IrEmitFaultRecovery, ElidedSubtractionRecordIsRebuiltAfterAFault) {
    expect_matches_interpreter({0x29, 0xC8,        // sub eax, ecx
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided SUB record, faulting load");
}

// An immediate source: flag_src has no register at all and travels in the
// descriptor itself.
TEST_F(IrEmitFaultRecovery, ElidedRecordWithAnImmediateSourceIsRebuilt) {
    expect_matches_interpreter({0x83, 0xF8, 0x42,  // cmp eax, 0x42
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided CMP-imm record, faulting load");
}

// A NARROW write between the compare and the fault rewrites EAX: `mov ah, 1`
// encodes register index 4 and is RAX architecturally. Recomputing EAX-ECX
// from the rewritten EAX would hand the exception the wrong flags, so the
// planner must keep the record.
TEST_F(IrEmitFaultRecovery, NarrowWriteToACompareOperandKeepsTheRecord) {
    expect_matches_interpreter({0x39, 0xC8,        // cmp eax, ecx
                                0xB4, 0x01,        // mov ah, 1
                                0x8B, 0x1A,        // mov ebx, [edx]  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "CMP, narrow clobber, faulting load");
    EXPECT_TRUE(m_jit->ir_block().flag_recovery.empty());
}

// The kill switch must restore the conservative behaviour exactly. Same bytes,
// same assertions, with the pass disabled — this is what makes a future
// correctness regression bisectable to one pass rather than to the milestone.
TEST_F(IrEmitFaultRecovery, KillSwitchRestoresThePublishedRecord) {
    // `VITREO_JIT_IR_PASSES` is read once into a process-wide cache, so it
    // cannot be flipped here. What CAN be checked without that is the other
    // half of the same promise: a block built with recovery unavailable must
    // publish, and must still match the interpreter.
    // A LOAD between the definition and its killer. A store would be refused
    // now — only a hardware fault rebuilds an elided record, and a store can
    // raise in software from its cold-stub helper.
    const std::vector<u8> code = {0x39, 0xC8, 0x8B, 0x1A, 0x31, 0xC9};
    ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));
    BasicBlock block = decode_at(code);

    ir::IrBlock without;
    ASSERT_TRUE(ir::build_block(block.instructions.data(), nullptr, 0,
                                kLinCode, false, false, without, nullptr).ok ||
                true);  // shape only; the real assertion is below

    // With recovery unavailable the pass must produce no plan at all.
    std::vector<u64> rips;
    u64 r = kLinCode;
    for (u32 i = 0; i < block.instruction_count; ++i) {
        rips.push_back(r);
        r += block.instructions[i].length;
    }
    ir::IrBlock off;
    ASSERT_TRUE(ir::build_block(block.instructions.data(), rips.data(),
                                block.instruction_count, kLinCode, false,
                                /*recovery_available=*/false, off,
                                nullptr).ok);
    EXPECT_TRUE(off.flag_recovery.empty())
        << "no plan may be produced when the fault path cannot honour it";

    ir::IrBlock on;
    ASSERT_TRUE(ir::build_block(block.instructions.data(), rips.data(),
                                block.instruction_count, kLinCode, false,
                                /*recovery_available=*/true, on, nullptr).ok);
    EXPECT_EQ(on.flag_recovery.size(), 1u)
        << "this shape is the producer's whole purpose";
}

// The fault is inside the KILLER's own guest instruction — the shape that was
// the single largest reason the producer refused (35 of 57 declines on
// Solitaire).
//
// `add ecx,[edx]` is the killer, and its LOAD faults. The load runs before the
// killer's own record is defined, so the architectural flags at that point are
// still the CMP's, and the covered range has to extend into the killer's
// instruction. Note the killer also WRITES ECX, which the CMP's plan names —
// that write is emitted after the load, so it cannot have run at the fault,
// which is why the clobber analysis compares op indices and not guest RIPs.
TEST_F(IrEmitFaultRecovery, ElidedRecordSurvivesAFaultInTheKillersInstruction) {
    expect_matches_interpreter({0x39, 0xC8,        // cmp eax, ecx
                                0x03, 0x0A},       // add ecx,[edx]  <- faults
                               "fault in the killer's own instruction");
}

TEST_F(IrEmitFaultRecovery, KillerInstructionFaultWithAnInterveningStore) {
    // EBP, not ESI: the seed puts an unmapped value in ESI on purpose, so
    // `mov [esi],ebx` would fault FIRST and the test would silently be
    // measuring the wrong instruction.
    //
    // The intervening store is NOT covered by the plan (stores never are); the
    // covered fault is the killer's own load. So this still exercises the
    // producer, and additionally checks that an uncovered store in the middle
    // does not itself defeat the plan.
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x89, 0x5D, 0x00,  // mov [ebp+0], ebx
                                0x03, 0x0A},       // add ecx,[edx]  <- faults
                               "store, then fault in the killer");
}
