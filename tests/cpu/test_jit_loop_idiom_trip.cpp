// SPDX-License-Identifier: LGPL-2.1-only
// WS2: closed-form trip solver vs brute force.

#include <gtest/gtest.h>

#include "core/cpu/jit/loop_idiom_trip.hpp"

using namespace vitreo;
using namespace vitreo::cpu::jit::loop_idiom;

namespace {

// The reference evaluates the Jcc condition from the CMP flag definitions
// (Intel SDM Vol. 2 CMP/Jcc), written independently of eval_cont.
bool ref_cont(Cond c, u64 a, u64 b, u32 width) {
    const u64 m = width == 64 ? ~u64{0} : (u64{1} << width) - 1;
    a &= m;
    b &= m;
    const u64 r = (a - b) & m;
    const bool cf = a < b;
    const bool zf = r == 0;
    const bool sf = (r >> (width - 1)) & 1;
    const bool sa = (a >> (width - 1)) & 1;
    const bool sb = (b >> (width - 1)) & 1;
    const bool of = (sa != sb) && (sf != sa);
    switch (c) {
    case Cond::Z: return zf;
    case Cond::NZ: return !zf;
    case Cond::B: return cf;
    case Cond::AE: return !cf;
    case Cond::BE: return cf || zf;
    case Cond::A: return !cf && !zf;
    case Cond::L: return sf != of;
    case Cond::GE: return sf == of;
    case Cond::LE: return zf || (sf != of);
    case Cond::G: return !zf && (sf == of);
    }
    return false;
}

}  // namespace

TEST(LoopIdiomTrip, MatchesBruteForce) {
    const Cond conds[] = {Cond::Z, Cond::NZ, Cond::B, Cond::AE, Cond::BE,
                          Cond::A, Cond::L, Cond::GE, Cond::LE, Cond::G};
    const i64 steps[] = {-16, -8, -4, -2, -1, 1, 2, 4, 8, 16, 3, -5};
    const u64 limits[] = {1, 7, 4095};
    const u64 vals[] = {0, 1, 2, 15, 16, 4094, 4095, 4096, 0x7FFFFFFFull, 0x80000000ull,
                        0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                        ~u64{0}, 0xFFFFF000ull, 0x7FFFF800ull};
    u64 checked = 0;
    for (u32 width : {32u, 64u}) {
        for (Cond c : conds) {
            for (bool lhs : {true, false}) {
                for (i64 step : steps) {
                    for (u64 limit : limits) {
                        for (u64 v1 : vals) {
                            for (u64 bound : vals) {
                                const TripResult t =
                                    first_induction_exit(c, lhs, v1, step, width, bound, limit);
                                const u64 m = width == 64 ? ~u64{0} : 0xFFFFFFFFull;
                                // Brute force over what the solver claims.
                                const u64 upto = t.kind == TripResult::Kind::Exit ? t.k : t.k;
                                ASSERT_GE(upto, t.kind == TripResult::Kind::Exit ? 1u : 0u);
                                ASSERT_LE(upto, limit)
                                    << "w=" << width << " c=" << int(c) << " lhs=" << lhs
                                    << " step=" << step << " v1=" << v1 << " b=" << bound
                                    << " kind=" << int(t.kind);
                                u64 first_exit = 0;
                                for (u64 k = 1; k <= upto; ++k) {
                                    const u64 v = (v1 + (k - 1) * static_cast<u64>(step)) & m;
                                    const bool cont = lhs ? ref_cont(c, v, bound, width)
                                                          : ref_cont(c, bound, v, width);
                                    if (!cont) {
                                        first_exit = k;
                                        break;
                                    }
                                }
                                if (t.kind == TripResult::Kind::Exit) {
                                    ASSERT_EQ(first_exit, t.k)
                                        << "w=" << width << " c=" << int(c) << " lhs=" << lhs
                                        << " step=" << step << " v1=" << v1 << " b=" << bound;
                                } else {
                                    ASSERT_EQ(first_exit, 0u)
                                        << "claimed no exit through " << t.k << " w=" << width
                                        << " c=" << int(c) << " step=" << step << " v1=" << v1
                                        << " b=" << bound;
                                    // Equality conditions see the whole window.
                                    if (c == Cond::Z || c == Cond::NZ) {
                                        ASSERT_EQ(t.k, limit);
                                    }
                                }
                                ++checked;
                            }
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(checked, 100000u);
}

TEST(LoopIdiomTrip, OrderedWithoutWrapSeesWholeWindow) {
    // A count-up loop far from any wrap is solved over the full window.
    const TripResult t = first_induction_exit(Cond::B, true, 1, 1, 32, 10000, 4095);
    EXPECT_EQ(t.kind, TripResult::Kind::NoExitWithin);
    EXPECT_EQ(t.k, 4095u);
    const TripResult e = first_induction_exit(Cond::B, true, 1, 1, 32, 100, 4095);
    EXPECT_EQ(e.kind, TripResult::Kind::Exit);
    EXPECT_EQ(e.k, 100u);
    // Near the unsigned wrap the window is cut at the wrap.
    const TripResult w = first_induction_exit(Cond::A, true, 0xFFFFFFF0ull, 1, 32, 0, 4095);
    EXPECT_EQ(w.kind, TripResult::Kind::NoExitWithin);
    EXPECT_EQ(w.k, 16u);
}
