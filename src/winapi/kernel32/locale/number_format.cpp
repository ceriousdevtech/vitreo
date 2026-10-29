// SPDX-License-Identifier: LGPL-2.1-only
/// @file number_format.cpp
/// @brief GetNumberFormat's number-string formatting (number_format.hpp).

#include "winapi/kernel32/locale/number_format.hpp"

#include <vector>

namespace vitreo::winapi {

bool number_format_spec_valid(const NumberFormatSpec& spec) noexcept {
    return spec.num_digits <= 9 && spec.leading_zero <= 1 && spec.negative_order <= 4 &&
           !spec.decimal.empty() && spec.decimal.size() <= 3 && spec.thousand.size() <= 3;
}

bool format_number_string(std::u16string_view value, const NumberFormatSpec& spec, std::u16string& out) {
    // Grammar.
    bool negative = false;
    usize pos = 0;
    if (!value.empty() && value[0] == u'-') {
        negative = true;
        pos = 1;
    }
    std::u16string int_digits;
    std::u16string frac_digits;
    bool seen_dot = false;
    for (; pos < value.size(); ++pos) {
        const char16_t c = value[pos];
        if (c == u'.') {
            if (seen_dot) return false;
            seen_dot = true;
        } else if (c >= u'0' && c <= u'9') {
            (seen_dot ? frac_digits : int_digits).push_back(c);
        } else {
            return false;
        }
    }

    // Round the fraction half-up to NumDigits.
    const usize nd = spec.num_digits;
    std::u16string digits = int_digits;
    std::u16string frac = frac_digits.substr(0, nd < frac_digits.size() ? nd : frac_digits.size());
    while (frac.size() < nd) frac.push_back(u'0');
    digits += frac;   // all kept digits, integer then fraction
    if (frac_digits.size() > nd && frac_digits[nd] >= u'5') {
        usize k = digits.size();
        bool carry = true;
        while (carry && k > 0) {
            --k;
            if (digits[k] == u'9') {
                digits[k] = u'0';
            } else {
                ++digits[k];
                carry = false;
            }
        }
        if (carry) digits.insert(digits.begin(), u'1');
    }
    const usize int_len = digits.size() - nd;
    std::u16string int_part = digits.substr(0, int_len);
    const std::u16string frac_part = digits.substr(int_len);
    usize first = 0;
    while (first < int_part.size() && int_part[first] == u'0') ++first;
    int_part.erase(0, first);

    bool zero = int_part.empty();
    for (char16_t c : frac_part) {
        if (c != u'0') zero = false;
    }
    if (zero) negative = false;

    // Integer part with the thousand separators.
    std::vector<u32> groups;
    for (u32 g = spec.grouping; ; g /= 10) {
        groups.insert(groups.begin(), g % 10);
        if (g < 10) break;
    }
    // groups[0] is the most significant digit: the group nearest the point.
    bool repeat_last = true;
    if (groups.size() > 1 && groups.back() == 0) {
        groups.pop_back();
        repeat_last = false;
    }
    if (groups.size() == 1 && groups[0] == 0) groups.clear();

    std::u16string grouped;
    if (int_part.empty()) {
        if (spec.leading_zero != 0 || nd == 0) grouped = u"0";
    } else if (groups.empty()) {
        grouped = int_part;
    } else {
        // Walk from the decimal point leftwards.
        std::vector<std::u16string> chunks;
        usize end = int_part.size();
        usize gi = 0;
        while (end > 0) {
            u32 size = 0;
            if (gi < groups.size()) {
                size = groups[gi];
            } else if (repeat_last) {
                size = groups.back();
            }
            ++gi;
            if (size == 0 || size >= end) {
                chunks.push_back(int_part.substr(0, end));
                break;
            }
            chunks.push_back(int_part.substr(end - size, size));
            end -= size;
        }
        for (usize k = chunks.size(); k-- > 0;) {
            grouped += chunks[k];
            if (k != 0) grouped += spec.thousand;
        }
    }

    std::u16string number = grouped;
    if (nd > 0) {
        number += spec.decimal;
        number += frac_part;
    }

    out.clear();
    if (!negative) {
        out = number;
        return true;
    }
    switch (spec.negative_order) {
    case 0: out = u"(" + number + u")"; break;
    case 1: out = u"-" + number; break;
    case 2: out = u"- " + number; break;
    case 3: out = number + u"-"; break;
    default: out = number + u" -"; break;
    }
    return true;
}

}  // namespace vitreo::winapi
