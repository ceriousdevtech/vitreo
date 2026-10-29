// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_jit_split_layout.cpp
/// @brief Branch relocation for hot/cold split block placement.
///
/// Expected encodings come from the stencil encoders (stencils.hpp), which are
/// pinned against the Arm ARM by their own tests -- not from the relocator.

#include <gtest/gtest.h>

#include <vector>

#include "core/cpu/jit/jit_split_layout.hpp"
#include "core/cpu/jit/stencils.hpp"

using namespace vitreo;
using namespace vitreo::cpu::jit;

namespace {
constexpr u32 kNop = 0xD503201Fu;
}

// Every crossing branch class gets the displacement of its new placement;
// same-side branches keep their words exactly.
TEST(JitSplitLayout, CrossingBranchesOfEveryClassAreRetargeted) {
    // hot: [0] b.ne ->cold6  [1] cbnz ->cold7  [2] tbnz ->cold8  [3] b ->1 (hot)
    //      [4] nop
    // cold: [5] b ->0 (back to hot)  [6] cbz ->4 (back)  [7] nop  [8] b ->7 (cold)
    std::vector<u32> w(9, kNop);
    w[0] = arm64::b_cond(arm64::NE, 6 * 4);
    w[1] = arm64::cbnz_x(3, 6 * 4);
    w[2] = arm64::tbnz(3, 3, 6 * 4);
    w[3] = arm64::b_imm(-2 * 4);
    w[5] = arm64::b_imm(-5 * 4);
    w[6] = arm64::cbz_w(0, -2 * 4);
    w[8] = arm64::b_imm(-1 * 4);
    const std::vector<u32> before = w;
    constexpr i64 kDelta = 1000;  // cold placed 1000 words further away
    ASSERT_TRUE(relocate_split_branches(w.data(), w.size(), 5, kDelta));
    EXPECT_EQ(w[0], arm64::b_cond(arm64::NE, (6 + kDelta) * 4));
    EXPECT_EQ(w[1], arm64::cbnz_x(3, (6 + kDelta) * 4));
    EXPECT_EQ(w[2], arm64::tbnz(3, 3, (6 + kDelta) * 4));
    EXPECT_EQ(w[3], before[3]) << "hot->hot unchanged";
    EXPECT_EQ(w[5], arm64::b_imm(-(5 + kDelta) * 4));
    EXPECT_EQ(w[6], arm64::cbz_w(0, -(2 + kDelta) * 4));
    EXPECT_EQ(w[8], before[8]) << "cold->cold unchanged";
}

// TBNZ reaches +-32 KiB: a crossing that would exceed it refuses the whole
// split and leaves every word untouched, so the block is placed contiguously.
TEST(JitSplitLayout, OutOfRangeCrossingRefusesAndLeavesTheBlockUntouched) {
    std::vector<u32> w(4, kNop);
    w[0] = arm64::tbnz(3, 3, 3 * 4);
    w[1] = arm64::b_cond(arm64::EQ, 2 * 4);
    const std::vector<u32> before = w;
    EXPECT_TRUE(relocate_split_branches(w.data(), w.size(), 2, 8187));
    w = before;
    EXPECT_FALSE(relocate_split_branches(w.data(), w.size(), 2, 8192));
    EXPECT_EQ(w, before);
}

// Branches out of the buffer, and PC-relative forms that cannot be retargeted
// by a word displacement, are refused rather than guessed about.
TEST(JitSplitLayout, ExternalTargetsAndOtherPcRelativeFormsRefuse) {
    std::vector<u32> w(3, kNop);
    w[1] = arm64::b_imm(10 * 4);
    EXPECT_FALSE(relocate_split_branches(w.data(), w.size(), 2, 4));
    w[1] = 0x90000000u;  // ADRP X0, .  (4 KiB pages, not words)
    EXPECT_FALSE(relocate_split_branches(w.data(), w.size(), 2, 4));
    w[1] = 0x10000020u | (1u << 29);  // ADR X0, .+1: not a word boundary
    EXPECT_FALSE(relocate_split_branches(w.data(), w.size(), 2, 4));
}

// ADR and LDR (literal) are PC-relative too. A block's raise exit records its
// own address with ADR X17, . (JitContext::raise_block_pc); refusing every
// ADR made each such block unsplittable -- 17% of Satisfactory's blocks, a
// third of its hot ones. Same side: unchanged. Crossing: retargeted.
TEST(JitSplitLayout, AdrAndLiteralLoadsAreRetargetedLikeBranches) {
    // hot: [0] adr x17, . (self)  [1] adr x3, ->cold4  [2] ldr x5, ->cold5
    //      [3] nop
    // cold: [4] adr x4, ->hot0  [5] (literal)  [6] ldr w6, ->hot3
    std::vector<u32> w(7, kNop);
    w[0] = arm64::adr_x(17, 0);
    w[1] = arm64::adr_x(3, 3 * 4);
    w[2] = 0x58000000u | (3u << 5) | 5u;   // LDR X5, .+12
    w[4] = arm64::adr_x(4, -4 * 4);
    w[6] = 0x18000000u | ((0x7FFFFu & static_cast<u32>(-3)) << 5) | 6u;  // LDR W6, .-12
    const std::vector<u32> before = w;
    constexpr i64 kDelta = 1000;
    ASSERT_TRUE(relocate_split_branches(w.data(), w.size(), 4, kDelta));
    EXPECT_EQ(w[0], before[0]) << "an ADR of its own address never moves";
    EXPECT_EQ(w[1], arm64::adr_x(3, (3 + kDelta) * 4));
    EXPECT_EQ(w[2], 0x58000000u | (static_cast<u32>(3 + kDelta) << 5) | 5u);
    EXPECT_EQ(w[4], arm64::adr_x(4, -(4 + kDelta) * 4));
    EXPECT_EQ(w[6], 0x18000000u |
                        ((0x7FFFFu & static_cast<u32>(-(3 + kDelta))) << 5) | 6u);
}

TEST(JitSplitLayout, PlacedOffsetsMoveOnlyTheColdPart) {
    EXPECT_EQ(split_placed_offset(0, 64, 4000), 0u);
    EXPECT_EQ(split_placed_offset(60, 64, 4000), 60u);
    EXPECT_EQ(split_placed_offset(64, 64, 4000), 4064u);
}
