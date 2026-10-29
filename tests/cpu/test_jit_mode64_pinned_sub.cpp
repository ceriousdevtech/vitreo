// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — x64 pinned-GPR SUB and self-test flag reuse

#include "jit_exec_test_fixture.hpp"

// The word-level assertions describe the stencil lowering. When Tier 1 took
// the block (VITREO_JIT_IR), its registers and flag representation are its
// own, so only the interpreter-checked results apply.

TEST_F(JitExecTest, Mode64PinnedSubSelfTestJnsUsesImmediateNzcv) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    constexpr u8 bx = static_cast<u8>(RegIndex::RBX);

    struct ExecCase {
        u32 initial;
        u32 result;
        bool taken;
    };
    struct ImmediateCase {
        u8 encoded;
        bool negative;
        std::array<ExecCase, 2> executions;
    };
    const ImmediateCase immediate_cases[] = {
        {0x01, false,
         {{{0x80000000u, 0x7FFFFFFFu, true},
           {0u, 0xFFFFFFFFu, false}}}},
        {0xFF, true,
         {{{0u, 1u, true},
           {0x7FFFFFFFu, 0x80000000u, false}}}},
    };

    for (const ImmediateCase& immediate : immediate_cases) {
        const u8 code[] = {
            0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
            0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
            0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
            0x40, 0x89, 0xDB,
            0x40, 0x83, 0xEB, immediate.encoded,  // sub ebx,imm8
            0x40, 0x85, 0xDB,                    // test ebx,ebx
            0x79, 0x05,                          // jns block end
            0xB8, 0x78, 0x56, 0x34, 0x12,       // fallthrough marker
        };
        BasicBlock block = build_block64(code, sizeof(code));
        ASSERT_EQ(block.instruction_count, 11u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);

        JitFaultMetadata metadata{};
        ASSERT_TRUE(jit_fault_metadata_for_pc(
            reinterpret_cast<uintptr_t>(compiled->code) +
                compiled->link_code_offset,
            metadata));
        const u8 bx_pin = metadata.pins.host_reg[bx];
        ASSERT_GE(bx_pin, 23u);
        ASSERT_LE(bx_pin, 28u);

        const auto* words = static_cast<const u32*>(compiled->code);
        const usize word_count = compiled->code_size / sizeof(u32);
        const u32 expected_immediate = immediate.negative
                                           ? arm64::adds_w_imm(bx_pin, bx_pin, 1)
                                           : arm64::subs_w_imm(bx_pin, bx_pin, 1);
        const u32 opposite_immediate = immediate.negative
                                           ? arm64::subs_w_imm(bx_pin, bx_pin, 1)
                                           : arm64::adds_w_imm(bx_pin, bx_pin, 1);
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count, expected_immediate)),
                  1u);
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count, opposite_immediate)),
                  0u);
        if (immediate.negative) {
            const std::array<u32, 3> materialized_sub_sequence = {
                arm64::movz_w(REG_SCRATCH1, 0xFFFF),
                arm64::movk_w(REG_SCRATCH1, 0xFFFF, 16),
                arm64::subs_w(bx_pin, bx_pin, REG_SCRATCH1),
            };
            EXPECT_EQ(std::search(words, words + word_count,
                                  materialized_sub_sequence.begin(),
                                  materialized_sub_sequence.end()),
                      words + word_count);
        } else {
            const std::array<u32, 2> materialized_sub_sequence = {
                arm64::movz_w(REG_SCRATCH1, 1),
                arm64::subs_w(bx_pin, bx_pin, REG_SCRATCH1),
            };
            EXPECT_EQ(std::search(words, words + word_count,
                                  materialized_sub_sequence.begin(),
                                  materialized_sub_sequence.end()),
                      words + word_count);
        }
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count,
                      arm64::ands_w(REG_SCRATCH0, bx_pin, bx_pin))),
                  0u);
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count,
                      arm64::ands_w(REG_SCRATCH0, REG_SCRATCH0,
                                    REG_SCRATCH1))),
                  0u);
        EXPECT_NE(std::find_if(words, words + word_count, [](u32 word) {
                      return (word & 0xFF00001Fu) ==
                             (0x54000000u | arm64::PL);
                  }),
                  words + word_count);

        for (const ExecCase& c : immediate.executions) {
            auto setup = [&](CpuState& cpu) {
                cpu.init_long_mode();
                cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
                cpu.gpr.r[bx] = 0xAAAAAAAA00000000ull | c.initial;
                cpu.gpr.set32(EAX, 0xCAFEBABEu);
                cpu.rip = 0x1000;
                cpu.flags.rflags = 0x2u;
            };

            CpuState reference;
            setup(reference);
            {
                Interpreter interpreter(reference, *m_memory);
                (void)interpreter.run(block.instruction_count);
            }
            reference.materialize_lazy_flags();

            setup(*m_cpu);
            ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu),
                      JitExecResult::Continue);
            m_cpu->materialize_lazy_flags();

            EXPECT_EQ(m_cpu->gpr.r[bx], c.result);
            EXPECT_EQ(m_cpu->gpr.r[bx], reference.gpr.r[bx]);
            EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
            EXPECT_EQ(m_cpu->gpr.get32(EAX),
                      c.taken ? 0xCAFEBABEu : 0x12345678u);
            EXPECT_EQ(m_cpu->rip, reference.rip);
            EXPECT_EQ(m_cpu->rip, 0x1023u);
            EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
            EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
            EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
            EXPECT_FALSE(m_cpu->flags.cf());
            EXPECT_FALSE(m_cpu->flags.of());
            EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
            EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
        }
    }
}

TEST_F(JitExecTest, Mode64PinnedNegativeSubWithoutSelfTestKeepsLazySource) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    constexpr u8 bx = static_cast<u8>(RegIndex::RBX);

    const u8 code[] = {
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB,
        0x40, 0x83, 0xEB, 0xFF,  // sub ebx,-1
        0x40, 0x89, 0xD8,        // mov eax,ebx
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 9u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    JitFaultMetadata metadata{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(
        reinterpret_cast<uintptr_t>(compiled->code) + compiled->link_code_offset,
        metadata));
    const u8 bx_pin = metadata.pins.host_reg[bx];
    ASSERT_GE(bx_pin, 23u);
    ASSERT_LE(bx_pin, 28u);

    if (!m_jit->ir_emit_active()) {
        const auto* words = static_cast<const u32*>(compiled->code);
        const usize word_count = compiled->code_size / sizeof(u32);
        // -1 is one MOVN (emit_mov_imm32); the point is that the source is
        // materialised in a register the lazy record can name.
        const std::array<u32, 2> materialized_sub_sequence = {
            arm64::movn_w(REG_SCRATCH1, 0, 0),
            arm64::subs_w(bx_pin, bx_pin, REG_SCRATCH1),
        };
        EXPECT_NE(std::search(words, words + word_count,
                              materialized_sub_sequence.begin(),
                              materialized_sub_sequence.end()),
                  words + word_count);
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count,
                      arm64::adds_w_imm(bx_pin, bx_pin, 1))),
                  0u);
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count,
                      arm64::subs_w_imm(bx_pin, bx_pin, 1))),
                  0u);
    }

    for (const u32 initial : {0u, 0x7FFFFFFFu}) {
        auto setup = [&](CpuState& cpu) {
            cpu.init_long_mode();
            cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
            cpu.gpr.r[bx] = 0xAAAAAAAA00000000ull | initial;
            cpu.rip = 0x1000;
            cpu.flags.rflags = 0x2u;
        };

        CpuState reference;
        setup(reference);
        {
            Interpreter interpreter(reference, *m_memory);
            (void)interpreter.run(block.instruction_count);
        }
        reference.materialize_lazy_flags();

        setup(*m_cpu);
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
        m_cpu->materialize_lazy_flags();

        EXPECT_EQ(m_cpu->gpr.r[bx], reference.gpr.r[bx]);
        EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
        EXPECT_EQ(m_cpu->rip, reference.rip);
        EXPECT_EQ(m_cpu->rip, 0x101Cu);
        EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
        EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
        EXPECT_EQ(m_cpu->flags.af(), reference.flags.af());
        EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
        EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
        EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
    }
}
