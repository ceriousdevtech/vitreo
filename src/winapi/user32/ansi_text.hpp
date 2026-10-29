#pragma once

/// @file ansi_text.hpp
/// @brief CP_ACP <-> UTF-16 for the user32 *A entry points, through the
/// measured MultiByteToWideChar / WideCharToMultiByte tables.

#include <vitreo/common.hpp>
#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <string>
#include <vector>

namespace vitreo::winapi::ansi_text {

inline std::wstring to_wide(const std::string& ansi) {
    if (ansi.empty()) return {};
    std::vector<u16> buf(ansi.size() * 2 + 4, 0);
    const auto r = codepage::multi_byte_to_wide(
        codepage::kCpAcp, 0, reinterpret_cast<const u8*>(ansi.data()), ansi.size(),
        buf.data(), buf.size());
    std::wstring out;
    out.reserve(r.count);
    for (u32 i = 0; i < r.count; ++i) out.push_back(static_cast<wchar_t>(buf[i]));
    return out;
}

inline std::string to_ansi(const std::wstring& wide) {
    if (wide.empty()) return {};
    std::vector<u16> src;
    src.reserve(wide.size());
    for (wchar_t c : wide) src.push_back(static_cast<u16>(c));
    std::vector<u8> buf(wide.size() * 4 + 4, 0);
    const auto r = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, src.data(), src.size(),
                                                buf.data(), buf.size(), nullptr, false);
    return std::string(reinterpret_cast<const char*>(buf.data()), r.count);
}

} // namespace vitreo::winapi::ansi_text
