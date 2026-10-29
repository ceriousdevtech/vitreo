// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_decode_memo.cpp
/// @brief Process-wide decoded-block memo: cross-thread sharing, budget, and
///        self-modified-code invalidation.
///
/// The regression these guard is specific and was live in production: the memo
/// entry held only a weak reference, and `publish_compiled` releases the IR of
/// a FULLY-NATIVE block, so the memo expired the instant the first thread
/// finished compiling. Every other guest thread then re-decoded from scratch —
/// which is why `decode_basic_block` was 30.5% of the execution loop on a
/// 40-thread CEF guest. `MemoHitsAfterFullyNativePublishOnAnotherThread` fails
/// if the strong reference is ever removed again.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cli;

namespace {

constexpr GuestAddr kBase = 0x140200000;

/// `xor eax,eax ; ret` — decodes to two instructions and compiles fully
/// native, which is exactly the class of block the weak reference dropped.
const std::vector<u8> kNativeBlock = {0x31, 0xC0, 0xC3};

class DecodeMemoTest : public ::testing::Test {
protected:
    void SetUp() override {
        reset_decode_memo();
        ASSERT_TRUE(m_memory.map_region(kBase, 0x1000,
                                        MemoryProtection::ReadWriteExecute));
    }
    void TearDown() override { reset_decode_memo(); }

    void write(GuestAddr addr, const std::vector<u8>& bytes) {
        ASSERT_TRUE(m_memory.write_bytes(addr, bytes.data(), bytes.size()));
    }

    GuestMemory m_memory;
};

/// Release the block's IR exactly as publish_compiled does for a block whose
/// every instruction got a native lowering.
void publish_fully_native(jit::BasicBlock& block) {
    jit::CompiledBlock compiled;
    compiled.guest_addr = block.start_addr;
    compiled.guest_size = block.byte_length;
    compiled.native_insns = block.instruction_count;
    jit::publish_compiled(block, compiled);
}

}  // namespace

// The bug, stated as a test: after a fully-native publish drops the compiling
// thread's IR, a second thread decoding the same address must still HIT the
// memo and receive the SAME array, not decode it again.
TEST_F(DecodeMemoTest, MemoHitsAfterFullyNativePublishOnAnotherThread) {
    write(kBase, kNativeBlock);

    auto first = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(first.instruction_count, 2u);
    const DecodedInstruction* shared_array = first.instructions.data();
    ASSERT_NE(shared_array, nullptr);

    // This is what made the weak reference expire in production.
    publish_fully_native(first);
    EXPECT_EQ(first.instructions.data(), nullptr);

    const auto before = decode_memo_stats();

    const DecodedInstruction* seen = nullptr;
    u32 count = 0;
    std::thread other([&] {
        auto second = decode_basic_block(m_memory, kBase, true);
        seen = second.instructions.data();
        count = second.instruction_count;
    });
    other.join();

    const auto after = decode_memo_stats();
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(seen, shared_array)
        << "second thread re-decoded instead of reusing the memoized array";
    EXPECT_EQ(after.hits, before.hits + 1);
    EXPECT_EQ(after.miss_expired, before.miss_expired)
        << "the memo's reference expired — the strong retention is gone";
}

TEST_F(DecodeMemoTest, StrongRetentionIsChargedAndReleased) {
    write(kBase, kNativeBlock);
    ASSERT_EQ(decode_memo_stats().retained_bytes, 0u);

    auto block = decode_basic_block(m_memory, kBase, true);
    publish_fully_native(block);

    const auto stats = decode_memo_stats();
    EXPECT_EQ(stats.strong_retained, 1u);
    EXPECT_EQ(stats.strong_entries, 1u);
    EXPECT_EQ(stats.retained_bytes, 2u * sizeof(DecodedInstruction));

    reset_decode_memo();
    EXPECT_EQ(decode_memo_stats().retained_bytes, 0u);
    EXPECT_EQ(decode_memo_stats().strong_entries, 0u);
}

// Correctness rests on decode being a pure function of (addr, mode, bytes).
// A strong reference must not turn the memo into a cache of stale code.
TEST_F(DecodeMemoTest, SelfModifiedCodeMissesAndReDecodes) {
    write(kBase, kNativeBlock);
    auto first = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(first.instruction_count, 2u);
    const Opcode xor_opcode = first.instructions[0].opcode;
    publish_fully_native(first);

    // inc eax ; ret — same length, different bytes.
    write(kBase, {0xFF, 0xC0, 0xC3});
    const auto before = decode_memo_stats();
    auto second = decode_basic_block(m_memory, kBase, true);
    const auto after = decode_memo_stats();

    ASSERT_EQ(second.instruction_count, 2u);
    EXPECT_NE(second.instructions[0].opcode, xor_opcode);
    EXPECT_EQ(after.hits, before.hits);
    EXPECT_EQ(after.miss_hash, before.miss_hash + 1);
    // The superseded entry must give its budget back rather than leak it.
    EXPECT_EQ(after.retained_bytes, 2u * sizeof(DecodedInstruction));
    EXPECT_EQ(after.strong_entries, 1u);
}

// The same address decoded in the other mode is a different block. A hit here
// would execute 32-bit semantics for 64-bit code (or vice versa).
TEST_F(DecodeMemoTest, ModeMismatchDoesNotHit) {
    write(kBase, kNativeBlock);
    (void)decode_basic_block(m_memory, kBase, true);

    const auto before = decode_memo_stats();
    (void)decode_basic_block(m_memory, kBase, false);
    const auto after = decode_memo_stats();

    EXPECT_EQ(after.hits, before.hits);
    EXPECT_EQ(after.miss_key, before.miss_key + 1);
}

// Many threads hammering one address must produce one shared array and no
// torn bookkeeping: every thread sees the full instruction count, and the
// retained budget matches exactly one entry.
TEST_F(DecodeMemoTest, ConcurrentDecodersConvergeOnOneArray) {
    write(kBase, kNativeBlock);

    constexpr int kThreads = 8;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    std::vector<u32> counts(kThreads, 0);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load()) {}
            for (int i = 0; i < 200; ++i) {
                auto b = decode_basic_block(m_memory, kBase, true);
                counts[static_cast<usize>(t)] = b.instruction_count;
                EXPECT_EQ(b.instructions.size(), b.instruction_count);
            }
        });
    }
    while (ready.load() != kThreads) {}
    go.store(true);
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(counts[static_cast<usize>(t)], 2u);
    }
    const auto stats = decode_memo_stats();
    EXPECT_EQ(stats.entries, 1u);
    EXPECT_EQ(stats.strong_entries, 1u);
    EXPECT_EQ(stats.retained_bytes, 2u * sizeof(DecodedInstruction));
    EXPECT_GT(stats.hits, 0u);
}

// Distinct addresses accumulate distinct entries, and the budget tracks them.
TEST_F(DecodeMemoTest, DistinctAddressesEachRetainOnce) {
    for (u32 i = 0; i < 16; ++i) {
        write(kBase + i * 8, kNativeBlock);
        auto b = decode_basic_block(m_memory, kBase + i * 8, true);
        publish_fully_native(b);
    }
    const auto stats = decode_memo_stats();
    EXPECT_EQ(stats.entries, 16u);
    EXPECT_EQ(stats.strong_entries, 16u);
    EXPECT_EQ(stats.retained_bytes, 16u * 2u * sizeof(DecodedInstruction));
}

// The env-flag cache must still be A/B-able in one process, or the guarded
// terminal-island tests silently stop testing what they name.
TEST_F(DecodeMemoTest, EnvFlagCacheIsRefreshable) {
    write(kBase, {0x0F, 0x84, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x0B, 0x90, 0xC3});

    setenv("VITREO_JIT_GUARDED_TERMINAL_TRACES", "0", 1);
    refresh_decode_env_flags();
    reset_decode_memo();
    const auto off = decode_basic_block(m_memory, kBase, true);

    setenv("VITREO_JIT_GUARDED_TERMINAL_TRACES", "1", 1);
    refresh_decode_env_flags();
    reset_decode_memo();
    const auto on = decode_basic_block(m_memory, kBase, true);

    unsetenv("VITREO_JIT_GUARDED_TERMINAL_TRACES");
    refresh_decode_env_flags();

    EXPECT_LT(off.instruction_count, on.instruction_count)
        << "cached decode flags ignored a same-process environment change";
}
