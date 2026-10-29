// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file crt_math_exact.hpp
/// @brief Correctly-rounded asin/acos/atan/tan for the guest CRT.
///
/// Measured against real Windows (tests/crt_oracle/crt_surface_runner) and
/// against 300-bit references: macOS's arm64 libm is 1 ULP off for these
/// functions at ordinary arguments -- acos(0.5), asin(0.5), atan(0.5),
/// atan(2), atan(10), tan(1), tan(10), tan(0.1) all differ from the correctly
/// rounded result, and Windows gets every one of them right. Passing libm's
/// answer through to the guest therefore diverges from Windows at exactly the
/// small angles games actually use.
///
/// These refine libm's result using double-double arithmetic (~106 bits) and
/// round once. They are *corrections*, not replacements: whenever the
/// refinement cannot be computed reliably -- a vanishing derivative, a
/// non-finite intermediate, an argument outside the reduction's valid range --
/// the libm result is returned unchanged, so the output is never worse than
/// the library's.
///
/// Deliberately NOT applied to sin/cos/tan at huge arguments. There Windows is
/// the inaccurate one (it does not do full Payne-Hanek reduction) and libm is
/// already correct; see the notes in crt_math.cpp.

namespace vitreo::winapi::exact {

/// Correctly-rounded asin. Falls back to std::asin near |x| == 1.
[[nodiscard]] double asin(double x) noexcept;

/// Correctly-rounded acos. Falls back to std::acos near |x| == 1.
[[nodiscard]] double acos(double x) noexcept;

/// Correctly-rounded atan over the whole finite range.
[[nodiscard]] double atan(double x) noexcept;

/// Correctly-rounded tan for |x| <= 2^20; larger arguments fall back to
/// std::tan, whose argument reduction is already the accurate one.
[[nodiscard]] double tan(double x) noexcept;

} // namespace vitreo::winapi::exact
