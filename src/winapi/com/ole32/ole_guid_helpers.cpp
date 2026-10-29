// SPDX-License-Identifier: LGPL-2.1-only

/// @file ole_guid_helpers.cpp
/// @brief Shared GUID compare/parse/format helpers for OLE32 TUs.

#include "ole_guid_internal.hpp"

#include <cstring>
#include <string>
#include <string_view>

namespace vitreo::winapi {

namespace {

[[nodiscard]] int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

template<typename T>
[[nodiscard]] bool parse_hex(std::string_view s, T& out) {
    T value = 0;
    for (char c : s) {
        const int hv = hex_value(c);
        if (hv < 0) return false;
        value = static_cast<T>((value << 4) | static_cast<T>(hv));
    }
    out = value;
    return true;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

bool guid_equal(const Guid& a, const Guid& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
}

bool parse_guid_string(std::string_view s, Guid& out) {
    s = trim_ascii(s);
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
        s.remove_prefix(1);
        s.remove_suffix(1);
    }

    if (s.size() != 36) {
        return false;
    }
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') {
        return false;
    }

    u32 d1 = 0;
    u16 d2 = 0;
    u16 d3 = 0;
    u16 d4a = 0;
    u64 d4b = 0;

    if (!parse_hex<u32>(s.substr(0, 8), d1)) return false;
    if (!parse_hex<u16>(s.substr(9, 4), d2)) return false;
    if (!parse_hex<u16>(s.substr(14, 4), d3)) return false;
    if (!parse_hex<u16>(s.substr(19, 4), d4a)) return false;
    if (!parse_hex<u64>(s.substr(24, 12), d4b)) return false;

    Guid g{};
    g.Data1 = d1;
    g.Data2 = d2;
    g.Data3 = d3;
    g.Data4[0] = static_cast<u8>((d4a >> 8) & 0xFF);
    g.Data4[1] = static_cast<u8>(d4a & 0xFF);
    // d4b is 48 bits; place into remaining 6 bytes big-endian.
    for (int i = 0; i < 6; ++i) {
        const int shift = (5 - i) * 8;
        g.Data4[2 + i] = static_cast<u8>((d4b >> shift) & 0xFF);
    }

    out = g;
    return true;
}

std::string format_guid(const Guid& g) {
    auto to_hex = [](u64 value, int width) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.resize(static_cast<usize>(width));
        for (int i = width - 1; i >= 0; --i) {
            out[static_cast<usize>(i)] = kHex[value & 0xF];
            value >>= 4;
        }
        return out;
    };

    const u16 d4a = static_cast<u16>((static_cast<u16>(g.Data4[0]) << 8) | g.Data4[1]);
    u64 d4b = 0;
    for (int i = 0; i < 6; ++i) {
        d4b = (d4b << 8) | static_cast<u64>(g.Data4[2 + i]);
    }

    std::string s;
    s.reserve(38);
    s.push_back('{');
    s += to_hex(g.Data1, 8);
    s.push_back('-');
    s += to_hex(g.Data2, 4);
    s.push_back('-');
    s += to_hex(g.Data3, 4);
    s.push_back('-');
    s += to_hex(d4a, 4);
    s.push_back('-');
    s += to_hex(d4b, 12);
    s.push_back('}');
    return s;
}

} // namespace vitreo::winapi
