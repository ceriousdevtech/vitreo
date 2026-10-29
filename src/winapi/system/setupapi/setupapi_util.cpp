// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace vitreo::winapi::setupapi_internal {

void log_unimplemented_once(const char* api, const char* what) {
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(std::string(api) + '|' + what).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s: %s\n", api, what);
    }
}

// ---- strings ----------------------------------------------------------------

[[nodiscard]] char ascii_upper_char(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

[[nodiscard]] std::string ascii_upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = ascii_upper_char(c);
    return out;
}

[[nodiscard]] std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (ascii_upper_char(a[i]) != ascii_upper_char(b[i])) return false;
    }
    return true;
}

/// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" from a GUID in memory order.
[[nodiscard]] std::string guid_string(const Guid& g, bool upper) {
    char text[40];
    const u32 d1 = static_cast<u32>(g[0]) | (static_cast<u32>(g[1]) << 8) |
                   (static_cast<u32>(g[2]) << 16) | (static_cast<u32>(g[3]) << 24);
    const u32 d2 = static_cast<u32>(g[4]) | (static_cast<u32>(g[5]) << 8);
    const u32 d3 = static_cast<u32>(g[6]) | (static_cast<u32>(g[7]) << 8);
    std::snprintf(text, sizeof(text),
                  upper ? "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}"
                        : "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                  d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return text;
}

[[nodiscard]] std::optional<Guid> parse_guid_string(std::string_view text) {
    if (text.size() != 38 || text.front() != '{' || text.back() != '}') return std::nullopt;
    std::string hex;
    for (usize i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (text[i] != '-') return std::nullopt;
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
        hex.push_back(text[i]);
    }
    auto byte_at = [&hex](usize i) {
        return static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    };
    Guid g{};
    // Data1..Data3 are little-endian in memory; Data4 is a byte array.
    for (usize i = 0; i < 4; ++i) g[i] = byte_at(3 - i);
    g[4] = byte_at(5); g[5] = byte_at(4);
    g[6] = byte_at(7); g[7] = byte_at(6);
    for (usize i = 8; i < 16; ++i) g[i] = byte_at(i);
    return g;
}

/// A device instance ID is <enumerator>\<device-id>\<instance>: exactly three
/// non-empty components (s7_cfgmgr_probe locate_onepart/twopart/fourpart/
/// path/trailing all CR_INVALID_DEVICE_ID), no spaces, commas or controls,
/// shorter than MAX_DEVICE_ID_LEN (200).
[[nodiscard]] bool valid_instance_id(std::string_view id) {
    if (id.empty() || id.size() >= 200) return false;
    usize parts = 1;
    usize run = 0;
    for (const char c : id) {
        if (c == '\\') {
            if (run == 0) return false;
            ++parts;
            run = 0;
            continue;
        }
        if (static_cast<unsigned char>(c) <= 0x20 || c == ',') return false;
        ++run;
    }
    return parts == 3 && run != 0;
}

[[nodiscard]] std::string first_component(std::string_view id) {
    const auto pos = id.find('\\');
    return std::string(pos == std::string_view::npos ? id : id.substr(0, pos));
}

[[nodiscard]] std::vector<u8> utf16_bytes(std::string_view text) {
    std::vector<u16> units = utf8_to_utf16(text);
    units.push_back(0);
    std::vector<u8> bytes(units.size() * 2);
    std::memcpy(bytes.data(), units.data(), bytes.size());
    return bytes;
}

[[nodiscard]] std::vector<u8> utf16_multi_bytes(const std::vector<std::string>& list) {
    std::vector<u8> bytes;
    for (const auto& item : list) {
        const auto one = utf16_bytes(item);
        bytes.insert(bytes.end(), one.begin(), one.end());
    }
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

[[nodiscard]] std::optional<std::string> read_guest_string(
    Marshaler& m, GuestAddr ptr, bool wide) {
    if (ptr == 0) return std::nullopt;
    auto value = wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
    if (value.is_err()) return std::nullopt;
    return value.value();
}

[[nodiscard]] std::optional<Guid> read_guid(Marshaler& m, GuestAddr ptr) {
    if (ptr == 0) return std::nullopt;
    Guid g{};
    if (m.memory().read_bytes(ptr, g.data(), g.size()).is_err()) return std::nullopt;
    return g;
}

} // namespace vitreo::winapi::setupapi_internal
