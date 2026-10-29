// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"
#include "core/math/float80.hpp"
#include "core/math/host_fenv.hpp"
#include <bit>
#include <cmath>
#include <cstring>

namespace vitreo::cpu {

/// x87 FPU control word bits
enum FpuControlBits : u16 {
    FPU_CW_IM = 1 << 0,   // Invalid operation mask
    FPU_CW_DM = 1 << 1,   // Denormalized operand mask
    FPU_CW_ZM = 1 << 2,   // Zero divide mask
    FPU_CW_OM = 1 << 3,   // Overflow mask
    FPU_CW_UM = 1 << 4,   // Underflow mask
    FPU_CW_PM = 1 << 5,   // Precision mask
    FPU_CW_PC_MASK = 3 << 8,   // Precision control
    FPU_CW_RC_MASK = 3 << 10,  // Rounding control
    FPU_CW_X  = 1 << 12,  // Infinity control (legacy)
};

/// FPU precision control values
enum class FpuPrecision : u8 {
    Single = 0,     // 24 bits (float)
    Reserved = 1,
    Double = 2,     // 53 bits (double)
    Extended = 3,   // 64 bits (long double)
};

/// FPU rounding control values
enum class FpuRounding : u8 {
    Nearest = 0,    // Round to nearest (even)
    Down = 1,       // Round toward -infinity
    Up = 2,         // Round toward +infinity
    Truncate = 3,   // Round toward zero
};

/// x87 FPU status word bits
enum FpuStatusBits : u16 {
    FPU_SW_IE = 1 << 0,   // Invalid operation exception
    FPU_SW_DE = 1 << 1,   // Denormalized operand exception
    FPU_SW_ZE = 1 << 2,   // Zero divide exception
    FPU_SW_OE = 1 << 3,   // Overflow exception
    FPU_SW_UE = 1 << 4,   // Underflow exception
    FPU_SW_PE = 1 << 5,   // Precision exception
    FPU_SW_SF = 1 << 6,   // Stack fault
    FPU_SW_ES = 1 << 7,   // Exception summary
    FPU_SW_C0 = 1 << 8,   // Condition code 0
    FPU_SW_C1 = 1 << 9,   // Condition code 1
    FPU_SW_C2 = 1 << 10,  // Condition code 2
    FPU_SW_TOP_MASK = 7 << 11,  // Top of stack pointer
    FPU_SW_C3 = 1 << 14,  // Condition code 3
    FPU_SW_B  = 1 << 15,  // FPU busy
};

/// 80-bit extended precision floating point
/// On ARM64 we cannot use native long double (which is IEEE quad precision),
/// so we store the x87 format manually: 64-bit mantissa + 16-bit exponent/sign.
#pragma pack(push, 1)
struct Float80 {
    u64 mantissa;         // Explicit integer bit + 63-bit fraction
    u16 exponent_sign;    // 15-bit biased exponent + 1-bit sign
    
    constexpr Float80() noexcept : mantissa(0), exponent_sign(0) {}

    [[nodiscard]] bool is_nan() const noexcept {
        return (exponent_sign & 0x7FFFu) == 0x7FFFu &&
               mantissa != 0x8000000000000000ULL;
    }

    [[nodiscard]] bool is_signaling_nan() const noexcept {
        return is_nan() && (mantissa & 0x4000000000000000ULL) == 0;
    }

    /// True only for an x87 double-extended denormal operand. A zero has the
    /// same exponent field but no significand; pseudo-denormals have the
    /// explicit integer bit set and are not classified here.
    [[nodiscard]] bool is_denormal() const noexcept {
        return (exponent_sign & 0x7FFFu) == 0 && mantissa != 0 &&
               (mantissa & 0x8000000000000000ULL) == 0;
    }

    /// Construct an 80-bit x87 value from the explicit significand and the
    /// packed 15-bit biased exponent + sign. Used for FLD* hardware constants
    /// that are not exactly representable as IEEE 754 binary64.
    [[nodiscard]] static constexpr Float80 from_extended(
        u64 mantissa, u16 exponent_sign) noexcept {
        Float80 v;
        v.mantissa = mantissa;
        v.exponent_sign = exponent_sign;
        return v;
    }

    /// Intel SDM Vol. 2 FILD: every signed integer is exact in double-extended
    /// (64-bit explicit significand). Do not convert through binary64 — integers
    /// with |n| > 2^53 lose low bits there.
    [[nodiscard]] static Float80 from_i64(i64 value) noexcept {
        if (value == 0) {
            return Float80{};
        }
        const bool sign = value < 0;
        if (value == static_cast<i64>(0x8000000000000000ULL)) {
            return from_extended(0x8000000000000000ULL,
                                 static_cast<u16>(0x8000u | (16383 + 63)));
        }
        const u64 mag =
            sign ? static_cast<u64>(-value) : static_cast<u64>(value);
        const u16 top = static_cast<u16>(
            63u - static_cast<u16>(std::countl_zero(mag)));
        return from_extended(
            mag << (63 - top),
            static_cast<u16>((sign ? 0x8000u : 0u) |
                             static_cast<u16>(16383u + top)));
    }
    
    /// Convert from double (IEEE 754 binary64 to x87 extended)
    explicit Float80(double value) noexcept {
        // Extract IEEE 754 double components
        u64 bits;
        std::memcpy(&bits, &value, 8);
        
        bool sign = (bits >> 63) != 0;
        i32 exp = static_cast<i32>((bits >> 52) & 0x7FF);
        u64 frac = bits & 0x000FFFFFFFFFFFFFULL;
        
        if (exp == 0x7FF) {
            // Infinity or NaN
            exponent_sign = static_cast<u16>(0x7FFF | (sign ? 0x8000u : 0));
            mantissa = (frac == 0) ? 0x8000000000000000ULL : (0xC000000000000000ULL | (frac << 11));
            return;
        }
        
        if (exp == 0) {
            if (frac == 0) {
                mantissa = 0;
                exponent_sign = (sign ? 0x8000u : 0);
                return;
            }

            // Binary64 subnormals are normal numbers in x87 extended format.
            // Normalize the fraction into x87's explicit integer bit and
            // adjust the exponent to preserve the value exactly.
            const i32 top_bit = 63 - static_cast<i32>(std::countl_zero(frac));
            mantissa = frac << (63 - top_bit);
            const i32 new_exp = top_bit - 1074 + 16383;
            exponent_sign = static_cast<u16>(new_exp);
            if (sign) exponent_sign |= 0x8000u;
            return;
        }
        
        // Convert bias: double uses 1023, x87 uses 16383
        i32 new_exp = exp - 1023 + 16383;
        
        // x87 has explicit integer bit; double has implicit 1
        // Shift fraction and add explicit 1 bit
        mantissa = (1ULL << 63) | (frac << 11);
        exponent_sign = static_cast<u16>(new_exp);
        if (sign) exponent_sign |= 0x8000u;
    }
    
    /// Convert to double (may lose precision)
    [[nodiscard]] double to_double() const noexcept {
        bool sign = (exponent_sign & 0x8000u) != 0;
        i32 exp = exponent_sign & 0x7FFF;
        
        if (exp == 0x7FFF) {
            // Infinity or NaN
            u64 bits = sign ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
            if (mantissa != 0x8000000000000000ULL) {
                // NaN, not infinity. The payload has to come back with it:
                // Float80(double) stores it as `frac << 11`, so the inverse
                // is `>> 11`. Setting only the quiet bit -- as this did --
                // flattened every NaN to a bare 7FF8000000000000, so a guest
                // that inspects or forwards a payload saw a different value
                // than real silicon produces (float 7F800001 must widen to
                // 7FF8000020000000, not 7FF8000000000000).
                //
                // The quiet bit is still forced on, which also keeps the
                // significand non-zero so a payload that shifts out cannot
                // turn the NaN into an infinity.
                bits |= ((mantissa >> 11) & 0x000FFFFFFFFFFFFFULL);
                bits |= 0x0008000000000000ULL;  // quiet it
            }
            double result;
            std::memcpy(&result, &bits, 8);
            return result;
        }
        
        if (exp == 0 && mantissa == 0) {
            return sign ? -0.0 : 0.0;
        }
        
        // Convert bias back
        i32 new_exp = exp - 16383 + 1023;
        
        // Clamp to double range
        if (new_exp >= 0x7FF) {
            // Overflow to infinity
            u64 bits = sign ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
            double result;
            std::memcpy(&result, &bits, 8);
            return result;
        }
        if (new_exp <= 0) {
            // Values loaded from a binary64 subnormal are exactly
            // representable here. Convert the normalized x87 significand
            // back to binary64's denormalized fraction field.
            const i32 shift = 12 - new_exp;
            const u64 frac = shift < 64 ? (mantissa >> shift) : 0;
            const u64 bits = (static_cast<u64>(sign) << 63) | frac;
            double result;
            std::memcpy(&result, &bits, 8);
            return result;
        }
        
        // Narrow the 64-bit significand to binary64's 53, rounding to nearest
        // with ties to even.
        //
        // This used to truncate, which was invisible for as long as every
        // value in an ST register had come from a double and its low 11 bits
        // were already zero. Once the arithmetic is done in the architectural
        // 80-bit format (core/math/float80.hpp) those bits carry real
        // information, and dropping them makes FST/FSTP m64 report a result
        // one ULP low -- measured against the oracle as 0x...55 where hardware
        // stores 0x...56.
        u64 sig53 = mantissa >> 11;                 // integer bit now at 52
        const bool round_bit = ((mantissa >> 10) & 1) != 0;
        const bool sticky = (mantissa & 0x3FFULL) != 0;
        if (round_bit && (sticky || (sig53 & 1) != 0)) {
            ++sig53;
            if ((sig53 >> 53) != 0) {   // carried out of the significand
                sig53 >>= 1;
                ++new_exp;
                if (new_exp >= 0x7FF) {
                    const u64 inf = sign ? 0xFFF0000000000000ULL
                                         : 0x7FF0000000000000ULL;
                    double overflowed;
                    std::memcpy(&overflowed, &inf, 8);
                    return overflowed;
                }
            }
        }
        const u64 frac = sig53 & 0x000FFFFFFFFFFFFFULL;
        u64 bits = (static_cast<u64>(sign) << 63) |
                   (static_cast<u64>(new_exp) << 52) |
                   frac;
        
        double result;
        std::memcpy(&result, &bits, 8);
        return result;
    }
    
    /// Check if this is zero
    [[nodiscard]] bool is_zero() const noexcept {
        return mantissa == 0 && (exponent_sign & 0x7FFF) == 0;
    }

    /// Check if this is negative
    [[nodiscard]] bool is_negative() const noexcept {
        return (exponent_sign & 0x8000) != 0;
    }

    /// Truncate toward zero to a signed 64-bit integer, without going through
    /// IEEE 754 double (which would lose 11 bits of mantissa precision).
    /// Returns INT64_MIN on overflow / NaN (matches x86 "indefinite integer").
    [[nodiscard]] i64 to_i64_truncate() const noexcept {
        i32 exp = exponent_sign & 0x7FFF;
        bool sign = (exponent_sign & 0x8000) != 0;

        // Zero, denorms -> 0
        if (exp == 0) return 0;

        // NaN / Inf -> indefinite
        if (exp == 0x7FFF) return static_cast<i64>(0x8000000000000000ULL);

        // Unbiased exponent
        i32 e = exp - 16383;

        // |value| < 1 -> truncates to 0
        if (e < 0) return 0;

        // |value| >= 2^63 -> overflow (indefinite); note INT64_MIN = -2^63 is
        // the one legal value with e == 63 and sign==1 and mantissa == 2^63.
        if (e >= 63) {
            if (e == 63 && sign && mantissa == 0x8000000000000000ULL) {
                return static_cast<i64>(0x8000000000000000ULL);  // -2^63
            }
            return static_cast<i64>(0x8000000000000000ULL);  // overflow indefinite
        }

        // mantissa has explicit integer bit at position 63.
        // Truncated integer magnitude = mantissa >> (63 - e).
        u64 mag = mantissa >> (63 - e);
        return sign ? -static_cast<i64>(mag) : static_cast<i64>(mag);
    }
};
#pragma pack(pop)

static_assert(sizeof(Float80) == 10, "Float80 must be 10 bytes");

/// Intel SDM Vol. 1 §8.3.4 / Vol. 2 FLD1..FLDZ: with RC = round-to-nearest
/// (FNINIT control word 0x037F) these are the 8087/287 80-bit encodings.
/// Internal 66-bit constants are rounded to double-extended; #P is not raised.
/// Reserved upper half of every 16-bit field in the 32-bit protected-mode
/// FPU environment image (FNSTENV / FSTENV / FNSAVE, Intel SDM Vol. 1 Fig 8-9).
///
/// The 16-bit control/status/tag/selector fields sit in 32-bit slots and real
/// hardware leaves the unused upper halves as ALL ONES -- measured against
/// Intel silicon, which returns 0xFFFF at image offsets 2..3, 6..7, 10..11 and
/// 26..27. Zero-extending produces an image a guest can reload via
/// FLDENV/FRSTOR, so the error propagates rather than staying cosmetic.
///
/// Lives here because THREE places build this image: the interpreter
/// (ops/fpu.cpp), the JIT's native FNSTENV stencil (jit/ops/fpu.cpp) and the
/// JIT's FNSAVE helper (jit/jit_helpers.cpp).
inline constexpr u32 kFpuEnvReservedHigh = 0xFFFF0000u;

namespace x87_const {
/// The FLDcc constants, ROUNDED PER THE CONTROL WORD'S RC FIELD.
///
/// Intel SDM Vol. 1 8.3.8: the x87 constants are held internally with more
/// precision than the 64-bit significand can carry, and FLDPI/FLDL2T/FLDL2E/
/// FLDLG2/FLDLN2 round that value according to RC. Returning the
/// round-to-nearest form unconditionally is wrong for three of the four RC
/// settings, and it is a silent 1-ULP error: measured on real Intel silicon
/// with RC = round-toward-zero, hardware gives FLDPI ...C234 where
/// round-to-nearest is ...C235.
///
/// Every one of these is irrational, so there are always significand bits
/// beyond bit 0: truncation always loses something, and round-up is always
/// trunc+1. All five are positive, so round-toward-zero and round-down agree.
/// FLD1 and FLDZ are exact and unaffected.
struct RoundedConst {
    u64 trunc;      ///< round-toward-zero / round-down significand
    u64 nearest;    ///< round-to-nearest-even significand
    u16 exponent;   ///< biased exponent + sign (all constants positive)
};

inline constexpr RoundedConst kPi{
    0xC90FDAA22168C234ULL, 0xC90FDAA22168C235ULL, 0x4000};
inline constexpr RoundedConst kL2T{
    0xD49A784BCD1B8AFEULL, 0xD49A784BCD1B8AFEULL, 0x4000};
inline constexpr RoundedConst kL2E{
    0xB8AA3B295C17F0BBULL, 0xB8AA3B295C17F0BCULL, 0x3FFF};
inline constexpr RoundedConst kLg2{
    0x9A209A84FBCFF798ULL, 0x9A209A84FBCFF799ULL, 0x3FFD};
inline constexpr RoundedConst kLn2{
    0xB17217F7D1CF79ABULL, 0xB17217F7D1CF79ACULL, 0x3FFE};

/// Pick the significand RC selects. `control_word` is the live x87 FCW.
inline constexpr Float80 rounded(const RoundedConst& c,
                                 u16 control_word) noexcept {
    switch ((control_word >> 10) & 3u) {
        case 0:  return Float80::from_extended(c.nearest, c.exponent);
        case 1:  return Float80::from_extended(c.trunc, c.exponent);
        case 2:  return Float80::from_extended(c.trunc + 1u, c.exponent);
        default: return Float80::from_extended(c.trunc, c.exponent);
    }
}

// Exact — no significand bits are lost, so RC cannot change them.
inline constexpr Float80 fld1() noexcept {
    return Float80::from_extended(0x8000000000000000ULL, 0x3FFF);
}
inline constexpr Float80 fldz() noexcept {
    return Float80::from_extended(0, 0);
}

inline constexpr Float80 fldpi(u16 cw) noexcept  { return rounded(kPi, cw); }
inline constexpr Float80 fldl2t(u16 cw) noexcept { return rounded(kL2T, cw); }
inline constexpr Float80 fldl2e(u16 cw) noexcept { return rounded(kL2E, cw); }
inline constexpr Float80 fldlg2(u16 cw) noexcept { return rounded(kLg2, cw); }
inline constexpr Float80 fldln2(u16 cw) noexcept { return rounded(kLn2, cw); }
}  // namespace x87_const

/// x87 FPU tag values
enum class FpuTag : u8 {
    Valid = 0,      // Valid non-zero
    Zero = 1,       // Valid zero
    Special = 2,    // NaN, infinity, denormal
    Empty = 3,      // Empty slot
};

/// x87 FPU state
/// x86 NaN semantics for the x87 binary arithmetic operations.
///
/// Intel SDM Vol. 1 4.8.3.4 ("Operating on NaNs") and Table 4-7: when either
/// source is a NaN, the result is that NaN made quiet with its payload
/// intact, and when both are NaNs the one with the larger significand wins.
/// When neither source is a NaN but the operation is invalid (0/0, inf-inf,
/// 0*inf, inf/inf), the result is the QNaN floating-point indefinite, which
/// has its SIGN BIT SET (0xFFF8000000000000).
///
/// ARM disagrees on both counts: it produces a positive default QNaN and
/// discards the incoming payload. The JIT's native paths and this interpreter
/// both inherit host arithmetic, so they agreed with each other while both
/// differing from real silicon -- 165 of the 166 fp_oracle divergences
/// against the Windows box were this, spread evenly across PC=24/53/64
/// (the precision-control field is unrelated).
enum class X87Op : u8 { Add, Sub, Mul, Div };

[[nodiscard]] inline double x87_arith(double a, double b, X87Op op) noexcept {
    double result;
    switch (op) {
    case X87Op::Add: result = a + b; break;
    case X87Op::Sub: result = a - b; break;
    case X87Op::Mul: result = a * b; break;
    default:         result = a / b; break;
    }

    const auto to_bits = [](double d) noexcept {
        u64 v; std::memcpy(&v, &d, sizeof(v)); return v;
    };
    const auto from_bits = [](u64 v) noexcept {
        double d; std::memcpy(&d, &v, sizeof(d)); return d;
    };
    constexpr u64 kSignificand = 0x000F'FFFF'FFFF'FFFFull;
    constexpr u64 kQuietBit    = 1ull << 51;
    const auto is_nan = [&](u64 v) noexcept {
        return ((v >> 52) & 0x7FFull) == 0x7FFull && (v & kSignificand) != 0;
    };

    const u64 ba = to_bits(a);
    const u64 bb = to_bits(b);
    const bool na = is_nan(ba);
    const bool nb = is_nan(bb);
    if (na || nb) {
        u64 pick;
        if (na && nb) {
            pick = (bb & kSignificand) > (ba & kSignificand) ? bb : ba;
        } else {
            pick = na ? ba : bb;
        }
        return from_bits(pick | kQuietBit);   // quiet, keeping payload + sign
    }
    if (is_nan(to_bits(result))) {
        return from_bits(0xFFF8'0000'0000'0000ull);   // real indefinite
    }
    return result;
}

struct FpuState {
    // FPU register stack (ST0-ST7) - 80 bits each, padded to 16 bytes
    alignas(16) Float80 st[8];
    u8 _padding[8 * 6];  // Padding to 16 bytes each
    
    u16 control_word;    // FCW
    u16 status_word;     // FSW
    u16 tag_word;        // FTW (2 bits per register)
    u16 fop;             // Last FPU opcode
    
    u64 fip;             // FPU instruction pointer
    u64 fdp;             // FPU data pointer
    
    constexpr FpuState() noexcept
        : st{}, _padding{}, control_word(0x037F), status_word(0),
          tag_word(0xFFFF), fop(0), fip(0), fdp(0) {}
    
    /// Get the current stack top index (0-7)
    [[nodiscard]] u8 top() const noexcept {
        return (status_word >> 11) & 7;
    }
    
    /// Set the stack top index
    void set_top(u8 value) noexcept {
        status_word = static_cast<u16>((status_word & ~FPU_SW_TOP_MASK) | ((value & 7) << 11));
    }
    
    /// Get physical register index for ST(i)
    [[nodiscard]] u8 st_index(u8 i) const noexcept {
        return (top() + i) & 7;
    }
    
    /// Get ST(i) value
    [[nodiscard]] const Float80& get_st(u8 i) const noexcept {
        return st[st_index(i)];
    }
    
    /// Set ST(i) value
    void set_st(u8 i, Float80 value) noexcept {
        // Intel SDM Vol. 1, 8.4: an unmasked invalid-operation or
        // divide-by-zero exception suppresses the primary register result.
        // OE/UE/PE deliberately do not participate here: register-destination
        // forms deliver their rounded/scaled result for those classes.
        if (has_unmasked_exception(FPU_SW_IE | FPU_SW_DE | FPU_SW_ZE)) return;
        st[st_index(i)] = value;
    }

    [[nodiscard]] bool has_unmasked_exception(u16 exceptions) const noexcept {
        const u16 raised = static_cast<u16>(status_word & exceptions & 0x3Fu);
        return (raised & static_cast<u16>(~control_word)) != 0;
    }

    /// Recompute the architectural exception-summary state after changing
    /// the control word. Intel SDM Vol. 2, FLDCW: exposing an already-set
    /// sticky exception makes it pending for the next waiting x87
    /// instruction; FLDCW itself does not raise #MF.
    void refresh_exception_summary() noexcept {
        const u16 unmasked = static_cast<u16>(
            (status_word & 0x3Fu) & static_cast<u16>(~control_word));
        if (unmasked != 0) {
            status_word = static_cast<u16>(status_word | FPU_SW_ES | FPU_SW_B);
        } else {
            status_word = static_cast<u16>(status_word & ~(FPU_SW_ES | FPU_SW_B));
        }
    }

    /// Record sticky x87 exception flags and keep ES/B as the architectural
    /// summary of the currently unmasked set. Exception delivery remains the
    /// responsibility of the next waiting instruction.
    void record_exceptions(u16 exceptions) noexcept {
        status_word = static_cast<u16>(status_word | (exceptions & 0x3Fu));
        const u16 unmasked = static_cast<u16>(
            (status_word & 0x3Fu) & static_cast<u16>(~control_word));
        if (unmasked != 0) {
            status_word = static_cast<u16>(status_word | FPU_SW_ES | FPU_SW_B);
        }
    }

    /// Widen an IEEE binary32/binary64 memory source while retaining the
    /// source-format classification that Float80(value) necessarily loses.
    /// Intel/AMD raise #D when an x87 arithmetic/compare consumes a
    /// denormalized source and also when FLD loads denormal binary32/64 (but
    /// not merely because a widened normal result is tiny). Callers then use
    /// the ordinary push/set/pop commit gates for the unmasked case.
    [[nodiscard]] Float80 prepare_ieee_source(float value) noexcept {
        const u32 bits = std::bit_cast<u32>(value);
        if ((bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0) {
            record_exceptions(FPU_SW_DE);
        }
        return Float80(static_cast<double>(value));
    }

    [[nodiscard]] Float80 prepare_ieee_source(double value) noexcept {
        const u64 bits = std::bit_cast<u64>(value);
        if ((bits & 0x7FF0000000000000ULL) == 0 &&
            (bits & 0x000FFFFFFFFFFFFFULL) != 0) {
            record_exceptions(FPU_SW_DE);
        }
        return Float80(value);
    }

    /// The interpreter widens m32 operands to binary64 in the integer domain
    /// so signaling-NaN payloads survive. Carry the original binary32
    /// denormal classification alongside that widened value; inspecting the
    /// binary64 encoding here would miss #D because every binary32 subnormal
    /// is normal after widening.
    [[nodiscard]] Float80 prepare_binary32_source(
        double widened, bool source_was_denormal) noexcept {
        if (source_was_denormal) record_exceptions(FPU_SW_DE);
        return Float80(widened);
    }

    /// Record the invalid-operation side of an x87 comparison. Ordered
    /// FCOM-family operations raise #IA for any NaN; quiet FUCOM-family
    /// operations raise it only for SNaN. False means the destination flags
    /// must remain unchanged because #IA is unmasked.
    [[nodiscard]] bool prepare_compare(Float80 a, Float80 b,
                                       bool quiet) noexcept {
        // A binary32/64 memory source may already have raised unmasked #D
        // while it was widened by prepare_ieee_source(). Do not evaluate a
        // later comparison class or alter condition codes in that case.
        if (has_unmasked_exception(FPU_SW_DE)) return false;
        const bool invalid = quiet
            ? (a.is_signaling_nan() || b.is_signaling_nan())
            : (a.is_nan() || b.is_nan());
        if (invalid) record_exceptions(FPU_SW_IE);
        if (a.is_denormal() || b.is_denormal()) {
            record_exceptions(FPU_SW_DE);
        }
        return !has_unmasked_exception(FPU_SW_IE | FPU_SW_DE);
    }

    /// Set C3/C2/C0 for an x87 status-word comparison.
    [[nodiscard]] bool compare(Float80 a, Float80 b,
                               bool quiet) noexcept {
        if (!prepare_compare(a, b, quiet)) return false;
        const bool unordered = a.is_nan() || b.is_nan();

        status_word = static_cast<u16>(
            status_word & ~(FPU_SW_C0 | FPU_SW_C2 | FPU_SW_C3));
        if (unordered) {
            status_word = static_cast<u16>(
                status_word | FPU_SW_C0 | FPU_SW_C2 | FPU_SW_C3);
        } else {
            const double da = a.to_double();
            const double db = b.to_double();
            if (da < db) status_word = static_cast<u16>(status_word | FPU_SW_C0);
            else if (da == db) status_word = static_cast<u16>(status_word | FPU_SW_C3);
        }
        return true;
    }
    
    /// Get tag for register
    [[nodiscard]] FpuTag get_tag(u8 reg) const noexcept {
        return static_cast<FpuTag>((tag_word >> (reg * 2)) & 3);
    }
    
    /// Set tag for register
    void set_tag(u8 reg, FpuTag tag) noexcept {
        u16 shift = static_cast<u16>(reg * 2);
        u16 mask = static_cast<u16>(3 << shift);
        tag_word = static_cast<u16>((tag_word & ~mask) | (static_cast<u16>(tag) << shift));
    }
    
    /// Push value onto FPU stack. Returns false when an unmasked stack
    /// overflow leaves the push pending for later #MF delivery.
    bool push(Float80 value) noexcept {
        // A source fault earlier in the same instruction suppresses a later
        // push (for example FLD ST(i) with an empty source).
        if (has_unmasked_exception(FPU_SW_IE | FPU_SW_DE | FPU_SW_ZE)) return false;
        const u8 destination = static_cast<u8>((top() - 1) & 7);
        // Intel SDM Vol. 1 section 8.5.1.1: pushing onto a register whose tag
        // is not Empty is stack overflow (#IS). With #I masked, TOP advances
        // and the destination receives the real indefinite QNaN. With #I
        // unmasked, TOP, tags, and register contents remain unchanged while
        // ES/B record the pending exception.
        //
        // Measured on the `Windows` host 2026-08-30: nine consecutive FLD1
        // from an empty stack end with FSW=0x3A41 (IE | SF | C1, TOP=7),
        // FTW=0xFF and ST0 = 0xFFFFC000000000000000.
        if (get_tag(destination) != FpuTag::Empty) {
            raise_stack_overflow();
            if ((control_word & 1u) == 0) {
                status_word = static_cast<u16>(status_word | FPU_SW_ES | FPU_SW_B);
                return false;
            }
            set_top(destination);
            value = indefinite();
            st[top()] = value;
            set_tag(top(), FpuTag::Special);
            return true;
        }
        set_top(destination);
        st[top()] = value;
        set_tag(top(), value.is_zero() ? FpuTag::Zero : FpuTag::Valid);
        return true;
    }

    /// Commit a two-result x87 operation atomically with respect to an
    /// unmasked stack-overflow fault. FPTAN, FSINCOS and FXTRACT first replace
    /// ST(0) and then push their second result; Intel requires the original
    /// TOP, tags and ST values to survive when that push raises unmasked #IS.
    bool replace_st0_and_push(Float80 replacement,
                              Float80 pushed) noexcept {
        const u8 old_top = top();
        const Float80 old_st0 = st[old_top];
        const u16 old_tags = tag_word;
        const u8 push_destination = static_cast<u8>((old_top - 1) & 7);
        const bool masked_overflow =
            get_tag(push_destination) != FpuTag::Empty &&
            (control_word & FPU_CW_IM) != 0;
        set_st(0, replacement);
        if (push(pushed)) {
            if (masked_overflow) {
                // Windows/Intel masked #IS completion for these two-result
                // operations writes indefinite to both results: the pushed
                // ST0 and the replaced value now visible as ST1.
                st[old_top] = indefinite();
                set_tag(old_top, FpuTag::Special);
            }
            return true;
        }
        // push() made no stack mutation for an unmasked #IS, but the first
        // result was already staged. Roll it back while retaining FSW flags.
        st[old_top] = old_st0;
        tag_word = old_tags;
        set_top(old_top);
        return false;
    }

    /// The x87 "real indefinite": the default QNaN with the sign bit set. The
    /// masked result of every invalid-operation exception, stack faults
    /// included.
    [[nodiscard]] static Float80 indefinite() noexcept {
        return Float80::from_extended(0xC000000000000000ull, 0xFFFFu);
    }

    /// Stack UNDERFLOW: an instruction sourced a register marked Empty.
    /// C1 is CLEARED, which is how a reader tells underflow from overflow.
    void raise_stack_underflow() noexcept {
        status_word = static_cast<u16>(
            (status_word | FPU_SW_IE | FPU_SW_SF) & ~FPU_SW_C1);
        record_exceptions(FPU_SW_IE);
    }

    /// Stack OVERFLOW: a push landed on a register that was not Empty.
    /// C1 is SET.
    void raise_stack_overflow() noexcept {
        status_word = static_cast<u16>(
            status_word | FPU_SW_IE | FPU_SW_SF | FPU_SW_C1);
        record_exceptions(FPU_SW_IE);
    }

    /// Read ST(i) as an instruction SOURCE.
    ///
    /// Distinct from get_st(), which is the raw accessor used for state
    /// marshalling (FXSAVE, context capture) and must never fault. Every
    /// architectural source read goes through here so an empty slot raises
    /// #IS and yields the indefinite QNaN, which then propagates through the
    /// arithmetic by the ordinary NaN rules.
    [[nodiscard]] Float80 read_src(u8 i) noexcept {
        if (get_tag(st_index(i)) == FpuTag::Empty) {
            raise_stack_underflow();
            return indefinite();
        }
        return get_st(i);
    }

    /// Checked source read of ST(0) followed by the pop. The fault comes from
    /// the READ; the stack still moves, exactly as hardware does -- an FSTP of
    /// an empty ST(0) sets IE|SF and TOP still advances.
    [[nodiscard]] Float80 pop_src() noexcept {
        const Float80 v = read_src(0);
        pop();
        return v;
    }
    
    /// Pop value from FPU stack
    Float80 pop() noexcept {
        Float80 value = st[top()];
        if (has_unmasked_exception(FPU_SW_IE | FPU_SW_DE | FPU_SW_ZE)) return value;
        set_tag(top(), FpuTag::Empty);
        set_top((top() + 1) & 7);
        return value;
    }
    
    /// Get rounding control
    [[nodiscard]] FpuRounding rounding() const noexcept {
        return static_cast<FpuRounding>((control_word >> 10) & 3);
    }
    
    /// Get precision control
    [[nodiscard]] FpuPrecision precision() const noexcept {
        return static_cast<FpuPrecision>((control_word >> 8) & 3);
    }

    /// Round an arithmetic result to the significand width selected by the
    /// precision-control field.
    ///
    /// Intel SDM Vol. 1 8.1.5.2 lists exactly which instructions PC affects:
    /// FADD/FSUB/FMUL/FDIV (with their P and I variants) and FSQRT. It does
    /// NOT affect loads, stores, FRNDINT, or the transcendentals, so this must
    /// be applied at those arithmetic sites rather than inside `set_st`.
    ///
    /// This matters far beyond the odd low bit: `IDirect3D9::CreateDevice`
    /// without D3DCREATE_FPU_PRESERVE switches the x87 unit to single
    /// precision, so a D3D9 game runs its whole simulation under PC=24 and
    /// every result it computes is rounded to a 24-bit significand.
    ///
    /// Results are computed in binary64, so Double and Extended are already
    /// at or below the requested width and pass through. Extended therefore
    /// still yields 53 bits rather than the architectural 64 — a pre-existing
    /// limitation of computing in `double`, unchanged by this rounding step.
    [[nodiscard]] double round_result_to_precision(double value) const noexcept {
        if (precision() != FpuPrecision::Single) {
            return value;
        }
        // PC narrows the *significand* only; the exponent keeps its full
        // range. Converting through `float` would be wrong twice over: it
        // clamps the exponent (1.0 / FLT_MIN_DENORM overflows to infinity
        // where hardware yields a finite 2^149) and it flushes results that
        // are merely subnormal as a float. So round the binary64 fraction
        // from 52 bits to 23 in place, leaving the exponent alone.
        u64 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const u64 exponent = (bits >> 52) & 0x7FF;
        // Inf/NaN carry no roundable significand; a subnormal binary64 is far
        // below the range PC applies to and is left exactly as computed.
        if (exponent == 0x7FF || exponent == 0) {
            return value;
        }
        constexpr int kDroppedBits = 52 - 23;
        constexpr u64 kDropMask = (1ULL << kDroppedBits) - 1;
        const u64 dropped = bits & kDropMask;
        const u64 kept = bits & ~kDropMask;
        constexpr u64 kHalf = 1ULL << (kDroppedBits - 1);
        // Round to nearest, ties to even. A carry out of the fraction flows
        // into the exponent field on its own, which is the behaviour wanted.
        // Rounding must follow RC, not always round-to-nearest. Measured
        // against silicon: with RC = round-down and PC = 24, hardware
        // truncates where nearest-even would round up, giving a 1-ULP
        // difference in the significand AND the opposite C1.
        //   RC 00 nearest-even, 01 toward -inf, 10 toward +inf, 11 toward 0.
        // The bit pattern is sign-magnitude, so "toward -inf" means step away
        // from zero for negatives and truncate for positives (and vice versa).
        const bool negative = (bits >> 63) != 0;
        bool step_up = false;   // increase magnitude by one ULP
        switch ((control_word >> 10) & 3u) {
            case 0:
                step_up = dropped > kHalf ||
                          (dropped == kHalf &&
                           ((kept >> kDroppedBits) & 1) != 0);
                break;
            case 1:  step_up = (dropped != 0) && negative;  break;
            case 2:  step_up = (dropped != 0) && !negative; break;
            default: step_up = false;                       break;
        }
        u64 rounded = kept;
        if (step_up) {
            rounded += (1ULL << kDroppedBits);
        }
        double result;
        std::memcpy(&result, &rounded, sizeof(result));
        return result;
    }
    
    /// Reset to initial state (FINIT)
    /// Force the reserved bits of a loaded x87 control word the way hardware
    /// does, so FNSTCW reads back what silicon would report.
    ///
    /// Bit 6 reads back SET, bit 7 and bits 13-15 read back CLEAR; bits 8-12
    /// (PC, RC and the 287 infinity-control bit) are kept. Storing the raw
    /// 16 bits instead leaves a control word no real CPU can produce, which a
    /// guest sees the moment it does FLDCW/FNSTCW round-tripping.
    ///
    /// Measured on real silicon (the `Windows` host, 2026-08-30) rather than
    /// read off a table, because the reserved-bit behaviour is not uniform:
    ///
    ///   FLDCW 0xFFFF -> FNSTCW 0x1F7F      FLDCW 0x84FF -> 0x047F
    ///   FLDCW 0x0000 -> FNSTCW 0x0040      FLDCW 0xE0A6 -> 0x0066
    ///
    /// All four are exactly (cw & 0x1F3F) | 0x0040.
    static constexpr u16 kControlWordKeepMask = 0x1F3Fu;
    static constexpr u16 kControlWordForcedBits = 0x0040u;
    [[nodiscard]] static constexpr u16 sanitize_control_word(u16 cw) noexcept {
        return static_cast<u16>((cw & kControlWordKeepMask) |
                                kControlWordForcedBits);
    }

    void reset() noexcept {
        control_word = 0x037F;
        status_word = 0;
        tag_word = 0xFFFF;  // All empty
        fop = 0;
        fip = 0;
        fdp = 0;
    }
    
    // ========================================================================
    // MMX Register Access
    // MMX registers (MM0-MM7) share the low 64 bits of the FPU registers.
    // Unlike FPU, MMX uses absolute register indices (0-7), not stack-relative.
    // Using MMX instructions invalidates all FPU register contents.
    // ========================================================================
    
    /// Get MM register (direct index, not stack-relative)
    [[nodiscard]] u64 get_mm(u8 reg) const noexcept {
        return st[reg & 7].mantissa;
    }
    
    /// Set MM register (direct index, not stack-relative)
    /// Also marks the register as used (tag = empty, exponent_sign = 0xFFFF)
    void set_mm(u8 reg, u64 value) noexcept {
        reg &= 7;
        st[reg].mantissa = value;
        // When writing MMX, set top 16 bits to all 1s (marks as "in use" for MMX)
        st[reg].exponent_sign = 0xFFFFu;
    }
    
    /// Round a result to the precision RC/PC selects and signal #P if that
    /// rounding lost anything.
    ///
    /// Precision control is architecturally visible twice over: in the stored
    /// significand AND in the status word. Rounding the value without raising
    /// #P leaves a result that looks right but whose FSW does not, which is
    /// what FIMUL/FIADD m32int showed against silicon under PC=24: significand
    /// low bits kept, FSW 0x3800 where hardware gives 0x3a20 (PE|C1).
    [[nodiscard]] double round_and_signal_precision(double value) noexcept {
        const double rounded = round_result_to_precision(value);
        if (rounded != value) {
            record_exceptions(FPU_SW_PE);
            // C1 is the round-up indicator whenever #P is raised, and "up"
            // means the MAGNITUDE grew -- not that the signed value did.
            // Measured: a negative product rounded away from zero comes back
            // with C1 SET on silicon, though its signed value decreased.
            const double abs_rounded = rounded < 0.0 ? -rounded : rounded;
            const double abs_value = value < 0.0 ? -value : value;
            if (abs_rounded > abs_value) {
                status_word = static_cast<u16>(status_word | FPU_SW_C1);
            } else {
                status_word = static_cast<u16>(status_word & ~FPU_SW_C1);
            }
        }
        return rounded;
    }

    /// Perform an x87 binary arithmetic operation and record its flags.
    ///
    /// THE single entry point for FADD/FSUB/FMUL/FDIV, shared by the
    /// interpreter and the JIT's helper path so the two cannot drift.
    ///
    /// Under PC = 64 (extended) the arithmetic is done in the architectural
    /// 80-bit format by core/math/float80.hpp. Evaluating it in binary64
    /// instead delivers a 53-bit significand, losing the low 11 bits of every
    /// inexact result -- barely visible on one operation, cumulative once
    /// results feed back in, and directly measurable against the oracle.
    ///
    /// Under PC = 53 the binary64 path is already exactly right (both round
    /// the infinitely precise result to 53 bits, once), and under PC = 24
    /// round_and_signal_precision narrows it. Those keep the host FPU, which
    /// is why the common case -- Windows sets PC = 53 at CRT startup -- pays
    /// nothing for this.
    [[nodiscard]] Float80 arith_x87(Float80 a, Float80 b, X87Op op) noexcept {
        // See prepare_ieee_source(): source-format #D is known before the
        // widened Float80 reaches this entry point. An unmasked
        // pre-computation exception suppresses evaluation as well as commit.
        if (has_unmasked_exception(FPU_SW_DE)) return a;
        // A stack-source fault supplies the real-indefinite placeholder only
        // for masked completion. It is not an operand for a second arithmetic
        // operation (which could spuriously add PE/IE on the host).
        const Float80 ind = indefinite();
        const auto is_indefinite = [&](const Float80& v) noexcept {
            return v.mantissa == ind.mantissa &&
                   v.exponent_sign == ind.exponent_sign;
        };
        if ((status_word & FPU_SW_SF) != 0 &&
            (is_indefinite(a) || is_indefinite(b))) {
            return ind;
        }
        // Intel SDM Vol. 1 section 8.5.2 and AMD APM Vol. 1 section
        // 6.8.2.3: a denormalized *source operand* raises #D independently
        // of whether the eventual result is tiny (#U). With #D unmasked the
        // destination is not committed; set_st/pop enforce that rule after
        // this shared interpreter/JIT helper records the pending exception.
        if (a.is_denormal() || b.is_denormal()) {
            record_exceptions(FPU_SW_DE);
            if (has_unmasked_exception(FPU_SW_DE)) return a;
        }
        // Keep the common Windows PC=53 path on native binary64, but do not
        // convert operands outside binary64's normal exponent range through
        // double.  That conversion would become Inf/zero before SoftF80 can
        // produce the PC/RC-rounded, exponent-scaled register result required
        // for unmasked x87 #O/#U.  Extreme values are rare and correctness on
        // this helper path is worth the software operation.
        const auto outside_binary64_normal_range = [](Float80 v) noexcept {
            const u16 exponent = static_cast<u16>(v.exponent_sign & 0x7FFFu);
            constexpr u16 kBinary64MinNormalX87Exponent = 15361; // -1022 + 16383
            constexpr u16 kBinary64MaxNormalX87Exponent = 17406; //  1023 + 16383
            return exponent != 0 && exponent != 0x7FFFu &&
                   (exponent < kBinary64MinNormalX87Exponent ||
                    exponent > kBinary64MaxNormalX87Exponent);
        };
        const bool require_soft_range =
            outside_binary64_normal_range(a) ||
            outside_binary64_normal_range(b);
        if (precision() != FpuPrecision::Extended && !require_soft_range) {
            // This is an interpreter/delegated-helper path, not the native JIT
            // lowering. Isolate its host flags so IE/ZE can suppress the
            // destination before the caller's set_st() and all other classes
            // become visible immediately in FSW/ES/B.
            math::clear_host_fp_exceptions();
            const double r = x87_arith(a.to_double(), b.to_double(), op);
            record_exceptions(math::harvest_x87_exceptions());
            return Float80(round_and_signal_precision(r));
        }
        u16 status_bits = 0;
        const math::SoftF80 sa{a.mantissa, a.exponent_sign};
        const math::SoftF80 sb{b.mantissa, b.exponent_sign};
        math::SoftF80 r{};
        switch (op) {
        case X87Op::Add: r = math::f80_add(sa, sb, control_word, &status_bits); break;
        case X87Op::Sub: r = math::f80_sub(sa, sb, control_word, &status_bits); break;
        case X87Op::Mul: r = math::f80_mul(sa, sb, control_word, &status_bits); break;
        default:         r = math::f80_div(sa, sb, control_word, &status_bits); break;
        }
        record_soft_status(status_bits);
        return Float80::from_extended(r.significand, r.sign_exp);
    }

    /// Narrow an ST value for a STORE to m64 / m32.
    ///
    /// A store is a rounding step and obeys RC like any other: FSTP m64 of an
    /// 80-bit 10/3 must truncate under round-toward-zero, not round to
    /// nearest. Float80::to_double rounds to nearest unconditionally -- right
    /// as a general narrowing, wrong as an x87 store -- so route the store
    /// through the same rounder the arithmetic uses, then convert. The value
    /// is exact in the destination format by then, so the conversion cannot
    /// round a second time.
    [[nodiscard]] double store_m64(Float80 v) noexcept {
        u16 status_bits = 0;
        const math::SoftF80 r = math::f80_round_to_format(
            {v.mantissa, v.exponent_sign}, control_word, 53,
            -1022, 1023, &status_bits);
        record_soft_status(status_bits);
        return Float80::from_extended(r.significand, r.sign_exp).to_double();
    }

    [[nodiscard]] float store_m32(Float80 v) noexcept {
        u16 status_bits = 0;
        const math::SoftF80 r = math::f80_round_to_format(
            {v.mantissa, v.exponent_sign}, control_word, 24,
            -126, 127, &status_bits);
        record_soft_status(status_bits);
        return static_cast<float>(
            Float80::from_extended(r.significand, r.sign_exp).to_double());
    }

    /// Memory-destination x87 instructions suppress their primary store when
    /// conversion raises an unmasked exception. This check belongs after the
    /// narrowing helper, because that is where SNaN, overflow, underflow and
    /// precision are discovered, but before either the guest write or FSTP pop.
    [[nodiscard]] bool memory_store_result_committable() const noexcept {
        return !has_unmasked_exception(
            FPU_SW_IE | FPU_SW_DE | FPU_SW_ZE |
            FPU_SW_OE | FPU_SW_UE | FPU_SW_PE);
    }

    /// Convert ST(0) for FIST/FISTP/FISTTP before the memory or stack commit.
    /// Intel SDM Vol. 2, FIST/FISTP and FISTTP: an unmasked #IA, #D or #P
    /// suppresses the destination write and (for a pop form) the pop. Masked
    /// #IA stores the width-specific integer indefinite; masked #P stores the
    /// rounded result. The returned image is meaningful only when `commit` is
    /// true.
    [[nodiscard]] u64 store_integer(Float80 value, u32 width_bytes,
                                    bool truncate, bool* commit) noexcept {
        const auto indefinite = [width_bytes]() noexcept -> u64 {
            if (width_bytes == 2) return 0x8000ULL;
            if (width_bytes == 4) return 0x80000000ULL;
            return 0x8000000000000000ULL;
        };
        const auto invalid = [&]() noexcept -> u64 {
            status_word = static_cast<u16>(status_word & ~FPU_SW_C1);
            record_exceptions(FPU_SW_IE);
            *commit = !has_unmasked_exception(FPU_SW_IE);
            return indefinite();
        };

        *commit = false;
        status_word = static_cast<u16>(status_word & ~FPU_SW_C1);
        // read_src() may already have recorded an unmasked empty-stack fault.
        if (has_unmasked_exception(FPU_SW_IE | FPU_SW_DE)) {
            return indefinite();
        }
        if (value.is_denormal()) {
            record_exceptions(FPU_SW_DE);
            if (has_unmasked_exception(FPU_SW_DE)) return indefinite();
        }

        const u16 exponent = static_cast<u16>(value.exponent_sign & 0x7FFFu);
        const bool explicit_integer =
            (value.mantissa & 0x8000000000000000ULL) != 0;
        // Infinity, either NaN class, and unsupported unnormal encodings raise
        // invalid operation before any integer result is committed.
        if (exponent == 0x7FFFu ||
            (exponent != 0 && !explicit_integer)) {
            return invalid();
        }

        // Intel SDM Vol. 2 FILD / FIST / FISTP: an integer loaded by FILD is
        // exact in double-extended precision (64-bit significand), and FIST
        // rounds that value per RC. Host long double is binary64 on ARM64, so
        // a conversion through it changes every integer past 2^53. Round from
        // the significand with integer arithmetic. RC still decides ties;
        // the precision-control field does not apply to integer stores.
        const bool neg = value.is_negative();
        const i32 unbiased = exponent == 0
            ? (1 - 16383) : (static_cast<i32>(exponent) - 16383);
        if (value.mantissa != 0 && unbiased > 63) return invalid();

        i64 integer = 0;
        bool inexact = false;
        bool rounded_up = false;
        if (value.mantissa != 0 && unbiased < 63) {
            const i32 shift = 63 - unbiased;
            u64 ip = 0;
            bool frac_nonzero = false;
            bool above_half = false;
            bool tie = false;
            if (shift >= 64) {
                // |value| < 1. shift == 64 is the binade of 0.5; anything
                // finer is strictly below one half.
                frac_nonzero = true;
                if (shift == 64) {
                    tie = value.mantissa == 0x8000000000000000ULL;
                    above_half = value.mantissa > 0x8000000000000000ULL;
                }
            } else {
                ip = value.mantissa >> static_cast<u32>(shift);
                const u64 half = 1ULL << static_cast<u32>(shift - 1);
                const u64 frac =
                    value.mantissa & ((1ULL << static_cast<u32>(shift)) - 1);
                frac_nonzero = frac != 0;
                tie = frac == half;
                above_half = frac > half;
            }
            const auto mode = static_cast<FpuRounding>((control_word >> 10) & 3u);
            bool round_away = false;
            if (!truncate) {
                switch (mode) {
                case FpuRounding::Down:
                    round_away = neg && frac_nonzero;
                    break;
                case FpuRounding::Up:
                    round_away = !neg && frac_nonzero;
                    break;
                case FpuRounding::Truncate:
                    break;
                case FpuRounding::Nearest:
                default:
                    if (above_half) round_away = true;
                    else if (tie) round_away = (ip & 1ULL) != 0;
                    break;
                }
            }
            u64 mag = ip;
            if (round_away) ++mag;
            if (!neg) {
                if (mag > 0x7FFFFFFFFFFFFFFFULL) return invalid();
                integer = static_cast<i64>(mag);
            } else if (mag > 0x8000000000000000ULL) {
                return invalid();
            } else if (mag == 0x8000000000000000ULL) {
                integer = static_cast<i64>(0x8000000000000000ULL);
            } else {
                integer = -static_cast<i64>(mag);
            }
            inexact = frac_nonzero;
            if (frac_nonzero) rounded_up = neg ? !round_away : round_away;
        } else if (value.mantissa != 0) {
            // unbiased == 63: magnitude is at least 2^63. The only in-range
            // signed result is INT64_MIN.
            if (!(neg && value.mantissa == 0x8000000000000000ULL)) return invalid();
            integer = static_cast<i64>(0x8000000000000000ULL);
        }

        const bool fits = width_bytes >= 8
            ? true
            : width_bytes == 4
                ? (integer >= static_cast<i64>(-2147483647 - 1) &&
                   integer <= static_cast<i64>(2147483647))
                : (integer >= static_cast<i64>(-32768) &&
                   integer <= static_cast<i64>(32767));
        if (!fits) return invalid();
        if (inexact) {
            record_exceptions(FPU_SW_PE);
            if (rounded_up) {
                status_word = static_cast<u16>(status_word | FPU_SW_C1);
            }
        }
        *commit = !has_unmasked_exception(FPU_SW_IE | FPU_SW_DE | FPU_SW_PE);
        if (width_bytes == 2) {
            return static_cast<u64>(static_cast<u16>(static_cast<i16>(integer)));
        }
        if (width_bytes == 4) {
            return static_cast<u64>(static_cast<u32>(static_cast<i32>(integer)));
        }
        return static_cast<u64>(integer);
    }

    /// FSQRT under the same rule as arith_x87.
    [[nodiscard]] Float80 sqrt_x87(Float80 a) noexcept {
        if (a.is_denormal()) {
            record_exceptions(FPU_SW_DE);
            if (has_unmasked_exception(FPU_SW_DE)) return a;
        }
        if (precision() != FpuPrecision::Extended) {
            const double v = a.to_double();
            if (v < 0.0) {
                record_exceptions(FPU_SW_IE);
                double indef;
                const u64 bits = 0xFFF8'0000'0000'0000ull;
                std::memcpy(&indef, &bits, sizeof(indef));
                return Float80(indef);
            }
            math::clear_host_fp_exceptions();
            const double result = std::sqrt(v);
            record_exceptions(math::harvest_x87_exceptions());
            return Float80(round_and_signal_precision(result));
        }
        u16 status_bits = 0;
        const math::SoftF80 r =
            math::f80_sqrt({a.mantissa, a.exponent_sign}, control_word,
                           &status_bits);
        record_soft_status(status_bits);
        return Float80::from_extended(r.significand, r.sign_exp);
    }

    /// Fold the software unit's exception bits into FSW. The sticky exception
    /// flags accumulate; C1 is the rounding-direction indicator and is
    /// ASSIGNED, because "did not round up" has to clear it.
    void record_soft_status(u16 status_bits) noexcept {
        record_exceptions(static_cast<u16>(status_bits & 0x3Fu));
        if (status_bits & math::kF80C1) {
            status_word = static_cast<u16>(status_word | FPU_SW_C1);
        } else {
            status_word = static_cast<u16>(status_word & ~FPU_SW_C1);
        }
    }

    /// Signal #P for an x87 transcendental whose hardware result is inexact.
    ///
    /// Measured against real Intel silicon, NOT derived from the mathematics:
    /// hardware sets PE for F2XM1(1.0) -> 1.0 and FYL2XP1(1.0, 1.0) -> 1.0,
    /// both mathematically EXACT and exactly representable, and does NOT set it
    /// when the result is zero. So the flag tracks the hardware's internal
    /// polynomial approximation; deciding "was this inexact?" in wider
    /// precision gives the wrong answer for the 1.0 cases.
    ///
    /// Lives here because the interpreter (ops/fpu.cpp) and the JIT
    /// (jit/jit_helpers.cpp) carry SEPARATE copies of these lowerings, and a
    /// rule implemented in only one of them fixes only one engine.
    ///
    /// @param result    the delivered double result
    /// @param reference the same value computed in wider precision, used for
    ///                  the C1 round-up indicator
    void signal_transcendental_precision(double result,
                                         long double reference) noexcept {
        if (result == 0.0) return;   // covers +0.0 and -0.0
        record_exceptions(FPU_SW_PE);
        // Intel SDM: with #P raised, C1 is the round-up indicator, and "up"
        // means the MAGNITUDE grew -- see round_and_signal_precision, where a
        // negative result rounded away from zero comes back C1-SET on silicon.
        const long double delivered = static_cast<long double>(result);
        const long double abs_delivered = delivered < 0.0L ? -delivered : delivered;
        const long double abs_reference = reference < 0.0L ? -reference : reference;
        if (abs_delivered > abs_reference) {
            status_word = static_cast<u16>(status_word | FPU_SW_C1);
        } else {
            status_word = static_cast<u16>(status_word & ~FPU_SW_C1);
        }
    }

    /// Enter MMX mode, as every MMX instruction except EMMS does.
    ///
    /// Intel SDM: an MMX instruction sets the x87 tag word to all-Valid and
    /// TOP to 0. This is architecturally visible through FNSTENV/FXSAVE, and
    /// it applies to instructions that only READ an MM register (PMOVMSKB,
    /// PEXTRW, MOVD r32/MOVQ2DQ) just as much as to ones that write, which is
    /// why it lives here rather than only in set_mm().
    void mmx_enter() noexcept {
        tag_word = 0x0000;  // all Valid
        set_top(0);
    }

    /// Mark all FPU registers as empty (EMMS instruction)
    void emms() noexcept {
        tag_word = 0xFFFF;  // All registers empty
    }
};

} // namespace vitreo::cpu
