// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file double_double.hpp
/// @brief Double-double (~106 bit) arithmetic and the trig kernels built on it.
///
/// Shared by two callers with different goals:
///   * the guest CRT, which wants *correctly rounded* results, because macOS's
///     arm64 libm is 1 ULP off for asin/acos/atan/tan at ordinary arguments
///     and real Windows is not;
///   * the x87 instruction emulation, which wants to reproduce what the
///     hardware actually does -- including its deliberately truncated pi.
///
/// Header-only and inline: these are small, hot, and want to be seen through.

#include <cmath>

namespace vitreo::math {

struct DD { double hi, lo; };

inline DD quick_two_sum(double a, double b) noexcept {
    const double s = a + b;
    return {s, b - (s - a)};        // requires |a| >= |b|
}
inline DD two_sum(double a, double b) noexcept {
    const double s = a + b;
    const double v = s - a;
    return {s, (a - (s - v)) + (b - v)};
}
inline DD two_prod(double a, double b) noexcept {
    const double p = a * b;
    return {p, std::fma(a, b, -p)};
}
inline DD dd_add(DD a, DD b) noexcept {
    DD s = two_sum(a.hi, b.hi);
    DD t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return quick_two_sum(s.hi, s.lo);
}
inline DD dd_sub(DD a, DD b) noexcept { return dd_add(a, {-b.hi, -b.lo}); }
inline DD dd_mul(DD a, DD b) noexcept {
    DD p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}
inline DD dd_div(DD a, DD b) noexcept {
    const double q1 = a.hi / b.hi;
    DD r = dd_sub(a, dd_mul(b, {q1, 0.0}));
    const double q2 = r.hi / b.hi;
    r = dd_sub(r, dd_mul(b, {q2, 0.0}));
    const double q3 = r.hi / b.hi;
    DD q = quick_two_sum(q1, q2);
    return dd_add(q, {q3, 0.0});
}
/// Collapse to the nearest double. hi and lo are non-overlapping, so their
/// sum rounds exactly once.
inline double dd_to_double(DD a) noexcept { return a.hi + a.lo; }

// pi/2 split across three doubles: ~159 bits. The third limb is what keeps
// the reduction honest once n gets into the hundreds.
constexpr double kPio2Hi  = 1.5707963267948966e+00;
constexpr double kPio2Mid = 6.123233995736766e-17;
constexpr double kPio2Lo  = -1.4973849048591698e-33;

// ---------------------------------------------------------------------------
// sin/cos on a reduced argument |r| <= pi/4, by Taylor series.
//
// Terms run to x^27 / x^28: at |r| <= 0.7854 the first omitted term is below
// 2^-112, comfortably under the ~106 bits a DD carries.
// ---------------------------------------------------------------------------
inline DD dd_sin_small(DD r) noexcept {
    // Building the series by repeated division by (2k+2)(2k+3) keeps every
    // constant an exactly representable integer. Precomputing 1/(2k+1)! as
    // plain doubles instead costs ~1e-18 on the leading term alone -- about
    // 0.005 ULP, which is small enough to look harmless and large enough to
    // misround a fraction of a percent of arguments.
    const DD r2 = dd_mul(r, r);
    DD term = r;
    DD sum = r;
    for (int k = 1; k <= 17; ++k) {
        const double d = static_cast<double>(2 * k) * static_cast<double>(2 * k + 1);
        term = dd_div(dd_mul(term, r2), {d, 0.0});
        term = {-term.hi, -term.lo};
        sum = dd_add(sum, term);
        if (std::fabs(term.hi) < 1e-40 * std::fabs(sum.hi)) break;
    }
    return sum;
}

inline DD dd_cos_small(DD r) noexcept {
    const DD r2 = dd_mul(r, r);
    DD term = {1.0, 0.0};
    DD sum = {1.0, 0.0};
    for (int k = 1; k <= 17; ++k) {
        const double d = static_cast<double>(2 * k - 1) * static_cast<double>(2 * k);
        term = dd_div(dd_mul(term, r2), {d, 0.0});
        term = {-term.hi, -term.lo};
        sum = dd_add(sum, term);
        if (std::fabs(term.hi) < 1e-40 * std::fabs(sum.hi)) break;
    }
    return sum;
}

/// Reduce x modulo pi/2. Returns the quadrant in @p quadrant and the residual
/// in [-pi/4, pi/4]. Valid while |x| stays small enough that the three-limb
/// pi/2 covers n's magnitude -- the callers all guard this.
inline DD reduce_pio2(double x, int& quadrant) noexcept {
    const double n = std::nearbyint(x / kPio2Hi);
    quadrant = static_cast<int>(static_cast<long long>(n) & 3);
    DD r = dd_sub({x, 0.0}, dd_mul({n, 0.0}, {kPio2Hi, 0.0}));
    r = dd_sub(r, dd_mul({n, 0.0}, {kPio2Mid, 0.0}));
    r = dd_sub(r, dd_mul({n, 0.0}, {kPio2Lo, 0.0}));
    return r;
}


/// sin and cos of an arbitrary moderate argument, to double-double.
inline void dd_sincos(double x, DD& s, DD& c) noexcept {
    int q = 0;
    const DD r = reduce_pio2(x, q);
    const DD sr = dd_sin_small(r);
    const DD cr = dd_cos_small(r);
    switch (q) {
        case 0: s = sr;               c = cr;               break;
        case 1: s = cr;               c = {-sr.hi, -sr.lo}; break;
        case 2: s = {-sr.hi, -sr.lo}; c = {-cr.hi, -cr.lo}; break;
        default: s = {-cr.hi, -cr.lo}; c = sr;              break;
    }
}

inline bool finite_dd(DD a) noexcept {
    return std::isfinite(a.hi) && std::isfinite(a.lo);
}

} // namespace vitreo::math
