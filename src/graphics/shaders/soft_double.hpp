// SPDX-License-Identifier: LGPL-2.1-only
/// @file soft_double.hpp
/// @brief IEEE-754 binary64 on 32-bit integer operations: the SM5
///        double-precision instructions (D3D11.3 22.14-22.17) for GPUs
///        without fp64 (every Apple GPU).
///
/// A double is its register pair: the low word in .x/.z and the high word in
/// .y/.w (D3D11.3 3.1.4). Every algorithm here is written once over an
/// abstract machine @c M so that the translator runs it to EMIT SPIR-V
/// (dxbc_to_spirv.cpp) and the unit tests run it on the host against the
/// CPU's own IEEE double arithmetic. The code is straight-line (every branch
/// is computed and selected) except for the one fixed-trip loop of the
/// divide, which @c M provides.
///
/// Semantics -- D3D11.3 3.1.4 (IEEE-754, round to nearest even, denormals
/// honoured on input and output) with the choices the spec leaves open taken
/// as MEASURED on WARP (tools/oracle_probe/d3d11_doubles_probe.expected):
///   - a NaN operand returns that NaN quieted (bit 51 set), the first NaN in
///     source order; an invalid operation (inf-inf, 0*inf, 0/0, inf/inf)
///     returns 0xFFF8000000000000. dfma evaluates the product's NaN rules
///     before the addend's (0*inf + NaN is the invalid NaN).
///   - dmin/dmax: a NaN operand yields the other operand, both NaN src0;
///     equal operands (+0/-0 included) yield src1 (the order dependence
///     3.1.3.2 permits).
///   - drcp is the correctly rounded 1/x (WARP's; the spec allows 1 ULP).
///   - dtof rounds to nearest even and produces float denormals; a NaN keeps
///     its sign and top payload bits, quieted. ftod is exact (float
///     denormals normalised), NaN payload shifted up, quieted.
///   - dtoi/dtou truncate, clamp to the integer range, NaN -> 0 (22.17.3/4).
///   - _sat on a double result: NaN -> +0, below zero -> +0 (-0 stays -0),
///     1.0 and above -> 1.0.
/// The 128-bit / rounding structure follows Berkeley SoftFloat 3
/// (roundPackToF64, f64_mul, f64_to_f32) restricted to round-to-nearest-even.
///
/// Machine interface (all values 32-bit unsigned @c U or boolean @c B):
///   U k(u32); U add(U,U); U sub(U,U); U band(U,U); U bor(U,U); U bxor(U,U);
///   U shl(U,U); U shr(U,U)            -- count in [0, 31] only (SPIR-V and
///                                        MSL leave larger counts undefined)
///   std::pair<U,U> mul_wide(U,U)      -- 32x32 product, (low, high)
///   U msb(U)                          -- highest set bit, 0xFFFFFFFF for 0
///   B ult(U,U); B slt(U,U); B eq(U,U); B ne(U,U);
///   B land(B,B); B lor(B,B); B lnot(B); U sel(B,U,U);
///   std::array<U,N> loop(u32 count, std::array<U,N> init, F body)
///                                     -- body(std::array<U,N>&) run count
///                                        times on the carried state

#pragma once

#include <vitreo/types.hpp>

#include <array>
#include <utility>

namespace vitreo::graphics::soft_double {

template <class M>
class SoftDouble {
public:
    using U = typename M::U;
    using B = typename M::B;
    /// 64 bits: a double, or a significand.
    struct W {
        U lo;
        U hi;
    };

    explicit SoftDouble(M& machine) : m_(machine) {}

    // -- 22.14 arithmetic ----------------------------------------------------

    W add(W a, W b) {
        const U sa = sign_of(a), sb = sign_of(b);
        // |x| >= |y|: the result takes x's sign unless it is an exact zero.
        const B swap = ult64(W{a.lo, abs_hi(a)}, W{b.lo, abs_hi(b)});
        const W x = sel(swap, b, a), y = sel(swap, a, b);
        const U sx = m_.sel(swap, sb, sa), sy = m_.sel(swap, sa, sb);
        const B same = m_.eq(sx, sy);
        const U ex = exp_of(x), ey = exp_of(y);
        const B dx = m_.eq(ex, k(0)), dy = m_.eq(ey, k(0));
        // Significands with the implicit bit at 62 (a denormal has none and
        // the exponent of the smallest normal).
        const W mx = shl64(with_implicit(x, dx), k(10));
        const W my = shl64(with_implicit(y, dy), k(10));
        const U ex1 = m_.sel(dx, k(1), ex), ey1 = m_.sel(dy, k(1), ey);
        const W yal = shr64_jam(my, m_.sub(ex1, ey1));
        const W r = sel(same, add64(mx, yal), sub64(mx, yal));
        // Normalise the MSB to bit 62: a carry into bit 63 shifts right one
        // (with jam), a cancellation shifts left.
        const B carry = m_.ne(m_.band(r.hi, k(0x80000000u)), k(0));
        const U lz = clz64(r);
        const W n = sel(carry, shr64_jam(r, k(1)),
                        shl64(r, m_.band(m_.sub(lz, k(1)), k(63))));
        const U e = m_.sel(carry, ex1, m_.sub(ex1, lz));
        W out = round_pack(sx, e, n);
        // x + (-x) is +0 in round-to-nearest; (-0) + (-0) is -0.
        out = sel(is_zero64(r), zero(m_.sel(same, sx, k(0))), out);
        const B infa = is_inf(a), infb = is_inf(b);
        out = sel(infb, b, out);
        out = sel(infa, sel(m_.land(infb, m_.ne(sa, sb)), default_nan(), a),
                  out);
        out = sel(is_nan(b), quiet(b), out);
        return sel(is_nan(a), quiet(a), out);
    }

    W mul(W a, W b) {
        const U sz = m_.bxor(sign_of(a), sign_of(b));
        const Norm na = normalize(a), nb = normalize(b);
        U ez = m_.sub(m_.add(na.exp, nb.exp), k(0x3FF));
        // (2^62..2^63) x (2^63..2^64): the high half has its MSB at 61/62.
        const Q p = mul128(shl64(na.sig, k(10)), shl64(nb.sig, k(11)));
        W s{m_.bor(p.w2, flag(m_.ne(m_.bor(p.w0, p.w1), k(0)))), p.w3};
        const B low = m_.ult(s.hi, k(0x40000000u));
        s = sel(low, shl64(s, k(1)), s);
        ez = m_.sel(low, m_.sub(ez, k(1)), ez);
        W out = round_pack(sz, ez, s);
        const B za = is_zero(a), zb = is_zero(b);
        const B zero_operand = m_.lor(za, zb);
        out = sel(zero_operand, zero(sz), out);
        out = sel(m_.lor(is_inf(a), is_inf(b)),
                  sel(zero_operand, default_nan(), inf(sz)), out);
        out = sel(is_nan(b), quiet(b), out);
        return sel(is_nan(a), quiet(a), out);
    }

    W div(W a, W b) {
        const U sz = m_.bxor(sign_of(a), sign_of(b));
        const Norm na = normalize(a), nb = normalize(b);
        U ez = m_.add(m_.sub(na.exp, nb.exp), k(0x3FE));
        // Dividend in [divisor, 2 * divisor): the quotient is in [1, 2).
        const B small = ult64(na.sig, nb.sig);
        const W dividend = sel(small, shl64(na.sig, k(1)), na.sig);
        ez = m_.sel(small, m_.sub(ez, k(1)), ez);
        // 55 quotient bits by restoring division: 53 significant, the
        // guard bit, and one more; the remainder is the sticky bit.
        const W divisor = nb.sig;
        const auto state = m_.loop(
            55u, std::array<U, 4>{k(0), k(0), dividend.lo, dividend.hi},
            [&](std::array<U, 4>& s) {
                const W q{s[0], s[1]};
                const W rem{s[2], s[3]};
                const B ge = m_.lnot(ult64(rem, divisor));
                const W left = sel(ge, sub64(rem, divisor), rem);
                const W shifted = shl1(q);
                s[0] = m_.bor(shifted.lo, flag(ge));
                s[1] = shifted.hi;
                const W next = shl1(left);
                s[2] = next.lo;
                s[3] = next.hi;
            });
        const W q{state[0], state[1]};
        const B inexact = m_.lnot(is_zero64(W{state[2], state[3]}));
        W s = shl64(q, k(8));
        s.lo = m_.bor(s.lo, flag(inexact));
        W out = round_pack(sz, ez, s);
        const B za = is_zero(a), zb = is_zero(b);
        const B infa = is_inf(a), infb = is_inf(b);
        out = sel(m_.lor(za, infb), zero(sz), out);
        out = sel(zb, sel(za, default_nan(), inf(sz)), out);
        out = sel(infa, sel(infb, default_nan(), inf(sz)), out);
        out = sel(is_nan(b), quiet(b), out);
        return sel(is_nan(a), quiet(a), out);
    }

    /// drcp: WARP returns the correctly rounded reciprocal (measured), which
    /// is inside 22.14.5's 1 ULP and exact when representable.
    W rcp(W a) { return div(W{k(0), k(0x3FF00000u)}, a); }

    /// dfma: a * b + c with a single rounding (22.14.7: 0.5 ULP for the
    /// fused operation).
    W fma(W a, W b, W c) {
        const U sp = m_.bxor(sign_of(a), sign_of(b));
        const U sc = sign_of(c);
        const Norm na = normalize(a), nb = normalize(b), nc = normalize(c);
        // Exact product: (2^62..2^63)^2, MSB at bit 124 or 125, scale
        // 2^(ea + eb - 2170). Addend: significand << 72, MSB at bit 124,
        // scale 2^(ec - 1147).
        const Q p = mul128(shl64(na.sig, k(10)), shl64(nb.sig, k(10)));
        const Q cq{k(0), k(0), m_.shl(nc.sig.lo, k(8)),
                   m_.bor(m_.shl(nc.sig.hi, k(8)),
                          m_.shr(nc.sig.lo, k(24)))};
        const B zc = is_zero(c);
        const U eab = m_.add(na.exp, nb.exp);
        // d = product scale - addend scale; a zero addend never moves the
        // product (its significand is 0 whatever d is).
        const U d = m_.sel(zc, k(200),
                           m_.sub(m_.sub(eab, nc.exp), k(1023)));
        const B dneg = m_.slt(d, k(0));
        const Q pa = sel(dneg, shr128_jam(p, m_.sub(k(0), d)), p);
        const Q ca = sel(dneg, cq, shr128_jam(cq, d));
        const U scale = m_.sel(dneg, m_.sub(nc.exp, k(1147)),
                               m_.sub(eab, k(2170)));
        const B same = m_.eq(sp, sc);
        const B p_ge = m_.lnot(ult128(pa, ca));
        const Q r = sel(same, add128(pa, ca),
                        sel(p_ge, sub128(pa, ca), sub128(ca, pa)));
        const U sr = m_.sel(m_.lor(same, p_ge), sp, sc);
        // MSB to bit 126, the high 64 bits (MSB at 62) jam the rest.
        const U lz = clz128(r);
        const Q rn = shl128(r, m_.band(m_.sub(lz, k(1)), k(127)));
        const W s{m_.bor(rn.w2, flag(m_.ne(m_.bor(rn.w0, rn.w1), k(0)))),
                  rn.w3};
        W out = round_pack(sr, m_.add(m_.sub(scale, lz), k(1149)), s);
        out = sel(is_zero128(r), zero(k(0)), out);
        const B zp = m_.lor(is_zero(a), is_zero(b));
        const B infp = m_.lor(is_inf(a), is_inf(b));
        const B infc = is_inf(c);
        // (+-0) + c: c, and +0 unless both zeros are negative.
        out = sel(zp, sel(zc, zero(m_.band(sp, sc)), c), out);
        out = sel(infc, c, out);
        out = sel(infp, sel(m_.land(infc, m_.ne(sp, sc)), default_nan(),
                            inf(sp)),
                  out);
        out = sel(is_nan(c), quiet(c), out);
        out = sel(m_.land(infp, zp), default_nan(), out);
        out = sel(is_nan(b), quiet(b), out);
        return sel(is_nan(a), quiet(a), out);
    }

    W min(W a, W b) {
        W out = sel(less(a, b), a, b);
        out = sel(is_nan(a), b, out);
        return sel(is_nan(b), a, out);
    }

    W max(W a, W b) {
        W out = sel(less(b, a), a, b);
        out = sel(is_nan(a), b, out);
        return sel(is_nan(b), a, out);
    }

    // -- 22.15 comparisons (NaN: false, dne true; +0 == -0) -------------------

    B eq(W a, W b) {
        return m_.land(m_.lnot(unordered(a, b)),
                       m_.lor(eq64(a, b), both_zero(a, b)));
    }
    B lt(W a, W b) { return m_.land(m_.lnot(unordered(a, b)), less(a, b)); }
    B ge(W a, W b) {
        return m_.land(m_.lnot(unordered(a, b)), m_.lnot(less(a, b)));
    }

    // -- 22.17 conversions ----------------------------------------------------

    /// dtof: round to nearest even (SoftFloat f64_to_f32/roundPackToF32).
    U to_float(W a) {
        const U sign = m_.band(a.hi, k(0x80000000u));
        const U e = exp_of(a);
        const U fhi = m_.band(a.hi, k(0x000FFFFFu));
        // The 52-bit fraction >> 22 with the lost bits jammed into bit 0.
        const U frac30 = m_.bor(
            m_.bor(m_.shl(fhi, k(10)), m_.shr(a.lo, k(22))),
            flag(m_.ne(m_.band(a.lo, k(0x003FFFFFu)), k(0))));
        const U exp = m_.sub(e, k(0x381));
        U sig = m_.bor(frac30, k(0x40000000u));
        const B tiny = m_.slt(exp, k(0));
        sig = m_.sel(tiny, shr32_jam(sig, m_.sub(k(0), exp)), sig);
        const U ef = m_.sel(tiny, k(0), exp);
        const U round_bits = m_.band(sig, k(0x7F));
        const U inc = m_.add(sig, k(0x40));
        const B overflow = m_.land(
            m_.lnot(tiny),
            m_.lor(m_.slt(k(0xFD), exp),
                   m_.land(m_.eq(exp, k(0xFD)),
                           m_.ne(m_.band(inc, k(0x80000000u)), k(0)))));
        U r = m_.shr(inc, k(7));
        r = m_.sel(m_.eq(round_bits, k(0x40)), m_.band(r, k(~1u)), r);
        const U packed = m_.bor(
            sign, m_.add(m_.shl(m_.sel(m_.eq(r, k(0)), k(0), ef), k(23)), r));
        U out = m_.sel(overflow, m_.bor(sign, k(0x7F800000u)), packed);
        // +-0 and double denormals: far below the float range.
        out = m_.sel(m_.eq(e, k(0)), sign, out);
        const U payload = m_.bor(m_.shl(fhi, k(3)), m_.shr(a.lo, k(29)));
        out = m_.sel(is_inf(a), m_.bor(sign, k(0x7F800000u)), out);
        return m_.sel(is_nan(a),
                      m_.bor(m_.bor(sign, k(0x7FC00000u)), payload), out);
    }

    /// ftod: exact; a float denormal is normalised, NaN quieted.
    W from_float(U f) {
        const U sign = m_.band(f, k(0x80000000u));
        const U e = m_.band(m_.shr(f, k(23)), k(0xFF));
        const U m = m_.band(f, k(0x007FFFFFu));
        const B denormal = m_.eq(e, k(0));
        // clz(m) - 8 moves the MSB of a denormal fraction to bit 23.
        const U shift = m_.band(m_.sub(clz32(m), k(8)), k(31));
        const U mn = m_.sel(denormal,
                            m_.band(m_.shl(m, shift), k(0x007FFFFFu)), m);
        const U en = m_.sel(denormal, m_.sub(k(1), shift), e);
        W out{m_.shl(mn, k(29)),
              m_.bor(sign, m_.bor(m_.shl(m_.add(en, k(896)), k(20)),
                                  m_.shr(mn, k(3))))};
        out = sel(m_.land(denormal, m_.eq(m, k(0))), W{k(0), sign}, out);
        const W special{m_.shl(m, k(29)),
                        m_.bor(m_.bor(sign, k(0x7FF00000u)),
                               m_.bor(m_.shr(m, k(3)),
                                      m_.sel(m_.ne(m, k(0)),
                                             k(0x00080000u), k(0))))};
        return sel(m_.eq(e, k(0xFF)), special, out);
    }

    /// dtoi: toward zero, clamped to [-2^31, 2^31 - 1], NaN -> 0.
    U to_int(W a) {
        const U e = exp_of(a);
        const B negative = m_.ne(sign_of(a), k(0));
        const U v = integer_part(a, e);
        U out = m_.sel(negative, m_.sub(k(0), v), v);
        out = m_.sel(m_.ult(e, k(1023)), k(0), out);
        out = m_.sel(m_.ult(k(1053), e),
                     m_.sel(negative, k(0x80000000u), k(0x7FFFFFFFu)), out);
        return m_.sel(is_nan(a), k(0), out);
    }

    /// dtou: toward zero, clamped to [0, 2^32 - 1], NaN -> 0.
    U to_uint(W a) {
        const U e = exp_of(a);
        U out = integer_part(a, e);
        out = m_.sel(m_.ult(e, k(1023)), k(0), out);
        out = m_.sel(m_.ult(k(1054), e), k(0xFFFFFFFFu), out);
        out = m_.sel(m_.ne(sign_of(a), k(0)), k(0), out);
        return m_.sel(is_nan(a), k(0), out);
    }

    W from_int(U i) {
        const B negative = m_.slt(i, k(0));
        W out = from_uint(m_.sel(negative, m_.sub(k(0), i), i));
        out.hi = m_.bor(out.hi, m_.sel(negative, k(0x80000000u), k(0)));
        return out;
    }

    W from_uint(U u) {
        const U top = m_.msb(u);
        const W m = shl64(W{u, k(0)}, m_.band(m_.sub(k(52), top), k(63)));
        const W out{m.lo, m_.bor(m_.shl(m_.add(k(1023), top), k(20)),
                                 m_.band(m.hi, k(0x000FFFFFu)))};
        return sel(m_.eq(u, k(0)), W{k(0), k(0)}, out);
    }

    // -- _sat ---------------------------------------------------------------

    /// A double result under _sat (measured on WARP for every double
    /// instruction): NaN -> +0, negative -> +0 but -0 stays, >= 1 -> 1.0.
    W saturate(W a) {
        const B negative = m_.ne(sign_of(a), k(0));
        const B negative_zero =
            m_.land(m_.eq(a.hi, k(0x80000000u)), m_.eq(a.lo, k(0)));
        W out = sel(m_.land(m_.lnot(negative),
                            m_.lnot(m_.ult(a.hi, k(0x3FF00000u)))),
                    W{k(0), k(0x3FF00000u)}, a);
        out = sel(m_.land(negative, m_.lnot(negative_zero)), W{k(0), k(0)},
                  out);
        return sel(is_nan(a), W{k(0), k(0)}, out);
    }

    /// dtof_sat: a float saturate, which flushes a denormal and -0 to +0
    /// (measured).
    U saturate_float(U f) {
        U out = m_.sel(m_.ult(f, k(0x3F800000u)), f, k(0x3F800000u));
        out = m_.sel(m_.ult(f, k(0x00800000u)), k(0), out);
        out = m_.sel(m_.ne(m_.band(f, k(0x80000000u)), k(0)), k(0), out);
        return m_.sel(
            m_.ult(k(0x7F800000u), m_.band(f, k(0x7FFFFFFFu))), k(0), out);
    }

private:
    struct Q {  // 128 bits, w0 least significant
        U w0, w1, w2, w3;
    };
    struct Norm {  // a finite value's significand (bit 52 set) and exponent
        W sig;
        U exp;
    };

    M& m_;

    U k(u32 v) { return m_.k(v); }
    U flag(B b) { return m_.sel(b, k(1), k(0)); }
    W sel(B c, W a, W b) { return W{m_.sel(c, a.lo, b.lo), m_.sel(c, a.hi, b.hi)}; }
    Q sel(B c, const Q& a, const Q& b) {
        return Q{m_.sel(c, a.w0, b.w0), m_.sel(c, a.w1, b.w1),
                 m_.sel(c, a.w2, b.w2), m_.sel(c, a.w3, b.w3)};
    }

    // -- fields and classes ---------------------------------------------------
    U sign_of(W a) { return m_.shr(a.hi, k(31)); }
    U exp_of(W a) { return m_.band(m_.shr(a.hi, k(20)), k(0x7FF)); }
    U abs_hi(W a) { return m_.band(a.hi, k(0x7FFFFFFFu)); }
    B is_nan(W a) {
        const U h = abs_hi(a);
        return m_.lor(m_.ult(k(0x7FF00000u), h),
                      m_.land(m_.eq(h, k(0x7FF00000u)), m_.ne(a.lo, k(0))));
    }
    B is_inf(W a) {
        return m_.land(m_.eq(abs_hi(a), k(0x7FF00000u)), m_.eq(a.lo, k(0)));
    }
    B is_zero(W a) { return m_.eq(m_.bor(abs_hi(a), a.lo), k(0)); }
    W quiet(W a) { return W{a.lo, m_.bor(a.hi, k(0x00080000u))}; }
    W default_nan() { return W{k(0), k(0xFFF80000u)}; }
    W inf(U sign) { return W{k(0), m_.bor(m_.shl(sign, k(31)), k(0x7FF00000u))}; }
    W zero(U sign) { return W{k(0), m_.shl(sign, k(31))}; }
    W with_implicit(W a, B denormal) {
        const U f = m_.band(a.hi, k(0x000FFFFFu));
        return W{a.lo, m_.sel(denormal, f, m_.bor(f, k(0x00100000u)))};
    }
    /// A finite operand's 53-bit significand (bit 52 set) and exponent; a
    /// denormal is shifted up with its exponent lowered below 1.
    Norm normalize(W a) {
        const U e = exp_of(a);
        const W f{a.lo, m_.band(a.hi, k(0x000FFFFFu))};
        const B denormal = m_.eq(e, k(0));
        const U shift = m_.sub(clz64(f), k(11));  // 53 for a zero
        return Norm{sel(denormal, shl64(f, shift),
                        W{f.lo, m_.bor(f.hi, k(0x00100000u))}),
                    m_.sel(denormal, m_.sub(k(1), shift), e)};
    }
    B unordered(W a, W b) { return m_.lor(is_nan(a), is_nan(b)); }
    B both_zero(W a, W b) { return m_.land(is_zero(a), is_zero(b)); }
    /// a < b for non-NaN operands, +0 == -0: the sign-magnitude order.
    B less(W a, W b) { return m_.land(m_.lnot(both_zero(a, b)), ult64(key(a), key(b))); }
    W key(W a) {
        const B negative = m_.ne(sign_of(a), k(0));
        return sel(negative, W{m_.bxor(a.lo, k(~0u)), m_.bxor(a.hi, k(~0u))},
                   W{a.lo, m_.bor(a.hi, k(0x80000000u))});
    }
    /// |a| truncated, for 1023 <= e <= 1054 (other exponents are clamped by
    /// the callers).
    U integer_part(W a, U e) {
        const W m{a.lo, m_.bor(m_.band(a.hi, k(0x000FFFFFu)), k(0x00100000u))};
        const U shift = m_.sub(k(1075), e);
        return shr64(m, m_.sel(m_.ult(shift, k(63)), shift, k(63))).lo;
    }

    /// SoftFloat roundPackToF64, round to nearest even: @p sig has its MSB
    /// at bit 62 (or below, for a result that is denormal), 10 rounding
    /// bits, value sig * 2^(exp - 1084); @p exp is signed.
    W round_pack(U sign, U exp, W sig) {
        const B tiny = m_.slt(exp, k(0));
        const W s = sel(tiny, shr64_jam(sig, m_.sub(k(0), exp)), sig);
        const U e = m_.sel(tiny, k(0), exp);
        const U round_bits = m_.band(s.lo, k(0x3FF));
        const W inc = add64(s, W{k(0x200), k(0)});
        const B overflow = m_.land(
            m_.lnot(tiny),
            m_.lor(m_.slt(k(0x7FD), exp),
                   m_.land(m_.eq(exp, k(0x7FD)),
                           m_.ne(m_.band(inc.hi, k(0x80000000u)), k(0)))));
        W r = shr64(inc, k(10));
        r.lo = m_.sel(m_.eq(round_bits, k(0x200)), m_.band(r.lo, k(~1u)), r.lo);
        const U e2 = m_.sel(is_zero64(r), k(0), e);
        const W packed{r.lo, m_.add(m_.add(m_.shl(sign, k(31)),
                                           m_.shl(e2, k(20))),
                                    r.hi)};
        return sel(overflow, inf(sign), packed);
    }

    // -- 32/64/128-bit integer helpers ----------------------------------------
    U clz32(U x) { return m_.sub(k(31), m_.msb(x)); }  // 32 for 0
    U clz64(W a) {
        return m_.sel(m_.ne(a.hi, k(0)), clz32(a.hi),
                      m_.add(k(32), clz32(a.lo)));
    }
    U clz128(const Q& a) {
        return m_.sel(m_.ne(a.w3, k(0)), clz32(a.w3),
               m_.sel(m_.ne(a.w2, k(0)), m_.add(k(32), clz32(a.w2)),
               m_.sel(m_.ne(a.w1, k(0)), m_.add(k(64), clz32(a.w1)),
                      m_.add(k(96), clz32(a.w0)))));
    }
    /// x >> (32 - n) and x << (32 - n) for n in [0, 31]; 0 when n == 0.
    U shr_complement(U x, U n) { return m_.shr(m_.shr(x, k(1)), m_.sub(k(31), n)); }
    U shl_complement(U x, U n) { return m_.shl(m_.shl(x, k(1)), m_.sub(k(31), n)); }
    U shr32_jam(U a, U n) {
        const U nc = m_.sel(m_.ult(n, k(31)), n, k(31));
        const U r = m_.shr(a, nc);
        return m_.bor(r, flag(m_.ne(m_.shl(r, nc), a)));
    }

    B is_zero64(W a) { return m_.eq(m_.bor(a.lo, a.hi), k(0)); }
    B eq64(W a, W b) { return m_.land(m_.eq(a.lo, b.lo), m_.eq(a.hi, b.hi)); }
    B ult64(W a, W b) {
        return m_.lor(m_.ult(a.hi, b.hi),
                      m_.land(m_.eq(a.hi, b.hi), m_.ult(a.lo, b.lo)));
    }
    W add64(W a, W b) {
        const U lo = m_.add(a.lo, b.lo);
        return W{lo, m_.add(m_.add(a.hi, b.hi), flag(m_.ult(lo, a.lo)))};
    }
    W sub64(W a, W b) {
        return W{m_.sub(a.lo, b.lo),
                 m_.sub(m_.sub(a.hi, b.hi), flag(m_.ult(a.lo, b.lo)))};
    }
    W shl1(W a) {
        return W{m_.shl(a.lo, k(1)),
                 m_.bor(m_.shl(a.hi, k(1)), m_.shr(a.lo, k(31)))};
    }
    /// n in [0, 63].
    W shl64(W a, U n) {
        const U n5 = m_.band(n, k(31));
        const B words = m_.ne(m_.band(n, k(32)), k(0));
        const U lo = m_.shl(a.lo, n5);
        const U hi = m_.bor(m_.shl(a.hi, n5), shr_complement(a.lo, n5));
        return W{m_.sel(words, k(0), lo), m_.sel(words, lo, hi)};
    }
    /// n in [0, 63].
    W shr64(W a, U n) {
        const U n5 = m_.band(n, k(31));
        const B words = m_.ne(m_.band(n, k(32)), k(0));
        const U hi = m_.shr(a.hi, n5);
        const U lo = m_.bor(m_.shr(a.lo, n5), shl_complement(a.hi, n5));
        return W{m_.sel(words, hi, lo), m_.sel(words, k(0), hi)};
    }
    /// Any unsigned n: the bits shifted out are ORed into bit 0 (SoftFloat
    /// shiftRightJam64; a count of 63 already leaves only that bit).
    W shr64_jam(W a, U n) {
        const U nc = m_.sel(m_.ult(n, k(63)), n, k(63));
        const W r = shr64(a, nc);
        return W{m_.bor(r.lo, flag(m_.lnot(eq64(shl64(r, nc), a)))), r.hi};
    }

    std::pair<U, U> adc(U a, U b, U carry) {
        const U s = m_.add(a, b);
        const U s2 = m_.add(s, carry);
        return {s2, m_.bor(flag(m_.ult(s, a)), flag(m_.ult(s2, s)))};
    }
    std::pair<U, U> sbb(U a, U b, U borrow) {
        const U d = m_.sub(a, b);
        const U d2 = m_.sub(d, borrow);
        return {d2, m_.bor(flag(m_.ult(a, b)), flag(m_.ult(d, borrow)))};
    }
    Q add128(const Q& a, const Q& b) {
        const auto [w0, c0] = adc(a.w0, b.w0, k(0));
        const auto [w1, c1] = adc(a.w1, b.w1, c0);
        const auto [w2, c2] = adc(a.w2, b.w2, c1);
        return Q{w0, w1, w2, m_.add(m_.add(a.w3, b.w3), c2)};
    }
    Q sub128(const Q& a, const Q& b) {
        const auto [w0, b0] = sbb(a.w0, b.w0, k(0));
        const auto [w1, b1] = sbb(a.w1, b.w1, b0);
        const auto [w2, b2] = sbb(a.w2, b.w2, b1);
        return Q{w0, w1, w2, m_.sub(m_.sub(a.w3, b.w3), b2)};
    }
    B ult128(const Q& a, const Q& b) {
        const W ah{a.w2, a.w3}, bh{b.w2, b.w3};
        return m_.lor(ult64(ah, bh),
                      m_.land(eq64(ah, bh),
                              ult64(W{a.w0, a.w1}, W{b.w0, b.w1})));
    }
    B eq128(const Q& a, const Q& b) {
        return m_.land(eq64(W{a.w0, a.w1}, W{b.w0, b.w1}),
                       eq64(W{a.w2, a.w3}, W{b.w2, b.w3}));
    }
    B is_zero128(const Q& a) {
        return m_.eq(m_.bor(m_.bor(a.w0, a.w1), m_.bor(a.w2, a.w3)), k(0));
    }
    Q mul128(W a, W b) {
        const auto [p00l, p00h] = m_.mul_wide(a.lo, b.lo);
        const auto [p01l, p01h] = m_.mul_wide(a.lo, b.hi);
        const auto [p10l, p10h] = m_.mul_wide(a.hi, b.lo);
        const auto [p11l, p11h] = m_.mul_wide(a.hi, b.hi);
        Q r{p00l, p00h, p11l, p11h};
        r = add128(r, Q{k(0), p01l, p01h, k(0)});
        return add128(r, Q{k(0), p10l, p10h, k(0)});
    }
    /// n in [0, 127].
    Q shl128(const Q& a, U n) {
        const U n5 = m_.band(n, k(31));
        const U words = m_.shr(n, k(5));
        const U t0 = m_.shl(a.w0, n5);
        const U t1 = m_.bor(m_.shl(a.w1, n5), shr_complement(a.w0, n5));
        const U t2 = m_.bor(m_.shl(a.w2, n5), shr_complement(a.w1, n5));
        const U t3 = m_.bor(m_.shl(a.w3, n5), shr_complement(a.w2, n5));
        const B e0 = m_.eq(words, k(0)), e1 = m_.eq(words, k(1)),
                e2 = m_.eq(words, k(2));
        const U z = k(0);
        return Q{m_.sel(e0, t0, z),
                 m_.sel(e0, t1, m_.sel(e1, t0, z)),
                 m_.sel(e0, t2, m_.sel(e1, t1, m_.sel(e2, t0, z))),
                 m_.sel(e0, t3, m_.sel(e1, t2, m_.sel(e2, t1, t0)))};
    }
    /// n in [0, 127].
    Q shr128(const Q& a, U n) {
        const U n5 = m_.band(n, k(31));
        const U words = m_.shr(n, k(5));
        const U t3 = m_.shr(a.w3, n5);
        const U t2 = m_.bor(m_.shr(a.w2, n5), shl_complement(a.w3, n5));
        const U t1 = m_.bor(m_.shr(a.w1, n5), shl_complement(a.w2, n5));
        const U t0 = m_.bor(m_.shr(a.w0, n5), shl_complement(a.w1, n5));
        const B e0 = m_.eq(words, k(0)), e1 = m_.eq(words, k(1)),
                e2 = m_.eq(words, k(2));
        const U z = k(0);
        return Q{m_.sel(e0, t0, m_.sel(e1, t1, m_.sel(e2, t2, t3))),
                 m_.sel(e0, t1, m_.sel(e1, t2, m_.sel(e2, t3, z))),
                 m_.sel(e0, t2, m_.sel(e1, t3, z)),
                 m_.sel(e0, t3, z)};
    }
    /// Any unsigned n (a count of 127 leaves only the jam bit for the
    /// values here, whose MSB is at most bit 126).
    Q shr128_jam(const Q& a, U n) {
        const U nc = m_.sel(m_.ult(n, k(127)), n, k(127));
        Q r = shr128(a, nc);
        r.w0 = m_.bor(r.w0, flag(m_.lnot(eq128(shl128(r, nc), a))));
        return r;
    }
};

} // namespace vitreo::graphics::soft_double
