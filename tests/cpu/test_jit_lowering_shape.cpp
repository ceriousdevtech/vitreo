// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — emitted shape of flag records and pinned lowering

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, AddpsSpecialGateTestsResultNotOperands) {
    // ADD/SUB/MUL PS/PD diverge from ARM only when the RESULT is a NaN
    // (sse_float_semantics.hpp / Intel SDM Vol. 1 §4.8.3.4), and they touch no
    // MXCSR sticky bit. With FPCR.AH installed for every SSE mode even that
    // divergence is gone (math::sse_alternate_nan_handling), so there is no
    // check at all; without it, one FCMEQ on the result replaces the whole
    // per-operand exponent gate. Guarding the shape here: an operand gate that
    // creeps back costs ~5x on every packed-FP instruction. The VALUES are
    // the special-value parity tests' (JitSseFpParityTest and friends).
    const u8 code[] = {0x0F, 0x58, 0xC1};  // addps xmm0,xmm1
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const bool gated = !vitreo::math::sse_alternate_nan_handling();
    EXPECT_EQ(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::fcmeq_v(
                       2, 0, 0, vitreo::cpu::jit::arm64::FS_S)), gated)
        << "packed-FP special-value gate is no longer a result NaN test, or "
           "is still emitted under FPCR.AH";
    EXPECT_EQ(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::uminv_v(
                       2, 2, vitreo::cpu::jit::arm64::VS_B)), gated)
        << "packed-FP gate regressed to scalar lane folding";
    // None of the old operand-inspection shapes may return.
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mvni_v4s_finite_threshold(4)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::cmhi_v(
                       2, 2, 4, vitreo::cpu::jit::arm64::VS_S)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::and_v16b(2, 0, 4)));

    // DIVPS keeps its operand gate: MXCSR.ZE and the exactness residual are
    // only defined for finite operands.
    const u8 div_code[] = {0x0F, 0x5E, 0xC1};  // divps xmm0,xmm1
    BasicBlock div_block = build_block(div_code, sizeof(div_code));
    div_block.start_addr = 0x3000;
    CompiledBlock* div = m_jit->compile(div_block);
    ASSERT_NE(div, nullptr);
    EXPECT_TRUE(code_contains_word(
        *div, vitreo::cpu::jit::arm64::umaxv_v(
                  2, 2, vitreo::cpu::jit::arm64::VS_B)));
}

// A 64-bit publisher writes flag_src and flag_dst with ONE STP. The layout
// this needs is pinned by static_assert in jit_helpers.hpp, but the EMITTER's
// choice is not: `emit_lazy_flag_store_raw` keeps a two-STR fallback for the
// cases that cannot pair, and falling into it is silent. It is also ~10% on
// sc_blocksize (one store is ~9.9% of bsz1;
// jit-refactor/FLAG-PUBLICATION.md §3), so this asserts the pair is actually
// emitted.
TEST_F(JitExecTest, Wide64FlagRecordPublishesAsOneStorePair) {
    // add rax, rbx -- a full 64-bit flag writer, nothing after it to kill the
    // record, so the block must publish it.
    const u8 code[] = {0x48, 0x01, 0xD8};
    BasicBlock block = build_block64(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 pair_imm =
        static_cast<u32>(vitreo::cpu::jit::flag_src_offset() / sizeof(u64));
    bool found = false;
    for (u8 src = 0; src < 32 && !found; ++src) {
        for (u8 dst = 0; dst < 32 && !found; ++dst) {
            found = code_contains_word(
                *compiled,
                vitreo::cpu::jit::arm64::stp_x_off(
                    src, dst, vitreo::cpu::jit::REG_CPU_STATE,
                    static_cast<i32>(pair_imm)));
        }
    }
    EXPECT_TRUE(found)
        << "64-bit flag publication fell back to two separate STRs; "
           "flag_src/flag_dst must stay adjacent, 8-aligned and within STP's "
           "imm7 of the CpuState base";
}

TEST_F(JitExecTest, Narrow32FlagRecordPublishesAsOneQStore) {
    // add eax, ebx -- 32-bit full flag writer. Must publish flag_src/flag_dst
    // with one STR Q (via FMOV+INS packing), not two STRs: the third store was
    // ~10% of sc_blocksize bsz1.
    const u8 code[] = {0x01, 0xD8};
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 q_imm =
        static_cast<u32>(vitreo::cpu::jit::flag_src_offset() >> 4);
    bool found = false;
    for (u8 vt = 0; vt < 32 && !found; ++vt) {
        found = code_contains_word(
            *compiled,
            vitreo::cpu::jit::arm64::str_q_uoff(
                vt, vitreo::cpu::jit::REG_CPU_STATE, q_imm));
    }
    EXPECT_TRUE(found)
        << "32-bit flag publication fell back to two STRs; flag_src must stay "
           "16-aligned so FMOV+INS+STR Q can publish the pair without a "
           "second GPR scratch";
}

TEST_F(JitExecTest, FlagOverlayPairUsesHalfwordAccesses) {
    const u8 add_code[] = {0x01, 0xD8};  // add eax,ebx
    m_cpu->cf_overlay = 2;
    m_cpu->of_overlay = 2;
    EXPECT_EQ(jit_run(add_code, sizeof(add_code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->cf_overlay, 0);
    EXPECT_EQ(m_cpu->of_overlay, 0);

    BasicBlock add_block = build_block(add_code, sizeof(add_code));
    CompiledBlock* add = m_jit->compile(add_block);
    ASSERT_NE(add, nullptr);
    // The write side is now ONE word store, not a STRB plus a paired STRH: a
    // full flag writer owns CF and OF, so clearing both overlays rides in the
    // same aligned word as the op byte. Publishing the record is what a block
    // boundary costs and the cost is the store COUNT -- one store is ~9.9% of
    // sc_blocksize bsz1 (jit-refactor/FLAG-PUBLICATION.md §3) -- so a
    // regression to separate stores here is a measurable throughput
    // regression, which is what this assertion guards.
    EXPECT_TRUE(code_contains_word(
        *add, vitreo::cpu::jit::arm64::str_w_uoff(
                  vitreo::cpu::jit::REG_SCRATCH3,
                  vitreo::cpu::jit::REG_CPU_STATE,
                  vitreo::cpu::jit::flag_word_offset() >> 2)))
        << "full flag writer regressed to separate op / CF / OF stores";

    const u8 jnz_code[] = {0x75, 0x00};  // jnz next instruction
    BasicBlock jnz_block = build_block(jnz_code, sizeof(jnz_code));
    CompiledBlock* jnz = m_jit->compile(jnz_block);
    ASSERT_NE(jnz, nullptr);
    EXPECT_TRUE(code_contains_word(
        *jnz, vitreo::cpu::jit::arm64::ldrh_uoff(
                  2, vitreo::cpu::jit::REG_CPU_STATE,
                  vitreo::cpu::jit::cf_overlay_offset() >> 1)))
        << "lazy condition walk regressed to separate CF/OF overlay loads";
}

TEST_F(JitExecTest, Mode32CmpZeroUsesWzrWithoutMaterializingImmediate) {
    const u8 zero_code[] = {0x83, 0x3E, 0x00};  // cmp dword [esi],0
    BasicBlock zero_block = build_block(zero_code, sizeof(zero_code));
    CompiledBlock* zero = m_jit->compile(zero_block);
    ASSERT_NE(zero, nullptr);
    EXPECT_TRUE(code_contains_word(
        *zero, vitreo::cpu::jit::arm64::subs_w(
                   vitreo::cpu::jit::REG_SCRATCH0,
                   vitreo::cpu::jit::REG_SCRATCH0, 31)));
    EXPECT_FALSE(code_contains_word(
        *zero, vitreo::cpu::jit::arm64::movz_w(
                   vitreo::cpu::jit::REG_SCRATCH1, 0)))
        << "PE32 CMP zero regressed to materializing an immediate";

    const u8 nonzero_code[] = {0x83, 0x3E, 0x01};  // cmp dword [esi],1
    BasicBlock nonzero_block = build_block(nonzero_code,
                                           sizeof(nonzero_code));
    nonzero_block.start_addr = 0x2000;
    CompiledBlock* nonzero = m_jit->compile(nonzero_block);
    ASSERT_NE(nonzero, nullptr);
    EXPECT_TRUE(code_contains_word(
        *nonzero, vitreo::cpu::jit::arm64::movz_w(
                      vitreo::cpu::jit::REG_SCRATCH1, 1)))
        << "nonzero CMP immediate must retain ordinary materialization";
}

TEST_F(JitExecTest, Mode32AdcSbbZeroUseWzrAndSkipSourcePackOr) {
    const struct {
        std::array<u8, 3> code;
        u32 arithmetic;
    } cases[] = {
        {{{0x83, 0xD1, 0x00}},
         vitreo::cpu::jit::arm64::adcs_w(
             vitreo::cpu::jit::REG_SCRATCH0,
             vitreo::cpu::jit::REG_SCRATCH0, 31)},  // adc ecx,0
        {{{0x83, 0xD9, 0x00}},
         vitreo::cpu::jit::arm64::sbcs_w(
             vitreo::cpu::jit::REG_SCRATCH0,
             vitreo::cpu::jit::REG_SCRATCH0, 31)},  // sbb ecx,0
    };
    for (const auto& c : cases) {
        BasicBlock block = build_block(c.code.data(), c.code.size());
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_TRUE(code_contains_word(*compiled, c.arithmetic));
        EXPECT_FALSE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::movz_w(
                           vitreo::cpu::jit::REG_SCRATCH1, 0)));
        EXPECT_FALSE(code_contains_word(*compiled, 0xAA010084u))
            << "zero source must not be ORed into packed lazy flag_src";
        const u32 src_off = vitreo::cpu::jit::flag_src_offset();
        const u32 dst_off = vitreo::cpu::jit::flag_dst_offset();
        EXPECT_TRUE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::add_x_imm(
                           vitreo::cpu::jit::REG_SCRATCH3,
                           vitreo::cpu::jit::REG_CPU_STATE, src_off)));
        EXPECT_TRUE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::stp_x_off(
                           vitreo::cpu::jit::REG_SCRATCH4,
                           vitreo::cpu::jit::REG_SCRATCH0,
                           vitreo::cpu::jit::REG_SCRATCH3, 0)));
        EXPECT_FALSE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::str_x_uoff(
                           vitreo::cpu::jit::REG_SCRATCH4,
                           vitreo::cpu::jit::REG_CPU_STATE, src_off >> 3)));
        EXPECT_FALSE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::str_x_uoff(
                           vitreo::cpu::jit::REG_SCRATCH3,
                           vitreo::cpu::jit::REG_CPU_STATE, dst_off >> 3)));
        EXPECT_FALSE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::mov_w(
                           vitreo::cpu::jit::REG_SCRATCH3,
                           vitreo::cpu::jit::REG_SCRATCH0)));
    }
}

TEST_F(JitExecTest, Mode32MovWritesPinnedDestinationDirectly) {
    const u8 reg_code[] = {0x89, 0xD0};  // mov eax,edx
    BasicBlock block = build_block(reg_code, sizeof(reg_code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(23, 25)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(0, 25)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(23, 0)));

    const u8 imm_code[] = {0xB8, 0x42, 0x00, 0x00, 0x00};
    block = build_block(imm_code, sizeof(imm_code));
    compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::movz_w(23, 0x42)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::movz_w(0, 0x42)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(23, 0)));
}

TEST_F(JitExecTest, Mode32BinaryAluWritesPinnedDestinationDirectly) {
    const u8 reg_code[] = {0x01, 0xD0};  // add eax,edx
    BasicBlock block = build_block(reg_code, sizeof(reg_code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::adds_w(23, 23, 25)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(0, 23)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(1, 25)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(23, 0)));

    m_cpu->gpr.set32(EAX, 0xFFFFFFFFu);
    m_cpu->gpr.set32(EDX, 1u);
    EXPECT_EQ(jit_run(reg_code, sizeof(reg_code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.zf());

    // Preserve both pre-write operands for lazy flags on a self-operation.
    const u8 self_code[] = {0x01, 0xC0};  // add eax,eax
    block = build_block(self_code, sizeof(self_code));
    block.start_addr = 0x2000;
    compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(0, 23)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(1, 23)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::adds_w(0, 0, 1)));

    // A direct write to the ESP pin must invalidate the cached [esp+disp]
    // frame before the following memory access.
    const u8 esp_code[] = {
        0x8B, 0x4C, 0x24, 0x04,  // mov ecx,[esp+4]
        0x83, 0xC4, 0x04,        // add esp,4
        0x8B, 0x44, 0x24, 0x04,  // mov eax,[esp+4]
    };
    m_cpu->rip = 0x1000;
    m_cpu->gpr.set32(ESP, 0x4000);
    ASSERT_TRUE(m_memory->write_u32(0x4004, 0x11111111u));
    ASSERT_TRUE(m_memory->write_u32(0x4008, 0x22222222u));
    CpuState reference = *m_cpu;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, esp_code, sizeof(esp_code)));
    Interpreter interpreter(reference, *m_memory);
    ASSERT_EQ(interpreter.run(3), ExecResult::Continue);
    EXPECT_EQ(jit_run(esp_code, sizeof(esp_code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), reference.gpr.get32(ESP));
    EXPECT_EQ(m_cpu->gpr.get32(ECX), reference.gpr.get32(ECX));
    EXPECT_EQ(m_cpu->gpr.get32(EAX), reference.gpr.get32(EAX));
}

TEST_F(JitExecTest, Mode32ShiftWritesPinnedDestinationDirectly) {
    const u8 code[] = {0xD1, 0xE0};  // shl eax,1
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(4, 23)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::lsl_w_imm(23, 23, 1)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(0, 23)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(4, 0)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_w(23, 0)));

    m_cpu->gpr.set32(EAX, 0x80000001u);
    EXPECT_EQ(jit_run(code, sizeof(code)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 2u);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_TRUE(m_cpu->flags.of());
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.sf());

    // Intel SDM Vol. 2: a masked count of zero preserves destination and flags.
    const u8 cl_zero[] = {0xD3, 0xE0};  // shl eax,cl
    m_cpu->gpr.set32(EAX, 0x12345678u);
    m_cpu->gpr.set32(ECX, 0u);
    m_cpu->flags.rflags = 0x8D7u;
    m_cpu->flag_op = CpuState::FlagOp::None;
    const u64 flags_before = m_cpu->flags.rflags;
    EXPECT_EQ(jit_run(cl_zero, sizeof(cl_zero)), JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 0x12345678u);
    EXPECT_EQ(m_cpu->flags.rflags, flags_before);
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
}

TEST_F(JitExecTest, Mode32LoopBackUsesBitTestIterationCap) {
    const u8 code[] = {
        0x40,                         // inc eax
        0x83, 0xE9, 0x01,             // sub ecx,1
        0x75, 0xFA,                   // jnz loop header
    };
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::add_w_imm(
                       vitreo::cpu::jit::REG_LOOP_CTR,
                       vitreo::cpu::jit::REG_LOOP_CTR, 1)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::tbnz(
                       vitreo::cpu::jit::REG_LOOP_CTR, 16, 8)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::sub_w_imm(
                       vitreo::cpu::jit::REG_LOOP_CTR,
                       vitreo::cpu::jit::REG_LOOP_CTR, 1)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::movz_w(
                       vitreo::cpu::jit::REG_SCRATCH1, 0xFFFFu)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::subs_w(
                       31, vitreo::cpu::jit::REG_LOOP_CTR,
                       vitreo::cpu::jit::REG_SCRATCH1)));
}

TEST_F(JitExecTest, Mode32PinEntryUsesFourPairLoadsAndPreservesHighHalves) {
    const u8 code[] = {0x90};  // nop
    for (u8 reg = 0; reg < 8; ++reg) {
        m_cpu->gpr.r[reg] = (0xA0B0C000ull + reg) << 32
                          | (0x10203040ull + reg);
    }
    const std::array<u64, 8> before = {
        m_cpu->gpr.r[0], m_cpu->gpr.r[1], m_cpu->gpr.r[2], m_cpu->gpr.r[3],
        m_cpu->gpr.r[4], m_cpu->gpr.r[5], m_cpu->gpr.r[6], m_cpu->gpr.r[7],
    };

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::ldp_x_off(
                       23, 24, vitreo::cpu::jit::REG_CPU_STATE, 0)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::ldp_x_off(
                       25, 26, vitreo::cpu::jit::REG_CPU_STATE, 2)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::ldp_x_off(
                       12, 13, vitreo::cpu::jit::REG_CPU_STATE, 4)));
    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::ldp_x_off(
                       27, 28, vitreo::cpu::jit::REG_CPU_STATE, 6)));
    for (u8 reg = 0; reg < 8; ++reg) {
        EXPECT_FALSE(code_contains_word(
            *compiled, vitreo::cpu::jit::arm64::ldr_w_uoff(
                           static_cast<u8>(reg < 4 ? 23 + reg
                                                   : reg < 6 ? 8 + reg
                                                             : 21 + reg),
                           vitreo::cpu::jit::REG_CPU_STATE,
                           vitreo::cpu::jit::gpr_off(reg) >> 2)));
    }

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
    for (u8 reg = 0; reg < 8; ++reg) {
        EXPECT_EQ(m_cpu->gpr.r[reg], before[reg]);
    }
}
