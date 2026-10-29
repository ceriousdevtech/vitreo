/// @file path_encoding.cpp

#include "capsule/vfs/path_encoding.hpp"

#include <vitreo/common.hpp>

namespace vitreo::capsule {
namespace {

// Windows-1252 C1 range (0x80–0x9F). 0xA0–0xFF map 1:1 to Unicode.
constexpr u16 kCp1252_80_9F[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

[[nodiscard]] bool is_valid_utf8(std::string_view s) noexcept {
    usize i = 0;
    while (i < s.size()) {
        const auto b = static_cast<u8>(s[i]);
        usize need = 0;
        if (b < 0x80) {
            ++i;
            continue;
        }
        if ((b & 0xE0) == 0xC0) {
            need = 1;
            if ((b & 0xFE) == 0xC0) return false;  // overlong
        } else if ((b & 0xF0) == 0xE0) {
            need = 2;
        } else if ((b & 0xF8) == 0xF0) {
            need = 3;
            if (b > 0xF4) return false;
        } else {
            return false;
        }
        if (i + need >= s.size()) return false;
        for (usize j = 1; j <= need; ++j) {
            if ((static_cast<u8>(s[i + j]) & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

[[nodiscard]] void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::string guest_path_to_host_utf8(std::string_view path) {
    if (path.empty() || is_valid_utf8(path)) {
        return std::string(path);
    }
    return ansi_to_utf8(path);
}

std::string ansi_to_utf8(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (unsigned char b : path) {
        u32 cp = b;
        if (b >= 0x80 && b <= 0x9F) {
            cp = kCp1252_80_9F[b - 0x80];
        }
        // 0xA0–0xFF are Latin-1 / CP1252 identical to Unicode.
        append_utf8(out, cp);
    }
    return out;
}

}  // namespace vitreo::capsule
