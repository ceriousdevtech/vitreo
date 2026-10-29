// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file x87_transcendental.hpp
/// @brief FSIN/FCOS/FPTAN semantics as the x87 hardware actually defines them.
///
/// x87 does NOT reduce modulo the real pi. It carries a 66-bit truncated
/// approximation, so its results drift from the true value as the argument
/// grows -- by design, and identically on every x86 part. Emulating these
/// instructions with the host's `std::sin` (which does a correct Payne-Hanek
/// reduction) is therefore wrong in the direction that looks *better*.
///
/// Verified against real Windows: a 66-bit pi reproduces the 32-bit CRT's
/// sin/cos/tan bit-for-bit at 1e8 and 123456.789, where the host libm answer
/// differs. Windows' 32-bit CRT routes these through x87.
///
/// The other half of the contract is the out-of-range rule: for |ST(0)| >= 2^63
/// the instruction computes nothing. It sets C2 and leaves ST(0) untouched,
/// leaving the caller to do its own reduction.

#include <vitreo/common.hpp>

namespace vitreo::math {

/// True when x87 would refuse to reduce and set C2 instead of computing.
[[nodiscard]] bool x87_trig_out_of_range(double x) noexcept;

/// sin/cos/tan using x87's truncated pi. Callers must check
/// x87_trig_out_of_range() first -- these assume it returned false.
[[nodiscard]] double x87_sin(double x) noexcept;
[[nodiscard]] double x87_cos(double x) noexcept;
[[nodiscard]] double x87_tan(double x) noexcept;

} // namespace vitreo::math
