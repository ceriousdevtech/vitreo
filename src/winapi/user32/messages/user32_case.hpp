#pragma once
/// @file user32_case.hpp
/// @brief USER's one-to-one character case mapping (CharUpper*/CharLower*).
///
/// Measured on Windows 11 (tools/oracle_probe/s2msg_case_probe) and generated
/// into user32_case_table.inc: it is neither towupper nor LCMapString's full
/// case mapping (U+0130 stays U+0130 under CharLowerBuffW).

#include <vitreo/common.hpp>
#include <algorithm>
#include <iterator>

namespace vitreo::winapi::user32_case {

struct CasePair {
    u16 from;
    u16 to;
};

#include "user32_case_table.inc"

template <usize N>
[[nodiscard]] inline u16 lookup(const CasePair (&table)[N], u16 unit) noexcept {
    const auto* end = std::end(table);
    const auto* it = std::lower_bound(std::begin(table), end, unit,
        [](const CasePair& p, u16 v) { return p.from < v; });
    return (it != end && it->from == unit) ? it->to : unit;
}

[[nodiscard]] inline u16 upper(u16 unit) noexcept { return lookup(kUpperW, unit); }
[[nodiscard]] inline u16 lower(u16 unit) noexcept { return lookup(kLowerW, unit); }
/// Code page 1252 bytes (the ANSI code page Vitreo defaults to).
[[nodiscard]] inline u8 upper_ansi(u8 byte) noexcept {
    return static_cast<u8>(lookup(kUpperA, byte));
}
[[nodiscard]] inline u8 lower_ansi(u8 byte) noexcept {
    return static_cast<u8>(lookup(kLowerA, byte));
}

} // namespace vitreo::winapi::user32_case
