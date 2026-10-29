// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_ir_release.cpp
/// @brief Decoded-IR retention policy: what may be released, what must be
///        retained, and that a released block re-decodes transparently.
///
/// `DecodedInstruction` is 144 bytes and the block cache used to keep the full
/// decoded array for every cached block forever. Measured on a logged-in
/// Battle.net guest: 136,178 live blocks / 957,731 compiled x86 instructions,
/// ~400 MB of dirty MALLOC_SMALL per process attributed to Vitreo's own heap.
///
/// The array may only be dropped when generated code did NOT bake
/// `&instructions[i]` into the arena. These tests pin both arms: the release
/// arm (so the win cannot silently regress) and — much more importantly — the
/// FAIL-CLOSED arm, because freeing an array a delegate stencil still points
/// into is a use-after-free that surfaces as guest corruption far from here.

#include <gtest/gtest.h>

#include <vector>

#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cli;

namespace {

constexpr GuestAddr kBase = 0x140300000;

/// `xor eax,eax ; ret` — two instructions, no delegate.
const std::vector<u8> kBlockBytes = {0x31, 0xC0, 0xC3};

class IrReleaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        reset_decode_memo();
        jit::reset_decoded_release_stats();
        ASSERT_TRUE(m_memory.map_region(kBase, 0x1000,
                                        MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory.write_bytes(kBase, kBlockBytes.data(),
                                         kBlockBytes.size()));
    }
    void TearDown() override {
        reset_decode_memo();
        jit::reset_decoded_release_stats();
    }

    GuestMemory m_memory;
};

}  // namespace

// The release arm. A block with no baked pointers gives its array back while
// keeping every cheap scalar the cache and the linked fast path read.
TEST_F(IrReleaseTest, ReleasesArrayButKeepsScalars) {
    if (!jit::release_decoded_enabled()) GTEST_SKIP();

    auto block = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(block.instruction_count, 2u);
    ASSERT_EQ(block.instructions.size(), 2u);
    const u32 byte_length = block.byte_length;
    const bool ends_with_branch = block.ends_with_branch;

    EXPECT_TRUE(jit::release_block_decoded(block, /*has_baked_pointers=*/false));

    EXPECT_EQ(block.instructions.size(), 0u);
    EXPECT_EQ(block.instructions.capacity(), 0u);
    EXPECT_TRUE(block.instructions_released);
    // The scalars are what dispatch, the churn guard and the L1 tail read.
    EXPECT_EQ(block.instruction_count, 2u);
    EXPECT_EQ(block.byte_length, byte_length);
    EXPECT_EQ(block.ends_with_branch, ends_with_branch);

    const auto stats = jit::decoded_release_stats();
    EXPECT_EQ(stats.releases, 1u);
    EXPECT_EQ(stats.released_bytes, 2u * sizeof(DecodedInstruction));
}

// The fail-closed arm. This is the one that matters: a delegate-bearing block
// must NEVER lose its array, whatever the gate says.
TEST_F(IrReleaseTest, RetainsWhenGeneratedCodeBakedPointers) {
    auto block = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(block.instructions.size(), 2u);
    const DecodedInstruction* before = block.instructions.data();

    EXPECT_FALSE(jit::release_block_decoded(block, /*has_baked_pointers=*/true));

    EXPECT_EQ(block.instructions.size(), 2u);
    EXPECT_EQ(block.instructions.data(), before);
    EXPECT_FALSE(block.instructions_released);
    EXPECT_EQ(jit::decoded_release_stats().retained_delegate, 1u);
    EXPECT_EQ(jit::decoded_release_stats().releases, 0u);
}

// A released block re-decodes transparently and reproduces the same IR.
TEST_F(IrReleaseTest, RematerializeRebuildsIdenticalInstructions) {
    if (!jit::release_decoded_enabled()) GTEST_SKIP();

    auto block = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(block.instruction_count, 2u);
    const Opcode op0 = block.instructions[0].opcode;
    const Opcode op1 = block.instructions[1].opcode;

    ASSERT_TRUE(jit::release_block_decoded(block, false));
    ASSERT_TRUE(block.instructions.empty());

    ASSERT_TRUE(rematerialize_block_instructions(m_memory, block));
    ASSERT_EQ(block.instructions.size(), 2u);
    EXPECT_EQ(block.instructions[0].opcode, op0);
    EXPECT_EQ(block.instructions[1].opcode, op1);
    EXPECT_FALSE(block.instructions_released);
    EXPECT_EQ(jit::decoded_release_stats().redecodes, 1u);
}

// Rematerializing a block that still holds its array is a no-op, and does not
// charge a re-decode.
TEST_F(IrReleaseTest, RematerializeIsNoOpWhenArrayStillHeld) {
    auto block = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(block.instructions.size(), 2u);
    const DecodedInstruction* before = block.instructions.data();

    EXPECT_TRUE(rematerialize_block_instructions(m_memory, block));
    EXPECT_EQ(block.instructions.data(), before);
    EXPECT_EQ(jit::decoded_release_stats().redecodes, 0u);
}

// Self-modified code: the re-decode must REFUSE rather than hand back an array
// describing bytes the cached entry's shape no longer matches. The caller
// treats that as "invalidate and single-step", never as "execute anyway".
TEST_F(IrReleaseTest, RematerializeFailsWhenGuestBytesChanged) {
    if (!jit::release_decoded_enabled()) GTEST_SKIP();

    auto block = decode_basic_block(m_memory, kBase, true);
    ASSERT_EQ(block.instruction_count, 2u);
    ASSERT_TRUE(jit::release_block_decoded(block, false));

    // `nop ; nop ; nop` — same span, different shape (three instructions).
    const std::vector<u8> rewritten = {0x90, 0x90, 0x90};
    ASSERT_TRUE(m_memory.write_bytes(kBase, rewritten.data(),
                                     rewritten.size()));

    EXPECT_FALSE(rematerialize_block_instructions(m_memory, block));
    EXPECT_TRUE(block.instructions.empty());
    EXPECT_EQ(jit::decoded_release_stats().redecode_failures, 1u);
}

// A block that never decoded anything is not a release candidate and is not a
// re-decode candidate either — `instruction_count == 0` is the "interpret this
// one instruction at a time" signal, not a released array.
TEST_F(IrReleaseTest, EmptyBlockIsNeitherReleasedNorRematerialized) {
    jit::BasicBlock empty;
    empty.start_addr = kBase;
    empty.is_64bit = true;

    EXPECT_FALSE(jit::release_block_decoded(empty, false));
    EXPECT_FALSE(empty.instructions_released);
    EXPECT_FALSE(rematerialize_block_instructions(m_memory, empty));
    EXPECT_EQ(jit::decoded_release_stats().redecodes, 0u);
    EXPECT_EQ(jit::decoded_release_stats().redecode_failures, 0u);
}
