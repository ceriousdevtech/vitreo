// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — x64 PUSH/POP/CALL/RET through the pinned RSP

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, Mode64PushUsesPinnedRspAndPinnedSource) {
    const u8 code[] = {0x53};  // push rbx
    BasicBlock block = build_block64(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    if (m_jit->ir_emit_active()) {
        GTEST_SKIP() << "stencil PUSH lowering; Tier 1 also commits first "
                        "(rsp_undo) but with its own address registers, "
                        "covered by the Mode64*StackWriteFault* tests";
    }

    // The hot form is `SUB pin, pin, #8; STR`. Intel SDM Vol. 2 PUSH: a
    // faulting stack write commits nothing, so the decrement is recorded on
    // the instruction's fault site and both fault paths add the 8 back
    // (JitFlagRecovery::rsp_undo) -- precision costs no hot instruction.
    const u32 decrement = vitreo::cpu::jit::arm64::sub_x_imm(
        vitreo::cpu::jit::REG_PIN_ESP, vitreo::cpu::jit::REG_PIN_ESP, 8);
    EXPECT_EQ(count_word(*compiled, decrement), 1u);
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_x(
                       vitreo::cpu::jit::REG_PIN_ESP,
                       vitreo::cpu::jit::REG_CALL_TMP)))
        << "x64 PUSH needs no slot copy to commit RSP after the store";
    const u32* at = nullptr;
    for (const WordRange& range : block_word_ranges(*compiled)) {
        const u32* hit = std::find(range.begin, range.end, decrement);
        if (hit != range.end) {
            at = hit;
            break;
        }
    }
    ASSERT_NE(at, nullptr);
    JitFaultMetadata metadata{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(reinterpret_cast<uintptr_t>(at + 1),
                                          metadata));
    ASSERT_TRUE(metadata.has_guest_rip);
    EXPECT_EQ(metadata.guest_rip, block.start_addr);
    EXPECT_EQ(metadata.flags.rsp_undo, 8)
        << "a fault in the store adds the decrement back";
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_x(
                       vitreo::cpu::jit::REG_SCRATCH2,
                       vitreo::cpu::jit::REG_PIN_ESP)))
        << "x64 PUSH must not copy the pinned RSP through X2";
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_x(
                       vitreo::cpu::jit::REG_SCRATCH0, 26)))
        << "the pinned RBX value must flow directly into the checked store";
}

TEST_F(JitExecTest, Mode64PopUsesPinnedRspAndPinnedDestination) {
    const u8 code[] = {0x59};  // pop rcx
    BasicBlock block = build_block64(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    EXPECT_TRUE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::add_x_imm(
                       vitreo::cpu::jit::REG_PIN_ESP,
                       vitreo::cpu::jit::REG_PIN_ESP, 8)));
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_x(
                       vitreo::cpu::jit::REG_CALL_TMP,
                       vitreo::cpu::jit::REG_PIN_ESP)))
        << "ordinary x64 POP must not copy the pinned RSP through X21";
    EXPECT_FALSE(code_contains_word(
        *compiled, vitreo::cpu::jit::arm64::mov_x(
                       vitreo::cpu::jit::REG_PIN_ESP,
                       vitreo::cpu::jit::REG_CALL_TMP)))
        << "ordinary x64 POP must increment the pinned RSP directly";

    JitFaultMetadata metadata{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(
        reinterpret_cast<uintptr_t>(compiled->code) + compiled->link_code_offset,
        metadata));
    const u8 rcx_pin =
        metadata.pins.host_reg[static_cast<u8>(RegIndex::RCX)];
    ASSERT_GE(rcx_pin, 23u);
    ASSERT_LE(rcx_pin, 28u);
    EXPECT_EQ(count_word(
                  *compiled, vitreo::cpu::jit::arm64::mov_x(
                                 rcx_pin, vitreo::cpu::jit::REG_SCRATCH0)),
              1u)
        << "only the cold checked-helper return may move X0 into pinned RCX";
}

TEST_F(JitExecTest, Mode64CallAndRetUpdatePinnedRspDirectly) {
    const struct {
        std::vector<u8> code;
        u32 stack_address;
        u32 stack_commit;
    } cases[] = {
        {{0xE8, 0x00, 0x00, 0x00, 0x00},
         vitreo::cpu::jit::arm64::sub_x_imm(
             vitreo::cpu::jit::REG_CALL_TMP,
             vitreo::cpu::jit::REG_PIN_ESP, 8),
         vitreo::cpu::jit::arm64::mov_x(
             vitreo::cpu::jit::REG_PIN_ESP,
             vitreo::cpu::jit::REG_CALL_TMP)},
        {{0xC3},
         vitreo::cpu::jit::arm64::add_x_imm(
             vitreo::cpu::jit::REG_PIN_ESP,
             vitreo::cpu::jit::REG_PIN_ESP, 8),
         0},
    };

    // Tier 1 commits CALL's RSP - 8 into the pin before the store and
    // records the 8 as the site's rsp_undo, so it has no X21 copy;
    // Mode64CallStackWriteFaultDoesNotCommitRspOrRip holds both to the SDM.
    const u32 ir_commit_first = vitreo::cpu::jit::arm64::sub_x_imm(
        vitreo::cpu::jit::REG_PIN_ESP, vitreo::cpu::jit::REG_PIN_ESP, 8);
    for (const auto& c : cases) {
        BasicBlock block = build_block64(c.code.data(), c.code.size());
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        if (m_jit->ir_emit_active() && c.stack_commit != 0) {
            EXPECT_TRUE(code_contains_word(*compiled, ir_commit_first));
            continue;
        }
        EXPECT_TRUE(code_contains_word(*compiled, c.stack_address));
        if (c.stack_commit != 0) {
            EXPECT_TRUE(code_contains_word(*compiled, c.stack_commit));
        }
    }
}

TEST_F(JitExecTest, Mode64CallStackWriteFaultDoesNotCommitRspOrRip) {
    const u8 code[] = {0xE8, 0x10, 0x00, 0x00, 0x00};
    constexpr u64 kUnmappedStackTop = 0x20000008ull;
    m_cpu->init_long_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.rsp = kUnmappedStackTop;

    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->pending_raise.args[0], 1u);
    EXPECT_EQ(m_cpu->pending_raise.args[1], kUnmappedStackTop - 8);
    EXPECT_EQ(m_cpu->gpr.rsp, kUnmappedStackTop);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
}

TEST_F(JitExecTest, Mode64IndirectCallStackWriteFaultDoesNotCommitRspOrRip) {
    const u8 code[] = {0xFF, 0xD0};  // call rax
    constexpr u64 kUnmappedStackTop = 0x20000008ull;
    m_cpu->init_long_mode();
    m_cpu->rip = 0x1000;
    m_cpu->gpr.rax = 0x1080;
    m_cpu->gpr.rsp = kUnmappedStackTop;

    EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
    ASSERT_TRUE(m_cpu->pending_raise.active);
    EXPECT_EQ(m_cpu->pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_cpu->pending_raise.args[0], 1u);
    EXPECT_EQ(m_cpu->pending_raise.args[1], kUnmappedStackTop - 8);
    EXPECT_EQ(m_cpu->gpr.rsp, kUnmappedStackTop);
    EXPECT_EQ(m_cpu->rip, 0x1000u);
}
