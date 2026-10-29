// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file float80.hpp
/// @brief Correctly-rounded arithmetic on the x87 double-extended format.
///
/// x87 arithmetic is specified on an 80-bit format with a **64-bit explicit
/// significand**. Evaluating it in binary64 gives 53 bits, so every inexact
/// result loses the low 11 -- invisible on a single operation with tidy
/// operands, and cumulative once results feed back in. Block fuzzing against
/// the oracle shows it plainly: Vitreo returning `...14982800` where hardware
/// has `...14982b7a`.
///
/// This unit does the arithmetic in the architectural format. Values are
/// unpacked to (sign, exponent, significand), combined exactly into a 128-bit
/// intermediate, and rounded ONCE to the width precision control selects --
/// which is what makes it correct where computing in `double` and rounding
/// afterwards is not: that rounds twice.
///
/// Scope is the arithmetic Intel SDM Vol. 1 8.1.5.2 lists as PC-affected:
/// FADD/FSUB/FMUL/FDIV and FSQRT. The transcendentals are deliberately NOT
/// here -- hardware computes those with its own internal polynomial and the
/// SDM does not specify their exact results, so a "more correct" evaluation
/// would diverge from silicon rather than match it (see x87_transcendental.hpp
/// for the same argument applied to FSIN/FCOS/FPTAN).
///
/// The layout mirrors `vitreo::cpu::Float80` field for field, but this header
/// deliberately does not include it: core/math depends only on common.hpp, and
/// converting at the call site is two moves.

#include <vitreo/common.hpp>

namespace vitreo::math {

/// One x87 double-extended value: a 64-bit significand whose bit 63 is the
/// EXPLICIT integer bit, and a 15-bit biased exponent packed with the sign.
struct SoftF80 {
    u64 significand;
    u16 sign_exp;
};

/// x87 status-word bits this unit reports. Callers OR them into FSW.
inline constexpr u16 kF80Invalid   = 0x0001;  // IE
inline constexpr u16 kF80Denormal  = 0x0002;  // DE
inline constexpr u16 kF80DivByZero = 0x0004;  // ZE
inline constexpr u16 kF80Overflow  = 0x0008;  // OE
inline constexpr u16 kF80Underflow = 0x0010;  // UE
inline constexpr u16 kF80Precision = 0x0020;  // PE
inline constexpr u16 kF80C1        = 0x0200;  // rounding direction (round-up)

/// Each entry point takes the guest control word (for RC and PC) and ORs the
/// exceptions it raised into `*status`. `*status` is not cleared first, so a
/// caller can accumulate across a sequence. For an unmasked overflow or
/// underflow, register arithmetic returns Intel/AMD's exponent-scaled handler
/// value (true result divided/multiplied by 2^24576); masked completion keeps
/// the ordinary infinity/max-finite or gradual-underflow result.
SoftF80 f80_add(SoftF80 a, SoftF80 b, u16 control_word, u16* status) noexcept;
SoftF80 f80_sub(SoftF80 a, SoftF80 b, u16 control_word, u16* status) noexcept;
SoftF80 f80_mul(SoftF80 a, SoftF80 b, u16 control_word, u16* status) noexcept;
SoftF80 f80_div(SoftF80 a, SoftF80 b, u16 control_word, u16* status) noexcept;
SoftF80 f80_sqrt(SoftF80 a, u16 control_word, u16* status) noexcept;

/// Round a value to a destination format's significand width and normal
/// exponent range under the control word's rounding mode, delivering the exact
/// destination value still encoded as Float80.
///
/// This is what a STORE to m32/m64 needs. The narrowing is a rounding step in
/// its own right and obeys RC like any other -- FSTP m64 of an 80-bit 10/3
/// under round-toward-zero must truncate, not round to nearest. Rounding here
/// leaves the low bits clear and normalises destination subnormals into the
/// wider encoding, so the subsequent conversion to binary64 or binary32 is
/// exact.
SoftF80 f80_round_to_format(SoftF80 a, u16 control_word, int prec_bits,
                            int min_normal_exp, int max_normal_exp,
                            u16* status) noexcept;

}  // namespace vitreo::math
