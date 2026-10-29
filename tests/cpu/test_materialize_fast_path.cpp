// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Fast-path materialize_lazy_flags must match FlagState::materialize.

#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/state/flag_state.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

u64 arith_mask(u64 rflags) noexcept {
    return rflags & 0x8D5u;  // CF PF AF ZF SF OF
}

void expect_fast_matches_flagstate(CpuState::FlagOp op, u64 src, u64 dst) {
    CpuState fast;
    fast.init_protected_mode();
    fast.flags.rflags = 0x246;  // typical boot-ish
    fast.flag_op = op;
    fast.flag_src = src;
    fast.flag_dst = dst;
    fast.materialize_lazy_flags();

    Flags slow_flags;
    slow_flags.rflags = 0x246;
    FlagState fs;
    fs.flag_op = op;
    fs.flag_src = src;
    fs.flag_dst = dst;
    fs.materialize(slow_flags);

    EXPECT_EQ(arith_mask(fast.flags.rflags), arith_mask(slow_flags.rflags))
        << "op=" << static_cast<unsigned>(op)
        << " src=0x" << std::hex << src << " dst=0x" << dst;
    EXPECT_EQ(fast.flag_op, CpuState::FlagOp::None);
}

TEST(MaterializeFastPathTest, Add32MatchesFlagState) {
    const std::array<std::pair<u64, u64>, 6> cases = {{
        {1, 2},
        {0, 0},
        {0xFFFFFFFFu, 1},
        {0x7FFFFFFFu, 1},
        {0x80000000u, 0x80000000u},
        {0x12345678u, 0x9ABCDEF0u},
    }};
    for (const auto& [src, dst] : cases) {
        // flag_dst is architectural result; flag_src is addend.
        // Reconstruct: result = dst, addend = src, original = dst - src.
        expect_fast_matches_flagstate(CpuState::FlagOp::Add32, src, dst);
    }
}

TEST(MaterializeFastPathTest, Logic32MatchesFlagState) {
    for (auto op : {CpuState::FlagOp::And32, CpuState::FlagOp::Or32,
                    CpuState::FlagOp::Xor32}) {
        expect_fast_matches_flagstate(op, 0, 0);
        expect_fast_matches_flagstate(op, 0, 0x80000000u);
        expect_fast_matches_flagstate(op, 0, 0xFFFFFFFFu);
        expect_fast_matches_flagstate(op, 0, 0x10u);
    }
}

TEST(MaterializeFastPathTest, Shr32MatchesFlagState) {
    // flag_dst = (count << 32) | result; flag_src = original
    expect_fast_matches_flagstate(
        CpuState::FlagOp::Shr32, 0x80000000u,
        (u64{1} << 32) | 0x40000000u);
    expect_fast_matches_flagstate(
        CpuState::FlagOp::Shr32, 0x00000003u,
        (u64{2} << 32) | 0x00000000u);
    expect_fast_matches_flagstate(
        CpuState::FlagOp::Shr32, 0xABCDEF01u,
        (u64{0} << 32) | 0xABCDEF01u);  // count 0: flags unchanged
}

TEST(MaterializeFastPathTest, Inc32PreservesCf) {
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.flags.rflags = 0x247;  // CF set
    // Inc32: flag_src low = original, bit32 = saved CF; flag_dst = result
    cpu.flag_op = CpuState::FlagOp::Inc32;
    cpu.flag_src = (u64{1} << 32) | 0x7FFFFFFFu;
    cpu.flag_dst = 0x80000000u;
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());
    EXPECT_TRUE(cpu.flags.of());  // 0x7FFFFFFF + 1 overflows signed
    EXPECT_TRUE(cpu.flags.sf());
}

}  // namespace
