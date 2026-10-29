// SPDX-License-Identifier: LGPL-2.1-only
// soft_double.hpp -- the SM5 double-precision instructions as IEEE-754 on
// 32-bit integer operations -- run on the host, and the reference model of
// the double differential checked against WARP.
//
// 1. SoftDouble on a host machine against the CPU's own IEEE-754 binary64
//    arithmetic (D3D11.3 3.1.4: IEEE-754, round to nearest even, denormals
//    honoured), with the NaN results WARP produces (quiet the first NaN
//    operand; invalid -> 0xFFF8000000000000), over every triple of an edge
//    set and a million random/structured operand triples. The machine also
//    traps any shift count the GPU would leave undefined.
// 2. double_probe_model.h against tools/oracle_probe/d3d11_doubles_probe.expected
//    (WARP on Windows): equal word for word except in WARP's documented
//    deviations from the spec, whose counts are pinned.

#include <gtest/gtest.h>

#include "graphics/shaders/soft_double.hpp"
#include "double_probe_model.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#include "../oracle_data/d3d11_doubles_shaders.h"
#pragma clang diagnostic pop

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
    U msb(U x) {
        return x == 0u ? 0xFFFFFFFFu
                       : 31u - static_cast<U>(__builtin_clz(x));
    }
    B ult(U a, U b) { return a < b; }
    B slt(U a, U b) {
        return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b);
    }
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

using Soft = graphics::soft_double::SoftDouble<HostMachine>;

std::uint64_t join(Soft::W w) {
    return static_cast<std::uint64_t>(w.hi) << 32 | w.lo;
}
Soft::W split(std::uint64_t u) {
    return Soft::W{static_cast<std::uint32_t>(u),
                   static_cast<std::uint32_t>(u >> 32)};
}

using double_probe::as_double;
using double_probe::bits_of;
using double_probe::is_nan;

constexpr std::uint64_t kDefaultNaN = 0xFFF8000000000000ull;
std::uint64_t quiet(std::uint64_t u) { return u | (1ull << 51); }

// WARP's NaN results (d3d11_doubles_probe.expected dbin/drnd/dun): the first
// NaN operand in source order, quieted; an invalid operation's NaN is
// 0xFFF8000000000000.
std::uint64_t nan_rule(std::uint64_t a, std::uint64_t b, std::uint64_t ieee) {
    if (is_nan(a)) return quiet(a);
    if (is_nan(b)) return quiet(b);
    return is_nan(ieee) ? kDefaultNaN : ieee;
}

struct Checker {
    HostMachine machine;
    Soft soft{machine};
    int failures = 0;

    void check(const char* op, std::uint64_t a, std::uint64_t b,
               std::uint64_t c, std::uint64_t want, std::uint64_t got) {
        if (want == got) return;
        if (++failures <= 20) {
            ADD_FAILURE() << op << std::hex << " a=0x" << a << " b=0x" << b
                          << " c=0x" << c << ": want 0x" << want << " got 0x"
                          << got;
        }
    }

    void run(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
        const double x = as_double(a), y = as_double(b), z = as_double(c);
        check("dadd", a, b, 0, nan_rule(a, b, bits_of(x + y)),
              join(soft.add(split(a), split(b))));
        check("dmul", a, b, 0, nan_rule(a, b, bits_of(x * y)),
              join(soft.mul(split(a), split(b))));
        check("ddiv", a, b, 0, nan_rule(a, b, bits_of(x / y)),
              join(soft.div(split(a), split(b))));
        // dfma: the product's NaN rules come first (0 * inf + NaN is the
        // invalid NaN), then the addend's.
        const bool invalid_product = (std::isinf(x) && y == 0.0) ||
                                     (x == 0.0 && std::isinf(y));
        const std::uint64_t f = bits_of(std::fma(x, y, z));
        const std::uint64_t fma_want =
            is_nan(a) ? quiet(a)
            : is_nan(b) ? quiet(b)
            : invalid_product ? kDefaultNaN
            : is_nan(c) ? quiet(c)
            : is_nan(f) ? kDefaultNaN : f;
        check("dfma", a, b, c, fma_want,
              join(soft.fma(split(a), split(b), split(c))));
        check("drcp", a, 0, 0, is_nan(a) ? quiet(a) : bits_of(1.0 / x),
              join(soft.rcp(split(a))));
        check("dmin", a, b, 0, double_probe::dmin(a, b),
              join(soft.min(split(a), split(b))));
        check("dmax", a, b, 0, double_probe::dmax(a, b),
              join(soft.max(split(a), split(b))));
        check("deq", a, b, 0, x == y, soft.eq(split(a), split(b)));
        check("dlt", a, b, 0, x < y, soft.lt(split(a), split(b)));
        check("dge", a, b, 0, x >= y, soft.ge(split(a), split(b)));
        // dtof: a NaN keeps its sign and top payload bits, quieted.
        const std::uint32_t f32 =
            is_nan(a) ? static_cast<std::uint32_t>(a >> 63) << 31 |
                            0x7FC00000u |
                            static_cast<std::uint32_t>((a >> 29) & 0x7FFFFFu)
                      : double_probe::dtof(a);
        check("dtof", a, 0, 0, f32, soft.to_float(split(a)));
        check("dtoi", a, 0, 0, double_probe::dtoi(a), soft.to_int(split(a)));
        check("dtou", a, 0, 0, double_probe::dtou(a), soft.to_uint(split(a)));
        check("dsat", a, 0, 0, double_probe::dsat(a),
              join(soft.saturate(split(a))));
        const auto word = static_cast<std::uint32_t>(b);
        const std::uint64_t from_float =
            double_probe::is_nan32(word)
                ? static_cast<std::uint64_t>(word >> 31) << 63 |
                      0x7FF8000000000000ull |
                      static_cast<std::uint64_t>(word & 0x7FFFFFu) << 29
                : double_probe::ftod(word);
        check("ftod", word, 0, 0, from_float, join(soft.from_float(word)));
        check("itod", word, 0, 0, double_probe::itod(word),
              join(soft.from_int(word)));
        check("utod", word, 0, 0, double_probe::utod(word),
              join(soft.from_uint(word)));
        check("fsat", word, 0, 0, double_probe::fsat(word),
              soft.saturate_float(word));
    }
};

const std::vector<std::uint64_t> kEdges = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull,
    0xBFF0000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull,
    0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull,
    0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
    0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000123ull,
    0x7FF0000000000001ull, 0x3FE0000000000000ull, 0x4340000000000000ull,
    0x3CA0000000000000ull, 0x7FE0000000000000ull, 0x0008000000000000ull,
    0x0000000000000003ull, 0x3FEFFFFFFFFFFFFFull, 0x3FF0000000000001ull,
    0x41E0000000000000ull, 0xC1E0000000000000ull, 0x41DFFFFFFFE00000ull,
    0x41EFFFFFFFF00000ull, 0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull,
    0x36A0000000000000ull, 0x3690000000000000ull, 0x36A8000000000000ull,
    0x380FFFFFFFFFFFFFull, 0x3FF0000010000000ull, 0xC00FFFFFFFFFFFFFull,
};

} // namespace

TEST(SoftDouble, EveryEdgeTripleMatchesHostIeee) {
    Checker checker;
    for (const auto a : kEdges) {
        for (const auto b : kEdges) {
            for (const auto c : kEdges) checker.run(a, b, c);
        }
    }
    EXPECT_EQ(checker.failures, 0);
    EXPECT_FALSE(checker.machine.undefined_shift);
}

TEST(SoftDouble, RandomOperandsMatchHostIeee) {
    Checker checker;
    std::mt19937_64 rng(0xD0B1Eu);
    const auto with_exponent = [&](std::uint64_t lo, std::uint64_t span) {
        return (rng() & 0x800FFFFFFFFFFFFFull) | ((lo + rng() % span) << 52);
    };
    const auto operand = [&](unsigned kind) -> std::uint64_t {
        switch (kind % 6u) {
        case 0: return rng();                          // every class
        case 1: return with_exponent(1023 - 40, 80);   // rounding
        case 2: return with_exponent(0, 60);           // denormal results
        case 3: return with_exponent(2000, 47);        // overflow
        case 4:                                        // denormal operands
            return (rng() & 0x800FFFFFFFFFFFFFull) >> (rng() % 52);
        default: return kEdges[rng() % kEdges.size()];
        }
    };
    for (int i = 0; i < 1000000 && checker.failures == 0; ++i) {
        std::uint64_t a = operand(static_cast<unsigned>(rng()));
        std::uint64_t b = operand(static_cast<unsigned>(rng()));
        std::uint64_t c = operand(static_cast<unsigned>(rng()));
        // Add cancellation and fma cancellation against the product.
        if (i % 7 == 0) b = a ^ 0x8000000000000000ull ^ (rng() & 0xFFFFu);
        if (i % 11 == 0) {
            c = bits_of(-(as_double(a) * as_double(b))) ^ (rng() & 0xFFu);
        }
        checker.run(a, b, c);
    }
    EXPECT_EQ(checker.failures, 0);
    EXPECT_FALSE(checker.machine.undefined_shift);
}

namespace {

std::map<std::string, std::vector<std::uint32_t>> load_expected() {
    std::map<std::string, std::vector<std::uint32_t>> words;
    std::ifstream file(std::string(VITREO_SOURCE_DIR) +
                       "/tests/oracle_data/d3d11_doubles_probe.expected");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        std::string name, offset;
        // "<shader> <word offset>: <eight hex words>"
        if (!(fields >> name >> offset) || offset.size() < 2 ||
            offset.back() != ':' ||
            offset.find_first_not_of("0123456789") != offset.size() - 1) {
            continue;
        }
        std::string word;
        while (fields >> word) {
            words[name].push_back(
                static_cast<std::uint32_t>(std::stoul(word, nullptr, 16)));
        }
    }
    return words;
}

/// Output words per thread (per group for the lane-uniform *uni shaders).
unsigned columns_of(const std::string& name) {
    static const std::map<std::string, unsigned> columns = {
        {"dbin", 18},  {"drnd", 14},   {"dun", 8},     {"dconv", 6},
        {"dvec", 20},  {"dmod", 32},   {"p_dsat", 32}, {"p_dabs", 32},
        {"p_dnabs", 32}, {"dimm", 10}, {"p_dimm1", 32}, {"duni", 4},
        {"iuni", 4},     {"fmauni", 2},
    };
    return columns.at(name);
}

} // namespace

// The model is D3D11.3 plus WARP's choices; WARP itself departs from the
// spec in three places, measured and pinned here (count per shader column):
//  - dtoi/dtou/itod computed per thread take another lane's clamp/sign
//    decision (dun/dconv/dmod/p_*): the lane-uniform duni/iuni rows, where
//    all 64 lanes convert the same value, are right (dtoi, itod exactly).
//  - dtou loses precision next to 2^31 even lane-uniform (duni column 1:
//    2147483649.0 -> 0x80000000, 2147483647.99976 -> 0x7ffffffe; 22.17.4
//    rounds toward zero).
//  - dfma is not fused at the extremes (fmauni is lane-uniform): results
//    whose addend dominates come out halved, and a product that overflows
//    before an infinite / huge addend gives NaN (22.14.7: 0.5 ULP for the
//    fused operation).
TEST(SoftDouble, ProbeModelMatchesWarp) {
    const auto warp = load_expected();
    ASSERT_FALSE(warp.empty()) << "d3d11_doubles_probe.expected unreadable";
    const double_probe::Model model(g_dbl_input);
    // (shader, word column) -> documented mismatches.
    struct Deviation {
        const char* shader;
        unsigned column;
        unsigned count;
    };
    // Columns: dbin 8/9 dfma; drnd 10/11 dfma(a, b, -(a*b)); dun 3 dtoi,
    // 4 dtou; dconv 2/3 itod; dvec 16/17 itod; dmod (and the patched
    // p_*) 6/7 dfma, 20/21 itod, 25 dtoi, 26 dtou; duni 1 dtou; fmauni 0/1.
    // A double that differs only in its exponent differs in the high word.
    const Deviation deviations[] = {
        // dtoi/dtou/itod per thread: another lane's decision.
        {"dun", 3, 433},     {"dun", 4, 229},      {"dconv", 3, 9},
        {"dvec", 17, 7},     {"dmod", 21, 7},      {"dmod", 25, 14},
        {"dmod", 26, 11},    {"p_dsat", 21, 7},    {"p_dsat", 25, 14},
        {"p_dsat", 26, 11},  {"p_dabs", 21, 7},    {"p_dabs", 25, 14},
        {"p_dabs", 26, 11},  {"p_dnabs", 21, 7},   {"p_dnabs", 25, 14},
        {"p_dnabs", 26, 11}, {"p_dimm1", 21, 7},  {"p_dimm1", 25, 14},
        {"p_dimm1", 26, 11},
        // dtou next to 2^31, lane-uniform.
        {"duni", 1, 5},
        // dfma not fused at the extremes (p_dsat's saturate hides both).
        {"dbin", 9, 30},     {"drnd", 11, 37},     {"fmauni", 1, 30},
        {"dmod", 7, 2},      {"p_dabs", 7, 2},     {"p_dnabs", 7, 2},
        {"p_dimm1", 7, 2},
    };
    std::map<std::pair<std::string, unsigned>, unsigned> mismatches;
    for (const auto& shader : g_dbl_shaders) {
        const auto want = model.answer(shader.name);
        const auto it = warp.find(shader.name);
        ASSERT_NE(it, warp.end()) << shader.name;
        const auto& got = it->second;
        ASSERT_EQ(want.size(), shader.words) << shader.name;
        ASSERT_EQ(got.size(), shader.words) << shader.name;
        const unsigned columns = columns_of(shader.name);
        for (unsigned i = 0; i < want.size(); ++i) {
            const unsigned other =
                want[i].kind == double_probe::Kind::DoubleLo ? i + 1
                : want[i].kind == double_probe::Kind::DoubleHi ? i - 1
                                                               : i;
            if (!double_probe::matches(got[i], want[i], got[other])) {
                ++mismatches[{shader.name, i % columns}];
            }
        }
    }
    for (const auto& d : deviations) {
        const auto it = mismatches.find({d.shader, d.column});
        const unsigned seen = it == mismatches.end() ? 0u : it->second;
        EXPECT_EQ(seen, d.count) << d.shader << " column " << d.column;
        if (it != mismatches.end()) mismatches.erase(it);
    }
    for (const auto& [where, count] : mismatches) {
        ADD_FAILURE() << "WARP differs from the model in " << where.first
                      << " column " << where.second << ": " << count
                      << " words";
    }
}
