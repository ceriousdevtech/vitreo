// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 precise load faults and faulting-RIP publication. Split from
// test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

TEST_F(JitLinear64Test, OutOfWindowLoadUsesCheckedHelperWithFullAddress) {
    const std::vector<u8> code = {0x8B, 0x03}; // mov eax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr outside = 0x00000007F0000000ull;
    CpuState cpu = cpu64(outside);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.args[1], outside);
}

TEST_F(JitLinear64Test, InWindowUnmappedDirectLoadRaisesPreciseReadAv) {
    const std::vector<u8> code = {0x8B, 0x03}; // mov eax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_TRUE(has_guarded_linear64_load32(emitted))
        << "test did not reach the guarded ordered linear64 load path";

    CpuState cpu = cpu64(kUnmapped64);
    for (u8 reg = 0; reg < 16; ++reg) {
        cpu.gpr.set64(reg, 0xA500000000000000ull + reg);
    }
    cpu.gpr.set64(EBX, kUnmapped64);
    cpu.flags.rflags = 0x8D7u;
    std::array<u64, 16> expected_gpr{};
    for (u8 reg = 0; reg < 16; ++reg) {
        expected_gpr[reg] = cpu.gpr.get64(reg);
    }

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_TRUE(cpu.pending_raise.from_helper);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.flags, 0u);
    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
    for (u8 reg = 0; reg < 16; ++reg) {
        EXPECT_EQ(cpu.gpr.get64(reg), expected_gpr[reg])
            << "register=" << static_cast<unsigned>(reg);
    }
}

TEST_F(JitLinear64Test,
       AliasingUnmappedLoadRaisesReadAvWithoutCommittingDestination) {
    // Intel SDM Vol. 2, MOV: the source memory read completes before the
    // destination register is written. `mov rdx,[rdx]` with RDX=2 therefore
    // faults at address 2 and must leave RDX holding the faulting address.
    const std::vector<u8> code = {0x48, 0x8B, 0x12};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    CpuState cpu = cpu64(0);
    cpu.gpr.rdx = 2;
    cpu.flags.rflags = 0x8D7u;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_TRUE(cpu.pending_raise.from_helper);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    ASSERT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
    EXPECT_EQ(cpu.pending_raise.args[1], 2u);
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(cpu.gpr.rdx, 2u);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
}

TEST_F(JitLinear64Test, HostPcMapRecoversSecondInstructionRip) {
    // mov rax,42 ; mov eax,[rbx]. A block-entry RIP cannot satisfy this test:
    // the fault belongs to the second guest instruction at byte offset 10.
    const std::vector<u8> code = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x8B, 0x03,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    GuestAddr mapped_rip = 0;
    bool found_second = false;
    bool pre_access_stores_rip = false;
    bool saw_guest_load = false;
    const uintptr_t native = reinterpret_cast<uintptr_t>(compiled->code);
    const u32 rip_store = arm64::str_x_uoff(
        REG_SCRATCH0, REG_CPU_STATE,
        static_cast<u32>(offsetof(CpuState, rip)) >> 3);
    for (u32 offset = 0; offset < compiled->code_size; offset += 4) {
        if (jit_guest_rip_for_pc(native + offset, mapped_rip) &&
            mapped_rip == kCode64 + 10) {
            found_second = true;
            u32 word = 0;
            std::memcpy(&word, static_cast<const u8*>(compiled->code) + offset,
                        sizeof(word));
            if (!saw_guest_load) {
                pre_access_stores_rip |= word == rip_store;
                saw_guest_load = is_acquire_load32(word, 17);
            }
        }
    }
    ASSERT_TRUE(found_second) << "second instruction has no native RIP map";
    ASSERT_TRUE(saw_guest_load) << "second instruction did not emit direct load";
    EXPECT_FALSE(pre_access_stores_rip)
        << "direct x64 memory path still publishes RIP before its hot access";

    CpuState cpu = cpu64(kUnmapped64);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
    EXPECT_EQ(cpu.rip, kCode64 + 10);
}

TEST_F(JitLinear64Test, ColdHelperPublishesSecondInstructionRip) {
    // Same two-instruction shape as the native-fault test, but the address is
    // outside the reserved VA window and therefore takes the checked helper.
    const std::vector<u8> code = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x8B, 0x03,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr outside = 0x00000007F0000000ull;
    CpuState cpu = cpu64(outside);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
    EXPECT_EQ(cpu.pending_raise.args[1], outside);
    EXPECT_EQ(cpu.rip, kCode64 + 10);
    EXPECT_EQ(cpu.gpr.get64(EAX), 42u)
        << "faulting helper load overwrote its pinned destination";
}

TEST_F(JitLinear64Test, LinkedColdHelperPublishesFirstInstructionRip) {
    const bool was_linked = JitCompiler::jit_link_enabled();
    struct RestoreLinkMode {
        bool enabled;
        ~RestoreLinkMode() {
            if (enabled) JitCompiler::enable_jit_link();
            else JitCompiler::disable_jit_link();
        }
    } restore{was_linked};
    JitCompiler::enable_jit_link();
    constexpr GuestAddr target = kCode64 + 0x100;
    const std::vector<u8> load{0x8B,0x03}; // mov eax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(target, load.data(), load.size()));
    auto target_block = build64(load);
    target_block.start_addr = target;
    auto* cached = m_cache->insert(std::move(target_block));
    ASSERT_NE(cached, nullptr);
    auto* target_code = m_jit->compile(*cached);
    ASSERT_NE(target_code, nullptr);
    publish_compiled(*cached, *target_code);
    ASSERT_NE(cached->compiled_link_code, nullptr);
    const std::vector<u8> jump{0xE9,0xFB,0,0,0};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, jump.data(), jump.size()));
    auto source = build64(jump);
    auto* compiled = m_jit->compile(source);
    ASSERT_NE(compiled, nullptr);
    constexpr GuestAddr outside = 0x00000007F0000000ull;
    CpuState cpu = cpu64(outside);
    cpu.gpr.rax = 42;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[1], outside);
    EXPECT_EQ(cpu.rip, target);
    EXPECT_EQ(cpu.gpr.rax, 42u);
}

TEST_F(JitLinear64Test, MaskmovqSecondInstructionUsesMappedAndColdRip) {
    // mov rax,42 ; maskmovq mm0,mm1. Select every byte so the first lane
    // reaches either the direct hardware store or its checked-helper stub.
    const std::vector<u8> code = {
        0x48, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0xF7, 0xC1,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 word_count = compiled->code_size / 4;
    std::vector<u32> emitted(word_count);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_TRUE(has_guarded_linear64_store(emitted, 1));

    const u32 rip_store = arm64::str_x_uoff(
        REG_SCRATCH0, REG_CPU_STATE,
        static_cast<u32>(offsetof(CpuState, rip)) >> 3);
    const uintptr_t native = reinterpret_cast<uintptr_t>(compiled->code);
    bool saw_byte_store = false;
    bool pre_access_stores_rip = false;
    for (u32 offset = 0; offset < compiled->code_size; offset += 4) {
        GuestAddr mapped_rip = 0;
        if (!jit_guest_rip_for_pc(native + offset, mapped_rip) ||
            mapped_rip != kCode64 + 10) {
            continue;
        }
        u32 word = 0;
        std::memcpy(&word, static_cast<const u8*>(compiled->code) + offset,
                    sizeof(word));
        if (!saw_byte_store) {
            pre_access_stores_rip |= word == rip_store;
            saw_byte_store = is_release_store(word, 8, 1);
        }
    }
    ASSERT_TRUE(saw_byte_store);
    EXPECT_FALSE(pre_access_stores_rip);

    const auto run_fault = [&](GuestAddr target) {
        CpuState cpu = cpu64(0);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RDI), target);
        cpu.fpu.set_mm(0, 0x1122334455667788ull);
        cpu.fpu.set_mm(1, 0x8080808080808080ull);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        EXPECT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], 1u);
        EXPECT_EQ(cpu.pending_raise.args[1], target);
        EXPECT_EQ(cpu.rip, kCode64 + 10);
    };
    run_fault(kUnmapped64);
    run_fault(0x00000007F0000000ull);
}

TEST_F(JitLinear64Test, DirectLoadSeesPageBecomeNoAccess) {
    const std::vector<u8> code = {0x8B, 0x03}; // mov eax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    ASSERT_TRUE(m_memory->write_u32(kData64 + 0x80, 0xA1B2C3D4u));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_TRUE(has_guarded_linear64_load32(emitted))
        << "test did not compile the guarded ordered linear64 load";

    CpuState prime = cpu64(kData64 + 0x80);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, prime), JitExecResult::Continue);
    ASSERT_FALSE(prime.pending_raise.active);
    ASSERT_EQ(prime.gpr.get32(EAX), 0xA1B2C3D4u);

    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                         MemoryProtection::None));
    CpuState cpu = cpu64(kData64 + 0x80);
    cpu.gpr.set64(EAX, 0x1122334455667788ull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
    EXPECT_EQ(cpu.pending_raise.args[1], kData64 + 0x80);
    EXPECT_EQ(cpu.gpr.get64(EAX), 0x1122334455667788ull);
}

TEST_F(JitLinear64Test, WrappingLastByteUsesCheckedHelperWithoutAliasing) {
    const std::vector<u8> code = {0x48, 0x8B, 0x03}; // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr wrapping = 0xFFFFFFFFFFFFFFFCull;
    CpuState cpu = cpu64(wrapping);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.args[1], wrapping);
}

}  // namespace
