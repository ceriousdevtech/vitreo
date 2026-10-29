// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>
#include "core/cpu/state/flag_state.hpp"

using namespace vitreo::cpu;

// ===========================================================================
// FlagOp::None — flags struct is authoritative, materialize is a no-op
// ===========================================================================

TEST(FlagStateTest, NoneDoesNotModifyFlags) {
    Flags flags;
    flags.set_cf(true);
    flags.set_zf(true);

    FlagState fs;
    fs.flag_op = FlagState::FlagOp::None;
    fs.materialize(flags);

    EXPECT_TRUE(flags.cf());
    EXPECT_TRUE(flags.zf());
}

// ===========================================================================
// FlagOp::Add32
// ===========================================================================

TEST(FlagStateTest, Add32_NoOverflow) {
    // 3 + 5 = 8
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Add32;
    fs.flag_src = 5;          // b operand
    fs.flag_dst = 8;          // result

    Flags flags;
    fs.materialize(flags);

    EXPECT_FALSE(flags.cf());  // no carry
    EXPECT_FALSE(flags.of());  // no overflow
    EXPECT_FALSE(flags.zf());  // result != 0
    EXPECT_FALSE(flags.sf());  // result positive
}

TEST(FlagStateTest, Add32_ZeroResult) {
    // 0 + 0 = 0
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Add32;
    fs.flag_src = 0;
    fs.flag_dst = 0;

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.zf());
    EXPECT_FALSE(flags.sf());
    EXPECT_FALSE(flags.cf());
}

TEST(FlagStateTest, Add32_UnsignedCarry) {
    // 0xFFFFFFFF + 1 = 0x00000000 (carry)
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Add32;
    fs.flag_src = 1;
    fs.flag_dst = 0;          // result wraps to 0

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.cf());
    EXPECT_TRUE(flags.zf());
}

TEST(FlagStateTest, Add32_SignedOverflow) {
    // 0x7FFFFFFF + 1 = 0x80000000 (positive + positive = negative → overflow)
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Add32;
    fs.flag_src = 1;
    fs.flag_dst = 0x80000000u;

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.of());
    EXPECT_TRUE(flags.sf());
    EXPECT_FALSE(flags.zf());
}

// ===========================================================================
// FlagOp::Sub32
// ===========================================================================

TEST(FlagStateTest, Sub32_Simple) {
    // 10 - 3 = 7
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Sub32;
    fs.flag_src = 3;          // b operand
    fs.flag_dst = 7;          // result

    Flags flags;
    fs.materialize(flags);

    EXPECT_FALSE(flags.cf());  // no borrow
    EXPECT_FALSE(flags.of());
    EXPECT_FALSE(flags.zf());
    EXPECT_FALSE(flags.sf());
}

TEST(FlagStateTest, Sub32_Borrow) {
    // 3 - 5 = 0xFFFFFFFE (borrow)
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Sub32;
    fs.flag_src = 5;
    fs.flag_dst = 0xFFFFFFFEu; // 3 - 5

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.cf());   // borrow occurred
    EXPECT_TRUE(flags.sf());   // result is negative (high bit set)
}

TEST(FlagStateTest, Sub32_ZeroResult) {
    // 5 - 5 = 0
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Sub32;
    fs.flag_src = 5;
    fs.flag_dst = 0;

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.zf());
    EXPECT_FALSE(flags.cf());
    EXPECT_FALSE(flags.of());
}

// ===========================================================================
// FlagOp::And32 — clears CF and OF, sets ZF/SF
// ===========================================================================

TEST(FlagStateTest, And32_ZeroResult) {
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::And32;
    fs.flag_dst = 0;

    Flags flags;
    flags.set_cf(true);  // should be cleared
    flags.set_of(true);  // should be cleared
    fs.materialize(flags);

    EXPECT_FALSE(flags.cf());
    EXPECT_FALSE(flags.of());
    EXPECT_TRUE(flags.zf());
    EXPECT_FALSE(flags.sf());
}

TEST(FlagStateTest, And32_NonZeroNegative) {
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::And32;
    fs.flag_dst = 0x80000001u;  // high bit set

    Flags flags;
    fs.materialize(flags);

    EXPECT_FALSE(flags.cf());
    EXPECT_FALSE(flags.of());
    EXPECT_FALSE(flags.zf());
    EXPECT_TRUE(flags.sf());
}

// ===========================================================================
// FlagOp::Sub8 — 8-bit subtraction
// ===========================================================================

TEST(FlagStateTest, Sub8_ZeroResult) {
    // 0x42 - 0x42 = 0
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Sub8;
    fs.flag_src = 0x42;
    fs.flag_dst = 0;

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.zf());
    EXPECT_FALSE(flags.sf());
    EXPECT_FALSE(flags.cf());
}

TEST(FlagStateTest, Sub8_Borrow) {
    // 1 - 2 = 0xFF (borrow)
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Sub8;
    fs.flag_src = 2;
    fs.flag_dst = 0xFF;

    Flags flags;
    fs.materialize(flags);

    EXPECT_TRUE(flags.cf());
    EXPECT_TRUE(flags.sf());
    EXPECT_FALSE(flags.zf());
}

// ===========================================================================
// Materialize resets flag_op to None
// ===========================================================================

TEST(FlagStateTest, MaterializeResetsOp) {
    FlagState fs;
    fs.flag_op  = FlagState::FlagOp::Add32;
    fs.flag_src = 1;
    fs.flag_dst = 2;

    Flags flags;
    fs.materialize(flags);

    EXPECT_EQ(fs.flag_op, FlagState::FlagOp::None);
}

// ===========================================================================
// try_condition() — direct condition evaluation without materializing
//
// The JIT's Jcc/SETcc/CMOVcc helpers answer a condition straight from the
// lazy state and only fall back to materialize() when the recorded op does
// not determine it. materialize() is the CPU semantic reference, so the fast
// path is only correct if it agrees with it for every (FlagOp, cc) pair it
// claims to handle. Divergence here is silent in the guest, so sweep it
// exhaustively rather than spot-checking.
//
// Each case is evaluated against two different seed rflags values. A flag the
// reference leaves *unchanged* (undefined per Intel, or not recoverable from
// this lazy encoding) therefore differs between the two seeds, so a fast path
// that wrongly claims to know it cannot match both.
// ===========================================================================

namespace {

using vitreo::u8;
using vitreo::u64;
using vitreo::usize;

int reference_condition(const Flags& f, u8 cc) {
    const int cf = static_cast<int>(f.cf());
    const int zf = static_cast<int>(f.zf());
    const int sf = static_cast<int>(f.sf());
    const int of = static_cast<int>(f.of());
    const int pf = static_cast<int>(f.pf());
    switch (cc & 0xF) {
    case 0x0: return of;
    case 0x1: return of ^ 1;
    case 0x2: return cf;
    case 0x3: return cf ^ 1;
    case 0x4: return zf;
    case 0x5: return zf ^ 1;
    case 0x6: return cf | zf;
    case 0x7: return (cf | zf) ^ 1;
    case 0x8: return sf;
    case 0x9: return sf ^ 1;
    case 0xA: return pf;
    case 0xB: return pf ^ 1;
    case 0xC: return sf ^ of;
    case 0xD: return (sf ^ of) ^ 1;
    case 0xE: return zf | (sf ^ of);
    default:  return (zf | (sf ^ of)) ^ 1;
    }
}

} // namespace

using vitreo::u8;
using vitreo::u64;
using vitreo::usize;

TEST(FlagStateTest, TryConditionMatchesMaterializeExhaustively) {
    // Operand patterns chosen to hit each width's sign bit, zero, all-ones,
    // and — for the shift encodings, which pack the count into bits 32..36 of
    // flag_dst — counts of 0, 1, and several multi-bit values.
    static constexpr u64 kValues[] = {
        0x0000000000000000ull, 0x0000000000000001ull, 0x000000000000007Full,
        0x0000000000000080ull, 0x00000000000000FFull, 0x0000000000007FFFull,
        0x0000000000008000ull, 0x000000000000FFFFull, 0x000000007FFFFFFFull,
        0x0000000080000000ull, 0x00000000FFFFFFFFull, 0x0000000100000001ull,
        0x0000000100000000ull, 0x0000000500000080ull, 0x000000080000FFFFull,
        0x0000001F80000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull, 0xDEADBEEFCAFEF00Dull,
    };
    static constexpr u64 kSeeds[] = {0x0000000000000002ull,   // all status bits 0
                                     0x0000000000000AD5ull};  // all status bits 1

    const int last_op = static_cast<int>(FlagState::FlagOp::Ror64_1);
    usize fast_hits = 0, total = 0;

    for (int op = 1; op <= last_op; ++op) {
        for (u64 src : kValues) {
            for (u64 dst : kValues) {
                for (u8 cc = 0; cc < 16; ++cc) {
                    FlagState probe;
                    probe.flag_op = static_cast<FlagState::FlagOp>(op);
                    probe.flag_src = src;
                    probe.flag_dst = dst;
                    const int fast = probe.try_condition(cc);
                    ++total;
                    if (fast < 0) continue;
                    ++fast_hits;

                    for (u64 seed : kSeeds) {
                        Flags flags;
                        flags.rflags = seed;
                        FlagState ref = probe;
                        ref.materialize(flags);
                        EXPECT_EQ(fast, reference_condition(flags, cc))
                            << "op=" << op << " cc=" << int(cc)
                            << " src=" << std::hex << src << " dst=" << dst
                            << " seed=" << seed << std::dec;
                    }
                }
            }
        }
    }

    // The fast path is a performance change: guard against it silently
    // regressing into "always fall back".
    EXPECT_GT(fast_hits * 2, total) << "fast path covers " << fast_hits
                                    << " of " << total << " cases";
}

TEST(FlagStateTest, TryConditionLeavesLazyStateIntact) {
    FlagState fs;
    fs.flag_op = FlagState::FlagOp::Sub32;
    fs.flag_src = 5;
    fs.flag_dst = 0;

    EXPECT_EQ(fs.try_condition(0x4), 1);              // ZF set
    EXPECT_EQ(fs.flag_op, FlagState::FlagOp::Sub32);  // still lazy

    Flags flags;
    flags.rflags = 2;
    fs.materialize(flags);
    EXPECT_TRUE(flags.zf());
    EXPECT_EQ(fs.flag_op, FlagState::FlagOp::None);
}

TEST(FlagStateTest, TryConditionDeclinesWhenFlagOpIsNone) {
    FlagState fs;
    for (u8 cc = 0; cc < 16; ++cc) EXPECT_EQ(fs.try_condition(cc), -1);
}
