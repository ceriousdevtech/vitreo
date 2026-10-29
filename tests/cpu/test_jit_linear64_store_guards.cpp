// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 live store guards: the pinned base, write-watch, protection and
// self-modifying code during native execution. Split from test_jit_linear.cpp;
// shared fixtures and matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

TEST_F(JitLinear64Test, ScalarStoreHotPathsUsePinnedLinearBase) {
    struct Case {
        std::vector<u8> code;
        int width;
    };
    const std::vector<Case> cases = {
        {{0x88, 0x03}, 1},
        {{0x66, 0x89, 0x03}, 2},
        {{0x89, 0x03}, 4},
        {{0x48, 0x89, 0x03}, 8},
    };

    for (const Case& c : cases) {
        SCOPED_TRACE("store width=" + std::to_string(c.width));
        ASSERT_TRUE(m_memory->write_bytes(kCode64, c.code.data(), c.code.size()));
        BasicBlock block = build64(c.code);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        const u32 words = compiled->code_size / 4;
        std::vector<u32> emitted(words);
        std::memcpy(emitted.data(), compiled->code, compiled->code_size);

        EXPECT_TRUE(has_guarded_linear64_store(emitted, c.width))
            << "no exact-address ordered store with guarded crossing fallback";
        EXPECT_FALSE(has_linear64_hot_helper_call(emitted, c.width, true))
            << "store reservation/page checks must stay inline";
        EXPECT_TRUE(has_linear64_reservation_check(emitted, c.width, true))
            << "linear64 store lost its reservation check";
        // The first branch to the cold path bounds the hot path: the inline
        // reservation CBNZ X8, or under range-by-fault the check byte's
        // CBNZ X3.
        const u32 fail_word = count_range_fault_sites(emitted) != 0
                                ? 0xB5000003u : 0xB5000008u;
        const auto fail_branch = std::find_if(
            emitted.begin(), emitted.end(), [fail_word](u32 word) {
                return (word & 0xFF00001Fu) == fail_word;
            });
        ASSERT_NE(fail_branch, emitted.end())
            << "linear64 store does not branch cold on a failed check";
        // The SMC obligation is now a bit in the per-host-page check byte.
        // The two acquire-loaded code-span compares must stay off the hot path.
        EXPECT_EQ(std::count(emitted.begin(), fail_branch, arm64::ldar_x(3, 3)), 0)
            << "linear64 store still acquire-loads the x64 SMC span per store";
        EXPECT_EQ(std::find(emitted.begin(), emitted.end(),
                            arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                              JIT_CTX_OFF_CODE_SPAN64_HI >> 3)),
                  emitted.end())
            << "linear64 store still reads the x64 code span per store";
        // The write-watch obligation is kLinearCheckWatched in the helper's
        // check byte. The site must not grow a process-global watch probe.
        EXPECT_EQ(std::find(emitted.begin(), emitted.end(),
                            arm64::ldr_x_uoff(3, REG_LINEAR64_CHECK_MAP, 0)),
                  emitted.end())
            << "linear64 store still dereferences X15 as a watch slot";
        EXPECT_EQ(std::find(emitted.begin(), emitted.end(),
                            arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                              JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3)),
                  emitted.end())
            << "linear64 store still reloads the check-map base per store";
        const std::array<u32, 2> old_watch_probe = {
            arm64::ldr_x_uoff(3, REG_JIT_CTX,
                              JIT_CTX_OFF_WRITE_WATCH_PAGES >> 3),
            arm64::ldr_x_uoff(3, 3, 0),
        };
        EXPECT_TRUE(std::search(emitted.begin(), emitted.end(),
                                old_watch_probe.begin(), old_watch_probe.end()) ==
                    emitted.end())
            << "linear64 store still reloads the watch-slot address per store";
        const std::array<u32, 3> old_cycle = {
            arm64::mov_x(17, 9), arm64::mov_x(9, 10),
            arm64::mov_x(10, 17)};
        EXPECT_TRUE(std::search(emitted.begin(), emitted.end(),
                                old_cycle.begin(), old_cycle.end()) ==
                    emitted.end())
            << "X9/X10 scalar store cycle still uses a third hot-path move";
    }
}

// A MEM_WRITE_WATCH allocation can become live after execute() enters native
// code. The pinned register must retain the publication slot's address (not a
// snapshot of its null value), and helper returns must restore that address.
TEST_F(JitLinear64Test, StoreSeesWriteWatchActivatedDuringNativeExecution) {
    constexpr GuestAddr fill_addr = 0x00000001A0000000ull;
    constexpr usize fill_size = 2 * 1024 * 1024;
    constexpr GuestAddr watched_store = kData64 + 0x1040;
    constexpr usize watched_pages = 4;

    ASSERT_TRUE(m_memory->map_region(fill_addr, fill_size,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(kData64, 0));

    // MOV dword ptr [RSI],1 ; REP STOSB ; MOV [RBX],EAX. DF=1 keeps the REP
    // helper busy long enough to publish the watch after native entry.
    const std::vector<u8> code = {
        0xC7, 0x06, 0x01, 0x00, 0x00, 0x00,
        0xF3, 0xAA,
        0x89, 0x03,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    Byte* started_host = m_memory->host_ptr_mut(kData64, sizeof(u32));
    ASSERT_NE(started_host, nullptr);
    auto& started_word = *reinterpret_cast<u32*>(started_host);
    std::atomic_ref<u32> started(started_word);
    std::atomic<bool> execution_done{false};
    JitExecResult result = JitExecResult::Continue;
    CpuState cpu = cpu64(watched_store);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSI), kData64);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RDI),
                  fill_addr + fill_size - 1);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), fill_size);
    cpu.gpr.set32(EAX, 0xA5A5A5A5u);
    cpu.flags.set_df(true);

    std::vector<u8> dirty((watched_pages + 7) / 8, 0);
    std::vector<u8> armed((watched_pages + 7) / 8, 0);
    std::thread writer([&] {
        result = test::execute_like_dispatcher(*m_jit, *compiled, cpu);
        execution_done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    while (started.load(std::memory_order_acquire) == 0 &&
           !execution_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool saw_entry = started.load(std::memory_order_acquire) != 0;
    const bool activated_during_execute =
        saw_entry && !execution_done.load(std::memory_order_acquire);
    if (activated_during_execute) {
        write_watch::register_region(kData64, 0x4000, dirty.data(),
                                     armed.data(), /*executable=*/false);
    }
    writer.join();
    if (activated_during_execute) {
        write_watch::unregister_region(kData64);
    }

    ASSERT_TRUE(saw_entry) << "writer never reached the native entry marker";
    ASSERT_TRUE(activated_during_execute)
        << "REP window ended before write-watch activation";
    ASSERT_EQ(result, JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_TRUE(write_watch::test_bit(dirty.data(), 1))
        << "store missed write-watch activation during native execution";
}

TEST_F(JitLinear64Test, StoreSeesReadOnlyProtectionDuringNativeExecution) {
    constexpr GuestAddr fill_addr = 0x00000001A0000000ull;
    constexpr usize fill_size = 2 * 1024 * 1024;
    constexpr GuestAddr protected_store = kData64 + 0x240;
    constexpr u32 initial_value = 0x11223344u;
    constexpr u32 rejected_value = 0xA5A5A5A5u;

    ASSERT_TRUE(m_memory->map_region(fill_addr, fill_size,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(kData64, 0));
    ASSERT_TRUE(m_memory->write_u32(protected_store, initial_value));

    // Publish an entry marker, remain inside this execute() in REP STOSB, then
    // attempt a scalar store after another thread changes the target page to
    // read-only. The live protection generation must reject any cached slot.
    const std::vector<u8> code = {
        0xC7, 0x06, 0x01, 0x00, 0x00, 0x00,
        0xF3, 0xAA,
        0x89, 0x03,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    Byte* started_host = m_memory->host_ptr_mut(kData64, sizeof(u32));
    ASSERT_NE(started_host, nullptr);
    auto& started_word = *reinterpret_cast<u32*>(started_host);
    std::atomic_ref<u32> started(started_word);
    std::atomic<bool> execution_done{false};
    JitExecResult result = JitExecResult::Continue;
    CpuState cpu = cpu64(protected_store);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSI), kData64);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RDI),
                  fill_addr + fill_size - 1);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), fill_size);
    cpu.gpr.set32(EAX, rejected_value);
    cpu.flags.set_df(true);

    std::thread writer([&] {
        result = test::execute_like_dispatcher(*m_jit, *compiled, cpu);
        execution_done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    while (started.load(std::memory_order_acquire) == 0 &&
           !execution_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool saw_entry = started.load(std::memory_order_acquire) != 0;
    const bool protected_during_execute =
        saw_entry && !execution_done.load(std::memory_order_acquire);
    if (protected_during_execute) {
        EXPECT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                              MemoryProtection::Read));
    }
    writer.join();

    ASSERT_TRUE(saw_entry) << "writer never reached the native entry marker";
    ASSERT_TRUE(protected_during_execute)
        << "REP window ended before the protection change";
    ASSERT_EQ(result, JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], protected_store);
    auto value = m_memory->read_u32(protected_store);
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value(), initial_value);
}

TEST_F(JitLinear64Test, StoreIntoHighCodePageInvalidatesCachedBlock) {
    constexpr GuestAddr victim_addr = kCode64 + 0x1000;
    const std::vector<u8> victim_code = {0xB8, 0x11, 0x22, 0x33, 0x44,
                                         0xC3};
    ASSERT_TRUE(m_memory->write_bytes(victim_addr, victim_code.data(),
                                      victim_code.size()));
    BasicBlock victim;
    victim.start_addr = victim_addr;
    victim.is_64bit = true;
    {
        MemoryByteReader reader(victim_code.data(), victim_code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64,
                                         instruction),
                      DecodeError::Success);
            victim.instructions.push_back(instruction);
        }
        victim.byte_length = static_cast<u32>(reader.position());
        victim.instruction_count =
            static_cast<u32>(victim.instructions.size());
    }
    ASSERT_NE(m_jit->compile(victim), nullptr);
    m_cache->insert(victim);
    ASSERT_EQ(m_cache->size(), 1u);
    ASSERT_TRUE(m_memory->jit_code_pages_overlap(victim_addr, 4));

    const std::vector<u8> store = {0x89, 0x03}; // mov [rbx],eax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, store.data(), store.size()));
    BasicBlock writer = build64(store);
    CompiledBlock* compiled = m_jit->compile(writer);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64(victim_addr);
    cpu.gpr.set32(EAX, 0x90909090u);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    ASSERT_TRUE(m_memory->read_u32(victim_addr).is_ok());
    EXPECT_EQ(m_memory->read_u32(victim_addr).value(), 0x90909090u);
    EXPECT_EQ(m_cache->size(), 0u)
        << "linear64 store bypassed high-address SMC invalidation";
}

// A second thread can publish executable code after this JIT has entered a
// native block. The store guard must acquire-load GuestMemory's live x64 code
// span; a span copied at execute() entry misses the newly retained page and
// lets the direct store leave the other JIT's translation stale.
TEST_F(JitLinear64Test, StoreSeesCodePagePublishedDuringNativeExecution) {
    constexpr GuestAddr started_addr = kData64;
    constexpr GuestAddr fill_addr = 0x00000001A0000000ull;
    constexpr usize fill_size = 2 * 1024 * 1024;
    constexpr GuestAddr victim_addr = kCode64 + 0x2000;

    ASSERT_TRUE(m_memory->map_region(fill_addr, fill_size,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(started_addr, 0));

    // MOV dword ptr [RSI],1 ; REP STOSB ; MOV [RBX],EAX. DF=1 deliberately
    // routes the REP helper through its element loop, providing a bounded
    // native-execution window without allocating or touching a huge buffer.
    const std::vector<u8> writer_code = {
        0xC7, 0x06, 0x01, 0x00, 0x00, 0x00,
        0xF3, 0xAA,
        0x89, 0x03,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCode64, writer_code.data(),
                                      writer_code.size()));
    BasicBlock writer_block = build64(writer_code);
    ASSERT_EQ(writer_block.instruction_count, 3u);
    CompiledBlock* writer = m_jit->compile(writer_block);
    ASSERT_NE(writer, nullptr);

    const std::vector<u8> victim_code = {
        0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3,
    };
    ASSERT_TRUE(m_memory->write_bytes(victim_addr, victim_code.data(),
                                      victim_code.size()));
    BasicBlock victim;
    victim.start_addr = victim_addr;
    victim.is_64bit = true;
    {
        MemoryByteReader reader(victim_code.data(), victim_code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64,
                                         instruction),
                      DecodeError::Success);
            victim.instructions.push_back(instruction);
        }
        victim.byte_length = static_cast<u32>(reader.position());
        victim.instruction_count =
            static_cast<u32>(victim.instructions.size());
    }

    JitCompiler other_jit(*m_memory);
    BlockCache other_cache;
    other_jit.set_block_cache(&other_cache);
    CompiledBlock* victim_compiled = other_jit.compile(victim);
    ASSERT_NE(victim_compiled, nullptr);
    ASSERT_FALSE(m_memory->jit_code_pages_overlap(victim_addr, 4));

    Byte* started_host = m_memory->host_ptr_mut(started_addr, sizeof(u32));
    ASSERT_NE(started_host, nullptr);
    auto& started_word = *reinterpret_cast<u32*>(started_host);
    std::atomic_ref<u32> started(started_word);
    std::atomic<bool> execution_done{false};
    JitExecResult writer_result = JitExecResult::Continue;
    CpuState cpu = cpu64(victim_addr);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSI), started_addr);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RDI),
                  fill_addr + fill_size - 1);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), fill_size);
    cpu.gpr.set32(EAX, 0x90909090u);
    cpu.flags.set_df(true);

    std::thread writer_thread([&] {
        writer_result = test::execute_like_dispatcher(*m_jit, *writer, cpu);
        execution_done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    while (started.load(std::memory_order_acquire) == 0 &&
           !execution_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    const bool saw_entry = started.load(std::memory_order_acquire) != 0;
    const bool published_during_execute =
        saw_entry && !execution_done.load(std::memory_order_acquire);
    if (published_during_execute) {
        BasicBlock* cached_victim = other_cache.insert(victim);
        if (cached_victim != nullptr) {
            publish_compiled(*cached_victim, *victim_compiled);
        }
    }
    writer_thread.join();

    ASSERT_TRUE(saw_entry) << "writer never reached the native entry marker";
    ASSERT_TRUE(published_during_execute)
        << "REP window ended before the second JIT could publish code";
    ASSERT_EQ(writer_result, JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    ASSERT_EQ(other_cache.size(), 1u);
    EXPECT_TRUE(other_jit.synchronize_executable_writes());
    EXPECT_EQ(other_cache.size(), 0u)
        << "linear64 store missed code published after execute() entry";
}

// Hardware enforcement must not cost the write-watch dirty set a single page.
//
// An under-reported GetWriteWatch is a silent lie that makes a guest collector
// free live objects (docs: getwritewatch_gc_landmine), and it is exactly the
// kind of thing a change to *when host pages get mprotected* could break. The
// store here runs at 6 GiB, on a page whose protection is now stated by the
// host page rather than by a slot table.
TEST_F(JitLinear64Test, WriteWatchStillSeesDirectStoresUnderHardwareProtection) {
    ASSERT_TRUE(m_memory->linear_hardware_protection());
    std::array<u8, 8> dirty{};
    std::array<u8, 8> armed{};
    write_watch::register_region(kData64, 0x4000, dirty.data(), armed.data(),
                                 /*executable=*/false);

    const std::vector<u8> code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);

    CpuState cpu = cpu64(kData64 + 0x1000);   // second 4 KiB guest page
    cpu.gpr.set64(EAX, 0x0BADC0DE0BADC0DEull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);

    const bool marked = write_watch::test_bit(dirty.data(), 1);
    write_watch::unregister_region(kData64);
    EXPECT_TRUE(marked)
        << "a direct store under hardware protection was not reported dirty";
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x1000).value_or(0),
              0x0BADC0DE0BADC0DEull);
}

}  // namespace
