// SPDX-License-Identifier: LGPL-2.1-only

/// @file x87_transcendental.cpp
/// @brief See x87_transcendental.hpp.

#include "core/math/x87_transcendental.hpp"
#include "core/math/double_double.hpp"

#include <cmath>

namespace vitreo::math {
namespace {

// x87's pi, truncated to 66 significant bits, halved. 66 bits needs exactly
// two doubles (53 + 13), so hi + lo represents it with no error at all --
// note lo's trailing zeros. Derived as floor(pi * 2^64) / 2^64 / 2.
constexpr double kX87Pio2Hi = 1.57079632679489655800e+00;
constexpr double kX87Pio2Lo = 6.12303176911188629106e-17;

/// Reduce x modulo x87's pi/2. Returns the quadrant and the residual.
DD x87_reduce(double x, int& quadrant) noexcept {
    const DD pio2 = dd_add({kX87Pio2Hi, 0.0}, {kX87Pio2Lo, 0.0});
    // The quotient is formed in double-double: at the top of the legal range
    // (just under 2^63) a plain double quotient is off by hundreds, which
    // would leave the residual hundreds of quadrants away from where it belongs.
    const double n = std::nearbyint(dd_to_double(dd_div({x, 0.0}, pio2)));
    quadrant = static_cast<int>(static_cast<long long>(n) & 3);
    // two_prod makes each partial product exact, so the (very heavy)
    // cancellation that follows removes only bits that are genuinely zero.
    const DD p1 = two_prod(n, kX87Pio2Hi);
    const DD p2 = two_prod(n, kX87Pio2Lo);
    DD r = dd_sub({x, 0.0}, p1);
    r = dd_sub(r, p2);
    return r;
}

void x87_sincos(double x, DD& s, DD& c) noexcept {
    int q = 0;
    const DD r = x87_reduce(x, q);
    const DD sr = dd_sin_small(r);
    const DD cr = dd_cos_small(r);
    switch (q) {
        case 0: s = sr;               c = cr;               break;
        case 1: s = cr;               c = {-sr.hi, -sr.lo}; break;
        case 2: s = {-sr.hi, -sr.lo}; c = {-cr.hi, -cr.lo}; break;
        default: s = {-cr.hi, -cr.lo}; c = sr;              break;
    }
}

DD dd_neg(DD a) noexcept { return {-a.hi, -a.lo}; }

// One of x87_sincos's two outputs, evaluating only the series that output
// takes in this quadrant -- the same selection as the table above, so the
// result is identical. FSIN/FCOS want one value; each series is ~17 DD
// divides.
DD x87_sin_only(double x) noexcept {
    int q = 0;
    const DD r = x87_reduce(x, q);
    switch (q) {
        case 0: return dd_sin_small(r);
        case 1: return dd_cos_small(r);
        case 2: return dd_neg(dd_sin_small(r));
        default: return dd_neg(dd_cos_small(r));
    }
}

DD x87_cos_only(double x) noexcept {
    int q = 0;
    const DD r = x87_reduce(x, q);
    switch (q) {
        case 0: return dd_cos_small(r);
        case 1: return dd_neg(dd_sin_small(r));
        case 2: return dd_neg(dd_cos_small(r));
        default: return dd_sin_small(r);
    }
}

} // namespace

bool x87_trig_out_of_range(double x) noexcept {
    // 2^63. NaN and infinity are out of range too -- they can never reduce.
    return !(std::fabs(x) < 9223372036854775808.0);
}

double x87_sin(double x) noexcept {
    if (!std::isfinite(x)) return std::sin(x);
    const DD s = x87_sin_only(x);
    return finite_dd(s) ? dd_to_double(s) : std::sin(x);
}

double x87_cos(double x) noexcept {
    if (!std::isfinite(x)) return std::cos(x);
    const DD c = x87_cos_only(x);
    return finite_dd(c) ? dd_to_double(c) : std::cos(x);
}

double x87_tan(double x) noexcept {
    if (!std::isfinite(x)) return std::tan(x);
    DD s, c;
    x87_sincos(x, s, c);
    if (!finite_dd(s) || !finite_dd(c) || c.hi == 0.0) return std::tan(x);
    const DD q = dd_div(s, c);
    return finite_dd(q) ? dd_to_double(q) : std::tan(x);
}

} // namespace vitreo::math
