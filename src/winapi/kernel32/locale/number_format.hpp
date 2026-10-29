#pragma once
/// @file number_format.hpp
/// @brief The number-string formatting of GetNumberFormatW / GetNumberFormatA
///        (Learn GetNumberFormatA, NUMBERFMTA), as measured on Windows
///        (tools/oracle_probe/getnumberformat_probe{32,64}.expected, gnfw-* /
///        gnfa-* tags).
///
/// Value grammar: digits, at most one '.', and a '-' only as the first
/// character; "", "-", "." and "-." format as zero (gnfw-us-null-fmt
/// value=""). The fraction is rounded half-up to NumDigits on the decimal
/// digits themselves, so arbitrarily long values format exactly
/// (value="999.995" -> 1,000.00); a result that is zero has no sign
/// (value="-0.004" -> 0.00).

#include <vitreo/common.hpp>

#include <string>
#include <string_view>

namespace vitreo::winapi {

/// The members of NUMBERFMTW, with the separators read.
struct NumberFormatSpec {
    u32 num_digits{2};
    u32 leading_zero{1};
    u32 grouping{3};
    std::u16string decimal{u"."};
    std::u16string thousand{u","};
    u32 negative_order{1};
};

/// True when @p spec is one Windows accepts: NumDigits 0-9, LeadingZero 0-1,
/// NegativeOrder 0-4, a decimal separator of 1-3 characters and a thousand
/// separator of 0-3, whatever NumDigits and Grouping are (gnfw-fmt-nd10,
/// -lz2, -no5, -decimal-len0/len4, -thousand-len4, -nd0-decimal-len4,
/// -gr0-thousand-len4). Every Grouping value is accepted (gr9, gr99, gr1230).
[[nodiscard]] bool number_format_spec_valid(const NumberFormatSpec& spec) noexcept;

/// Formats @p value under @p spec (which must be valid) into @p out, without
/// the terminating NUL. False when @p value breaks the grammar above.
///
/// Grouping is read as decimal digits: the most significant digit is the
/// size of the group nearest the decimal separator, each next digit the next
/// group; a final 0 stops grouping, otherwise the last size repeats (gr32
/// 1,23,45,67,89,012; gr320 1234567,89,012; gr10 12345678901,2; 0 none).
[[nodiscard]] bool format_number_string(std::u16string_view value, const NumberFormatSpec& spec,
                                        std::u16string& out);

}  // namespace vitreo::winapi
