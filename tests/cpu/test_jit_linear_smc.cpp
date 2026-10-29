// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// 32-bit linear-store self-modifying-code invalidation and the process code
// span. Split from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Gap #1: the linear store path must still drive SMC invalidation. A store
// whose address is in the compiled-code span routes through the write helper
// (which calls notify_executable_write), so a cached block on that page is
// invalidated — even though the raw inline store would have skipped it.
TEST_F(JitLinearTest, LinearStoreIntoCodePageInvalidatesBlock) {
    // Cache a victim block at the code region so its page is in the code span.
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44,  // MOV EAX, imm32
                              0xC3};                          // RET
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);
    ASSERT_FALSE(m_cache->range_is_code_free(kCode, 0x1000));

    // A storing block on a different code page writes into the victim's page.
    const GuestAddr store_site = kCode + 0x800;
    std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock;
    sblock.start_addr = store_site;
    {
        MemoryByteReader r(code.data(), code.size());
        DecodedInstruction i;
        ASSERT_EQ(decode_instruction(r, DecodeMode::Mode32, i), DecodeError::Success);
        sblock.instructions.push_back(i);
        sblock.byte_length = static_cast<u32>(r.position());
        sblock.instruction_count = 1;
    }
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kCode));  // write into the victim's page
    cpu.gpr.set32(EAX, 0x90909090);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);

    EXPECT_EQ(m_cache->size(), 0u)
        << "linear store into a code page skipped SMC invalidation";
}

// Cross-modifying code uses process memory, not a thread-private instruction
// cache. A store emitted by one JIT must notify when the target page is cached
// by another JIT attached to the same GuestMemory.
TEST_F(JitLinearTest, LinearStoreInvalidatesCodeCachedByAnotherJit) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    const GuestAddr store_site = kCode + 0x800;
    std::vector<u8> store = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    ASSERT_TRUE(m_memory->write_bytes(store_site, store.data(), store.size()));

    BasicBlock victim_block = build_block(victim);
    BasicBlock store_block;
    store_block.start_addr = store_site;
    {
        MemoryByteReader reader(store.data(), store.size());
        DecodedInstruction instruction;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instruction),
                  DecodeError::Success);
        store_block.instructions.push_back(instruction);
        store_block.byte_length = static_cast<u32>(reader.position());
        store_block.instruction_count = 1;
    }

    JitCompiler other_jit(*m_memory);
    BlockCache other_cache;
    other_jit.set_block_cache(&other_cache);
    ASSERT_NE(other_jit.compile(victim_block), nullptr);
    other_cache.insert(victim_block);
    ASSERT_EQ(other_cache.size(), 1u);
    ASSERT_TRUE(m_cache->range_is_code_free(kCode, 0x1000));
    ASSERT_FALSE(other_cache.range_is_code_free(kCode, 0x1000));

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kCode));
    cpu.gpr.set32(EAX, 0x90909090);
    CompiledBlock* writer = m_jit->compile(store_block);
    ASSERT_NE(writer, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *writer, cpu);

    EXPECT_EQ(other_cache.size(), 0u)
        << "linear cross-thread store skipped process-wide SMC invalidation";
}

// FlushInstructionCache must invalidate JIT translations covering the flushed
// range (Vitreo's instruction-cache analogue). Writes already notify; this
// covers the explicit Win32 flush after code generation.
TEST_F(JitLinearTest, FlushInstructionCacheInvalidatesCachedBlock) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44,  // MOV EAX, imm32
                              0xC3};                          // RET
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    // Simulate kernel32!FlushInstructionCache(hProcess, code, size).
    m_memory->notify_executable_write(kCode, victim.size());
    EXPECT_EQ(m_cache->size(), 0u)
        << "FlushInstructionCache-equivalent notify left stale JIT block";
    EXPECT_NE(m_jit->compile(vblock), nullptr)
        << "a single cache flush permanently disabled compilation";
}

TEST_F(JitLinearTest, ExecutableWritesToPagesWithoutCodeDoNotOverflowClear) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    // RWX data stores used to fill the 4096-slot executable-write log.
    // Overflow published (0, 0) and wiped every translation. Writes that
    // miss the compiled-page set must not publish.
    constexpr GuestAddr kRwxData = 0x20000;
    ASSERT_TRUE(m_memory->map_region(
                    kRwxData, 0x1000, MemoryProtection::ReadWriteExecute)
                    .is_ok());
    for (int i = 0; i < 5000; ++i) {
        m_memory->notify_executable_write(kRwxData, 4);
    }
    (void)m_jit->synchronize_executable_writes();
    EXPECT_EQ(m_cache->size(), 1u)
        << "RWX data-store flood cleared unrelated JIT blocks";
}

TEST_F(JitLinearTest, ExecutableWriteLogOverflowSentinelClearsAllBlocks) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    // (0, 0) is GuestMemory's lost-history sentinel (log overflow / torn
    // slot), not Win32 FlushInstructionCache(NULL). That API is an x86
    // I-cache barrier and must not wipe translations of unchanged bytes.
    m_memory->notify_executable_write(0, 0);
    EXPECT_EQ(m_cache->size(), 0u)
        << "executable-write overflow sentinel did not clear JIT cache";
}

TEST_F(JitLinearTest, ForeignThreadFlushInvalidatesOwningThreadCache) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    // The targeted observer is thread-local, but FlushInstructionCache is a
    // process contract. The generation carries a foreign-thread flush back to
    // this thread-owned cache at the next dispatcher safe point.
    std::thread foreign_flusher([&] {
        m_memory->notify_executable_write(kCode, victim.size());
    });
    foreign_flusher.join();

    EXPECT_TRUE(m_jit->synchronize_executable_writes());
    EXPECT_EQ(m_cache->size(), 0u)
        << "foreign-thread instruction-cache flush left stale JIT code";
    EXPECT_NE(m_jit->compile(vblock), nullptr)
        << "a single foreign flush permanently disabled compilation";
}

TEST_F(JitLinearTest, HeapAboveSingleImageIsOutsideProcessCodeSpan) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock block = build_block(victim);
    ASSERT_NE(m_jit->compile(block), nullptr);
    m_cache->insert(block);
    EXPECT_LE(m_memory->jit_code_span_low(), static_cast<u32>(kCode));
    EXPECT_LE(m_memory->jit_code_span_high(), static_cast<u32>(kData));
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(kData, 4));
    EXPECT_NE(m_memory->jit_code_chunk_refs(kCode), 0u);
    EXPECT_EQ(m_memory->jit_code_chunk_refs(kData), 0u)
        << "heap 2 MiB chunk must stay empty so linear stores skip the page map";
}

// Compiling at a low image and a high DLL must not treat the heap between
// them as code. The old [min,max) span did, so every decompress store took
// the C++ write helper.
TEST_F(JitLinearTest, DataBetweenDistantCodePagesStaysInline) {
    constexpr GuestAddr kHighCode = 0x02000000;
    ASSERT_TRUE(m_memory->map_region(
                    kHighCode, 0x4000, MemoryProtection::ReadWrite));

    auto cache_victim = [&](GuestAddr at) {
        std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
        ASSERT_TRUE(m_memory->write_bytes(at, victim.data(), victim.size()));
        BasicBlock b;
        b.start_addr = at;
        MemoryByteReader r(victim.data(), victim.size());
        DecodedInstruction i;
        while (r.remaining() > 0 &&
               decode_instruction(r, DecodeMode::Mode32, i)
                   == DecodeError::Success) {
            b.instructions.push_back(i);
        }
        b.byte_length = static_cast<u32>(r.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        ASSERT_NE(m_jit->compile(b), nullptr);
        m_cache->insert(b);
    };
    cache_victim(kCode);
    cache_victim(kHighCode);
    ASSERT_EQ(m_cache->size(), 2u);
    EXPECT_FALSE(m_cache->range_is_code_free(kCode, 0x1000));
    EXPECT_FALSE(m_cache->range_is_code_free(kHighCode, 0x1000));
    EXPECT_TRUE(m_cache->range_is_code_free(kData, 0x1000))
        << "heap between two images was treated as compiled code";
    EXPECT_LE(m_memory->jit_code_span_low(), static_cast<u32>(kCode));
    EXPECT_GE(m_memory->jit_code_span_high(), static_cast<u32>(kHighCode));
    EXPECT_TRUE(m_memory->jit_code_pages_overlap(kCode, 1));
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(kData, 0x1000));

    const GuestAddr store_site = kCode + 0x800;
    std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock;
    sblock.start_addr = store_site;
    {
        MemoryByteReader r(code.data(), code.size());
        DecodedInstruction i;
        ASSERT_EQ(decode_instruction(r, DecodeMode::Mode32, i),
                  DecodeError::Success);
        sblock.instructions.push_back(i);
        sblock.byte_length = static_cast<u32>(r.position());
        sblock.instruction_count = 1;
    }
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kData));
    cpu.gpr.set32(EAX, 0xAABBCCDDu);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);

    EXPECT_EQ(m_cache->size(), 2u)
        << "a data store inside the code span invalidated unrelated blocks";
    u32 got = 0;
    ASSERT_TRUE(m_memory->read_bytes(kData, reinterpret_cast<u8*>(&got), 4));
    EXPECT_EQ(got, 0xAABBCCDDu);
}

// .data next to .text shares the 2 MiB chunk, so the chunk reject cannot
// skip the page map. The unmarked page must still store inline (no SMC).
TEST_F(JitLinearTest, UnmarkedPageInLiveChunkDoesNotInvalidate) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    constexpr GuestAddr kNearData = 0x20000;  // same 2 MiB as kCode
    ASSERT_TRUE(m_memory->map_region(
                    kNearData, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    EXPECT_NE(m_memory->jit_code_chunk_refs(kNearData), 0u);
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(kNearData, 4));

    const GuestAddr store_site = kCode + 0x800;
    std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock;
    sblock.start_addr = store_site;
    {
        MemoryByteReader r(code.data(), code.size());
        DecodedInstruction i;
        ASSERT_EQ(decode_instruction(r, DecodeMode::Mode32, i),
                  DecodeError::Success);
        sblock.instructions.push_back(i);
        sblock.byte_length = static_cast<u32>(r.position());
        sblock.instruction_count = 1;
    }
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kNearData));
    cpu.gpr.set32(EAX, 0x55667788u);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);

    EXPECT_EQ(m_cache->size(), 1u)
        << "store to unmarked page in a live 2 MiB chunk invalidated code";
    u32 got = 0;
    ASSERT_TRUE(m_memory->read_bytes(kNearData, reinterpret_cast<u8*>(&got), 4));
    EXPECT_EQ(got, 0x55667788u);
}

// 32-bit linear store64 (MOVQ / FPU m64) used to always call jit_mem_write64.
// Heap between two images must stay inline, and a qword store into a code
// page must still invalidate.
TEST_F(JitLinearTest, DataBetweenDistantCodePagesQwordStoreStaysInline) {
    constexpr GuestAddr kHighCode = 0x02000000;
    ASSERT_TRUE(m_memory->map_region(
                    kHighCode, 0x4000, MemoryProtection::ReadWrite));

    auto cache_victim = [&](GuestAddr at) {
        std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
        ASSERT_TRUE(m_memory->write_bytes(at, victim.data(), victim.size()));
        BasicBlock b;
        b.start_addr = at;
        MemoryByteReader r(victim.data(), victim.size());
        DecodedInstruction i;
        while (r.remaining() > 0 &&
               decode_instruction(r, DecodeMode::Mode32, i)
                   == DecodeError::Success) {
            b.instructions.push_back(i);
        }
        b.byte_length = static_cast<u32>(r.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        ASSERT_NE(m_jit->compile(b), nullptr);
        m_cache->insert(b);
    };
    cache_victim(kCode);
    cache_victim(kHighCode);
    ASSERT_EQ(m_cache->size(), 2u);

    const u64 neighbor = 0x1111111111111111ull;
    ASSERT_TRUE(m_memory->write_bytes(
                    kData, reinterpret_cast<const u8*>(&neighbor), 8));
    ASSERT_TRUE(m_memory->write_bytes(
                    kData + 8, reinterpret_cast<const u8*>(&neighbor), 8));

    const GuestAddr store_site = kCode + 0x800;
    std::vector<u8> code = {0x0F, 0x7F, 0x03};  // MOVQ [EBX], MM0
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock;
    sblock.start_addr = store_site;
    {
        MemoryByteReader r(code.data(), code.size());
        DecodedInstruction i;
        ASSERT_EQ(decode_instruction(r, DecodeMode::Mode32, i),
                  DecodeError::Success);
        sblock.instructions.push_back(i);
        sblock.byte_length = static_cast<u32>(r.position());
        sblock.instruction_count = 1;
    }
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kData));
    cpu.fpu.set_mm(0, 0xAABBCCDD11223344ull);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_cache->size(), 2u)
        << "a qword data store inside the code span invalidated unrelated blocks";
    u64 got = 0;
    u64 after = 0;
    ASSERT_TRUE(m_memory->read_bytes(kData, reinterpret_cast<u8*>(&got), 8));
    ASSERT_TRUE(m_memory->read_bytes(kData + 8, reinterpret_cast<u8*>(&after), 8));
    EXPECT_EQ(got, 0xAABBCCDD11223344ull);
    EXPECT_EQ(after, neighbor) << "linear store64 wrote past 8 bytes";
}

TEST_F(JitLinearTest, LinearStore64IntoCodePageInvalidatesBlock) {
    std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    const GuestAddr store_site = kCode + 0x800;
    std::vector<u8> code = {0x0F, 0x7F, 0x03};  // MOVQ [EBX], MM0
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock;
    sblock.start_addr = store_site;
    {
        MemoryByteReader r(code.data(), code.size());
        DecodedInstruction i;
        ASSERT_EQ(decode_instruction(r, DecodeMode::Mode32, i),
                  DecodeError::Success);
        sblock.instructions.push_back(i);
        sblock.byte_length = static_cast<u32>(r.position());
        sblock.instruction_count = 1;
    }
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kCode));
    cpu.fpu.set_mm(0, 0x9090909090909090ull);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);

    EXPECT_EQ(m_cache->size(), 0u)
        << "linear store64 into a code page skipped SMC invalidation";
}

}  // namespace
