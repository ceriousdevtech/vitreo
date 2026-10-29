// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_math_exact.cpp
/// @brief Double-double refinement of libm's inverse-trig and tangent.
///
/// See crt_math_exact.hpp for why this exists. The structure is uniform:
/// take libm's result as a starting point that is already within a couple of
/// ULP, evaluate the *forward* function there to ~106 bits, and take one
/// Newton step. One step is enough because the starting point is that close;
/// the only precision that matters is in the residual.

#include "winapi/msvcrt/math/crt_math_exact.hpp"

#include "core/math/double_double.hpp"

#include <cmath>

namespace vitreo::winapi::exact {
namespace {

// ---------------------------------------------------------------------------
// Double-double arithmetic
//
// A DD represents hi + lo exactly, with lo below hi's last bit -- about 106
// bits of significand. two_prod is exact because fma computes a*b to infinite
// precision before the single rounding, so p + e == a*b with no error at all.
// ---------------------------------------------------------------------------
using namespace vitreo::math;   // DD and the trig kernels

/// atan of a double-double argument, |t| <= 1. Newton on f(y) = tan(y) - t,
/// whose derivative 1 + t^2 is well conditioned across the reduced range.
///
/// Takes and returns DD deliberately. The caller for |x| > 1 needs both: 1/x
/// is not representable as a double for most x (1/10 already is not), and
/// rounding the core result before subtracting it from pi/2 leaves ~0.05 ULP
/// of error in the answer -- enough to misround atan(2) and atan(10).
DD atan_core_dd(DD t) noexcept {
    const double y0 = std::atan(t.hi);
    if (!std::isfinite(y0)) return {y0, 0.0};
    DD s, c;
    dd_sincos(y0, s, c);
    if (!finite_dd(s) || !finite_dd(c) || c.hi == 0.0) return {y0, 0.0};
    const DD tan_y0 = dd_div(s, c);
    const DD err = dd_sub(tan_y0, t);
    const DD denom = dd_add({1.0, 0.0}, dd_mul(t, t));
    if (denom.hi == 0.0) return {y0, 0.0};
    const DD corr = dd_div(err, denom);
    if (!finite_dd(corr)) return {y0, 0.0};
    return dd_sub({y0, 0.0}, corr);
}

/// pi/2 as the nearest double-double (~107 bits).
inline DD pio2_dd() noexcept {
    return dd_add({kPio2Hi, 0.0}, {kPio2Mid, 0.0});
}

} // namespace

double asin(double x) noexcept {
    const double y0 = std::asin(x);
    if (!std::isfinite(y0) || !std::isfinite(x)) return y0;
    if (std::fabs(x) >= 1.0) return y0;
    DD s, c;
    dd_sincos(y0, s, c);
    if (!finite_dd(s) || !finite_dd(c)) return y0;
    // The Newton step divides by cos(y0), and sin(y0) - x cancels almost
    // completely as |x| -> 1. Propagating the DD's ~1.2e-32 absolute error
    // through that division stays under half an ULP of y0 as long as
    // |cos(y0)| > ~1e-8; below that libm is the better answer. The bound is
    // the derivative, not the argument -- an |x| > 0.999999 guard looks
    // equivalent and is roughly eight orders of magnitude too cautious.
    if (std::fabs(c.hi) < 1e-8) return y0;
    const DD err = dd_sub(s, {x, 0.0});
    const DD corr = dd_div(err, c);
    if (!finite_dd(corr)) return y0;
    return dd_to_double(dd_sub({y0, 0.0}, corr));
}

double acos(double x) noexcept {
    const double y0 = std::acos(x);
    if (!std::isfinite(y0) || !std::isfinite(x)) return y0;
    if (std::fabs(x) >= 1.0) return y0;
    DD s, c;
    dd_sincos(y0, s, c);
    if (!finite_dd(s) || !finite_dd(c)) return y0;
    // Same derivative bound as asin, mirrored: here the divisor is sin(y0).
    if (std::fabs(s.hi) < 1e-8) return y0;
    // f(y) = cos(y) - x, f'(y) = -sin(y)  =>  y1 = y0 + (cos(y0) - x)/sin(y0)
    const DD err = dd_sub(c, {x, 0.0});
    const DD corr = dd_div(err, s);
    if (!finite_dd(corr)) return y0;
    return dd_to_double(dd_add({y0, 0.0}, corr));
}

double atan(double x) noexcept {
    if (!std::isfinite(x)) return std::atan(x);
    const double ax = std::fabs(x);
    if (ax <= 1.0) {
        const DD r = atan_core_dd({x, 0.0});
        return finite_dd(r) ? dd_to_double(r) : std::atan(x);
    }
    // atan(x) = sign(x) * (pi/2 - atan(1/x)). Keeping the core argument inside
    // [-1, 1] avoids evaluating tan near pi/2, where the residual would lose
    // every bit it is meant to measure. 1/x is formed in DD because its
    // rounding error would otherwise land directly in the result.
    const DD inv = dd_div({1.0, 0.0}, {ax, 0.0});
    const DD core = atan_core_dd(inv);
    if (!finite_dd(core)) return std::atan(x);
    const DD r = dd_sub(pio2_dd(), core);
    if (!finite_dd(r)) return std::atan(x);
    const double v = dd_to_double(r);
    return x < 0.0 ? -v : v;
}

double tan(double x) noexcept {
    if (!std::isfinite(x)) return std::tan(x);
    // Past 2^20 the three-limb pi/2 no longer covers n, and libm's own
    // reduction is the accurate one -- hand it back.
    if (std::fabs(x) > 1048576.0) return std::tan(x);
    DD s, c;
    dd_sincos(x, s, c);
    if (!finite_dd(s) || !finite_dd(c) || c.hi == 0.0) return std::tan(x);
    const DD q = dd_div(s, c);
    if (!finite_dd(q)) return std::tan(x);
    return dd_to_double(q);
}

} // namespace vitreo::winapi::exact
