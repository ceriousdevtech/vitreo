// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 block-entry stack reservation/protection proofs and the protection
// lease. Split from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

TEST_F(JitLinear64Test, StackEntryReservationProofExecutesEligibleLoads) {
    // mov rax,[rsp+8] ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x48, 0x8B, 0x44, 0x24, 0x08,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(compiled->requires_protection_lease);
    EXPECT_EQ(compiled->link_entry(), nullptr);
    EXPECT_EQ(compiled->call_entry(), nullptr);
    CompiledBlock compiled_copy = *compiled;
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize word_count = compiled->code_size / sizeof(u32);
    EXPECT_EQ(std::count_if(words, words + word_count, [](u32 word) {
        return (word & 0xFF00001Fu) == 0xB5000008u; // CBNZ X8,cold
    }), 1) << "stack loads still carry per-access reservation branches";
    EXPECT_EQ(std::count_if(words, words + word_count, [](u32 word) {
        return (word & 0xFFF8001Fu) ==
               (arm64::tbnz(3, kLinearCheckMixedProtBit, 0) & 0xFFF8001Fu);
    }), 1) << "stack proof must carry one entry protection check, not one "
              "per load";
    constexpr u32 kMapLdrbMask = 0xFFE0FFE0u;
    EXPECT_EQ(std::count_if(words, words + word_count, [](u32 word) {
        return (word & kMapLdrbMask) ==
               (arm64::ldrb_reg(0, REG_LINEAR64_CHECK_MAP, 0) &
                kMapLdrbMask);
    }), 2) << "entry proof must load exactly its two endpoint map bytes";

    constexpr GuestAddr stack = kData64 + 0x1000;
    constexpr u64 first = 0x1122334455667788ull;
    constexpr u64 second = 0x8877665544332211ull;
    ASSERT_TRUE(m_memory->write_u64(stack + 8, first));
    ASSERT_TRUE(m_memory->write_u64(stack + 16, second));

    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = stack;
    cpu.flags.rflags = 0x8D7;
    cpu.jit_iters = 0xFFFFFFFFu;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, compiled_copy, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, first);
    EXPECT_EQ(cpu.gpr.rcx, second);
    EXPECT_EQ(cpu.gpr.rsp, stack);
    EXPECT_EQ(cpu.rip, kCode64 + code.size());
    EXPECT_EQ(cpu.jit_iters, 1u);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
}

TEST_F(JitLinear64Test, ProtectionLeaseBlocksMutationUntilReaderExits) {
    ASSERT_TRUE(m_memory->try_enter_linear_proof_lease());

    std::atomic<bool> writer_started{false};
    std::atomic<bool> writer_done{false};
    std::atomic<bool> writer_ok{false};
    std::thread writer([&] {
        writer_started.store(true, std::memory_order_release);
        const auto result = m_memory->protect_region(
            kData64, 0x4000, MemoryProtection::Read);
        writer_ok.store(result.is_ok(), std::memory_order_release);
        writer_done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    bool writer_pending = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!writer_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
            continue;
        }
        if (!m_memory->try_enter_linear_proof_lease()) {
            writer_pending = true;
            break;
        }
        m_memory->exit_linear_proof_lease();
        std::this_thread::yield();
    }
    const bool completed_while_held =
        writer_done.load(std::memory_order_acquire);

    m_memory->exit_linear_proof_lease();
    writer.join();

    EXPECT_TRUE(writer_pending);
    EXPECT_FALSE(completed_while_held);
    EXPECT_TRUE(writer_done.load(std::memory_order_acquire));
    EXPECT_TRUE(writer_ok.load(std::memory_order_acquire));
}

TEST_F(JitLinear64Test, PendingProtectionWriterDeoptsProofBlockBeforeBody) {
    // mov rax,[rsp+8] ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x48, 0x8B, 0x44, 0x24, 0x08,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CompiledBlock compiled_copy = *compiled;
    ASSERT_TRUE(compiled_copy.requires_protection_lease);
    ASSERT_TRUE(m_memory->try_enter_linear_proof_lease());

    std::atomic<bool> writer_started{false};
    std::atomic<bool> writer_ok{false};
    std::thread writer([&] {
        writer_started.store(true, std::memory_order_release);
        const auto result = m_memory->protect_region(
            kData64, 0x4000, MemoryProtection::Read);
        writer_ok.store(result.is_ok(), std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    bool writer_pending = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!writer_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
            continue;
        }
        if (!m_memory->try_enter_linear_proof_lease()) {
            writer_pending = true;
            break;
        }
        m_memory->exit_linear_proof_lease();
        std::this_thread::yield();
    }

    constexpr GuestAddr stack = kData64 + 0x1000;
    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = stack;
    cpu.gpr.rax = 0xAAAAAAAAAAAAAAAAull;
    cpu.gpr.rcx = 0xCCCCCCCCCCCCCCCCull;
    cpu.jit_iters = 0xFFFFFFFFu;
    const JitExecResult result = test::execute_like_dispatcher(*m_jit, compiled_copy, cpu);

    m_memory->exit_linear_proof_lease();
    writer.join();

    EXPECT_TRUE(writer_pending);
    EXPECT_EQ(result, JitExecResult::Unsupported);
    EXPECT_EQ(cpu.jit_iters, 0u);
    EXPECT_EQ(cpu.gpr.rax, 0xAAAAAAAAAAAAAAAAull);
    EXPECT_EQ(cpu.gpr.rcx, 0xCCCCCCCCCCCCCCCCull);
    EXPECT_TRUE(writer_ok.load(std::memory_order_acquire));
}

TEST_F(JitLinear64Test,
       StackEntryReservationProofRejectsOutOfWindowBeforeBody) {
    // mov rax,[rsp+8] ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x48, 0x8B, 0x44, 0x24, 0x08,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr outside = 0x00000007F0000000ull;
    constexpr u64 rax_poison = 0xAAAAAAAAAAAAAAAAull;
    constexpr u64 rcx_poison = 0xCCCCCCCCCCCCCCCCull;
    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = outside;
    cpu.gpr.rax = rax_poison;
    cpu.gpr.rcx = rcx_poison;
    cpu.rip = 0xBAD0BAD0BAD0BAD0ull;
    cpu.flags.rflags = 0x8D7;
    cpu.jit_iters = 0xFFFFFFFFu;

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Unsupported);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(cpu.jit_iters, 0u);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
    EXPECT_EQ(cpu.gpr.rsp, outside);
    EXPECT_EQ(cpu.gpr.rax, rax_poison);
    EXPECT_EQ(cpu.gpr.rcx, rcx_poison);
}

TEST_F(JitLinear64Test,
       StackEntryProtectionProofRejectsMixedHostPageBeforeBody) {
    // mov rax,[rsp+8] ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x48, 0x8B, 0x44, 0x24, 0x08,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CompiledBlock compiled_copy = *compiled;

    // Split one 16 KiB host page into readable and no-access 4 KiB guest
    // pages. The first load is permitted and the second is not; entry must
    // reject before either destination changes.
    ASSERT_TRUE(m_memory->protect_region(
        kData64 + 0x1000, 0x1000, MemoryProtection::None));
    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = kData64 + 0x0FF0;
    cpu.gpr.rax = 0xAAAAAAAAAAAAAAAAull;
    cpu.gpr.rcx = 0xCCCCCCCCCCCCCCCCull;
    cpu.jit_iters = 0xFFFFFFFFu;

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, compiled_copy, cpu),
              JitExecResult::Unsupported);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(cpu.jit_iters, 0u);
    EXPECT_EQ(cpu.gpr.rax, 0xAAAAAAAAAAAAAAAAull);
    EXPECT_EQ(cpu.gpr.rcx, 0xCCCCCCCCCCCCCCCCull);
}

TEST_F(JitLinear64Test, StackEntryReservationProofCoversMixedLoadWidths) {
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    m_jit->set_block_cache(m_cache.get());
    // mov eax,[rsp+8] ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x8B, 0x44, 0x24, 0x08,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 2u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CompiledBlock compiled_copy = *compiled;

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize word_count = compiled->code_size / sizeof(u32);
    const auto count_range_guards = [](const u32* begin, usize count) {
        const u32 crossing_extract = arm64::ubfx_x(
            8, 8, static_cast<u8>(kLinearHostPageShift), 1);
        usize guards = 0;
        for (usize i = 0; i < count; ++i) {
            if ((begin[i] & 0xFF00001Fu) != 0xB5000008u) continue;
            // UBFX crossing-bit ; CBNZ X8 is the scalar host-page crossing
            // branch, not the reservation-window guard counted here.
            if (i != 0 && begin[i - 1] == crossing_extract) continue;
            ++guards;
        }
        return guards;
    };
    const auto stack_guard_count = count_range_guards(words, word_count);
    EXPECT_EQ(stack_guard_count, 1)
        << "mixed-width stack loads did not share the range proof";
    EXPECT_EQ(std::count_if(words, words + word_count, [](u32 word) {
        return (word & 0xFFF8001Fu) ==
               (arm64::tbnz(3, kLinearCheckMixedProtBit, 0) & 0xFFF8001Fu);
    }), 1) << "mixed-width proof must share one entry protection check";

    constexpr GuestAddr stack = kData64 + 0x1000;
    constexpr u32 first = 0x89ABCDEFu;
    constexpr u64 second = 0x0123456789ABCDEFull;
    ASSERT_TRUE(m_memory->write_u32(stack + 8, first));
    ASSERT_TRUE(m_memory->write_u64(stack + 16, second));
    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = stack;
    cpu.gpr.rax = ~u64{0};
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, compiled_copy, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(first));
    EXPECT_EQ(cpu.gpr.rcx, second);
    EXPECT_EQ(cpu.rip, kCode64 + code.size());
}

TEST_F(JitLinear64Test, StackEntryProofAllowsRegisterOnlyMovFiller) {
    // mov rax,[rsp+8] ; mov rdx,rax ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x48, 0x8B, 0x44, 0x24, 0x08,
        0x48, 0x89, 0xC2,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CompiledBlock compiled_copy = *compiled;
    EXPECT_TRUE(compiled_copy.requires_protection_lease);
    EXPECT_EQ(compiled_copy.link_entry(), nullptr);

    constexpr GuestAddr stack = kData64 + 0x1000;
    constexpr u64 first = 0x1122334455667788ull;
    constexpr u64 second = 0x8877665544332211ull;
    ASSERT_TRUE(m_memory->write_u64(stack + 8, first));
    ASSERT_TRUE(m_memory->write_u64(stack + 16, second));
    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = stack;
    cpu.flags.rflags = 0x8D7;

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, compiled_copy, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, first);
    EXPECT_EQ(cpu.gpr.rdx, first);
    EXPECT_EQ(cpu.gpr.rcx, second);
    EXPECT_EQ(cpu.gpr.rsp, stack);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
    EXPECT_EQ(cpu.rip, kCode64 + code.size());
    EXPECT_EQ(cpu.jit_iters, 1u);
}

TEST_F(JitLinear64Test, StackEntryProofAllowsLeaFillerWithoutAccessOrFlags) {
    // mov rax,[rsp+8] ; lea rdx,[rsp+32] ; mov rcx,[rsp+16]
    const std::vector<u8> code = {
        0x48, 0x8B, 0x44, 0x24, 0x08,
        0x48, 0x8D, 0x54, 0x24, 0x20,
        0x48, 0x8B, 0x4C, 0x24, 0x10,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CompiledBlock compiled_copy = *compiled;
    EXPECT_TRUE(compiled_copy.requires_protection_lease);
    EXPECT_EQ(compiled_copy.link_entry(), nullptr);

    constexpr GuestAddr stack = kData64 + 0x1000;
    constexpr u64 first = 0x1122334455667788ull;
    constexpr u64 second = 0x8877665544332211ull;
    ASSERT_TRUE(m_memory->write_u64(stack + 8, first));
    ASSERT_TRUE(m_memory->write_u64(stack + 16, second));
    CpuState cpu = cpu64(0);
    cpu.gpr.rsp = stack;
    cpu.flags.rflags = 0x8D7;

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, compiled_copy, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, first);
    EXPECT_EQ(cpu.gpr.rdx, stack + 32);
    EXPECT_EQ(cpu.gpr.rcx, second);
    EXPECT_EQ(cpu.gpr.rsp, stack);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
    EXPECT_EQ(cpu.rip, kCode64 + code.size());
}

TEST_F(JitLinear64Test, StackEntryProofRejectsUnsafeInterveningInstructions) {
    struct Case {
        const char* name;
        std::vector<u8> middle;
        bool delegated;
    };
    const std::array<Case, 4> cases{{
        {"rsp-write", {0x48, 0x89, 0xC4}, false}, // mov rsp,rax
        {"lea-rsp-write", {0x48, 0x8D, 0x64, 0x24, 0x20}, false},
        {"extra-memory", {0x48, 0x8B, 0x13}, false}, // mov rdx,[rbx]
        {"delegate", {0x0F, 0xA2}, true}, // cpuid
        // JMP rel is RIP-only (Intel SDM Vol. 2 Jcc/JMP) and is a safe
        // invariant-range filler; it is not a reject case anymore.
    }};

    for (usize index = 0; index < cases.size(); ++index) {
        SCOPED_TRACE(cases[index].name);
        std::vector<u8> code = {
            0x48, 0x8B, 0x44, 0x24, 0x08,
        };
        code.insert(code.end(), cases[index].middle.begin(),
                    cases[index].middle.end());
        code.insert(code.end(), {
            0x48, 0x8B, 0x4C, 0x24, 0x10,
        });
        const GuestAddr address = kCode64 + 0x400 + index * 0x100;
        ASSERT_TRUE(m_memory->write_bytes(address, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = address;
        ASSERT_EQ(block.instruction_count, 3u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_FALSE(compiled->requires_protection_lease);
        EXPECT_NE(compiled->link_entry(), nullptr);
        if (cases[index].delegated) {
            EXPECT_EQ(compiled->native_insns, block.instruction_count - 1);
            EXPECT_NE(compiled->delegate_insns, nullptr);
        }
    }
}

}  // namespace
