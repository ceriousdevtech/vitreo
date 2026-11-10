// SPDX-License-Identifier: LGPL-2.1-only
/// @file soft_float.hpp
/// @brief IEEE-754 binary32 on 32-bit integer operations, denormals
///        honoured: SM 6.2 "fp32-denorm-mode"="preserve" for GPUs whose
///        float32 arithmetic flushes them (every Apple GPU, in every Metal
///        math mode).
///
/// Contract (DXC wiki "Denorm Mode": preserve keeps the denormal inputs and
/// outputs of add, subtract, multiply and divide; DXIL.rst FAdd / FDiv tables
/// "assuming fp32-denorm-mode=preserve"), with the choices measured on
/// WARP12 and the RTX 4080 (tests/oracle_data/d3d12_denorm_probe*.expected):
///   - add, mul, fma and div are correctly rounded, round to nearest even,
///     with denormal operands and results kept (both GPUs: add and mul bit
///     for bit; WARP's mad is unfused, the RTX's fused -- fma here is the
///     fused one; division differs between them by an ulp, this one is
///     the correctly rounded quotient);
///   - comparisons, min and max order denormals by value (both GPUs: every
///     compare, min, max, clamp and saturate of the corpus equal);
///   - a NaN operand returns that NaN quieted, the first in operand order;
///     an invalid operation (inf - inf, 0 * inf, 0 / 0, inf / inf) returns
///     0xFFC00000 (WARP's default NaN); x + (-x) is +0; min / max return
///     the other operand when one is NaN (DXIL.rst FMin / FMax).
///
/// Every algorithm is written once over the abstract machine @c M of
/// soft_double.hpp (the translator runs it to emit SPIR-V, the unit tests on
/// the host against the CPU's own binary32). Straight-line except the one
/// fixed-trip loop of the divide.

#pragma once

#include <vitreo/types.hpp>

#include <array>
#include <utility>

namespace vitreo::graphics::soft_float {

template <class M>
class SoftFloat {
public:
    using U = typename M::U;
    using B = typename M::B;

    explicit SoftFloat(M& machine) : m_(machine) {}

    // -- arithmetic ------------------------------------------------------------

    U add(U a, U b) {
        const U sa = sign_of(a), sb = sign_of(b);
        // |x| >= |y|; the result takes x's sign unless it is an exact zero.
        const B swap = m_.ult(abs(a), abs(b));
        const U x = m_.sel(swap, b, a), y = m_.sel(swap, a, b);
        const U sx = m_.sel(swap, sb, sa), sy = m_.sel(swap, sa, sb);
        const B same = m_.eq(sx, sy);
        const U ex = exp_of(x), ey = exp_of(y);
        const B dx = m_.eq(ex, k(0)), dy = m_.eq(ey, k(0));
        // Significands with the implicit bit at 30 (a denormal has none and
        // the exponent of the smallest normal).
        const U mx = m_.shl(with_implicit(x, dx), k(7));
        const U my = m_.shl(with_implicit(y, dy), k(7));
        const U ex1 = m_.sel(dx, k(1), ex), ey1 = m_.sel(dy, k(1), ey);
        const U yal = shr_jam(my, m_.sub(ex1, ey1));
        const U r = m_.sel(same, m_.add(mx, yal), m_.sub(mx, yal));
        // A carry into bit 31 shifts right one (with jam); a cancellation
        // shifts left until the MSB is at bit 30.
        const B carry = m_.ne(m_.band(r, k(0x80000000u)), k(0));
        const U lz = m_.sub(k(31), m_.msb(r));  // r == 0 is replaced below
        const U n = m_.sel(carry, shr_jam(r, k(1)),
                           m_.shl(r, m_.band(m_.sub(lz, k(1)), k(31))));
        const U e = m_.sel(carry, m_.add(ex1, k(1)), m_.sub(ex1, m_.sub(lz, k(1))));
        U out = round_pack(sx, e, n);
        // x + (-x) is +0 in round to nearest; (-0) + (-0) is -0.
        out = m_.sel(m_.eq(r, k(0)), m_.sel(same, sx, k(0)), out);
        const B infa = is_inf(a), infb = is_inf(b);
        out = m_.sel(infb, b, out);
        out = m_.sel(infa, m_.sel(m_.land(infb, m_.ne(sa, sb)), k(kDefaultNan), a), out);
        out = m_.sel(is_nan(b), quiet(b), out);
        return m_.sel(is_nan(a), quiet(a), out);
    }

    U sub(U a, U b) { return add(a, m_.bxor(b, k(0x80000000u))); }

    U mul(U a, U b) {
        const U sz = m_.bxor(sign_of(a), sign_of(b));
        const Norm na = normalize(a), nb = normalize(b);
        // (2^31..2^32) x (2^30..2^31): the high word has its MSB at 29 or 30.
        const auto p = m_.mul_wide(m_.shl(na.sig, k(8)), m_.shl(nb.sig, k(7)));
        const U lo = p.first, hi = p.second;
        const B low = m_.ult(hi, k(0x40000000u));
        // Shift a low product left one; the low word's top bit comes along.
        const U top = m_.sel(low, m_.bor(m_.shl(hi, k(1)), m_.shr(lo, k(31))), hi);
        const U rest = m_.sel(low, m_.shl(lo, k(1)), lo);
        const U sig = m_.bor(top, flag(m_.ne(rest, k(0))));
        const U ez = m_.sub(m_.add(na.exp, nb.exp), m_.sel(low, k(127), k(126)));
        U out = round_pack(sz, ez, sig);
        const B zero_operand = m_.lor(is_zero(a), is_zero(b));
        out = m_.sel(zero_operand, sz, out);
        out = m_.sel(m_.lor(is_inf(a), is_inf(b)),
                     m_.sel(zero_operand, k(kDefaultNan), m_.bor(sz, k(0x7F800000u))), out);
        out = m_.sel(is_nan(b), quiet(b), out);
        return m_.sel(is_nan(a), quiet(a), out);
    }

    /// a * b + c with one rounding.
    U fma(U a, U b, U c) {
        const U sp = m_.bxor(sign_of(a), sign_of(b)), sc = sign_of(c);
        const Norm na = normalize(a), nb = normalize(b), nc = normalize(c);
        // Both terms as W * 2^(E - 189) with the MSB at bit 61: the product
        // (48 bits) and the addend.
        const auto p = m_.mul_wide(na.sig, nb.sig);
        const W p14 = shl64(W{p.first, p.second}, k(14));
        // Both MSBs at bit 61, so the larger exponent is the larger value.
        const B plow = m_.ult(p14.hi, k(0x20000000u));
        const W pw = sel(plow, shl64(p14, k(1)), p14);
        const U ep = m_.sub(m_.add(na.exp, nb.exp), m_.sel(plow, k(126), k(125)));
        const W cw{k(0), m_.shl(nc.sig, k(6))};
        const U ec = m_.add(nc.exp, k(1));
        const B zero_product = m_.lor(is_zero(a), is_zero(b));
        const B zero_c = is_zero(c);
        // The larger magnitude first (an exact zero addend is never larger).
        const B c_bigger = m_.land(m_.lnot(zero_c),
                                   m_.lor(m_.slt(ep, ec), m_.land(m_.eq(ep, ec), ult64(pw, cw))));
        const W big = sel(c_bigger, cw, pw), small = sel(c_bigger, pw, cw);
        const U eb = m_.sel(c_bigger, ec, ep), es = m_.sel(c_bigger, ep, ec);
        const U sbig = m_.sel(c_bigger, sc, sp), ssmall = m_.sel(c_bigger, sp, sc);
        // Shift distance: any amount past 63 leaves only the sticky bit.
        const U dist = m_.sub(eb, es);
        const B far = m_.lor(m_.slt(dist, k(0)), m_.ult(k(63), dist));
        W aligned = shr64_jam(small, m_.sel(far, k(63), dist));
        aligned = sel(far, W{flag(m_.ne(m_.bor(small.lo, small.hi), k(0))), k(0)}, aligned);
        aligned = sel(zero_c, W{k(0), k(0)}, aligned);  // c is the small one here
        const B same = m_.eq(sbig, ssmall);
        const W s = sel(same, add64(big, aligned), sub64(big, aligned));
        // Normalise the MSB to bit 62 (the sum is below 2^63), then keep the
        // high word with the low word as sticky: MSB at 30.
        const U m = msb64(s);
        const U shift = m_.band(m_.sub(k(62), m), k(63));
        const W n = shl64(s, shift);
        const U sig = m_.bor(n.hi, flag(m_.ne(n.lo, k(0))));
        U out = round_pack(sbig, m_.sub(eb, shift), sig);
        const B zero_sum = m_.land(m_.eq(s.lo, k(0)), m_.eq(s.hi, k(0)));
        out = m_.sel(zero_sum, m_.sel(same, sbig, k(0)), out);
        // An exact zero product leaves c, signed by the rules of addition.
        out = m_.sel(zero_product,
                     m_.sel(zero_c, m_.sel(m_.eq(sp, sc), sc, k(0)), c), out);
        const B inf_product = m_.lor(is_inf(a), is_inf(b));
        const B invalid_product = m_.land(inf_product, zero_product);
        out = m_.sel(is_inf(c), c, out);
        out = m_.sel(inf_product,
                     m_.sel(m_.land(is_inf(c), m_.ne(sp, sc)), k(kDefaultNan),
                            m_.bor(sp, k(0x7F800000u))),
                     out);
        out = m_.sel(invalid_product, k(kDefaultNan), out);
        out = m_.sel(is_nan(c), quiet(c), out);
        out = m_.sel(is_nan(b), quiet(b), out);
        return m_.sel(is_nan(a), quiet(a), out);
    }

    U div(U a, U b) {
        const U sz = m_.bxor(sign_of(a), sign_of(b));
        const Norm na = normalize(a), nb = normalize(b);
        // The dividend in [divisor, 2 * divisor): the quotient's first bit
        // is 1. 31 restoring steps leave its MSB at bit 30.
        const B small = m_.ult(na.sig, nb.sig);
        const U dividend = m_.sel(small, m_.shl(na.sig, k(1)), na.sig);
        const U divisor = nb.sig;
        const std::array<U, 2> done = m_.loop(
            31u, std::array<U, 2>{k(0), dividend}, [&](std::array<U, 2>& s) {
                const B ge = m_.lnot(m_.ult(s[1], divisor));
                s[0] = m_.bor(m_.shl(s[0], k(1)), flag(ge));
                s[1] = m_.shl(m_.sel(ge, m_.sub(s[1], divisor), s[1]), k(1));
            });
        const U sig = m_.bor(done[0], flag(m_.ne(done[1], k(0))));
        const U ez = m_.add(m_.sub(na.exp, nb.exp), m_.sel(small, k(126), k(127)));
        U out = round_pack(sz, ez, sig);
        const B za = is_zero(a), zb = is_zero(b), ia = is_inf(a), ib = is_inf(b);
        out = m_.sel(m_.lor(za, ib), sz, out);
        out = m_.sel(m_.lor(zb, ia), m_.bor(sz, k(0x7F800000u)), out);
        out = m_.sel(m_.lor(m_.land(za, zb), m_.land(ia, ib)), k(kDefaultNan), out);
        out = m_.sel(is_nan(b), quiet(b), out);
        return m_.sel(is_nan(a), quiet(a), out);
    }

    // -- comparisons (ordered: false when either operand is NaN) ---------------

    B eq(U a, U b) {
        return m_.land(ordered(a, b), m_.eq(key(a), key(b)));
    }
    B lt(U a, U b) {
        return m_.land(ordered(a, b), m_.ult(key(a), key(b)));
    }
    B ge(U a, U b) {
        return m_.land(ordered(a, b), m_.lnot(m_.ult(key(a), key(b))));
    }

    /// DXIL.rst FMax / FMin: a NaN operand yields the other; otherwise
    /// max = a >= b ? a : b and min = a < b ? a : b ("so that if min(x,y) =
    /// x then max(x,y) = y"), +0 and -0 comparing equal.
    U max(U a, U b) {
        const U r = m_.sel(m_.ult(key(a), key(b)), b, a);
        return m_.sel(is_nan(a), b, m_.sel(is_nan(b), a, r));
    }
    U min(U a, U b) {
        const U r = m_.sel(m_.ult(key(a), key(b)), a, b);
        return m_.sel(is_nan(a), b, m_.sel(is_nan(b), a, r));
    }
    /// D3D saturate: clamp to [0, 1], NaN to +0, -0 to +0 (as max(x, +0)
    /// then min(., 1) of D3D11.3 3.1.3.4: the result is never -0).
    U saturate(U a) {
        const B below = m_.lor(is_nan(a), m_.lnot(m_.ult(key(k(0)), key(a))));
        const B above = m_.ult(key(k(0x3F800000u)), key(a));
        return m_.sel(below, k(0), m_.sel(above, k(0x3F800000u), a));
    }

private:
    static constexpr u32 kDefaultNan = 0xFFC00000u;

    struct W {
        U lo;
        U hi;
    };
    /// A finite nonzero value as sig * 2^(exp - 150), sig's MSB at bit 23;
    /// a denormal gets exp <= 0 (a signed 32-bit value). Zero, infinity and
    /// NaN give don't-care fields that the callers select away.
    struct Norm {
        U sig;
        U exp;
    };

    U k(u32 v) { return m_.k(v); }
    U flag(B b) { return m_.sel(b, k(1), k(0)); }
    U sign_of(U x) { return m_.band(x, k(0x80000000u)); }
    U abs(U x) { return m_.band(x, k(0x7FFFFFFFu)); }
    U exp_of(U x) { return m_.band(m_.shr(x, k(23)), k(0xFFu)); }
    U with_implicit(U x, B denormal) {
        return m_.bor(m_.band(x, k(0x7FFFFFu)), m_.sel(denormal, k(0), k(0x800000u)));
    }
    B is_nan(U x) { return m_.ult(k(0x7F800000u), abs(x)); }
    B is_inf(U x) { return m_.eq(abs(x), k(0x7F800000u)); }
    B is_zero(U x) { return m_.eq(abs(x), k(0)); }
    U quiet(U x) { return m_.bor(x, k(0x00400000u)); }
    B ordered(U a, U b) { return m_.lnot(m_.lor(is_nan(a), is_nan(b))); }
    /// An unsigned key in value order: -0 and +0 share one; NaN don't-care.
    U key(U x) {
        const U z = m_.sel(is_zero(x), k(0), x);
        return m_.sel(m_.ne(sign_of(z), k(0)), m_.bxor(z, k(0xFFFFFFFFu)),
                      m_.bor(z, k(0x80000000u)));
    }

    Norm normalize(U x) {
        const U e = exp_of(x);
        const U f = m_.band(x, k(0x7FFFFFu));
        const B denormal = m_.eq(e, k(0));
        // A denormal's MSB moves to bit 23; its exponent drops by the shift.
        const U shift = m_.band(m_.sub(k(23), m_.msb(f)), k(31));
        return Norm{m_.sel(denormal, m_.shl(f, shift), m_.bor(f, k(0x800000u))),
                    m_.sel(denormal, m_.sub(k(1), shift), e)};
    }

    /// x >> n with every shifted-out bit ORed into bit 0 (n unsigned; any
    /// n >= 31 leaves only that bit).
    U shr_jam(U x, U n) {
        const B far = m_.ult(k(31), n);
        const U c = m_.sel(far, k(31), n);
        const U lost = m_.band(x, m_.sub(m_.shl(k(1), c), k(1)));
        const U r = m_.bor(m_.shr(x, c), flag(m_.ne(lost, k(0))));
        return m_.sel(far, flag(m_.ne(x, k(0))), r);
    }

    /// sign | the binary32 nearest sig * 2^(e - 127 - 30) (sig's MSB at bit
    /// 30, the low 7 bits round and sticky; e a signed biased exponent),
    /// ties to even; below the normal range on the denormal grid; overflow
    /// to infinity.
    U round_pack(U sign, U e, U sig) {
        const B tiny = m_.slt(e, k(1));
        const U d = m_.sub(k(1), e);
        const U s = m_.sel(tiny, shr_jam(sig, m_.sel(tiny, d, k(0))), sig);
        const U ee = m_.sel(tiny, k(0), e);
        const U rbits = m_.band(s, k(0x7Fu));
        U r = m_.shr(m_.add(s, k(0x40u)), k(7));
        r = m_.sel(m_.eq(rbits, k(0x40u)), m_.band(r, k(0xFFFFFFFEu)), r);
        // r holds the implicit bit (bit 23); a carry reaches bit 24. Packing
        // adds (ee - 1) << 23 so the implicit bit lands in the exponent; a
        // denormal (ee 0, no bit 23) or one rounding up to the smallest
        // normal (bit 23) packs as itself.
        const U biased = m_.sel(tiny, k(0), m_.sub(ee, k(1)));
        const U packed = m_.add(m_.shl(biased, k(23)), r);
        const B overflow = m_.lor(m_.slt(k(0xFE), ee),
                                  m_.lnot(m_.ult(packed, k(0x7F800000u))));
        return m_.bor(sign, m_.sel(m_.land(m_.lnot(tiny), overflow), k(0x7F800000u), packed));
    }

    // -- 64-bit helpers --------------------------------------------------------

    W sel(B c, W a, W b) { return W{m_.sel(c, a.lo, b.lo), m_.sel(c, a.hi, b.hi)}; }
    B ult64(W a, W b) {
        return m_.lor(m_.ult(a.hi, b.hi), m_.land(m_.eq(a.hi, b.hi), m_.ult(a.lo, b.lo)));
    }
    W add64(W a, W b) {
        const U lo = m_.add(a.lo, b.lo);
        return W{lo, m_.add(m_.add(a.hi, b.hi), flag(m_.ult(lo, a.lo)))};
    }
    W sub64(W a, W b) {
        return W{m_.sub(a.lo, b.lo), m_.sub(m_.sub(a.hi, b.hi), flag(m_.ult(a.lo, b.lo)))};
    }
    /// a << n, n in [0, 63].
    W shl64(W a, U n) {
        const B big = m_.ult(k(31), n);
        const U s = m_.band(n, k(31));
        const U carry = m_.sel(m_.eq(s, k(0)), k(0), m_.shr(a.lo, m_.band(m_.sub(k(32), s), k(31))));
        const W smallw{m_.shl(a.lo, s), m_.bor(m_.shl(a.hi, s), carry)};
        const W bigw{k(0), m_.shl(a.lo, s)};
        return sel(big, bigw, smallw);
    }
    /// a >> n with jam, n in [0, 63].
    W shr64_jam(W a, U n) {
        const B big = m_.ult(k(31), n);
        const U s = m_.band(n, k(31));
        const U carry = m_.sel(m_.eq(s, k(0)), k(0), m_.shl(a.hi, m_.band(m_.sub(k(32), s), k(31))));
        const U mask = m_.sub(m_.shl(k(1), s), k(1));
        // n < 32: bits of lo below s are lost.
        const W smallw{m_.bor(m_.bor(m_.shr(a.lo, s), carry), flag(m_.ne(m_.band(a.lo, mask), k(0)))),
                       m_.shr(a.hi, s)};
        // n >= 32: all of lo and hi's bits below s are lost.
        const B lost = m_.lor(m_.ne(a.lo, k(0)), m_.ne(m_.band(a.hi, mask), k(0)));
        const W bigw{m_.bor(m_.shr(a.hi, s), flag(lost)), k(0)};
        return sel(big, bigw, smallw);
    }
    /// The highest set bit of a nonzero 64-bit value (0..63).
    U msb64(W a) {
        const B high = m_.ne(a.hi, k(0));
        return m_.sel(high, m_.add(m_.msb(a.hi), k(32)), m_.band(m_.msb(a.lo), k(63)));
    }

    M& m_;
};

}  // namespace vitreo::graphics::soft_float
