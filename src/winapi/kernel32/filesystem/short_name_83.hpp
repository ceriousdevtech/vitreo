// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file short_name_83.hpp
/// @brief Generate NTFS-style 8.3 short names (cAlternateFileName / ShortName).
///
/// Basis and filtering follow the public Windows NT `RtlGenerate8dot3Name` /
/// `GetNextWchar` contract (Microsoft Windows Research Kernel gen8dot3.c).
/// The LFN checksum used when the basis is at most two characters is the
/// Windows 10/11 form measured on Windows (`ntdll!RtlGenerate8dot3Name`):
/// uint16 fold with multiply-by-0x25, then abs(fold*0x12b9b0a5) % 1000000007.
/// Extensions are truncated to three characters without the "safe extension"
/// tilde replacement, matching FindFirstFileW / NTFS short names on the
/// oracle (e.g. long_name_file.text -> LONG_N~1.TEX).

#include "winapi/kernel32/locale/string_conv.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::short_name_83 {
namespace detail {

inline bool fat_illegal_ascii(char16_t wc) noexcept {
    static constexpr const char* kIllegal = "\"*+/,:;<=>?\\[]|";
    if (wc >= 0x80) {
        return false;
    }
    for (const char* p = kIllegal; *p; ++p) {
        if (static_cast<unsigned char>(*p) == wc) {
            return true;
        }
    }
    return false;
}

inline char16_t next_wchar(const std::vector<char16_t>& name, usize& index,
                           bool skip_dots) noexcept {
    while (index < name.size()) {
        char16_t wc = name[index++];
        if (wc <= u' ' || wc >= 127 || (wc == u'.' && skip_dots)) {
            continue;
        }
        if (fat_illegal_ascii(wc)) {
            wc = u'_';
        }
        if (wc >= u'a' && wc <= u'z') {
            wc = static_cast<char16_t>(wc - u'a' + u'A');
        }
        return wc;
    }
    return 0;
}

inline u16 lfn_checksum(const std::vector<char16_t>& name) noexcept {
    u32 accum = 0;
    for (char16_t wc : name) {
        accum = ((accum * 0x25u) + wc) & 0xffffu;
    }
    constexpr u32 kMagic = 0x12b9b0a5u;
    constexpr u32 kMod = 1000000007u;
    const i32 scaled = static_cast<i32>(static_cast<u32>(accum * kMagic));
    const u32 abs_scaled =
        scaled < 0 ? static_cast<u32>(-static_cast<i64>(scaled))
                   : static_cast<u32>(scaled);
    return static_cast<u16>(abs_scaled % kMod);
}

inline bool is_legal_dos_8dot3(const std::vector<char16_t>& name) noexcept {
    if (name.size() == 1 && name[0] == u'.') {
        return true;
    }
    if (name.size() == 2 && name[0] == u'.' && name[1] == u'.') {
        return true;
    }
    if (name.size() > 12) {
        return false;
    }
    usize dot = static_cast<usize>(-1);
    usize dots = 0;
    for (usize i = 0; i < name.size(); ++i) {
        const char16_t wc = name[i];
        if (wc > 0x7f || wc <= u' ') {
            return false;
        }
        if (fat_illegal_ascii(wc) && wc != u'.') {
            return false;
        }
        if (wc == u'.') {
            ++dots;
            dot = i;
            if (dots > 1) {
                return false;
            }
        }
    }
    if (dots == 0) {
        return name.size() <= 8;
    }
    if (dot == 0 || dot == name.size() - 1) {
        return name.size() <= 12;
    }
    return dot <= 8 && (name.size() - dot - 1) <= 3;
}

} // namespace detail

/// Return the 8.3 alternate name as UTF-16 (no NUL), or empty when Windows
/// would leave cAlternateFileName / ShortName empty.
inline std::vector<u16> generate(std::string_view utf8_name) {
    const std::vector<u16> wide = utf8_to_utf16(utf8_name);
    std::vector<char16_t> name(wide.begin(), wide.end());
    if (name.empty() || detail::is_legal_dos_8dot3(name)) {
        return {};
    }

    usize last_dot = static_cast<usize>(-1);
    usize index = 0;
    bool skip_dots = !name.empty() && name[0] == u'.';
    while (char16_t wc = detail::next_wchar(name, index, skip_dots)) {
        skip_dots = false;
        if (wc == u'.') {
            last_dot = index;
        }
    }
    if (last_dot == name.size()) {
        last_dot = static_cast<usize>(-1);
    }

    const usize last_dot_index =
        last_dot == static_cast<usize>(-1) ? name.size() + 1 : last_dot;

    char16_t basis[8]{};
    u32 basis_len = 0;
    index = 0;
    while (basis_len < 6) {
        const usize before = index;
        const char16_t wc = detail::next_wchar(name, index, true);
        if (wc == 0 || before >= last_dot_index) {
            break;
        }
        if (index > last_dot_index) {
            break;
        }
        basis[basis_len++] = wc;
    }

    if (basis_len <= 2) {
        u16 checksum = detail::lfn_checksum(name);
        for (u32 i = 0; i < 4; ++i) {
            const u16 nibble = static_cast<u16>(checksum & 0xfu);
            basis[basis_len + i] = static_cast<char16_t>(
                nibble <= 9 ? u'0' + nibble : u'A' + (nibble - 10));
            checksum >>= 4;
        }
        basis_len += 4;
    }

    char16_t ext[4]{};
    u32 ext_len = 0;
    if (last_dot != static_cast<usize>(-1)) {
        ext[0] = u'.';
        ext_len = 1;
        index = last_dot;
        while (ext_len < 4) {
            const char16_t wc = detail::next_wchar(name, index, true);
            if (wc == 0) {
                break;
            }
            ext[ext_len++] = wc;
        }
    }

    std::vector<u16> out;
    out.reserve(12);
    for (u32 i = 0; i < basis_len; ++i) {
        out.push_back(static_cast<u16>(basis[i]));
    }
    out.push_back(u'~');
    out.push_back(u'1');
    for (u32 i = 0; i < ext_len; ++i) {
        out.push_back(static_cast<u16>(ext[i]));
    }
    return out;
}

} // namespace vitreo::winapi::short_name_83
