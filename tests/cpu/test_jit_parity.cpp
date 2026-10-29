// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode)
//
// Runs the SAME x86 machine code through both the interpreter and the JIT,
// then diffs ALL CPU state (GPRs, flags, XMM, memory).  Any divergence is
// a potential source of the Steam.exe crash.

#include "jit_parity_fixture.hpp"

// ============================================================================
// CMP/TEST -> Jcc flag fusion parity
//
// The fusion emits the compare as SUBS/ANDS and the Jcc as a native CSET off
// NZCV instead of the cond helper. A wrong condition mapping would silently
// take the wrong branch, so verify EVERY Jcc, after both CMP and TEST, over
// operand pairs that exercise carry/zero/sign/overflow — the JIT's final RIP
// (branch taken vs not) must match the interpreter's.
// ============================================================================

TEST_F(JitParityTest, DelegatedXlatMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u8(0x2008, 0xA7).is_ok());
    check_parity({0xD7}, [](CpuState& cpu) {
        cpu.gpr.set32(EBX, 0x2000);
        cpu.gpr.set32(EAX, 0x12345608);
        cpu.flags.rflags = 0x8D7;
    });
}

// Run a {compare; Jcc +2} block through interpreter and JIT; the branch
// decision (final RIP) must match. check_parity can't be used here — it asserts
// ExecResult::Continue, which a taken branch violates.
namespace {
void expect_branch_matches(GuestMemory& mem,
                           std::initializer_list<u8> code_il, u32 a, u32 b) {
    // Fresh JitCompiler each call: the fixture's shared one would trip the
    // churn limit after ~kChurnRecompileLimit compiles of the same address.
    JitCompiler jit(mem);
    std::vector<u8> code(code_il);
    ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
    MemoryByteReader reader(code.data(), code.size());
    BasicBlock block; block.start_addr = 0x1000;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) != DecodeError::Success) break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, 2u);

    auto setup = [&](CpuState& c) {
        c.init_protected_mode(); c.gpr.set32(ESP, 0x10000); c.rip = 0x1000;
        c.gpr.set32(EAX, a); c.gpr.set32(EBX, b);
    };
    CpuState icpu; setup(icpu);
    { Interpreter interp(icpu, mem); (void)interp.run(2); }
    CpuState jcpu; setup(jcpu);
    ASSERT_TRUE(mem.protect_region(0x1000, 0x1000,
                                   MemoryProtection::ReadExecute).is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(mem.protect_region(0x1000, 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());

    EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
        << "branch diverged for a=0x" << std::hex << a << " b=0x" << b
        << ": interp rip=0x" << static_cast<u32>(icpu.rip)
        << " jit rip=0x" << static_cast<u32>(jcpu.rip);
}
}  // namespace

// Jcc rel8 opcodes 0x70..0x7F = O,NO,B,NB,Z,NZ,BE,A,S,NS,P,NP,L,GE,LE,G.
TEST_F(JitParityTest, Cmp8JccFusionAllConditions) {
    // CMP AL,BL / CMP AL,imm8. Unsigned Jcc fuse from zero-extended SUBS;
    // signed/overflow Jcc must still match the interpreter via lazy Sub8.
    const std::pair<u32, u32> pairs[] = {
        {0, 0}, {5, 5}, {5, 3}, {3, 5}, {0x7F, 0x80}, {0x80, 0x7F},
        {0xFF, 0x01}, {0x01, 0xFF}, {0x80, 0x80}, {0xFF, 0xFF},
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : pairs) {
            expect_branch_matches(*m_memory, {0x38, 0xD8, jcc, 0x02}, a, b);
            expect_branch_matches(
                *m_memory,
                {0x3C, static_cast<u8>(b), jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParityTest, CmpJccFusionAllConditions) {
    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {5, 3}, {3, 5}, {0x80000000u, 1}, {0xFFFFFFFFu, 1},
        {0, 0x80000000u}, {0x7FFFFFFFu, 0xFFFFFFFFu},
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc)
        for (auto [a, b] : pairs)
            expect_branch_matches(*m_memory, {0x39, 0xD8, jcc, 0x02}, a, b);
}

TEST_F(JitParityTest, TestJccFusionAllConditions) {
    const std::pair<u32, u32> pairs[] = {
        {0, 0}, {0xFF, 0x0F}, {0x80000000u, 0x80000000u},
        {0xFFFFFFFFu, 0x7FFFFFFFu}, {0xAAAAAAAAu, 0x55555555u},
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc)
        for (auto [a, b] : pairs)
            expect_branch_matches(*m_memory, {0x85, 0xD8, jcc, 0x02}, a, b);
}

// SUB EAX,EBX (2B C3, reg dest) -> Jcc must fuse like CMP (subtract flags).
TEST_F(JitParityTest, SubJccFusionAllConditions) {
    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {5, 3}, {3, 5}, {0x80000000u, 1}, {0xFFFFFFFFu, 1},
        {0, 0x80000000u}, {0x7FFFFFFFu, 0xFFFFFFFFu},
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc)
        for (auto [a, b] : pairs)
            expect_branch_matches(*m_memory, {0x2B, 0xC3, jcc, 0x02}, a, b);
}

// AND EAX,EBX (23 C3, reg dest) -> Jcc must fuse like TEST (AND flags).
TEST_F(JitParityTest, AndJccFusionAllConditions) {
    const std::pair<u32, u32> pairs[] = {
        {0, 0}, {0xFF, 0x0F}, {0x80000000u, 0x80000000u},
        {0xFFFFFFFFu, 0x7FFFFFFFu}, {0xAAAAAAAAu, 0x55555555u},
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc)
        for (auto [a, b] : pairs)
            expect_branch_matches(*m_memory, {0x23, 0xC3, jcc, 0x02}, a, b);
}

// INC/DEC leave ZF/SF/OF (not CF). Register INC/DEC→Jcc must fuse those
// conditions from ADDS/SUBS #1 and still honor a pre-existing CF.
TEST_F(JitParityTest, IncDecJccFusionAllConditions) {
    const u32 inc_vals[] = {
        0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
    };
    const u32 dec_vals[] = {
        0, 1, 0x80000000u, 0x80000001u, 0xFFFFFFFFu,
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (u32 a : inc_vals)
            expect_branch_matches(*m_memory, {0x40, jcc, 0x02}, a, 0);
        for (u32 a : dec_vals)
            expect_branch_matches(*m_memory, {0x48, jcc, 0x02}, a, 0);
    }
}

// -----------------------------------------------------------------------------
// A CF/OF overlay installed OVER a live lazy FlagOp, then consumed by a Jcc.
// -----------------------------------------------------------------------------
//
// This is the shape that shipped broken twice. The JIT's lazy-condition walk
// tests cf_overlay/of_overlay before its per-FlagOp fast paths; when that gate
// sat after the Sub32/And32/Add32 arms instead, `ADD; CLC; JB` recomputed CF
// from the ADD and branched on the carry the CLC had just cleared.
//
// The existing coverage did not have this shape. `IncDoesNotFusePreservedCarryIntoJb`
// is `CLC; INC; JB` — the overlay comes FIRST, so INC publishes its FlagOp
// afterwards and the gate ordering never matters. The LAHF tests do install an
// overlay over a live FlagOp, but they read it through AH, not through a
// branch, and it is the branch that regressed.
//
// check_parity compares RIP, so a wrongly-taken Jcc fails here. require_jit is
// set on every one: a JIT that quietly declined to compile these would pass by
// running the interpreter twice.
//
// Operands are chosen so the overlay and the pending FlagOp DISAGREE about CF.
// Equal values would make both answers identical and prove nothing.

TEST_F(JitParityTest, AddThenClcThenJbUsesTheOverlay) {
    // 01 D8 ADD EAX,EBX (CF=1) ; F8 CLC (CF=0) ; 72 02 JB — must NOT branch.
    check_parity({0x01, 0xD8, 0xF8, 0x72, 0x02}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFFFFFFu);
        cpu.gpr.set32(EBX, 0x00000001u);
    }, true, false, true);
}

TEST_F(JitParityTest, SubThenClcThenJbUsesTheOverlay) {
    // 29 D8 SUB EAX,EBX (borrow, CF=1) ; F8 CLC ; 72 02 JB — must NOT branch.
    check_parity({0x29, 0xD8, 0xF8, 0x72, 0x02}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x00000000u);
        cpu.gpr.set32(EBX, 0x00000001u);
    }, true, false, true);
}

// The condition-TRUE direction is covered with SETcc rather than Jcc.
//
// check_parity runs a block to completion and compares final state, so a taken
// branch leaves the block and the harness reports "interpreter failed" instead
// of comparing anything. SETcc and CMOVcc reach the same code:
// emit_eval_lazy_condition() is the single walk behind Jcc (jit_compiler.cpp),
// SETcc (ops/memory_ops.cpp) and CMOVcc (ops/ops64.cpp), and the overlay gate
// being tested is inside it. So this exercises the same gate with the opposite
// answer, and the result lands in a register the harness can see.

TEST_F(JitParityTest, AndThenStcThenSetbUsesTheOverlay) {
    // 21 D8 AND EAX,EBX (ANDS clears CF) ; F9 STC (CF=1) ; 0F 92 C0 SETB AL.
    // The opposite polarity of the ADD/CLC case: a walk that ignored the
    // overlay would answer 0 here and 1 there, so the pair pins both.
    check_parity({0x21, 0xD8, 0xF9, 0x0F, 0x92, 0xC0}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x0000FF00u);
        cpu.gpr.set32(EBX, 0x00FF00FFu);
    }, true, false, true);
}

TEST_F(JitParityTest, AddThenClcThenCmovbUsesTheOverlay) {
    // 01 D8 ADD (CF=1) ; F8 CLC (CF=0) ; 0F 42 C1 CMOVB EAX,ECX — the third
    // consumer of the same walk. CF is 0, so the move must NOT happen.
    check_parity({0x01, 0xD8, 0xF8, 0x0F, 0x42, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFFFFFFu);
        cpu.gpr.set32(EBX, 0x00000001u);
        cpu.gpr.set32(ECX, 0xDEADBEEFu);
    }, true, false, true);
}

TEST_F(JitParityTest, AddThenCmcThenJbUsesTheOverlay) {
    // 01 D8 ADD (CF=1) ; F5 CMC (CF=0) ; 72 02 JB — must NOT branch.
    check_parity({0x01, 0xD8, 0xF5, 0x72, 0x02}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFFFFFFu);
        cpu.gpr.set32(EBX, 0x00000001u);
    }, true, false, true);
}

TEST_F(JitParityTest, AddThenBtThenJbUsesTheOverlay) {
    // 01 D8 ADD (CF=1) ; 0F A3 D8 BT EAX,EBX ; 72 02 JB.
    // BT is a partial-flag writer: it installs a CF overlay from the selected
    // bit while leaving the ADD's FlagOp live. Sum is 0x8000_0000 and EBX
    // selects bit 3, which is 0 — so CF becomes 0 and JB must not branch,
    // disagreeing with the ADD's carry.
    check_parity({0x01, 0xD8, 0x0F, 0xA3, 0xD8, 0x72, 0x02},
                 [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFFFFFFu);
        cpu.gpr.set32(EBX, 0x00000003u);
    }, true, false, true);
}

TEST_F(JitParityTest, AddThenRorThenSetoReadsOfOverlay) {
    // 01 D8 ADD ; D1 C8 ROR EAX,1 ; 0F 90 C0 SETO AL — the OF overlay.
    // ROR by 1 defines OF as the XOR of the two top result bits and leaves the
    // ADD's FlagOp live, so SETO must answer from the overlay.
    //
    // Unlike the CF cases above, this one does NOT reach the JIT's inline
    // overlay gate: planting a defect in either gate (the generic walk in
    // emit_eval_lazy_condition and the predicted walk in
    // emit_eval_lazy_condition_predicted) leaves this test passing, so it is
    // answered by the cond helper, which reads the overlay in C++. Kept
    // because SETO-after-ROR is worth pinning either way, and named for what
    // it actually pins rather than for the gate it does not reach.
    check_parity({0x01, 0xD8, 0xD1, 0xC8, 0x0F, 0x90, 0xC0},
                 [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x7FFFFFFFu);
        cpu.gpr.set32(EBX, 0x00000001u);
    }, true, false, true);
}

// GAP CLOSED. This block previously recorded that the overlay gate in
// emit_eval_lazy_condition_predicted had no coverage: every overlay writer
// dropped the static kind prediction, so every consumer took the generic walk
// and planting a defect in the predicted walk's gate failed nothing.
//
// That conservatism was then measured and removed — CLC and STC leave the
// lazy record's kind byte alone, so the prediction now survives them. The
// consequence for this file is that five of the tests above (the CLC/STC
// ones) now exercise the PREDICTED walk instead of the generic one, and
// planting a defect in its gate fails them. The gate is load-bearing and
// covered.
//
// The remaining overlay writers — CMC, BT/BTS/BTR/BTC, the rotates — still
// drop the prediction and still route to the generic walk. If any of those is
// later exempted the same way, check that its lowering leaves flag_op alone
// first, and re-plant the predicted gate to confirm coverage followed.

TEST_F(JitParityTest, ClcOverlaySurvivesAFlagNeutralInstruction) {
    // 01 D8 ADD (CF=1) ; F8 CLC ; 89 CA MOV EDX,ECX ; 72 02 JB.
    // The MOV must not be treated as clearing the overlay: the gate has to
    // still fire one instruction later. This is the same shape as the deferred
    // lazy-flag store that a fused forward Jcc once dropped.
    check_parity({0x01, 0xD8, 0xF8, 0x89, 0xCA, 0x72, 0x02},
                 [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFFFFFFu);
        cpu.gpr.set32(EBX, 0x00000001u);
        cpu.gpr.set32(ECX, 0x12345678u);
    }, true, false, true);
}

TEST_F(JitParityTest, IncDoesNotFusePreservedCarryIntoJb) {
    // CLC; INC EAX; JB +2 — INC of 0xFFFFFFFF sets ARM C, but x86 CF stays 0.
    JitCompiler jit(*m_memory);
    const u8 code[] = {0xF8, 0x40, 0x72, 0x02};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    MemoryByteReader reader(code, sizeof(code));
    BasicBlock block;
    block.start_addr = 0x1000;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success)
            break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, 3u);

    auto setup = [](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set32(EAX, 0xFFFFFFFFu);
    };
    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(3);
    }
    CpuState jcpu;
    setup(jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadExecute)
                    .is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadWrite)
                    .is_ok());
    icpu.materialize_lazy_flags();
    jcpu.materialize_lazy_flags();
    EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip));
    EXPECT_FALSE(jcpu.flags.cf()) << "INC must preserve CF=0 across JB";
}

// Mid-trace forward Jcc (CMP; Jcc; MOV) must fuse from NZCV the same way a
// terminal Jcc does. Genome-style traces keep the fall-through in the block;
// a wrong CSET would skip or execute the MOV when the interpreter does not.
TEST_F(JitParityTest, CmpMidTraceJccFusionAllConditions) {
    const std::pair<u32, u32> pairs[] = {
        {5, 5}, {5, 3}, {3, 5}, {0x80000000u, 1}, {0xFFFFFFFFu, 1},
        {0, 0x80000000u}, {0x7FFFFFFFu, 0xFFFFFFFFu},
    };
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : pairs) {
            JitCompiler jit(*m_memory);
            // CMP EAX,EBX ; Jcc +5 ; MOV ECX,1
            const u8 code[] = {0x39, 0xD8, jcc, 0x05, 0xB9, 0x01, 0x00, 0x00,
                               0x00};
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
            MemoryByteReader reader(code, sizeof(code));
            BasicBlock block;
            block.start_addr = 0x1000;
            while (reader.remaining() > 0) {
                DecodedInstruction inst;
                if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
                    DecodeError::Success)
                    break;
                block.instructions.push_back(inst);
            }
            block.byte_length = static_cast<u32>(reader.position());
            block.instruction_count =
                static_cast<u32>(block.instructions.size());
            ASSERT_EQ(block.instruction_count, 3u);

            auto setup = [&](CpuState& c) {
                c.init_protected_mode();
                c.gpr.set32(ESP, 0x10000);
                c.rip = 0x1000;
                c.gpr.set32(EAX, a);
                c.gpr.set32(EBX, b);
                c.gpr.set32(ECX, 0);
            };
            CpuState icpu;
            setup(icpu);
            {
                Interpreter interp(icpu, *m_memory);
                (void)interp.run(2);
                if (icpu.rip == 0x1004) (void)interp.run(1);
            }
            CpuState jcpu;
            setup(jcpu);
            ASSERT_TRUE(m_memory
                            ->protect_region(0x1000, 0x1000,
                                             MemoryProtection::ReadExecute)
                            .is_ok());
            CompiledBlock* cb = jit.compile(block);
            ASSERT_NE(cb, nullptr);
            (void)test::execute_like_dispatcher(jit, *cb, jcpu);
            ASSERT_TRUE(m_memory
                            ->protect_region(0x1000, 0x1000,
                                             MemoryProtection::ReadWrite)
                            .is_ok());

            EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
                << "mid-trace Jcc 0x" << std::hex << static_cast<unsigned>(jcc)
                << " diverged for a=0x" << a << " b=0x" << b
                << ": interp rip=0x" << static_cast<u32>(icpu.rip)
                << " jit rip=0x" << static_cast<u32>(jcpu.rip);
            EXPECT_EQ(icpu.gpr.get32(ECX), jcpu.gpr.get32(ECX))
                << "mid-trace Jcc 0x" << std::hex << static_cast<unsigned>(jcc)
                << " MOV ECX mismatch for a=0x" << a << " b=0x" << b;
        }
    }
}

// A CMP whose flags are consumed by a LATER reader (not the immediate next
// instruction) must still be correct — fusion must not have "eaten" the flags.
TEST_F(JitParityTest, CmpThenNonJccThenReadsFlags) {
    // CMP EAX,EBX ; MOV ECX,1 ; SETB DL (0F 92 C2) — SETB reads CF from the CMP.
    check_parity({0x39, 0xD8, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x92, 0xC2},
        [](CpuState& c) { c.gpr.set32(EAX, 3); c.gpr.set32(EBX, 5); });
}

// ===========================================================================
// Flag-materialization cost (perf probe, disabled by default)
//
// Measures the exact shape that makes flag handling dominate real guest CPU:
// a flag-setting ALU op, an intervening instruction that invalidates the
// CMP/TEST -> Jcc NZCV fusion, then a conditional branch. The branch has to go
// through the cond helper, which used to materialize all six x86 flags to read
// one bit. Timing-based, so it is DISABLED_ and run explicitly:
//
//   ./build/tests/vitreo_cpu_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='*FlagCondHelperThroughput*'
// ===========================================================================
TEST(JitFlagPerf, DISABLED_FlagCondHelperThroughput) {
    // loop: add eax, ecx ; mov ebx, esi ; jnz loop
    static const u8 kCode[] = {0x01, 0xC8, 0x89, 0xF3, 0x75, 0xFA};

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWriteExecute).is_ok());
    ASSERT_TRUE(memory.write_bytes(0x1000, kCode, sizeof(kCode)));

    BasicBlock block;
    block.start_addr = 0x1000;
    MemoryByteReader reader(kCode, sizeof(kCode));
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, 3u);

    JitCompiler jit(memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);

    constexpr u32 kIterations = 40'000'000u;
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP, 0x10000);
    cpu.rip = 0x1000;
    cpu.gpr.set32(EAX, 0u - kIterations);  // counts up to zero
    cpu.gpr.set32(ECX, 1);
    cpu.gpr.set32(ESI, 0x12345678);

    const auto t0 = std::chrono::steady_clock::now();
    u64 dispatches = 0;
    while (cpu.gpr.get32(EAX) != 0) {
        ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, cpu), JitExecResult::Continue);
        cpu.rip = 0x1000;
        ++dispatches;
        ASSERT_LT(dispatches, kIterations) << "loop not making progress";
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double secs =
        std::chrono::duration<double>(t1 - t0).count();
    std::printf("cond-helper loop: %u iterations in %.3f s = %.1f M iter/s "
                "(%llu dispatches)\n",
                kIterations, secs, kIterations / secs / 1e6,
                static_cast<unsigned long long>(dispatches));
    EXPECT_EQ(cpu.gpr.get32(EBX), 0x12345678u);
}

// Intel SDM Vol. 3A 6.6: a fault is precise -- RIP names the faulting
// instruction and the state is the state before it. A memory operand that
// faults after a flag writer in the same block must report its own RIP, and
// the flags the earlier writer produced, even when the JIT elided that
// writer's flag record because the faulting instruction overwrites it.
TEST_F(JitParityTest, FaultingMemoryKillerKeepsThePriorWritersFlags) {
    struct Form { const char* name; std::vector<u8> bytes; };
    const Form forms[] = {
        {"add; cmp m32", {0x83, 0xC0, 0x01, 0x83, 0x3B, 0x00, 0x74, 0x00}},
        {"add; load; cmp m32", {0x83, 0xC0, 0x01, 0x8B, 0x11, 0x83, 0x3B, 0x00, 0x74, 0x00}},
        {"add; comiss m32", {0x83, 0xC0, 0x01, 0x0F, 0x2F, 0x03, 0x77, 0x00}},
        // Every checked-helper width and direction of the region model.
        {"add; cmp m8", {0x83, 0xC0, 0x01, 0x80, 0x3B, 0x00, 0x74, 0x00}},
        {"add; cmp m16", {0x83, 0xC0, 0x01, 0x66, 0x83, 0x3B, 0x00, 0x74, 0x00}},
        {"add; store m32; cmp", {0x83, 0xC0, 0x01, 0x89, 0x0B, 0x83, 0xF9, 0x00, 0x74, 0x00}},
        {"add; store m16; cmp", {0x83, 0xC0, 0x01, 0x66, 0x89, 0x0B, 0x83, 0xF9, 0x00, 0x74, 0x00}},
        {"add; store m8; cmp", {0x83, 0xC0, 0x01, 0x88, 0x0B, 0x83, 0xF9, 0x00, 0x74, 0x00}},
        {"add; movq xmm0, m64; cmp", {0x83, 0xC0, 0x01, 0xF3, 0x0F, 0x7E, 0x03, 0x83, 0xF9, 0x00, 0x74, 0x00}},
        {"add; movq m64, xmm0; cmp", {0x83, 0xC0, 0x01, 0x66, 0x0F, 0xD6, 0x03, 0x83, 0xF9, 0x00, 0x74, 0x00}},
    };
    for (const Form& form : forms) {
        SCOPED_TRACE(form.name);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, form.bytes.data(), form.bytes.size()));
        BasicBlock block = build_block(form.bytes.data(), form.bytes.size());
        const auto setup = [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFFFFFFu); cpu.gpr.set32(ECX, 0x2000u);
            cpu.gpr.set32(EBX, 0x11000u); cpu.flags.rflags = 0x2;
        };
        CpuState icpu; init_cpu(icpu); setup(icpu);
        { Interpreter interp(icpu, *m_memory); (void)interp.run(block.instruction_count); }
        icpu.materialize_lazy_flags();
        ASSERT_TRUE(icpu.flags.zf());
        CpuState jcpu; init_cpu(jcpu); setup(jcpu);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        jcpu.materialize_lazy_flags();
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip));
        for (int i = 0; i < 8; i++) EXPECT_EQ(icpu.gpr.get32(i), jcpu.gpr.get32(i)) << gpr_name(i);
        EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()); EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf());
        EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()); EXPECT_EQ(icpu.flags.of(), jcpu.flags.of());
        EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()); EXPECT_EQ(icpu.flags.af(), jcpu.flags.af());
    }
}
