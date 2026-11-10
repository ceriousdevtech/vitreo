// SPDX-License-Identifier: LGPL-2.1-only
// WS2: descriptor packing and the kill switch.

#include <gtest/gtest.h>

#include <climits>

#include "core/cpu/jit/loop_idiom_desc.hpp"
#include "core/cpu/jit/loop_idiom_gate.hpp"

using namespace vitreo;
using namespace vitreo::cpu::jit::loop_idiom;

namespace {

Desc max_desc(Kind kind) {
    Desc d;
    d.kind = kind;
    d.elem_log2 = 4;
    d.is64 = true;
    d.ordered = true;
    d.vector = true;
    d.dir_down = true;
    d.n_loads = 2;
    d.has_store = true;
    d.streams[0] = {kNoReg, 15, 3, INT32_MIN, true};
    d.streams[1] = {15, kNoReg, 0, INT32_MAX, false};
    d.streams[2] = {7, 8, 2, -1, true};
    d.n_ind = 3;
    d.ind[0] = {15, -16, true};
    d.ind[1] = {0, 16, false};
    d.ind[2] = {9, -1, true};
    d.has_ind_exit = true;
    d.ind_exit = {2, Cond::G, ValueSrc::Imm, 15, INT32_MIN, false, 1, true};
    d.loop_rip = (u64{1} << 48) - 1;
    if (kind == Kind::Fill) {
        d.fill = {ValueSrc::Gpr, 3, true, INT32_MAX};
    } else {
        d.n_data = 2;
        d.data[0] = {1, DataOp::AnyLaneEq, false, ValueSrc::Xmm, 15, true, INT32_MAX, 4};
        d.data[1] = {0, DataOp::StreamEq, true, ValueSrc::Gpr, 7, false, INT32_MIN, 3};
    }
    return d;
}

}  // namespace

TEST(LoopIdiomDesc, PackUnpackRoundTripsEveryField) {
    for (Kind k : {Kind::Fill, Kind::Copy, Kind::Scan, Kind::Compare}) {
        const Desc d = max_desc(k);
        const auto w = pack(d);
        ASSERT_TRUE(w.has_value()) << int(k);
        const auto back = unpack(*w);
        ASSERT_TRUE(back.has_value()) << int(k);
        EXPECT_TRUE(*back == d) << int(k);
        EXPECT_EQ(back->streams[0].disp_eff, INT32_MIN);
        EXPECT_EQ(back->streams[1].disp_eff, INT32_MAX);
        EXPECT_EQ(back->ind[0].step, -16);
        EXPECT_EQ(back->ind[1].step, 16);
        EXPECT_EQ(back->loop_rip, (u64{1} << 48) - 1);
    }
    // Default (all-zero-ish) descriptor too.
    Desc z;
    z.kind = Kind::Scan;
    const auto w = pack(z);
    ASSERT_TRUE(w);
    EXPECT_TRUE(*unpack(*w) == z);
}

TEST(LoopIdiomDesc, PackRejectsOverflow) {
    Desc d = max_desc(Kind::Copy);
    d.loop_rip = u64{1} << 48;
    EXPECT_FALSE(pack(d).has_value());
    d = max_desc(Kind::Copy);
    d.n_loads = 3;
    EXPECT_FALSE(pack(d).has_value());
    d = max_desc(Kind::Copy);
    d.ind[0].step = 17;
    EXPECT_FALSE(pack(d).has_value());
    d = max_desc(Kind::Copy);
    d.elem_log2 = 5;
    EXPECT_FALSE(pack(d).has_value());
    d = max_desc(Kind::Fill);
    d.n_data = 1;  // Fill shares data[0]'s bits with its source
    EXPECT_FALSE(pack(d).has_value());
    d = max_desc(Kind::Copy);
    d.streams[0].base = 17;
    EXPECT_FALSE(pack(d).has_value());
}

TEST(LoopIdiomDesc, UnpackRejectsGarbage) {
    const auto w = pack(max_desc(Kind::Scan));
    ASSERT_TRUE(w);
    Words bad = *w;
    bad[0] |= 0x1C;  // elem_log2 = 7
    EXPECT_FALSE(unpack(bad).has_value());
    bad = *w;
    bad[5] |= u64{1} << 63;  // a bit past the last field
    EXPECT_FALSE(unpack(bad).has_value());
    Words all_ones;
    all_ones.fill(~u64{0});
    EXPECT_FALSE(unpack(all_ones).has_value());
}

TEST(LoopIdiomGate, DefaultOnAndKillSwitch) {
    const bool env_off = [] {
        const char* v = std::getenv("VITREO_WS2");
        return v != nullptr && v[0] == '0' && v[1] == '\0';
    }();
    force_for_test(Force::None);
    EXPECT_EQ(enabled(), !env_off);
    EXPECT_FALSE(thresholds_bypassed());
    force_for_test(Force::Off);
    EXPECT_FALSE(enabled());
    force_for_test(Force::On);
    EXPECT_TRUE(enabled());
    EXPECT_FALSE(thresholds_bypassed());
    force_for_test(Force::OnNoThreshold);
    EXPECT_TRUE(enabled());
    EXPECT_TRUE(thresholds_bypassed());
    {
        ScopedForce s(Force::Off);
        EXPECT_FALSE(enabled());
    }
    EXPECT_TRUE(thresholds_bypassed());
    force_for_test(Force::None);
}
