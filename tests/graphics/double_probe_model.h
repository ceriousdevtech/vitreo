// SPDX-License-Identifier: LGPL-2.1-only
// The D3D11.3 answer for every output word of the double-precision
// differential (tools/oracle_probe/d3d11_doubles_shaders.hlsl), computed with
// the host CPU's IEEE-754 double arithmetic (round to nearest even, denormals
// on) and the choices the spec leaves open as WARP made them
// (d3d11_doubles_probe.expected): dmin/dmax return src1 for equal operands,
// drcp is correctly rounded, _sat as measured. A NaN answer accepts any NaN
// (3.1.3.2: "the exact bit pattern of the NaN is not required").
//
// Operand ORDER follows fxc's listing where the answer depends on it (dmin /
// dmax of +0 and -0): cs_dmod computes min(a, b) as `dmin r, b, a`.
//
// test_soft_double.cpp checks this model against WARP's words;
// test_shader_double_gpu.cpp checks Metal against this model.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace double_probe {

/// How an answer word compares: bit for bit, or a NaN answer (of a double
/// low/high word or a float) that any NaN satisfies.
enum class Kind : std::uint8_t { Exact, DoubleLo, DoubleHi, Float };

struct Word {
    std::uint32_t value;
    Kind kind;
    bool nan;  // the answer is a NaN: any NaN of the kind matches
};

// Input layout (d3d11_doubles_gen_input.py).
inline constexpr unsigned kDE = 0, kRT = 128, kUA = 12416, kFS = 14592,
                          kIS = 14848;

inline double as_double(std::uint64_t u) {
    double d;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}
inline std::uint64_t bits_of(double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}
inline float as_float(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
inline std::uint32_t bits_of(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
inline bool is_nan(std::uint64_t u) {
    return ((u >> 52) & 0x7FFu) == 0x7FFu && (u & 0xFFFFFFFFFFFFFull) != 0u;
}
inline bool is_nan32(std::uint32_t u) {
    return ((u >> 23) & 0xFFu) == 0xFFu && (u & 0x7FFFFFu) != 0u;
}
inline bool word_is_nan(std::uint32_t got, const Word& want,
                        std::uint32_t other_half) {
    switch (want.kind) {
    case Kind::DoubleLo:
        return is_nan(static_cast<std::uint64_t>(other_half) << 32 | got);
    case Kind::DoubleHi:
        return is_nan(static_cast<std::uint64_t>(got) << 32 | other_half);
    case Kind::Float: return is_nan32(got);
    case Kind::Exact: return false;
    }
    return false;
}

constexpr std::uint64_t kSign = 0x8000000000000000ull;

inline std::uint64_t dadd(std::uint64_t a, std::uint64_t b) {
    return bits_of(as_double(a) + as_double(b));
}
inline std::uint64_t dmul(std::uint64_t a, std::uint64_t b) {
    return bits_of(as_double(a) * as_double(b));
}
inline std::uint64_t ddiv(std::uint64_t a, std::uint64_t b) {
    return bits_of(as_double(a) / as_double(b));
}
inline std::uint64_t dfma(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return bits_of(std::fma(as_double(a), as_double(b), as_double(c)));
}
inline std::uint64_t drcp(std::uint64_t a) { return bits_of(1.0 / as_double(a)); }
/// 22.14.3: a NaN operand yields the other; equal operands src1 (WARP).
inline std::uint64_t dmin(std::uint64_t s0, std::uint64_t s1) {
    if (is_nan(s1)) return s0;
    if (is_nan(s0)) return s1;
    return as_double(s0) < as_double(s1) ? s0 : s1;
}
inline std::uint64_t dmax(std::uint64_t s0, std::uint64_t s1) {
    if (is_nan(s1)) return s0;
    if (is_nan(s0)) return s1;
    return as_double(s0) > as_double(s1) ? s0 : s1;
}
inline std::uint32_t mask(bool b) { return b ? 0xFFFFFFFFu : 0u; }
inline std::uint32_t dtof(std::uint64_t a) {
    return bits_of(static_cast<float>(as_double(a)));
}
/// 22.17.3: toward zero, clamped, NaN -> 0.
inline std::uint32_t dtoi(std::uint64_t a) {
    const double d = as_double(a);
    if (std::isnan(d)) return 0u;
    if (d >= 2147483647.0) return 0x7FFFFFFFu;
    if (d <= -2147483648.0) return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(d));
}
/// 22.17.4.
inline std::uint32_t dtou(std::uint64_t a) {
    const double d = as_double(a);
    if (std::isnan(d) || d <= 0.0) return 0u;
    if (d >= 4294967295.0) return 0xFFFFFFFFu;
    return static_cast<std::uint32_t>(d);
}
inline std::uint64_t ftod(std::uint32_t f) {
    return bits_of(static_cast<double>(as_float(f)));
}
inline std::uint64_t itod(std::uint32_t i) {
    return bits_of(static_cast<double>(static_cast<std::int32_t>(i)));
}
inline std::uint64_t utod(std::uint32_t u) {
    return bits_of(static_cast<double>(u));
}
/// _sat on a double result (measured on WARP): NaN -> +0, -0 stays,
/// negative -> +0, >= 1 -> 1.0.
inline std::uint64_t dsat(std::uint64_t x) {
    if (is_nan(x)) return 0u;
    if (x == kSign) return x;
    if ((x & kSign) != 0u) return 0u;
    if (as_double(x) >= 1.0) return 0x3FF0000000000000ull;
    return x;
}
/// dtof_sat (measured): a float saturate that flushes denormals and -0.
inline std::uint32_t fsat(std::uint32_t f) {
    if (is_nan32(f) || (f & 0x80000000u) != 0u || f < 0x00800000u) return 0u;
    return f >= 0x3F800000u ? 0x3F800000u : f;
}

class Model {
public:
    explicit Model(const std::uint32_t* input) : in_(input) {}

    std::uint64_t de(unsigned i) const { return d(kDE + 2 * i); }
    std::uint64_t rt(unsigned i) const { return d(kRT + 2 * i); }
    std::uint64_t ua(unsigned i) const { return d(kUA + 2 * i); }
    std::uint32_t fs(unsigned i) const { return in_[kFS + i]; }
    std::uint32_t is(unsigned i) const { return in_[kIS + i]; }

    /// Every output word of one probe shader (name as in the .expected).
    std::vector<Word> answer(const std::string& shader) const {
        std::vector<Word> out;
        if (shader == "dbin") {
            for (unsigned t = 0; t < 4096; ++t) {
                const unsigned i = t >> 6, j = t & 63;
                const auto a = de(i), b = de(j), c = de((i * 5 + j * 3) & 63);
                put(out, dadd(a, b));
                put(out, dadd(a, b ^ kSign));
                put(out, dmul(a, b));
                put(out, ddiv(a, b));
                put(out, dfma(a, b, c));
                put(out, dmin(a, b));
                put(out, dmax(a, b));
                const double x = as_double(a), y = as_double(b);
                exact(out, mask(x == y));
                exact(out, mask(x >= y));
                exact(out, mask(x < y));
                exact(out, mask(x != y));
            }
        } else if (shader == "drnd") {
            for (unsigned t = 0; t < 2048; ++t) {
                const auto a = rt(3 * t), b = rt(3 * t + 1), c = rt(3 * t + 2);
                put(out, dadd(a, b));
                put(out, dadd(a, b ^ kSign));
                put(out, dmul(a, b));
                put(out, ddiv(a, b));
                put(out, dfma(a, b, c));
                put(out, dfma(a, b, dmul(b, a) ^ kSign));
                put(out, drcp(a));
            }
        } else if (shader == "dun") {
            for (unsigned t = 0; t < 1088; ++t) {
                const auto a = ua(t);
                put(out, drcp(a));
                flt(out, dtof(a));
                exact(out, dtoi(a));
                exact(out, dtou(a));
                put_exact(out, a ^ kSign);
                exact(out, mask(as_double(a) == as_double(a)));
            }
        } else if (shader == "dconv") {
            for (unsigned t = 0; t < 256; ++t) {
                put(out, ftod(fs(t)));
                put(out, itod(is(t)));
                put(out, utod(is(t)));
            }
        } else if (shader == "dvec") {
            for (unsigned t = 0; t < 64; ++t) {
                const std::uint64_t v[2] = {de(t), de((t + 1) & 63)};
                const std::uint64_t u[2] = {de((t + 7) & 63), de((t * 3) & 63)};
                put(out, dadd(v[0], u[1]));
                put(out, dadd(v[1], u[0]));
                const std::uint64_t p[2] = {dmul(v[0], u[0]), dmul(v[1], u[1])};
                put(out, p[0]);
                put(out, p[1]);
                flt(out, dtof(p[0]));
                flt(out, dtof(p[1]));
                const bool lt[2] = {as_double(v[0]) < as_double(u[0]),
                                    as_double(v[1]) < as_double(u[1])};
                exact(out, mask(lt[0]));
                exact(out, mask(lt[1]));
                put_exact(out, lt[0] ? v[0] : u[0]);
                put_exact(out, lt[1] ? v[1] : u[1]);
                put(out, itod(is(t)));
                put(out, itod(is(t + 64)));
            }
        } else if (shader == "dmod" || shader == "p_dsat" ||
                   shader == "p_dabs" || shader == "p_dnabs" ||
                   shader == "p_dimm1") {
            const bool sat = shader == "p_dsat";
            const auto dbl = [&](std::uint64_t x) { put(out, sat ? dsat(x) : x); };
            const auto raw = [&](std::uint64_t x) {
                put_exact(out, sat ? dsat(x) : x);
            };
            for (unsigned t = 0; t < 64; ++t) {
                const auto a = de(t), b = de((t * 7 + 3) & 63),
                           c = de((t * 5 + 1) & 63);
                const auto sel = static_cast<std::uint32_t>(c);
                dbl(dadd(b, a));
                dbl(dmul(b, a));
                dbl(ddiv(a, b));
                dbl(dfma(a, b, c));
                dbl(dmin(b, a));  // fxc: dmin r1.xy, r1.zwzw (b), r2.xyxy (a)
                dbl(dmax(b, a));
                dbl(drcp(a));
                if (shader == "p_dabs") {
                    raw(a & ~kSign);
                } else if (shader == "p_dnabs") {
                    raw(a | kSign);
                } else if (shader == "p_dimm1") {
                    // A one-component d(0x3fb999999999999a): its first
                    // DWORD in every component (measured).
                    raw(0x9999999A9999999Aull);
                } else {
                    raw(a ^ kSign);
                }
                raw((sel & 1u) != 0u ? a : b);
                dbl(ftod(fs(t)));
                dbl(itod(is(t)));
                dbl(utod(is(t)));
                const std::uint32_t f = dtof(a);
                if (sat) {
                    exact(out, fsat(f));
                } else {
                    flt(out, f);
                }
                exact(out, dtoi(a));
                exact(out, dtou(a));
                const double x = as_double(a), y = as_double(b);
                exact(out, mask(y == x));
                exact(out, mask(x >= y));
                exact(out, mask(x < y));
                exact(out, mask(y != x));
                exact(out, 0u);
            }
        } else if (shader == "dimm") {
            // fxc: dmov_sat; dadd with d(0.1L, -(double)0.1f); dmul with
            // d(1e300L); dmin a, d(2.5e-310L) (a denormal).
            for (unsigned t = 0; t < 64; ++t) {
                const auto a = de(t);
                put(out, dsat(a));
                put(out, dadd(a, 0x3FB999999999999Aull));
                put(out, dmul(a, 0x7E37E43C8800759Cull));
                put(out, dmin(a, 0x00002E055C9A3F6Cull));
                put(out, dadd(a, 0xBFB99999A0000000ull));
            }
        } else if (shader == "duni") {
            for (unsigned g = 0; g < 1088; ++g) {
                const auto a = ua(g);
                exact(out, dtoi(a));
                exact(out, dtou(a));
                flt(out, dtof(a));
                exact(out, 0u);
            }
        } else if (shader == "iuni") {
            for (unsigned g = 0; g < 256; ++g) {
                put(out, itod(is(g)));
                put(out, utod(is(g)));
            }
        } else if (shader == "fmauni") {
            for (unsigned g = 0; g < 4096; ++g) {
                const unsigned i = g >> 6, j = g & 63;
                put(out, dfma(de(i), de(j), de((i * 5 + j * 3) & 63)));
            }
        }
        return out;
    }

private:
    std::uint64_t d(unsigned w) const {
        return static_cast<std::uint64_t>(in_[w + 1]) << 32 | in_[w];
    }
    static void put(std::vector<Word>& out, std::uint64_t x) {
        const bool nan = is_nan(x);
        out.push_back({static_cast<std::uint32_t>(x), Kind::DoubleLo, nan});
        out.push_back({static_cast<std::uint32_t>(x >> 32), Kind::DoubleHi, nan});
    }
    static void put_exact(std::vector<Word>& out, std::uint64_t x) {
        out.push_back({static_cast<std::uint32_t>(x), Kind::Exact, false});
        out.push_back({static_cast<std::uint32_t>(x >> 32), Kind::Exact, false});
    }
    static void flt(std::vector<Word>& out, std::uint32_t f) {
        out.push_back({f, Kind::Float, is_nan32(f)});
    }
    static void exact(std::vector<Word>& out, std::uint32_t v) {
        out.push_back({v, Kind::Exact, false});
    }

    const std::uint32_t* in_;
};

/// Whether @p got (with the other half of its double at @p other_half)
/// satisfies @p want.
inline bool matches(std::uint32_t got, const Word& want,
                    std::uint32_t other_half) {
    if (want.nan) return word_is_nan(got, want, other_half);
    return got == want.value;
}

} // namespace double_probe
