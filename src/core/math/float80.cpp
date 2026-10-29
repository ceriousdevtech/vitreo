// SPDX-License-Identifier: LGPL-2.1-only

/// @file float80.cpp
/// @brief See float80.hpp.

#include "core/math/float80.hpp"

#include <bit>

namespace vitreo::math {
namespace {

using u128 = unsigned __int128;

constexpr i32 kBias = 16383;
constexpr i32 kMinNormalExp = -16382;   // unbiased exponent of the smallest normal
constexpr i32 kMaxNormalExp = 16383;    // unbiased exponent of the largest normal
constexpr u16 kExpMask = 0x7FFF;
constexpr u16 kSignBit = 0x8000;

enum class Cls { Zero, Finite, Inf, NaN };

/// Uniform internal form: the value is exactly `sig * 2^exp`, with `sig` an
/// unsigned integer. Normals and denormals both land here without a special
/// case, which is what keeps gradual underflow from needing its own path.
struct Unpacked {
    bool sign;
    Cls cls;
    u64 sig;
    i32 exp;
    bool snan;
};

Unpacked unpack(SoftF80 v) noexcept {
    Unpacked u{};
    u.sign = (v.sign_exp & kSignBit) != 0;
    const u16 e = static_cast<u16>(v.sign_exp & kExpMask);
    u.sig = v.significand;
    if (e == kExpMask) {
        // The explicit integer bit distinguishes infinity (1.000...) from the
        // NaNs; a clear integer bit here is one of the unsupported encodings
        // (pseudo-NaN / pseudo-infinity), which modern x87 treats as invalid.
        const u64 frac = v.significand & 0x7FFFFFFFFFFFFFFFull;
        if (frac == 0 && (v.significand >> 63) != 0) {
            u.cls = Cls::Inf;
        } else {
            u.cls = Cls::NaN;
            // Quiet bit is bit 62; clear means signalling.
            u.snan = (v.significand & 0x4000000000000000ull) == 0;
        }
        return u;
    }
    if (e == 0) {
        if (v.significand == 0) { u.cls = Cls::Zero; return u; }
        // Denormal (or pseudo-denormal when bit 63 happens to be set; both
        // have the same value, exponent field 0 meaning 2^-16382).
        u.cls = Cls::Finite;
        u.exp = kMinNormalExp - 63;
        return u;
    }
    u.cls = Cls::Finite;
    u.exp = static_cast<i32>(e) - kBias - 63;
    return u;
}

SoftF80 pack_zero(bool sign) noexcept {
    return SoftF80{0, static_cast<u16>(sign ? kSignBit : 0)};
}

SoftF80 pack_inf(bool sign) noexcept {
    return SoftF80{0x8000000000000000ull,
                   static_cast<u16>((sign ? kSignBit : 0) | kExpMask)};
}

/// x87 "real indefinite": the default QNaN, sign set.
SoftF80 indefinite() noexcept {
    return SoftF80{0xC000000000000000ull, static_cast<u16>(kSignBit | kExpMask)};
}

SoftF80 quiet(SoftF80 v) noexcept {
    v.significand |= 0x4000000000000000ull;
    return v;
}

int precision_bits(u16 control_word) noexcept {
    switch ((control_word >> 8) & 3u) {
        case 0: return 24;   // single
        case 2: return 53;   // double
        default: return 64;  // extended (1 is reserved; hardware treats as 64)
    }
}

int rounding_control(u16 control_word) noexcept {
    return static_cast<int>((control_word >> 10) & 3u);
}

int msb128(u128 v) noexcept {
    const u64 hi = static_cast<u64>(v >> 64);
    if (hi != 0) return 127 - std::countl_zero(hi);
    return 63 - std::countl_zero(static_cast<u64>(v));
}

/// Round the exact value `sig * 2^exp` (sign applied separately) to `prec`
/// significand bits under rounding mode `rc`, and pack it.
///
/// `sticky_in` carries any nonzero bits already shifted out by the caller --
/// the division and square-root remainders -- so the round decision sees the
/// full exact residue rather than a truncated one.
SoftF80 round_pack(bool sign, i32 exp, u128 sig, bool sticky_in,
                   int prec, i32 min_normal_exp, i32 max_normal_exp,
                   u16 control_word, bool scale_unmasked_range,
                   u16* status) noexcept {
    if (sig == 0 && !sticky_in) return pack_zero(sign);

    const int rc = rounding_control(control_word);

    // Normalise so the leading 1 sits at bit 127; the value is then
    // (sig / 2^127) * 2^(exp + 127), i.e. unbiased exponent exp + 127.
    if (sig != 0) {
        const int sh = 127 - msb128(sig);
        if (sh > 0) { sig <<= sh; exp -= sh; }
    }
    i32 e = exp + 127;

    // Intel SDM Vol. 1 sections 8.5.4 and 8.5.5: an unmasked #O/#U
    // whose destination is an x87 register commits a *scaled* result for the
    // exception handler.  It is the unbounded result rounded under PC/RC,
    // with the exponent divided/multiplied by 2^24576 respectively.  Probe
    // this before gradual underflow discards the very bits the handler must
    // observe.  Masked underflow deliberately continues through the old
    // denormalisation path unchanged.
    if (scale_unmasked_range && e < min_normal_exp &&
        (control_word & kF80Underflow) == 0) {
        const int unbounded_discard = 128 - prec;
        u128 unbounded_kept = sig >> unbounded_discard;
        const u128 lost = sig & ((u128{1} << unbounded_discard) - 1);
        const bool round_bit =
            ((sig >> (unbounded_discard - 1)) & 1) != 0;
        const bool sticky = sticky_in ||
            ((lost & ((u128{1} << (unbounded_discard - 1)) - 1)) != 0);
        const bool inexact = round_bit || sticky;
        bool increment = false;
        switch (rc) {
            case 1: increment = sign && inexact; break;
            case 2: increment = !sign && inexact; break;
            case 3: increment = false; break;
            default:
                increment = round_bit &&
                    (sticky || ((unbounded_kept & 1) != 0));
                break;
        }
        i32 rounded_e = e;
        if (increment) {
            ++unbounded_kept;
            if (unbounded_kept >> prec) {
                unbounded_kept >>= 1;
                ++rounded_e;
            }
        }
        // Tininess is tested after rounding with an unbounded exponent.  An
        // exact tiny result does not raise #U and therefore still takes the
        // ordinary gradual-underflow path below.
        if (inexact && rounded_e < min_normal_exp) {
            *status |= (kF80Underflow | kF80Precision);
            if (increment) *status |= kF80C1;
            else *status &= static_cast<u16>(~kF80C1);
            constexpr i32 kScaleBias = 24576;  // 3 * 2^13
            const u64 sig64 =
                static_cast<u64>(unbounded_kept) << (64 - prec);
            return SoftF80{
                sig64,
                static_cast<u16>((sign ? kSignBit : 0) |
                                 static_cast<u16>(rounded_e + kScaleBias +
                                                  kBias))};
        }
    }

    // Bits kept vs discarded. Gradual underflow widens the discard by however
    // far the exponent falls short of the smallest normal, which is exactly
    // what makes a denormal result lose precision instead of range.
    int discard = 128 - prec;
    if (e < min_normal_exp) {
        const i64 extra = static_cast<i64>(min_normal_exp) - e;
        if (extra >= 128) {
            // Everything shifts out: the exact result is below half the
            // smallest denormal, so the rounded answer is a signed zero (or
            // the smallest denormal under a directed mode).
            *status |= (kF80Underflow | kF80Precision);
            const bool up = (rc == 2 && !sign) || (rc == 1 && sign);
            if (up) {
                *status |= kF80C1;
                // The smallest destination subnormal is exactly
                // 1.0 * 2^(min_normal_exp-(prec-1)). Represent that value as a
                // normal Float80 so the later binary32/64 encoding is exact.
                return SoftF80{
                    0x8000000000000000ull,
                    static_cast<u16>((sign ? kSignBit : 0) |
                        static_cast<u16>(min_normal_exp - (prec - 1) + kBias))};
            }
            return pack_zero(sign);
        }
        discard += static_cast<int>(extra);
        e = min_normal_exp;
    }

    u128 kept;
    bool round_bit = false;
    bool sticky = sticky_in;
    if (discard <= 0) {
        kept = sig;
    } else if (discard >= 128) {
        sticky = sticky || (sig != 0);
        kept = 0;
    } else {
        kept = sig >> discard;
        const u128 lost = sig & ((u128{1} << discard) - 1);
        round_bit = ((sig >> (discard - 1)) & 1) != 0;
        sticky = sticky || ((lost & ((u128{1} << (discard - 1)) - 1)) != 0);
    }

    bool increment = false;
    switch (rc) {
        case 1: increment = sign && (round_bit || sticky); break;   // toward -inf
        case 2: increment = !sign && (round_bit || sticky); break;  // toward +inf
        case 3: increment = false; break;                           // toward zero
        default:  // nearest, ties to even
            increment = round_bit && (sticky || ((kept & 1) != 0));
            break;
    }

    const bool inexact = round_bit || sticky;
    if (inexact) *status |= kF80Precision;

    if (increment) {
        ++kept;
        // A carry out of the significand widens it by one bit; renormalising
        // costs one exponent step, and can push a denormal up to normal.
        if (kept >> prec) {
            kept >>= 1;
            ++e;
        }
    }
    if (inexact) {
        if (increment) *status |= kF80C1;
        else *status &= static_cast<u16>(~kF80C1);
    }

    if (kept == 0) {
        // Underflowed all the way to zero.
        *status |= (kF80Underflow | kF80Precision);
        return pack_zero(sign);
    }

    // Denormal results keep exponent field 0; the value is carried entirely by
    // the significand, so detect "still below the smallest normal" by whether
    // the leading bit reached the top of the kept width.
    const bool is_normal = ((kept >> (prec - 1)) & 1) != 0;
    if (!is_normal) {
        if (inexact) *status |= kF80Underflow;
        // Normalise the destination subnormal into Float80. For the x87 format
        // itself the unbiased exponent can fall below its representable normal
        // floor, so retain the architectural exponent-zero encoding there.
        if (min_normal_exp == kMinNormalExp) {
            const u64 sig64 = static_cast<u64>(kept) << (64 - prec);
            return SoftF80{sig64, static_cast<u16>(sign ? kSignBit : 0)};
        }
        const int kept_msb = msb128(kept);
        const int normalise = (prec - 1) - kept_msb;
        const u64 sig64 = static_cast<u64>(kept) <<
                          (64 - prec + normalise);
        return SoftF80{
            sig64,
            static_cast<u16>((sign ? kSignBit : 0) |
                static_cast<u16>(e - normalise + kBias))};
    }

    if (e > max_normal_exp) {
        if (scale_unmasked_range && (control_word & kF80Overflow) == 0) {
            // Unmasked register #O exposes the rounded true result divided by
            // 2^24576.  This is normal extended precision for every finite
            // FADD/FSUB/FMUL/FDIV overflow, so it can be packed directly.
            // #P accompanies it only when rounding the scaled significand was
            // inexact; native Windows reports OE without PE for exact
            // max-finite * 2 under PC=64.
            *status |= kF80Overflow;
            if (!inexact) *status &= static_cast<u16>(~kF80C1);
            constexpr i32 kScaleBias = 24576;  // 3 * 2^13
            const u64 sig64 = static_cast<u64>(kept) << (64 - prec);
            return SoftF80{
                sig64,
                static_cast<u16>((sign ? kSignBit : 0) |
                                 static_cast<u16>(e - kScaleBias + kBias))};
        }
        *status |= (kF80Overflow | kF80Precision);
        // Intel SDM: a masked overflow delivers infinity under round-to-nearest
        // and the two "away" directed modes, and the largest finite magnitude
        // under the mode that rounds toward it.
        const bool to_inf = (rc == 0) || (rc == 2 && !sign) || (rc == 1 && sign);
        if (to_inf) { *status |= kF80C1; return pack_inf(sign); }
        *status &= static_cast<u16>(~kF80C1);
        u64 maxsig = ~u64{0};
        if (prec < 64) maxsig &= ~((u64{1} << (64 - prec)) - 1);
        return SoftF80{maxsig,
                       static_cast<u16>((sign ? kSignBit : 0) |
                                        static_cast<u16>(max_normal_exp + kBias))};
    }

    const u64 sig64 = static_cast<u64>(kept) << (64 - prec);
    return SoftF80{sig64,
                   static_cast<u16>((sign ? kSignBit : 0) |
                                    static_cast<u16>(e + kBias))};
}

/// Intel SDM Vol. 1 4.8.3.5: with two NaN operands the one with the LARGER
/// significand is delivered, quieted. A single NaN operand is delivered
/// quieted. An SNaN anywhere raises IE.
bool nan_result(const Unpacked& ua, const Unpacked& ub, SoftF80 a, SoftF80 b,
                u16* status, SoftF80* out) noexcept {
    const bool an = ua.cls == Cls::NaN;
    const bool bn = ub.cls == Cls::NaN;
    if (!an && !bn) return false;
    if (ua.snan || ub.snan) *status |= kF80Invalid;
    if (an && bn) {
        *out = quiet((ub.sig > ua.sig) ? b : a);
    } else {
        *out = quiet(an ? a : b);
    }
    return true;
}

SoftF80 add_sub(SoftF80 a, SoftF80 b, bool negate_b, u16 cw,
                u16* status) noexcept {
    const Unpacked ua = unpack(a);
    Unpacked ub = unpack(b);
    if (negate_b) {
        ub.sign = !ub.sign;
        b.sign_exp = static_cast<u16>(b.sign_exp ^ kSignBit);
    }

    SoftF80 nan{};
    if (nan_result(ua, ub, a, b, status, &nan)) return nan;

    const int rc = rounding_control(cw);
    if (ua.cls == Cls::Inf || ub.cls == Cls::Inf) {
        if (ua.cls == Cls::Inf && ub.cls == Cls::Inf) {
            // inf + (-inf) is invalid and delivers the indefinite QNaN.
            if (ua.sign != ub.sign) { *status |= kF80Invalid; return indefinite(); }
            return pack_inf(ua.sign);
        }
        return pack_inf(ua.cls == Cls::Inf ? ua.sign : ub.sign);
    }
    if (ua.cls == Cls::Zero && ub.cls == Cls::Zero) {
        // -0 + -0 is -0; otherwise the sum of opposite-signed zeros is +0,
        // except under round-toward-negative-infinity where it is -0.
        if (ua.sign == ub.sign) return pack_zero(ua.sign);
        return pack_zero(rc == 1);
    }
    if (ua.cls == Cls::Zero) return round_pack(
        ub.sign, ub.exp, u128{ub.sig}, false, precision_bits(cw),
        kMinNormalExp, kMaxNormalExp, cw, true, status);
    if (ub.cls == Cls::Zero) return round_pack(
        ua.sign, ua.exp, u128{ua.sig}, false, precision_bits(cw),
        kMinNormalExp, kMaxNormalExp, cw, true, status);

    // Align to the smaller exponent so both significands are exact integers at
    // the same scale, by shifting the LARGER operand up.
    //
    // The cap is 64, not more: a significand's leading bit sits at 63, so a
    // shift of 64 puts it at 127 and exactly fills the accumulator. Anything
    // beyond silently drops the top bits off the u128 -- which is how a result
    // of ~2^177 came back as zero. 64 is also enough for correct rounding: an
    // operand more than 64 binary places below the other cannot reach the
    // round bit at position (msb - 64), so it can only contribute sticky,
    // which is what the capped branch records.
    constexpr i32 kMaxAlign = 64;
    i32 ea = ua.exp, eb = ub.exp;
    u128 sa = ua.sig, sb = ub.sig;
    bool sticky = false;
    if (ea > eb) {
        i32 d = ea - eb;
        if (d > kMaxAlign) { sticky = (sb != 0); sb = 0; d = kMaxAlign; }
        sa <<= d; ea -= d;
        eb = ea;
    } else if (eb > ea) {
        i32 d = eb - ea;
        if (d > kMaxAlign) { sticky = (sa != 0); sa = 0; d = kMaxAlign; }
        sb <<= d; eb -= d;
        ea = eb;
    }
    // Both significands are now at the scale of the SHIFTED operand -- take it
    // from there, never from the operand that was capped away.
    //
    // Taking min(ea, eb) looked equivalent and is, right up until the shift is
    // capped: then the small operand was zeroed and its exponent is still the
    // original, far below the common scale, so the result came out too small
    // by exactly the amount clipped. `FADD m64` of 1.0 and a value with
    // exponent 177 produced a result with exponent 64 -- correct significand,
    // off by 2^113, which is 177 - 64.

    bool sign;
    u128 sum;
    if (ua.sign == ub.sign) {
        sign = ua.sign;
        sum = sa + sb;
    } else if (sa >= sb) {
        sign = ua.sign;
        sum = sa - sb;
        // The discarded low bits of the subtrahend borrow from the result.
        if (sticky && sum != 0) --sum;
    } else {
        sign = ub.sign;
        sum = sb - sa;
        if (sticky && sum != 0) --sum;
    }
    if (sum == 0 && !sticky) {
        // An exact cancellation is +0 except toward -inf, where it is -0.
        return pack_zero(rc == 1);
    }
    return round_pack(sign, ea, sum, sticky, precision_bits(cw),
                      kMinNormalExp, kMaxNormalExp, cw, true, status);
}

}  // namespace

SoftF80 f80_add(SoftF80 a, SoftF80 b, u16 cw, u16* status) noexcept {
    return add_sub(a, b, false, cw, status);
}

SoftF80 f80_sub(SoftF80 a, SoftF80 b, u16 cw, u16* status) noexcept {
    return add_sub(a, b, true, cw, status);
}

SoftF80 f80_mul(SoftF80 a, SoftF80 b, u16 cw, u16* status) noexcept {
    const Unpacked ua = unpack(a);
    const Unpacked ub = unpack(b);
    SoftF80 nan{};
    if (nan_result(ua, ub, a, b, status, &nan)) return nan;

    const bool sign = ua.sign != ub.sign;
    if (ua.cls == Cls::Inf || ub.cls == Cls::Inf) {
        // 0 * inf is invalid.
        if (ua.cls == Cls::Zero || ub.cls == Cls::Zero) {
            *status |= kF80Invalid;
            return indefinite();
        }
        return pack_inf(sign);
    }
    if (ua.cls == Cls::Zero || ub.cls == Cls::Zero) return pack_zero(sign);

    // 64x64 -> 128 is exact, so the product needs no sticky bit of its own.
    const u128 prod = static_cast<u128>(ua.sig) * static_cast<u128>(ub.sig);
    return round_pack(sign, ua.exp + ub.exp, prod, false, precision_bits(cw),
                      kMinNormalExp, kMaxNormalExp, cw, true, status);
}

SoftF80 f80_div(SoftF80 a, SoftF80 b, u16 cw, u16* status) noexcept {
    const Unpacked ua = unpack(a);
    const Unpacked ub = unpack(b);
    SoftF80 nan{};
    if (nan_result(ua, ub, a, b, status, &nan)) return nan;

    const bool sign = ua.sign != ub.sign;
    if (ua.cls == Cls::Inf) {
        if (ub.cls == Cls::Inf) { *status |= kF80Invalid; return indefinite(); }
        return pack_inf(sign);
    }
    if (ub.cls == Cls::Inf) return pack_zero(sign);
    if (ub.cls == Cls::Zero) {
        if (ua.cls == Cls::Zero) { *status |= kF80Invalid; return indefinite(); }
        *status |= kF80DivByZero;
        return pack_inf(sign);
    }
    if (ua.cls == Cls::Zero) return pack_zero(sign);

    // Rounding to 64 significand bits needs 66 quotient bits: 64 kept, plus a
    // round bit and a sticky. One 128/64 division cannot supply them -- with a
    // 64-bit dividend the numerator can only be shifted up by 64 before it
    // overflows 128 bits, and the quotient then has 64 or 65 significant bits.
    // In the 64-bit case the round bit is missing, round_pack sees a zero
    // there, and the result rounds DOWN where it should round up: exactly one
    // ULP low, which is what the oracle reported for FIDIV.
    //
    // So take a second pass over the remainder for 8 more bits. `r` is below
    // the divisor and therefore below 2^64, so `r << kRefine` cannot overflow.
    const int shift = 64;
    const int kRefine = 8;
    const u128 num = static_cast<u128>(ua.sig) << shift;
    const u128 den = static_cast<u128>(ub.sig);
    u128 q = num / den;
    const u128 r = num % den;
    const u128 q2 = (r << kRefine) / den;
    const u128 r2 = (r << kRefine) % den;
    q = (q << kRefine) | q2;
    return round_pack(sign, ua.exp - ub.exp - shift - kRefine, q, r2 != 0,
                      precision_bits(cw), kMinNormalExp, kMaxNormalExp, cw,
                      true, status);
}

SoftF80 f80_round_to_format(SoftF80 a, u16 cw, int prec_bits,
                            int min_normal_exp, int max_normal_exp,
                            u16* status) noexcept {
    const Unpacked ua = unpack(a);
    if (ua.cls == Cls::NaN) { if (ua.snan) *status |= kF80Invalid; return quiet(a); }
    if (ua.cls == Cls::Inf || ua.cls == Cls::Zero) return a;
    return round_pack(ua.sign, ua.exp, u128{ua.sig}, false, prec_bits,
                      min_normal_exp, max_normal_exp, cw, false, status);
}

SoftF80 f80_sqrt(SoftF80 a, u16 cw, u16* status) noexcept {
    const Unpacked ua = unpack(a);
    if (ua.cls == Cls::NaN) {
        if (ua.snan) *status |= kF80Invalid;
        return quiet(a);
    }
    if (ua.cls == Cls::Zero) return pack_zero(ua.sign);   // sqrt(-0) = -0
    if (ua.sign) { *status |= kF80Invalid; return indefinite(); }
    if (ua.cls == Cls::Inf) return pack_inf(false);

    // Exact powers of four do not need the restoring integer-square-root
    // loop below. In the unpacked representation a significand of exactly
    // 1.0 means value == 2^e; when e is even its square root is exactly
    // 2^(e/2) in every x87 precision and rounding mode. This is also the
    // common FLD m32/m64; FSQRT; FSTP shape used for magnitudes and distances.
    // Returning directly is architecturally important as well as faster:
    // Intel SDM Vol. 2, FSQRT sets PE/C1 only when rounding is required, and
    // this class has no discarded bits.
    constexpr u64 kIntegerBit = 0x8000000000000000ull;
    if (ua.sig == kIntegerBit) {
        const i32 unbiased_exp = ua.exp + 63;
        if ((unbiased_exp & 1) == 0) {
            return SoftF80{
                kIntegerBit,
                static_cast<u16>(kBias + unbiased_exp / 2),
            };
        }
    }

    // Work with an even exponent so the square root of the scaled significand
    // is itself a clean binary shift: value = sig * 2^exp, and with exp even
    // sqrt(value) = sqrt(sig) * 2^(exp/2).
    u128 sig = ua.sig;
    i32 exp = ua.exp;
    // Scale up so the integer square root produces ~65 significant bits.
    int sh = 126 - msb128(sig);
    if ((sh & 1) != 0) --sh;          // keep the shift even
    sig <<= sh;
    exp -= sh;
    if ((exp & 1) != 0) { sig <<= 1; --exp; }

    // Integer square root of a 128-bit value, bit-by-bit restoring. A 128-bit
    // radicand yields a 64-bit root, one bit short of what rounding to 64
    // significand bits needs -- the same shortfall as the division above.
    // Continuing the loop past the binary point with zero digit-pairs produces
    // the extra fractional bits; each one scales the root by two, so the
    // exponent carries `kFrac` back out.
    constexpr int kFrac = 4;
    u128 root = 0;
    u128 rem = 0;
    for (int i = 126; i >= 0; i -= 2) {
        rem = (rem << 2) | ((sig >> i) & 3);
        const u128 trial = (root << 2) | 1;
        root <<= 1;
        if (rem >= trial) { rem -= trial; root |= 1; }
    }
    for (int i = 0; i < kFrac; ++i) {
        rem <<= 2;
        const u128 trial = (root << 2) | 1;
        root <<= 1;
        if (rem >= trial) { rem -= trial; root |= 1; }
    }
    return round_pack(false, exp / 2 - kFrac, root, rem != 0,
                      precision_bits(cw), kMinNormalExp, kMaxNormalExp, cw,
                      true, status);
}

}  // namespace vitreo::math
