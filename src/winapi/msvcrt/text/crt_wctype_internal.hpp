#pragma once
/// @file crt_wctype_internal.hpp
/// @brief Wide-character classification shared across msvcrt/text.
///
/// Private to msvcrt's string implementation. Defined in crt_wctype.cpp; the
/// two helpers below are the only part of the wide-classification block that
/// other translation units need (the strtol parser, iswxdigit and iswctype).

#include <vitreo/common.hpp>

namespace vitreo::winapi {

/// iswxdigit as Windows answers it: ASCII hex digits plus their FULLWIDTH forms.
[[nodiscard]] bool win_iswxdigit(u32 c) noexcept;

/// The MSVC ctype property bits (_UPPER, _LOWER, _DIGIT, ...) for a wide
/// character; iswctype(c, desc) is a bitwise AND against these.
[[nodiscard]] u32 win_wctype_bits(u32 c) noexcept;

} // namespace vitreo::winapi
