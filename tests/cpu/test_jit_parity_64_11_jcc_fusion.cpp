// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): compare/Jcc
// fusion.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// Long-mode compare/Jcc fusion
//
// A terminal Jcc reads the ARM64 flags left by the preceding REX.W
// CMP/TEST/SUB/AND/ADD instead of calling a cond helper that rebuilds the
// condition from lazy flag state. The branch decision must be identical to the
// interpreter's for every x86 condition code, including the ones ARM64 cannot
// express directly (parity) and the ones whose carry sense differs between
// ADDS and SUBS. Operand pairs straddle the 64-bit signed/unsigned boundaries,
// where a fused condition derived from the wrong flag polarity diverges.
// ============================================================================

namespace {

// Run a {compare; Jcc +2} 64-bit block through both engines; the branch
// decision (final RIP) must match. check_parity is unusable here: a taken
// branch leaves RIP off the linear path it expects.
void expect_branch_matches64(GuestMemory& mem,
                             std::initializer_list<u8> code_il,
                             u64 a, u64 b,
                             u32 expect_insns = 2, u64 expected_rip = 0) {
    // Fresh JitCompiler per call: a shared one trips the churn recompile limit
    // after repeated compiles of the same address.
    JitCompiler jit(mem);
    std::vector<u8> code(code_il);
    ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
    MemoryByteReader reader(code.data(), code.size());
    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = true;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode64, inst)
                != DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, expect_insns);

    auto setup = [&](CpuState& c) {
        c.init_long_mode();
        c.gpr.set64(RSP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set64(RAX, a);
        c.gpr.set64(RBX, b);
        c.gpr.set64(RCX, 0x2000);  // data pointer for memory-operand forms
    };
    // Both engines must start from identical memory: a read-modify-write form
    // applied by the interpreter would otherwise leave the JIT operating on a
    // different value, and the branch would diverge for that reason alone.
    u8 data_before[16] = {};
    ASSERT_TRUE(mem.read_bytes(0x2000, data_before, sizeof(data_before)));

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, mem);
        (void)interp.run(block.instruction_count);
    }
    u8 data_after_interp[16] = {};
    ASSERT_TRUE(mem.read_bytes(0x2000, data_after_interp,
                               sizeof(data_after_interp)));

    ASSERT_TRUE(mem.write_bytes(0x2000, data_before, sizeof(data_before)));
    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);

    EXPECT_EQ(icpu.rip, jcpu.rip)
        << "branch diverged for a=0x" << std::hex << a << " b=0x" << b
        << ": interp rip=0x" << icpu.rip << " jit rip=0x" << jcpu.rip;
    if (expected_rip != 0) EXPECT_EQ(jcpu.rip, expected_rip);

    u8 data_after_jit[16] = {};
    ASSERT_TRUE(mem.read_bytes(0x2000, data_after_jit,
                               sizeof(data_after_jit)));
    EXPECT_EQ(std::memcmp(data_after_interp, data_after_jit,
                          sizeof(data_after_jit)), 0)
        << "memory diverged for a=0x" << std::hex << a << " b=0x" << b;
}

// Values chosen to exercise carry, borrow, signed overflow and the sign bit.
constexpr std::pair<u64, u64> kSubPairs[] = {
    {5, 5}, {5, 3}, {3, 5},
    {0x8000000000000000ull, 1},
    {0xFFFFFFFFFFFFFFFFull, 1},
    {0, 0x8000000000000000ull},
    {0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
    {0x100000000ull, 0xFFFFFFFFull},  // differs only above bit 31
};

constexpr std::pair<u64, u64> kLogicPairs[] = {
    {0, 0},
    {0xFFull, 0x0Full},
    {0x8000000000000000ull, 0x8000000000000000ull},
    {0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull},
    {0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull},
    {0x100000000ull, 0x100000000ull},  // zero in the low 32 bits only
};

}  // namespace

// Jcc rel8 opcodes 0x70..0x7F = O,NO,B,NB,Z,NZ,BE,A,S,NS,P,NP,L,GE,LE,G.
TEST_F(JitParity64Test, CmpJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSubPairs) {
            // CMP RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x39, 0xD8, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, Cmp8JccFusionAllConditions) {
    // Intel SDM CMP/Sub flags: unsigned conditions use borrow/zero;
    // signed, overflow and parity consumers must retain the 8-bit result.
    for (u8 jcc = 0x70; jcc <= 0x7f; ++jcc) {
        for (u64 a : {0u, 1u, 0x7fu, 0x80u, 0xffu}) {
            for (u64 b : {0u, 1u, 0x7fu, 0x80u, 0xffu}) {
                // Source-derived arithmetic and Intel SDM Jcc condition table,
                // independent of both engines' lazy flag representation.
                const u8 result = static_cast<u8>(a - b);
                const bool cf = a < b, zf = result == 0, sf = (result & 0x80) != 0;
                const int signed_result = static_cast<i8>(a) - static_cast<i8>(b);
                const bool of = signed_result < -128 || signed_result > 127;
                bool pf = true;
                for (u8 bits = result; bits != 0; bits >>= 1) pf ^= (bits & 1) != 0;
                const bool take[] = {of, !of, cf, !cf, zf, !zf, cf || zf,
                    !cf && !zf, sf, !sf, pf, !pf, sf != of, sf == of,
                    zf || sf != of, !zf && sf == of};
                const u64 expected = 0x1004 + (take[jcc - 0x70] ? 2 : 0);
                expect_branch_matches64(*m_memory, {0x38, 0xd8, jcc, 2}, a, b, 2, expected);
                expect_branch_matches64(*m_memory, {0x3c, static_cast<u8>(b), jcc, 2}, a, b, 2, expected);
                ASSERT_TRUE(m_memory->write_u8(0x2000, static_cast<u8>(a)));
                expect_branch_matches64(*m_memory, {0x38, 0x19, jcc, 2}, a, b, 2, expected);
                ASSERT_TRUE(m_memory->write_u8(0x2000, static_cast<u8>(b)));
                expect_branch_matches64(*m_memory, {0x3a, 0x01, jcc, 2}, a, b, 2, expected);
            }
        }
    }
}

TEST_F(JitParity64Test, SubJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSubPairs) {
            // SUB RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x2B, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

// ADDS leaves x86 CF equal to ARM64 C rather than its inverse, so the unsigned
// conditions need the opposite mapping and BE/A must not fuse at all.
TEST_F(JitParity64Test, AddJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSubPairs) {
            // ADD RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x03, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

// ANDS forces C and V to zero. x86 TEST/AND also clear CF/OF, so the signed and
// zero/sign conditions match, but the unsigned ones would invert and must stay
// on the helper.
TEST_F(JitParity64Test, TestJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kLogicPairs) {
            // TEST RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x85, 0xD8, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, AndJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kLogicPairs) {
            // AND RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x23, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

// OR/XOR have no flag-setting ARM64 form, so these must fall back to the cond
// helper and still branch identically.
TEST_F(JitParity64Test, OrXorJccStillMatchesWithoutFusion) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kLogicPairs) {
            // OR RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x0B, 0xC3, jcc, 0x02}, a, b);
            // XOR RAX, RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x33, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

// A memory source is read before the compare, so the compare's flags are still
// the live ones when the Jcc runs — even though the load's bounds check itself
// uses SUBS.
TEST_F(JitParity64Test, CmpMemSourceJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSubPairs) {
            ASSERT_TRUE(m_memory->write_u64(0x2000, b));
            // CMP RAX, [RCX] ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x3B, 0x01, jcc, 0x02}, a, b);
        }
    }
}

// The separate 64-bit memory-destination lowering does not yet retain NZCV
// across writeback, so it must use lazy flags rather than a false fusion.
TEST_F(JitParity64Test, SubMemDestJccDoesNotFuse) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSubPairs) {
            ASSERT_TRUE(m_memory->write_u64(0x2000, a));
            // SUB [RCX], RBX ; Jcc +2
            expect_branch_matches64(*m_memory,
                                    {0x48, 0x29, 0x19, jcc, 0x02}, a, b);
        }
    }
}

// Flags consumed by a later reader rather than the next instruction: fusion
// must not let the lazy flag state go unrecorded.
TEST_F(JitParity64Test, CmpThenMovThenJccMatchesInterpreter) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSubPairs) {
            // CMP RAX,RBX ; MOV RDX,RBX ; Jcc +2 — the MOV sits between the
            // producer and the branch, so the branch must not fuse.
            expect_branch_matches64(
                *m_memory,
                {0x48, 0x39, 0xD8, 0x48, 0x89, 0xDA, jcc, 0x02}, a, b,
                /*expect_insns=*/3);
        }
    }
}

// A mid-block Jcc whose taken edge leaves the block must still deliver the
// publisher's FULL lazy flags — AF and PF included — on that edge. The
// flag-liveness pass omits the publisher's lazy-flag store when a downstream
// full writer kills the flags on the fall-through path, relying on
// queue_mid_block_jcc_exit to re-emit the store on the taken edge from
// m_pending_stub_store. The 64-bit recorder (emit_set_lazy_flags64) must
// participate in that contract: NZCV kind bits carry N/Z/C/V across the edge,
// but AF and PF exist only in the lazy FlagOp record.
TEST_F(JitParity64Test, MidBlockTakenJccKeepsAfPfOfOmittedLazyStore) {
    // add rcx, rdx   ; seed flags: 0x0F + 0x01 = 0x10 -> AF=1, PF=0
    // add rax, rbx   ; publisher:  1 + 2 = 3          -> AF=0, PF=1, ZF=0
    // jnz +5         ; TAKEN, escapes the block
    // xor rcx, rcx   ; full flag writer on the fall-through only, which lets
    //                ; liveness omit the publisher's lazy store
    const u8 code[] = {
        0x48, 0x01, 0xD1,   // add rcx, rdx
        0x48, 0x01, 0xD8,   // add rax, rbx
        0x75, 0x05,         // jnz +5 (target 0x100D, outside the block)
        0x48, 0x31, 0xC9,   // xor rcx, rcx
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 4u);

    auto setup = [&](CpuState& c) {
        init_cpu(c);
        c.gpr.set64(RAX, 1);
        c.gpr.set64(RBX, 2);
        c.gpr.set64(RCX, 0x0F);
        c.gpr.set64(RDX, 0x01);
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        // Taken path executes exactly 3 instructions before leaving the block.
        interp.run(3);
    }
    icpu.materialize_lazy_flags();
    ASSERT_EQ(icpu.rip, 0x100Du) << "interpreter did not take the branch";
    ASSERT_FALSE(icpu.flags.af());
    ASSERT_TRUE(icpu.flags.pf());

    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    test::execute_like_dispatcher(*m_jit, *cb, jcpu);
    jcpu.materialize_lazy_flags();

    EXPECT_EQ(icpu.rip, jcpu.rip) << std::hex << "interp rip=0x" << icpu.rip
                                  << " jit rip=0x" << jcpu.rip;
    EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << "CF diverged";
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << "ZF diverged";
    EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << "SF diverged";
    EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << "OF diverged";
    EXPECT_EQ(icpu.flags.af(), jcpu.flags.af()) << "AF diverged (stale from"
        " the seed ADD: the omitted lazy store never reached the taken edge)";
    EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << "PF diverged (stale from"
        " the seed ADD: the omitted lazy store never reached the taken edge)";
}

// A forward Jcc whose target is INSIDE the block only exists once trace
// formation is on (VITREO_JIT_TRACES64). The join must preserve architectural
// lazy flags for either predecessor and must not reuse fall-through-only host
// NZCV. This originally caught an omitted-store bug when taken edges were cold
// exits; it now compares the complete internally-branched trace instead.
TEST_F(JitParity64Test, TracedForwardJccIntoBlockKeepsPublisherFlags) {
    ScopedEnvironmentVariable traces("VITREO_JIT_TRACES64", "1");
    m_jit = std::make_unique<JitCompiler>(*m_memory);

    // The publisher is deliberately NOT adjacent to the branch: a flag-neutral
    // MOV sits between them. The one-instruction pending-stub-store deferral
    // only reaches an immediately-following Jcc, so adjacency HIDES this bug —
    // the first version of this test was adjacent, passed against the restored
    // bug, and proved nothing. The real Steam failure had exactly this gap
    // (CMP at ea22, MOV at ea27, JB at ea2b).
    //
    //   0x1000 add rcx,rdx  seed: 0x0F+0x01 -> AF=1 PF=0
    //   0x1003 add rax,rbx  PUBLISHER: 1+2=3 -> AF=0 PF=1
    //   0x1006 mov rsi,rdi  flag-neutral, breaks deferral adjacency
    //   0x1009 jnz +3       TAKEN -> 0x100E, a target INSIDE the block
    //   0x100B xor rcx,rcx  fall-through killer (skipped when taken)
    //   0x100E mov r8,r9    branch target: neutral, so the publisher's flags
    //                       are still observable at the exit
    //   0x1011 xor rdx,rdx  the in-block killer the old model wrongly trusted
    const u8 code[] = {
        0x48, 0x01, 0xD1,
        0x48, 0x01, 0xD8,
        0x48, 0x89, 0xFE,
        0x75, 0x03,
        0x48, 0x31, 0xC9,
        0x4D, 0x89, 0xC8,
        0x48, 0x31, 0xD2,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 7u)
        << "the traced shape under test was not built";

    auto setup = [&](CpuState& c) {
        init_cpu(c);
        c.gpr.set64(RAX, 1);
        c.gpr.set64(RBX, 2);
        c.gpr.set64(RCX, 0x0F);
        c.gpr.set64(RDX, 0x01);
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(6);   // taken branch skips XOR RCX, then runs target + XOR
    }
    icpu.materialize_lazy_flags();
    ASSERT_EQ(icpu.rip, 0x1014u) << "interpreter did not finish the trace";

    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    test::execute_like_dispatcher(*m_jit, *cb, jcpu);
    jcpu.materialize_lazy_flags();

    EXPECT_EQ(jcpu.rip, icpu.rip) << "JIT did not finish the internal target";
    EXPECT_EQ(icpu.flags.af(), jcpu.flags.af()) << "AF diverged at join";
    EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << "PF diverged at join";
    EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << "CF diverged";
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << "ZF diverged";
    EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << "SF diverged";
    EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << "OF diverged";
}

// A 16/32-way unrolled loop only amortizes its edge overhead if trace decode
// reaches the counted backedge. This shape crosses both former limits: 17
// forward Jccs and 70 total instructions, followed by a taken backward latch.
TEST_F(JitParity64Test, LongTraceReachesBackwardLatchPastFormerLimits) {
    std::vector<u8> code;
    for (u32 i = 0; i < 17; ++i) {
        code.insert(code.end(), {
            0x48, 0xFF, 0xC0,       // inc rax
            0x48, 0x85, 0xC9,       // test rcx,rcx
            0x75, 0x01,             // jnz +1 (forward, skips the nop)
            0x90,                   // nop
        });
    }
    code.insert(code.end(), {
        0xFF, 0xCA,                 // dec edx
        0x0F, 0x85,                 // jnz rel32 (back to block start)
    });
    const i32 loop_disp = -static_cast<i32>(code.size() + sizeof(i32));
    for (u32 byte = 0; byte < sizeof(loop_disp); ++byte) {
        code.push_back(static_cast<u8>(
            static_cast<u32>(loop_disp) >> (byte * 8)));
    }

    ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
    BasicBlock block = cli::decode_basic_block(*m_memory, 0x1000, true);
    ASSERT_EQ(block.instruction_count, 70u)
        << "trace stopped at the former 64-instruction or 8-Jcc bound";
    ASSERT_EQ(block.byte_length, code.size());
    ASSERT_EQ(block.instructions[block.instruction_count - 1].opcode,
              Opcode::JNZ_REL32);

    auto setup = [&](CpuState& cpu) {
        init_cpu(cpu);
        cpu.gpr.set64(RAX, 3);
        cpu.gpr.set64(RCX, 0);  // all forward branches fall through
        cpu.gpr.set64(RDX, 2);  // latch decrements to one and branches back
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Branch);
        ASSERT_EQ(icpu.rip, 0x1000u);
        ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
    }

    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    test::execute_like_dispatcher(*m_jit, *compiled, jcpu);

    EXPECT_EQ(jcpu.rip, icpu.rip);
    EXPECT_EQ(jcpu.gpr.get64(RAX), icpu.gpr.get64(RAX));
    EXPECT_EQ(jcpu.gpr.get64(RCX), icpu.gpr.get64(RCX));
    EXPECT_EQ(jcpu.gpr.get64(RDX), icpu.gpr.get64(RDX));
    EXPECT_EQ(jcpu.rip, 0x1000u + code.size());
    EXPECT_EQ(jcpu.gpr.get64(RAX), 37u);
    EXPECT_EQ(jcpu.gpr.get64(RDX), 0u);
}

// ---------------------------------------------------------------------------
// 32-bit-operand compares inside a long-mode block. These dominate real x64
// code, and their flags are 32-bit-wide: the fused condition must come from a
// 32-bit ARM64 compare, so operands whose low halves agree while their upper
// halves differ (and vice versa) are the discriminating cases.
// ---------------------------------------------------------------------------

namespace {

constexpr std::pair<u64, u64> kSub32Pairs[] = {
    {5, 5}, {5, 3}, {3, 5},
    {0x80000000ull, 1},
    {0xFFFFFFFFull, 1},
    {0, 0x80000000ull},
    {0x7FFFFFFFull, 0xFFFFFFFFull},
    // Upper halves differ, low halves are equal: a 32-bit compare must report
    // equal. A 64-bit compare here reports not-equal.
    {0xFFFFFFFF00000005ull, 0x1111111100000005ull},
    // Upper halves equal, low halves differ across the 32-bit sign boundary.
    {0xABCDEF0080000000ull, 0xABCDEF007FFFFFFFull},
    // Low half zero with a non-zero upper half: ZF must come from the low half.
    {0x1234567800000000ull, 0x8765432100000000ull},
};

}  // namespace

TEST_F(JitParity64Test, Cmp32JccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            // CMP EAX, EBX ; Jcc +2
            expect_branch_matches64(*m_memory, {0x39, 0xD8, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, Sub32JccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            // SUB EAX, EBX ; Jcc +2
            expect_branch_matches64(*m_memory, {0x2B, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, Add32JccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            // ADD EAX, EBX ; Jcc +2
            expect_branch_matches64(*m_memory, {0x03, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, Test32JccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            // TEST EAX, EBX ; Jcc +2
            expect_branch_matches64(*m_memory, {0x85, 0xD8, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, And32JccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            // AND EAX, EBX ; Jcc +2
            expect_branch_matches64(*m_memory, {0x23, 0xC3, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, Cmp32MemSourceJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            ASSERT_TRUE(m_memory->write_u32(0x2000, static_cast<u32>(b)));
            // CMP EAX, [RCX] ; Jcc +2
            expect_branch_matches64(*m_memory, {0x3B, 0x01, jcc, 0x02}, a, b);
        }
    }
}

TEST_F(JitParity64Test, Sub32MemDestJccFusionAllConditions) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (auto [a, b] : kSub32Pairs) {
            ASSERT_TRUE(m_memory->write_u32(0x2000, static_cast<u32>(a)));
            // SUB [RCX], EBX ; Jcc +2
            expect_branch_matches64(*m_memory, {0x29, 0x19, jcc, 0x02}, a, b);
        }
    }
}
