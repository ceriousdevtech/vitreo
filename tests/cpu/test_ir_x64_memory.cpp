// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_memory.cpp
/// @brief Long-mode IR emission: 64-bit loads, stores, faults and live snapshots.

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
TEST_F(IrEmit64Memory, RipRelativeLoadsAndStoresKeepHighAddress) {
    constexpr GuestAddr data = kCode + 0x2000;
    ASSERT_TRUE(memory.map_region(data, 0x1000, MemoryProtection::ReadWrite));
    constexpr u64 value = 0xFEDCBA9876543210ull;
    for (bool wide : {false, true}) {
        ASSERT_TRUE(memory.write_u64(data, value));
        ASSERT_TRUE(memory.write_u64(data + 8, 0));
        std::vector<u8> bytes;
        if (wide) bytes.push_back(0x48);
        bytes.insert(bytes.end(), {0x8B, 0x05}); // mov eax/rax,[rip+disp32]
        append_disp32(bytes, static_cast<i64>(data - (kCode + bytes.size() + 4)));
        if (wide) bytes.push_back(0x48);
        bytes.insert(bytes.end(), {0x89, 0x05}); // mov [rip+disp32],eax/rax
        append_disp32(bytes, static_cast<i64>(data + 8 - (kCode + bytes.size() + 4)));
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        auto state = seed();
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        const u64 expected = wide ? value : static_cast<u32>(value);
        EXPECT_EQ(state.gpr.rax, expected);
        EXPECT_EQ(memory.read_u64(data + 8).value_or(0), expected);
        state.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, 0xAD7u);
        EXPECT_EQ(state.rip, kCode + bytes.size());
    }
}

TEST_F(IrEmit64Memory, MemoryImmediate64SignExtendsEncoded32Bits) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (u32 immediate : {0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu}) {
        // REX.W C7 /0: mov qword [r8+8],sign_extended_imm32.
        std::vector<u8> bytes{0x49, 0xC7, 0x40, 0x08};
        append_disp32(bytes, immediate);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        auto state = seed();
        state.gpr.r8 = kData;
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        const u64 expected = static_cast<u64>(static_cast<i64>(static_cast<i32>(immediate)));
        EXPECT_EQ(memory.read_u64(kData + 8).value_or(0), expected);
        EXPECT_EQ(state.gpr.rax, 0xFEDCBA9876543210ull);
        state.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, 0xAD7u);
    }
}

TEST_F(IrEmit64Memory, LoadAndStoreAcrossSeparateMappedRegions) {
    for (GuestAddr page : {kData, kData + 0x1000, kData + 0x4000, kData + 0x5000})
        ASSERT_TRUE(memory.map_region(page, 0x1000, MemoryProtection::ReadWrite));
    constexpr u64 value = 0x87654321FEDCBA98ull;
    for (bool wide : {false, true}) {
        const usize width = wide ? 8 : 4;
        const GuestAddr source = kData + 0x1000 - width / 2;
        const GuestAddr destination = kData + 0x5000 - width / 2;
        std::array<u8, 8> input{}, zero{};
        for (usize i = 0; i < width; ++i) input[i] = static_cast<u8>(value >> (i * 8));
        ASSERT_TRUE(memory.write_bytes(source, input.data(), width));
        ASSERT_TRUE(memory.write_bytes(destination, zero.data(), width));
        const u8 rex = wide ? 0x49 : 0x41;
        auto* native = compile({rex, 0x8B, 0x00, rex, 0x89, 0x01});
        ASSERT_NE(native, nullptr);
        auto state = seed();
        state.gpr.r8 = source;
        state.gpr.r9 = destination;
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        EXPECT_EQ(state.gpr.rax, wide ? value : static_cast<u32>(value));
        std::array<u8, 8> output{};
        ASSERT_TRUE(memory.read_bytes(destination, output.data(), width));
        EXPECT_EQ(output, input);
        state.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, 0xAD7u);
    }
}

TEST_F(IrEmit64Memory, NativeStoreInvalidatesExecutableTarget) {
    ASSERT_TRUE(memory.map_region(kData, 0x4000,
                                  MemoryProtection::ReadWriteExecute));
    const std::array<u8, 5> target_bytes{0xB8, 0x79, 0x35, 0x01, 0};
    ASSERT_TRUE(memory.write_bytes(kData, target_bytes.data(), target_bytes.size()));
    cache = std::make_unique<BlockCache>(4);
    jit->set_block_cache(cache.get());
    BasicBlock target;
    target.start_addr = kData;
    target.byte_length = static_cast<u32>(target_bytes.size());
    target.is_64bit = true;
    MemoryByteReader reader(target_bytes.data(), target_bytes.size());
    DecodedInstruction instruction;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction),
              DecodeError::Success);
    target.instructions.push_back(instruction);
    target.instruction_count = 1;
    cache->insert(std::move(target));
    auto* native = compile({0x49, 0x89, 0x00}); // mov [r8],rax
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.r8 = kData;
    ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
    ASSERT_FALSE(state.pending_raise.active);
    EXPECT_EQ(memory.read_u64(kData).value_or(0), state.gpr.rax);
    EXPECT_EQ(cache->lookup(kData), nullptr)
        << "executable guest stores must invalidate cached decoded instructions";
}

TEST_F(IrEmit64Memory, UnmappedHighAddressPreservesFaultState) {
    // CMP is a real preceding flag producer; the load must not expose flags
    // from region bounds checks or commit its destination before the fault.
    const std::vector<u8> bytes{0x48, 0x39, 0xD1, 0x49, 0x8B, 0x00};
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.rcx = 0x8000000000000000ull;
    state.gpr.rdx = 1;
    state.gpr.r8 = kData + 0x8000; // wholly unmapped, including host page
    CpuState oracle = state;
    Interpreter interpreter(oracle, memory);
    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
    oracle.materialize_lazy_flags();
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    ASSERT_GE(state.pending_raise.num_args, 2u);
    EXPECT_EQ(state.pending_raise.args[0], 0u);
    EXPECT_EQ(state.pending_raise.args[1], kData + 0x8000);
    EXPECT_EQ(state.rip, kCode + 3);
    EXPECT_EQ(state.gpr.rax, 0xFEDCBA9876543210ull);
    state.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
}

TEST_F(IrEmit64Memory, LiveScalarSnapshotsSurviveMemoryHelper) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_u64(kData, 0x8765432112345678ull));
    // Three commoned constants plus intermediate results exceed X0-X2. The
    // load's cold helper must not corrupt the constants reused afterward.
    const std::vector<u8> bytes{
        0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3,
        0x4D,0x8B,0x08,
        0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3};
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    for (u64 value : {0ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull}) {
        auto state = seed();
        state.gpr.rax = value;
        state.gpr.rbx = ~value;
        state.gpr.rdx = value ^ 0x8000000000000000ull;
        state.gpr.r8 = kData;
        auto oracle = state;
        Interpreter interpreter(oracle, memory);
        for (unsigned i = 0; i < 7; ++i)
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        ASSERT_FALSE(state.pending_raise.active);
        for (u8 reg = 0; reg < 16; ++reg)
            EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg)) << unsigned(reg);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
        EXPECT_EQ(state.rip, oracle.rip);
    }
}

TEST_F(IrEmit64Memory, FaultWithLiveSnapshotsExposesOnlyCommittedGuestState) {
    for (bool store : {false, true}) {
        const std::vector<u8> bytes{
            0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3,
            0x4D,static_cast<u8>(store ? 0x89 : 0x8B),0x08,
            0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3};
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
            auto state = seed();
        state.gpr.r8 = kData + 0x8000;
        state.gpr.r9 = 0xABCD0123456789EFull;
        state.gpr.rbx = 0xFFFFFFFFFFFFFFFFull;
        state.gpr.rdx = 0x7FFFFFFFFFFFFFFFull;
        auto oracle = state;
        Interpreter interpreter(oracle, memory);
        for (unsigned i = 0; i < 3; ++i)
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        (void)jit->execute(*native, state);
        ASSERT_TRUE(state.pending_raise.active);
        EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
        ASSERT_GE(state.pending_raise.num_args, 2u);
        EXPECT_EQ(state.pending_raise.args[0], store ? 1u : 0u);
        EXPECT_EQ(state.pending_raise.args[1], kData + 0x8000);
        EXPECT_EQ(state.rip, oracle.rip);
        for (u8 reg = 0; reg < 16; ++reg)
            EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg)) << unsigned(reg);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
    }
}

TEST_F(IrEmit64Memory, ReadOnlyStoreWithLiveSnapshotsDoesNotCommitFollowingWork) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::Read));
    const std::vector<u8> bytes{
        0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3,
        0x4D,0x89,0x08,
        0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3};
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.r8 = kData;
    state.gpr.r9 = 0x123456789ABCDEF0ull;
    auto oracle = state;
    Interpreter interpreter(oracle, memory);
    for (unsigned i = 0; i < 3; ++i)
        ASSERT_EQ(interpreter.step(), ExecResult::Continue);
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(state.rip, oracle.rip);
    EXPECT_EQ(memory.read_u64(kData).value_or(~u64{0}), 0u);
    for (u8 reg = 0; reg < 16; ++reg)
        EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg)) << unsigned(reg);
    state.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
}

TEST_F(IrEmit64Memory, NestedNativeExecutionDuringStorePreservesOuterSnapshots) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWriteExecute));
    cache = std::make_unique<BlockCache>();
    jit->set_block_cache(cache.get());

    GuestMemory nested_memory;
    ASSERT_TRUE(nested_memory.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> nested_bytes{
        0x48,0x83,0xC0,7, 0x48,0x83,0xC3,8, 0x48,0x83,0xC2,9,
        0x48,0x83,0xC0,7, 0x48,0x83,0xC3,8, 0x48,0x83,0xC2,9};
    ASSERT_TRUE(nested_memory.write_bytes(kCode, nested_bytes.data(), nested_bytes.size()));
    JitCompiler nested_jit(nested_memory);
    BasicBlock nested_block;
    nested_block.start_addr = kCode;
    nested_block.is_64bit = true;
    nested_block.byte_length = static_cast<u32>(nested_bytes.size());
    MemoryByteReader reader(nested_bytes.data(), nested_bytes.size());
    while (reader.remaining()) {
        DecodedInstruction instruction;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction), DecodeError::Success);
        nested_block.instructions.push_back(instruction);
    }
    nested_block.instruction_count = static_cast<u32>(nested_block.instructions.size());
    auto* nested_native = nested_jit.compile(nested_block);
    ASSERT_NE(nested_native, nullptr);
    ASSERT_TRUE(nested_jit.ir_emit_active());

    const std::vector<u8> bytes{
        0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3,
        0x4D,0x89,0x08,
        0x48,0x83,0xC0,1, 0x48,0x83,0xC3,2, 0x48,0x83,0xC2,3};
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.r8 = kData;
    state.gpr.r9 = 0x123456789ABCDEF0ull;
    auto oracle = state;
    Interpreter interpreter(oracle, memory);
    for (unsigned i = 0; i < 7; ++i)
        ASSERT_EQ(interpreter.step(), ExecResult::Continue);
    // The store helper only notifies writes that overlap a cached translation,
    // so the destination page must hold a block. Register it *after* the
    // interpreter oracle: that run stores to the same page and the compiler's
    // own observer would invalidate the entry, leaving the page code-free.
    BasicBlock watched;
    watched.start_addr = kData;
    watched.byte_length = 8;
    watched.is_64bit = true;
    cache->insert(std::move(watched));
    ASSERT_TRUE(memory.jit_code_pages_overlap(kData, sizeof(u64)));
    unsigned entries = 0;
    memory.set_thread_executable_write_observer([&](GuestAddr address, usize, u64) {
        if (address != kData) return false;
        ++entries;
        auto nested = seed();
        EXPECT_EQ(nested_jit.execute(*nested_native, nested), JitExecResult::Continue);
        EXPECT_EQ(nested.gpr.rax, 0xFEDCBA9876543210ull + 14);
        EXPECT_EQ(nested.gpr.rbx, 16u);
        EXPECT_EQ(nested.gpr.rdx, 18u);
        return false;
    });
    const auto result = jit->execute(*native, state);
    memory.clear_thread_executable_write_observer();
    ASSERT_EQ(result, JitExecResult::Continue);
    ASSERT_EQ(entries, 1u) << "must re-enter native execution from a guest store helper";
    ASSERT_FALSE(state.pending_raise.active);
    for (u8 reg = 0; reg < 16; ++reg)
        EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg)) << unsigned(reg);
    state.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
}
} // namespace
#endif
