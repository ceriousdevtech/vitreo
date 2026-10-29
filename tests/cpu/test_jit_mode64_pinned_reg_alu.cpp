// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — x64 pinned-GPR register ALU and self-test flag reuse

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, Mode64PinnedRegisterAluSelfTestJnsReusesResultNzcv) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    constexpr u8 bx = static_cast<u8>(RegIndex::RBX);
    constexpr u8 si = static_cast<u8>(RegIndex::RSI);

    struct ExecCase {
        u32 dst;
        u32 src;
        u32 result;
        bool taken;
    };
    struct AluCase {
        u8 opcode;
        u8 modrm;
        bool add;
        bool same_source;
        std::array<ExecCase, 2> executions;
    };
    const AluCase alu_cases[] = {
        {0x01, 0xF3, true, false,
         {{{0xFFFFFFFFu, 1u, 0u, true},
           {0x7FFFFFFFu, 1u, 0x80000000u, false}}}},
        {0x29, 0xF3, false, false,
         {{{1u, 1u, 0u, true},
           {0u, 1u, 0xFFFFFFFFu, false}}}},
        {0x01, 0xDB, true, true,
         {{{0u, 0u, 0u, true},
           {0x40000000u, 0x40000000u, 0x80000000u, false}}}},
        {0x29, 0xDB, false, true,
         {{{0u, 0u, 0u, true},
           {0x80000000u, 0x80000000u, 0u, true}}}},
    };

    for (const AluCase& alu : alu_cases) {
        const u8 code[] = {
            0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
            0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
            0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
            0x40, 0x89, 0xDB,
            0x40, alu.opcode, alu.modrm,  // add/sub ebx,ebx/esi
            0x40, 0x85, 0xDB,            // test ebx,ebx
            0x79, 0x05,                  // jns block end
            0xB8, 0x78, 0x56, 0x34, 0x12,
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
        const u8 si_pin = metadata.pins.host_reg[si];
        ASSERT_GE(bx_pin, 23u);
        ASSERT_LE(bx_pin, 28u);
        if (!alu.same_source) {
            ASSERT_GE(si_pin, 23u);
            ASSERT_LE(si_pin, 28u);
            ASSERT_NE(bx_pin, si_pin);
        }

        const u8 source_pin = alu.same_source ? bx_pin : si_pin;
        const u32 direct_alu = alu.add
                                   ? arm64::adds_w(bx_pin, bx_pin, source_pin)
                                   : arm64::subs_w(bx_pin, bx_pin, source_pin);
        const auto* words = static_cast<const u32*>(compiled->code);
        const usize word_count = compiled->code_size / sizeof(u32);
        // Tier 1 folds `add r,r` / `sub r,r` on its own terms (a shift, a
        // zeroing idiom); the S-form self-TEST reuse below is shared.
        if (!alu.same_source || !m_jit->ir_emit_active()) {
            EXPECT_EQ(static_cast<usize>(std::count(
                          words, words + word_count, direct_alu)),
                      1u);
        }
        if (alu.same_source) {
            const std::array<u32, 2> protective_sequence = {
                arm64::mov_w(REG_SCRATCH1, bx_pin),
                alu.add
                    ? arm64::adds_w(bx_pin, bx_pin, REG_SCRATCH1)
                    : arm64::subs_w(bx_pin, bx_pin, REG_SCRATCH1),
            };
            EXPECT_EQ(std::search(words, words + word_count,
                                  protective_sequence.begin(),
                                  protective_sequence.end()),
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

        for (const ExecCase& c : alu.executions) {
            auto setup = [&](CpuState& cpu) {
                cpu.init_long_mode();
                cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
                cpu.gpr.r[bx] = 0xAAAAAAAA00000000ull | c.dst;
                cpu.gpr.r[si] = 0xBBBBBBBB00000000ull | c.src;
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
            EXPECT_EQ(m_cpu->gpr.r[si], reference.gpr.r[si]);
            if (!alu.same_source) {
                EXPECT_EQ(m_cpu->gpr.r[si],
                          0xBBBBBBBB00000000ull | c.src);
            }
            EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
            EXPECT_EQ(m_cpu->gpr.get32(EAX),
                      c.taken ? 0xCAFEBABEu : 0x12345678u);
            EXPECT_EQ(m_cpu->rip, reference.rip);
            EXPECT_EQ(m_cpu->rip, 0x1022u);
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

TEST_F(JitExecTest, Mode64PinnedSelfAddWithoutTestKeepsProtectiveSource) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    constexpr u8 bx = static_cast<u8>(RegIndex::RBX);

    const u8 code[] = {
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB,
        0x40, 0x01, 0xDB,  // add ebx,ebx
        0x40, 0x89, 0xD8,  // mov eax,ebx
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

    // The protective source copy is how the stencil keeps an aliased ADD's
    // lazy source; Tier 1 records its own SSA operand instead.
    if (!m_jit->ir_emit_active()) {
        const auto* words = static_cast<const u32*>(compiled->code);
        const usize word_count = compiled->code_size / sizeof(u32);
        const std::array<u32, 2> protective_sequence = {
            arm64::mov_w(REG_SCRATCH1, bx_pin),
            arm64::adds_w(bx_pin, bx_pin, REG_SCRATCH1),
        };
        EXPECT_NE(std::search(words, words + word_count,
                              protective_sequence.begin(),
                              protective_sequence.end()),
                  words + word_count);
        EXPECT_EQ(static_cast<usize>(std::count(
                      words, words + word_count,
                      arm64::adds_w(bx_pin, bx_pin, bx_pin))),
                  0u);
    }

    for (const u32 initial : {0x40000000u, 0xFFFFFFFFu}) {
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
        EXPECT_EQ(m_cpu->rip, 0x101Bu);
        EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
        EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
        EXPECT_EQ(m_cpu->flags.af(), reference.flags.af());
        EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
        EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
        EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
    }
}

TEST_F(JitExecTest, Mode64PinnedAliasedAddJoinIntoSelfTestForcesAnds) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    constexpr u8 bx = static_cast<u8>(RegIndex::RBX);

    const u8 code[] = {
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB,
        0x85, 0xC9,              // test ecx,ecx; gives JZ native flags
        0x74, 0x03,              // jz directly to TEST
        0x40, 0x01, 0xDB,        // add ebx,ebx
        0x40, 0x85, 0xDB,        // test ebx,ebx
        0x79, 0x05,              // jns block end
        0xB8, 0x78, 0x56, 0x34, 0x12,
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 13u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    JitFaultMetadata metadata{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(
        reinterpret_cast<uintptr_t>(compiled->code) + compiled->link_code_offset,
        metadata));
    const u8 bx_pin = metadata.pins.host_reg[bx];
    ASSERT_GE(bx_pin, 23u);
    ASSERT_LE(bx_pin, 28u);

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize word_count = compiled->code_size / sizeof(u32);
    EXPECT_EQ(static_cast<usize>(std::count(
                  words, words + word_count,
                  arm64::adds_w(bx_pin, bx_pin, bx_pin))),
              1u);
    const std::array<u32, 2> protective_sequence = {
        arm64::mov_w(REG_SCRATCH1, bx_pin),
        arm64::adds_w(bx_pin, bx_pin, REG_SCRATCH1),
    };
    EXPECT_EQ(std::search(words, words + word_count,
                          protective_sequence.begin(),
                          protective_sequence.end()),
              words + word_count);
    EXPECT_EQ(static_cast<usize>(std::count(
                  words, words + word_count,
                  arm64::ands_w(REG_SCRATCH0, bx_pin, bx_pin))),
              1u)
        << "forward join reused single-predecessor ALU provenance";
    EXPECT_NE(std::find_if(words, words + word_count, [](u32 word) {
                  return (word & 0xFF00001Fu) ==
                         (0x54000000u | arm64::PL);
              }),
              words + word_count);

    struct Case {
        bool join_taken;
        u32 initial;
        u32 result;
        u32 eax;
    };
    const Case cases[] = {
        {true, 0x80000000u, 0x80000000u, 0x12345678u},
        {false, 0u, 0u, 0xCAFEBABEu},
    };
    for (const Case& c : cases) {
        auto setup = [&](CpuState& cpu) {
            cpu.init_long_mode();
            cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
            cpu.gpr.r[bx] = 0xAAAAAAAA00000000ull | c.initial;
            cpu.gpr.set32(ECX, c.join_taken ? 0u : 1u);
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
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
        m_cpu->materialize_lazy_flags();

        EXPECT_EQ(m_cpu->gpr.r[bx], c.result);
        EXPECT_EQ(m_cpu->gpr.r[bx], reference.gpr.r[bx]);
        EXPECT_EQ(m_cpu->gpr.get32(EAX), c.eax);
        EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
        EXPECT_EQ(m_cpu->rip, 0x1026u);
        EXPECT_EQ(m_cpu->rip, reference.rip);
        EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
        EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
        EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
        EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
        EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
        EXPECT_FALSE(m_cpu->flags.cf());
        EXPECT_FALSE(m_cpu->flags.of());
    }
}

TEST_F(JitExecTest, Mode64PinnedCompactAddColdJnsPublishesTestFlags) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    constexpr u8 bx = static_cast<u8>(RegIndex::RBX);

    const u8 code[] = {
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB, 0x40, 0x89, 0xDB,
        0x40, 0x89, 0xDB,
        0x40, 0x83, 0xC3, 0x01,  // add ebx,1
        0x40, 0x85, 0xDB,        // test ebx,ebx
        0x79, 0x20,              // jns to 0x103e, outside this block
        0xB8, 0x78, 0x56, 0x34, 0x12,
    };
    BasicBlock block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 11u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    JitFaultMetadata metadata{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(
        reinterpret_cast<uintptr_t>(compiled->code) + compiled->link_code_offset,
        metadata));
    const u8 bx_pin = metadata.pins.host_reg[bx];
    ASSERT_GE(bx_pin, 23u);
    ASSERT_LE(bx_pin, 28u);

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize word_count = compiled->code_size / sizeof(u32);
    EXPECT_EQ(static_cast<usize>(std::count(
                  words, words + word_count,
                  arm64::adds_w_imm(bx_pin, bx_pin, 1))),
              1u);
    const std::array<u32, 2> materialized_add_sequence = {
        arm64::movz_w(REG_SCRATCH1, 1),
        arm64::adds_w(bx_pin, bx_pin, REG_SCRATCH1),
    };
    EXPECT_EQ(std::search(words, words + word_count,
                          materialized_add_sequence.begin(),
                          materialized_add_sequence.end()),
              words + word_count);
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

    struct Case {
        u32 initial;
        u32 result;
        bool taken;
    };
    const Case cases[] = {
        {0xFFFFFFFFu, 0u, true},
        {0x7FFFFFFFu, 0x80000000u, false},
    };
    for (const Case& c : cases) {
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
            (void)interpreter.run(c.taken ? 10u : block.instruction_count);
        }
        setup(*m_cpu);
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::And32)
            << "escaping JNS replayed the omitted ADD instead of TEST";

        reference.materialize_lazy_flags();
        m_cpu->materialize_lazy_flags();
        EXPECT_EQ(m_cpu->gpr.r[bx], c.result);
        EXPECT_EQ(m_cpu->gpr.r[bx], reference.gpr.r[bx]);
        EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
        EXPECT_EQ(m_cpu->gpr.get32(EAX),
                  c.taken ? 0xCAFEBABEu : 0x12345678u);
        EXPECT_EQ(m_cpu->rip, c.taken ? 0x103Eu : 0x1023u);
        EXPECT_EQ(m_cpu->rip, reference.rip);
        EXPECT_EQ(m_cpu->flags.cf(), reference.flags.cf());
        EXPECT_EQ(m_cpu->flags.of(), reference.flags.of());
        EXPECT_EQ(m_cpu->flags.zf(), reference.flags.zf());
        EXPECT_EQ(m_cpu->flags.sf(), reference.flags.sf());
        EXPECT_EQ(m_cpu->flags.pf(), reference.flags.pf());
        EXPECT_FALSE(m_cpu->flags.cf());
        EXPECT_FALSE(m_cpu->flags.of());
    }
}
