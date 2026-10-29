#pragma once
/// @file crt_float_parse.hpp
/// @brief The UCRT's floating-point subject-sequence parser, shared by the
///        strtod family (a string source) and the scanf family (a
///        width-limited input source).
///
/// Contract: the Windows SDK's UCRT source, ucrt/inc/corecrt_internal_strtox.h
/// (parse_floating_point_from_source and its INF/NAN helpers, parse_digit,
/// wide_character_to_digit, is_space). It is one template over a character
/// source because the UCRT's is: the two callers differ only in what
/// restore_state can do. A string can always step back to a saved position;
/// an input adapter cannot, so a subject that has to back up by more than the
/// one character it can unget ("1e", "0x", "infin") fails in scanf where
/// strtod accepts its prefix. Both behaviours fall out of the same parser.
///
/// The parse yields the decimal or hexadecimal digits and the exponent the
/// UCRT's converter consumes; to_bits then produces the correctly rounded
/// value under the guest's rounding mode, as the UCRT's big-integer converter
/// does (it keeps at most 768 mantissa digits, so the parser truncates to the
/// same 768).

#include <vitreo/common.hpp>

#include <string>

namespace vitreo::winapi::crt_float {

/// floating_point_string::_mantissa capacity.
inline constexpr usize kMantissaDigits = 768;
/// maximum_temporary_decimal_exponent / minimum_temporary_decimal_exponent.
inline constexpr int kMaxTemporaryExponent = 5200;
inline constexpr int kMinTemporaryExponent = -5200;

/// floating_point_parse_result.
enum class Result : u8 {
    DecimalDigits,
    HexadecimalDigits,
    Zero,
    Infinity,
    QNan,
    SNan,
    Indeterminate,
    NoDigits,
    Underflow,
    Overflow,
};

/// floating_point_string: value = 0.mantissa x base^exponent, base 10 for
/// decimal digits and 2 (exponent) / 16 (digits) for hexadecimal.
struct FpString {
    bool negative{false};
    int exponent{0};
    std::string mantissa;  // digit values 0..15, trailing zeroes removed
};

/// wide_character_to_digit: ASCII plus the Unicode decimal-digit blocks.
int wide_character_to_digit(u32 c);

/// parse_digit: the digit's value (letters are 10..35), or ~0u.
/// @param wide the wchar_t overload (Unicode digits); the char overload
///        treats bytes 0x80..0xFF as signed, so they are never digits.
unsigned parse_digit(u32 c, bool wide);

/// is_space: "C" locale isspace for narrow characters; iswspace for wide
/// ones, which is the Windows C1_SPACE set (U+0085, U+00A0, U+1680,
/// U+2000..U+200A, U+2028, U+2029, U+202F, U+205F, U+3000 besides ASCII).
bool is_space(u32 c, bool wide);

/// is_digit_or_nondigit: 0-9, a-z, A-Z and '_' (the NAN(...) payload).
inline bool is_digit_or_nondigit(u32 c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

/// Which parser: the UCRT's, or the decimal-only one the system msvcrt.dll
/// uses in strtod (no hexadecimal, no INF/NAN, ASCII digits only).
struct Syntax {
    bool wide{false};
    bool ucrt{true};
};

namespace detail {

template <typename Source>
bool next_characters(const char* upper, const char* lower, u32& c, Source& source) {
    for (usize i = 0; upper[i] != 0; ++i) {
        if (c != static_cast<u32>(upper[i]) && c != static_cast<u32>(lower[i])) {
            return false;
        }
        c = source.get();
    }
    return true;
}

template <typename Source, typename State>
Result possible_infinity(u32& c, Source& source, State stored_state) {
    auto restore = [&]() {
        source.unget(c);
        c = 0;
        return source.restore_state(stored_state);
    };
    if (!next_characters("INF", "inf", c, source)) {
        (void)restore();
        return Result::NoDigits;
    }
    source.unget(c);
    stored_state = source.save_state();
    c = source.get();
    if (!next_characters("INITY", "inity", c, source)) {
        return restore() ? Result::Infinity : Result::NoDigits;
    }
    source.unget(c);
    return Result::Infinity;
}

template <typename Source, typename State>
Result possible_nan(u32& c, Source& source, State stored_state) {
    auto restore = [&]() {
        source.unget(c);
        c = 0;
        return source.restore_state(stored_state);
    };
    if (!next_characters("NAN", "nan", c, source)) {
        (void)restore();
        return Result::NoDigits;
    }
    source.unget(c);
    stored_state = source.save_state();
    c = source.get();
    if (c != '(') {
        return restore() ? Result::QNan : Result::NoDigits;
    }
    c = source.get();  // past the '('
    if (next_characters("SNAN)", "snan)", c, source)) {
        source.unget(c);
        return Result::SNan;
    }
    if (next_characters("IND)", "ind)", c, source)) {
        source.unget(c);
        return Result::Indeterminate;
    }
    while (c != ')' && c != 0) {
        if (!is_digit_or_nondigit(c)) {
            return restore() ? Result::QNan : Result::NoDigits;
        }
        c = source.get();
    }
    if (c != ')') {
        return restore() ? Result::QNan : Result::NoDigits;
    }
    return Result::QNan;
}

inline unsigned syntax_digit(u32 c, const Syntax& syntax) {
    if (!syntax.ucrt) {
        if (c >= '0' && c <= '9') return c - '0';
        return ~0u;
    }
    return parse_digit(c, syntax.wide);
}

} // namespace detail

/// parse_floating_point_from_source. The source provides get() (0 at the
/// end), unget(c), save_state() and restore_state(state).
template <typename Source>
Result parse_from_source(Source& source, const Syntax& syntax, FpString& fp) {
    fp = FpString{};
    auto stored_state = source.save_state();
    u32 c = source.get();
    auto restore = [&]() {
        source.unget(c);
        c = 0;
        return source.restore_state(stored_state);
    };

    while (is_space(c, syntax.wide)) c = source.get();

    fp.negative = c == '-';
    if (c == '-' || c == '+') c = source.get();

    if (syntax.ucrt && (c == 'I' || c == 'i')) {
        return detail::possible_infinity(c, source, stored_state);
    }
    if (syntax.ucrt && (c == 'N' || c == 'n')) {
        return detail::possible_nan(c, source, stored_state);
    }

    bool is_hexadecimal = false;
    if (syntax.ucrt && c == '0') {
        const auto next_stored_state = source.save_state();
        const u32 next_c = source.get();
        if (next_c == 'x' || next_c == 'X') {
            is_hexadecimal = true;
            c = source.get();
            // "0x" with no digits after it is the subject "0" followed by a
            // final string that starts with the 'x'.
            stored_state = next_stored_state;
        } else {
            source.unget(next_c);
        }
    }

    const unsigned max_digit = is_hexadecimal ? 0xFu : 9u;
    int exponent_adjustment = 0;
    bool found_digits = false;

    while (c == '0') {
        found_digits = true;
        c = source.get();
    }
    for (;; c = source.get()) {
        const unsigned d = detail::syntax_digit(c, syntax);
        if (d > max_digit) break;
        found_digits = true;
        if (fp.mantissa.size() < kMantissaDigits) fp.mantissa.push_back(static_cast<char>(d));
        ++exponent_adjustment;
    }
    if (c == '.') {  // the "C" locale's radix point
        c = source.get();
        if (fp.mantissa.empty()) {
            while (c == '0') {
                found_digits = true;
                --exponent_adjustment;
                c = source.get();
            }
        }
        for (;; c = source.get()) {
            const unsigned d = detail::syntax_digit(c, syntax);
            if (d > max_digit) break;
            found_digits = true;
            if (fp.mantissa.size() < kMantissaDigits) fp.mantissa.push_back(static_cast<char>(d));
        }
    }

    if (!found_digits) {
        if (!restore()) return Result::NoDigits;
        return is_hexadecimal ? Result::Zero : Result::NoDigits;
    }

    source.unget(c);
    stored_state = source.save_state();
    c = source.get();

    bool has_exponent = false;
    if (c == 'e' || c == 'E') has_exponent = !is_hexadecimal;
    if (c == 'p' || c == 'P') has_exponent = is_hexadecimal;

    int exponent = 0;
    if (has_exponent) {
        c = source.get();
        const bool exponent_negative = c == '-';
        if (c == '+' || c == '-') c = source.get();
        bool has_exponent_digits = false;
        while (c == '0') {
            has_exponent_digits = true;
            c = source.get();
        }
        for (;; c = source.get()) {
            const unsigned d = detail::syntax_digit(c, syntax);
            if (d >= 10) break;
            has_exponent_digits = true;
            exponent = exponent * 10 + static_cast<int>(d);
            if (exponent > kMaxTemporaryExponent) {
                exponent = kMaxTemporaryExponent + 1;
                break;
            }
        }
        while (detail::syntax_digit(c, syntax) < 10) c = source.get();
        if (exponent_negative) exponent = -exponent;
        if (!has_exponent_digits) {
            if (restore()) {
                c = source.get();  // re-read the exponent introducer
            } else {
                return Result::NoDigits;
            }
        }
    }

    source.unget(c);

    while (!fp.mantissa.empty() && fp.mantissa.back() == 0) fp.mantissa.pop_back();
    if (fp.mantissa.empty()) return Result::Zero;
    if (exponent > kMaxTemporaryExponent) return Result::Overflow;
    if (exponent < kMinTemporaryExponent) return Result::Underflow;
    exponent += exponent_adjustment * (is_hexadecimal ? 4 : 1);
    if (exponent > kMaxTemporaryExponent) return Result::Overflow;
    if (exponent < kMinTemporaryExponent) return Result::Underflow;
    fp.exponent = exponent;
    return is_hexadecimal ? Result::HexadecimalDigits : Result::DecimalDigits;
}

/// parse_floating_point_write_result as the requested type's bits.
struct Bits {
    u64 bits{0};
    /// SLD_OVERFLOW / SLD_UNDERFLOW: a finite subject that became +-inf or a
    /// nonzero one that became +-0 (strtod's ERANGE).
    bool range_error{false};
    /// SLD_NODIGITS: no subject sequence.
    bool no_digits{false};
};

/// @param rc the guest's MXCSR rounding control (0..3).
Bits to_bits(Result result, const FpString& fp, bool is_double, unsigned rc);

} // namespace vitreo::winapi::crt_float
