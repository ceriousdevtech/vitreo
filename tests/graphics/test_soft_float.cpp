// SPDX-License-Identifier: LGPL-2.1-only
// soft_float.hpp -- binary32 arithmetic with denormals honoured on 32-bit
// integer operations, the SM 6.2 "fp32-denorm-mode"="preserve" path -- run
// on the host against the CPU's own IEEE-754 binary32 (arm64 with FPCR.FZ
// clear: denormals kept, round to nearest even): every pair / triple of an
// edge set and millions of random operands biased to the denormal range and
// the normal/denormal boundary. NaN results follow soft_float.hpp's stated
// rules (the first NaN operand quieted; an invalid operation 0xFFC00000).
// The machine traps any shift count the GPU would leave undefined.

#include <gtest/gtest.h>

#include "graphics/shaders/soft_float.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace vitreo;

namespace {

struct HostMachine {
    using U = std::uint32_t;
    using B = bool;
    bool undefined_shift = false;

    U k(u32 v) { return v; }
    U add(U a, U b) { return a + b; }
    U sub(U a, U b) { return a - b; }
    U band(U a, U b) { return a & b; }
    U bor(U a, U b) { return a | b; }
    U bxor(U a, U b) { return a ^ b; }
    U shl(U a, U n) {
        if (n >= 32u) {
            undefined_shift = true;
            return 0u;
        }
        return a << n;
    }
    U shr(U a, U n) {
        if (n >= 32u) {
            undefined_shift = true;
            return 0u;
        }
        return a >> n;
    }
    std::pair<U, U> mul_wide(U a, U b) {
        const std::uint64_t p = static_cast<std::uint64_t>(a) * b;
        return {static_cast<U>(p), static_cast<U>(p >> 32)};
    }
    U msb(U x) { return x == 0u ? 0xFFFFFFFFu : 31u - static_cast<U>(__builtin_clz(x)); }
    B ult(U a, U b) { return a < b; }
    B slt(U a, U b) { return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b); }
    B eq(U a, U b) { return a == b; }
    B ne(U a, U b) { return a != b; }
    B land(B a, B b) { return a && b; }
    B lor(B a, B b) { return a || b; }
    B lnot(B a) { return !a; }
    U sel(B c, U a, U b) { return c ? a : b; }
    template <std::size_t N, class Body>
    std::array<U, N> loop(u32 count, std::array<U, N> state, Body&& body) {
        for (u32 i = 0; i < count; ++i) body(state);
        return state;
    }
};

using Soft = graphics::soft_float::SoftFloat<HostMachine>;

float f(u32 bits) {
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
u32 b(float v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    return bits;
}
bool nan(u32 x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }

/// The expected word of an arithmetic result: the host's value, except a
/// NaN, which is the first NaN operand quieted or else 0xFFC00000.
u32 expect_nan_rule(u32 host, std::initializer_list<u32> operands) {
    if (!nan(host)) return host;
    for (u32 x : operands) {
        if (nan(x)) return x | 0x00400000u;
    }
    return 0xFFC00000u;
}

std::vector<u32> edge_set() {
    std::vector<u32> v = {0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x0000FFFFu,
                          0x00400000u, 0x00400001u, 0x007FFFFEu, 0x007FFFFFu, 0x00800000u,
                          0x00800001u, 0x00FFFFFFu, 0x01000000u, 0x0C000000u, 0x1F800000u,
                          0x33800000u, 0x34000000u, 0x3F000000u, 0x3F800000u, 0x3F800001u,
                          0x3FC00000u, 0x40000000u, 0x4B800000u, 0x5F000000u, 0x7E800000u,
                          0x7F000000u, 0x7F7FFFFFu, 0x7F800000u, 0x7FC00000u, 0x7F800001u,
                          0x00123456u, 0x00654321u, 0x03ABCDEFu};
    const usize n = v.size();
    for (usize i = 0; i < n; ++i) v.push_back(v[i] | 0x80000000u);
    return v;
}

/// Random operands: a third denormals, a third in the lowest normal
/// binades, a third anywhere finite (plus the occasional special).
u32 random_operand(std::mt19937& rng) {
    const u32 r = rng();
    const u32 sign = r & 0x80000000u;
    switch (rng() % 8u) {
    case 0:
    case 1:
    case 2: return sign | (rng() & 0x7FFFFFu);
    case 3:
    case 4: return sign | (((1u + rng() % 24u) << 23) | (rng() & 0x7FFFFFu));
    case 5: return sign | (((100u + rng() % 54u) << 23) | (rng() & 0x7FFFFFu));
    case 6: return sign | (((rng() % 254u) + 1u) << 23) | (rng() & 0x7FFFFFu);
    default: {
        static const u32 special[] = {0u, 0x7F800000u, 0x7FC00000u, 0x3F800000u, 0x00800000u, 0x7F7FFFFFu};
        return sign | special[rng() % 6u];
    }
    }
}

struct Checker {
    HostMachine m;
    Soft s{m};
    int failures = 0;
    u64 checked = 0;

    void binary(u32 x, u32 y) {
        volatile float fx = f(x), fy = f(y);
        check("add", s.add(x, y), expect_nan_rule(b(fx + fy), {x, y}), x, y, 0);
        check("sub", s.sub(x, y), expect_nan_rule(b(fx - fy), {x, y ^ 0x80000000u}), x, y, 0);
        check("mul", s.mul(x, y), expect_nan_rule(b(fx * fy), {x, y}), x, y, 0);
        check("div", s.div(x, y), expect_nan_rule(b(fx / fy), {x, y}), x, y, 0);
        check("eq", s.eq(x, y) ? 1u : 0u, fx == fy ? 1u : 0u, x, y, 0);
        check("lt", s.lt(x, y) ? 1u : 0u, fx < fy ? 1u : 0u, x, y, 0);
        check("ge", s.ge(x, y) ? 1u : 0u, fx >= fy ? 1u : 0u, x, y, 0);
        // DXIL.rst FMax / FMin: NaN yields the other operand; max = a >= b
        // ? a : b, min = a < b ? a : b.
        const u32 mx = nan(x) ? y : nan(y) ? x : (fx >= fy ? x : y);
        const u32 mn = nan(x) ? y : nan(y) ? x : (fx < fy ? x : y);
        check("max", s.max(x, y), mx, x, y, 0);
        check("min", s.min(x, y), mn, x, y, 0);
    }
    void unary(u32 x) {
        const float fx = f(x);
        const u32 sat = nan(x) || !(fx > 0.0f) ? 0u : (fx > 1.0f ? 0x3F800000u : x);
        check("saturate", s.saturate(x), sat, x, 0, 0);
    }
    void ternary(u32 x, u32 y, u32 z) {
        volatile float fx = f(x), fy = f(y), fz = f(z);
        check("fma", s.fma(x, y, z), expect_nan_rule(b(std::fma(fx, fy, fz)), {x, y, z}), x, y, z);
    }
    void check(const char* op, u32 got, u32 want, u32 x, u32 y, u32 z) {
        ++checked;
        if (got == want && !m.undefined_shift) return;
        if (++failures <= 20) {
            ADD_FAILURE() << op << "(" << std::hex << x << ", " << y << ", " << z << ") = " << got
                          << ", want " << want << (m.undefined_shift ? " (undefined shift)" : "");
        }
        m.undefined_shift = false;
    }
};

}  // namespace

TEST(SoftFloat, HostIsNotFlushingDenormals) {
    volatile float tiny = f(0x00400000u);
    EXPECT_EQ(b(tiny + tiny), 0x00800000u);
    EXPECT_EQ(b(tiny * 0.5f), 0x00200000u);
}

TEST(SoftFloat, EdgeSetMatchesTheHost) {
    Checker c;
    const auto edges = edge_set();
    for (u32 x : edges) {
        c.unary(x);
        for (u32 y : edges) {
            c.binary(x, y);
            for (u32 z : edges) c.ternary(x, y, z);
        }
    }
    const u64 n = edges.size();
    EXPECT_EQ(c.checked, n + n * n * 9 + n * n * n);
    EXPECT_EQ(c.failures, 0);
}

TEST(SoftFloat, RandomOperandsMatchTheHost) {
    Checker c;
    std::mt19937 rng(0x5EED2026u);
    for (int i = 0; i < 2000000 && c.failures < 20; ++i) {
        const u32 x = random_operand(rng), y = random_operand(rng), z = random_operand(rng);
        c.unary(x);
        c.binary(x, y);
        c.ternary(x, y, z);
    }
    EXPECT_EQ(c.failures, 0);
}

TEST(SoftFloat, ProductsAndQuotientsCrossingIntoDenormalsMatchTheHost) {
    // Results right at the normal/denormal boundary: every rounding of a
    // product or quotient that lands on the denormal grid.
    Checker c;
    std::mt19937 rng(0xB0DA2026u);
    for (int i = 0; i < 1000000 && c.failures < 20; ++i) {
        const u32 sign = rng() & 0x80000000u;
        const u32 ea = 1u + rng() % 60u, eb = 127u - (rng() % 70u);
        const u32 x = sign | (ea << 23) | (rng() & 0x7FFFFFu);
        const u32 y = (rng() & 0x80000000u) | (eb << 23) | (rng() & 0x7FFFFFu);
        const u32 z = (rng() & 0x80000000u) | (rng() & 0xFFFFFFu);
        c.binary(x, y);
        c.binary(x, (y & 0x807FFFFFu) | ((254u - eb) << 23));
        c.ternary(x, y, z);
    }
    EXPECT_EQ(c.failures, 0);
}

TEST(SoftFloat, KnownDenormalResults) {
    // Values worked from IEEE-754 binary32 by hand (denormal grid 2^-149).
    HostMachine m;
    Soft s(m);
    EXPECT_EQ(s.add(0x00400000u, 0x00400000u), 0x00800000u);  // 2^-127 + 2^-127 = FLT_MIN
    EXPECT_EQ(s.sub(0x00800000u, 0x00000001u), 0x007FFFFFu);  // FLT_MIN - 2^-149
    EXPECT_EQ(s.mul(0x00800000u, 0x3F000000u), 0x00400000u);  // FLT_MIN * 0.5
    EXPECT_EQ(s.mul(0x00000003u, 0x3F000000u), 0x00000002u);  // 1.5 ulp -> tie to even 2
    EXPECT_EQ(s.mul(0x00000001u, 0x3F000000u), 0x00000000u);  // 0.5 ulp -> tie to even 0
    EXPECT_EQ(s.mul(0x80000001u, 0x3F400000u), 0x80000001u);  // -0.75 ulp -> -1 ulp
    EXPECT_EQ(s.div(0x00000003u, 0x40000000u), 0x00000002u);  // 3 / 2 ulps -> 2
    EXPECT_EQ(s.div(0x00400000u, 0x00200000u), 0x40000000u);  // denormal / denormal = 2
    EXPECT_EQ(s.fma(0x1CC00000u, 0x1C800000u, 0x00800000u), 0x00800300u);  // 1.5*2^-140 + 2^-126
    EXPECT_EQ(s.fma(0x00000001u, 0x3F800000u, 0x80000001u), 0x00000000u);  // exact zero -> +0
    EXPECT_EQ(s.max(0x00000001u, 0x00000000u), 0x00000001u);
    EXPECT_EQ(s.min(0x80000001u, 0x00000000u), 0x80000001u);
    EXPECT_TRUE(s.lt(0x00000001u, 0x00000002u));
    EXPECT_FALSE(s.eq(0x00000001u, 0x00000000u));
    EXPECT_TRUE(s.eq(0x80000000u, 0x00000000u));
    EXPECT_EQ(s.saturate(0x00000005u), 0x00000005u);
    EXPECT_EQ(s.saturate(0x80000005u), 0x00000000u);
    EXPECT_FALSE(m.undefined_shift);
}
