// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file http_headers.hpp
/// @brief Request-header lists and response-header queries shared by WinHTTP
///        and WinINet.
///
/// Semantics measured on Windows 11 by tools/oracle_probe/http_request_probe.c
/// (WinHTTP and -DPROBE_WININET builds): header lines are CR/LF separated;
/// a line without ':' (or an empty block) is rejected; ADD appends even when
/// the name exists; ADD_IF_NEW fails with HEADER_ALREADY_EXISTS; COALESCE,
/// COALESCE_WITH_COMMA append ", value" and COALESCE_WITH_SEMICOLON "; value"
/// to the first header of that name (or add it); REPLACE replaces the first
/// header of that name in place, removes it when the new value is empty, and
/// when the name is absent adds it with ADD — WinHTTP fails with
/// HEADER_NOT_FOUND without ADD while WinINet adds it anyway. WinINet trims
/// the value; WinHTTP keeps it as written after the colon.

#include <vitreo/common.hpp>

#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi::http_headers {

using List = std::vector<std::pair<std::string, std::string>>;

inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

inline std::string trim(const std::string& s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

struct Line { std::string name; std::string value; };

/// Split a header block into lines. Returns nullopt when a non-empty line has
/// no ':' or an empty name, or when the block holds no header at all.
inline std::optional<std::vector<Line>> parse_block(const std::string& block, bool trim_value) {
    std::vector<Line> lines;
    usize pos = 0;
    while (pos <= block.size()) {
        usize end = block.find('\n', pos);
        if (end == std::string::npos) end = block.size();
        std::string line = block.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = end + 1;
        if (line.empty()) { if (end == block.size()) break; continue; }
        const usize colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return std::nullopt;
        std::string value = line.substr(colon + 1);
        // One separating space belongs to the syntax, not the value.
        if (trim_value) value = trim(value);
        else if (!value.empty() && value[0] == ' ') value.erase(0, 1);
        lines.push_back({line.substr(0, colon), std::move(value)});
        if (end == block.size()) break;
    }
    if (lines.empty()) return std::nullopt;
    return lines;
}

inline List::iterator find(List& list, const std::string& name) {
    for (auto it = list.begin(); it != list.end(); ++it)
        if (iequals(it->first, name)) return it;
    return list.end();
}

enum class ApplyResult { Ok, AlreadyExists, NotFound };

/// Apply one header with the documented dwModifiers (flag bits only).
inline ApplyResult apply(List& list, const Line& line, u32 flags, bool replace_adds_missing,
                         u32 add, u32 add_if_new, u32 coalesce, u32 comma, u32 semicolon,
                         u32 replace) {
    auto existing = find(list, line.name);
    if ((flags & add_if_new) != 0) {
        if (existing != list.end()) return ApplyResult::AlreadyExists;
        list.emplace_back(line.name, line.value);
        return ApplyResult::Ok;
    }
    if ((flags & replace) != 0) {
        if (existing != list.end()) {
            if (trim(line.value).empty()) list.erase(existing);
            else existing->second = line.value;
            return ApplyResult::Ok;
        }
        if ((flags & add) == 0 && !replace_adds_missing) return ApplyResult::NotFound;
        if (!trim(line.value).empty()) list.emplace_back(line.name, line.value);
        return ApplyResult::Ok;
    }
    if ((flags & (coalesce | comma | semicolon)) != 0 && existing != list.end()) {
        existing->second += ((flags & semicolon) != 0 && (flags & comma) == 0) ? "; " : ", ";
        existing->second += line.value;
        return ApplyResult::Ok;
    }
    list.emplace_back(line.name, line.value);
    return ApplyResult::Ok;
}

/// Leading decimal digits of @p text as a DWORD (FLAG_NUMBER; "12x" -> 12).
inline u32 leading_number(const std::string& text) {
    usize i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    u64 value = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
        value = (value * 10 + static_cast<u64>(text[i] - '0')) & 0xFFFFFFFFull;
    return static_cast<u32>(value);
}

struct SystemTime { u16 year, month, day_of_week, day, hour, minute, second, milliseconds; };

/// Parse an HTTP-date (RFC 1123, RFC 850 or asctime form).
inline std::optional<SystemTime> parse_http_date(const std::string& text) {
    static const char* const months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (c == ' ' || c == ',' || c == '-' || c == '\t') {
            if (!current.empty()) tokens.push_back(current), current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    int day = -1, month = -1, year = -1, hour = -1, minute = -1, second = -1;
    for (const auto& t : tokens) {
        if (t.find(':') != std::string::npos) {
            if (std::sscanf(t.c_str(), "%d:%d:%d", &hour, &minute, &second) != 3) return std::nullopt;
            continue;
        }
        std::string lower;
        for (char c : t) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        bool matched_month = false;
        for (int m = 0; m < 12; ++m)
            if (lower.rfind(months[m], 0) == 0 && lower.size() == 3) { month = m + 1; matched_month = true; }
        if (matched_month) continue;
        if (!t.empty() && std::isdigit(static_cast<unsigned char>(t[0]))) {
            const int v = std::atoi(t.c_str());
            if (t.size() <= 2 && day < 0) day = v;
            else year = t.size() == 2 ? (v < 70 ? 2000 + v : 1900 + v) : v;
        }
    }
    if (day < 1 || month < 1 || year < 1601 || hour < 0 || minute < 0 || second < 0) return std::nullopt;
    // Day of week (Zeller-style, 0 = Sunday).
    int y = year, mo = month;
    if (mo < 3) { mo += 12; --y; }
    const int k = y % 100, j = y / 100;
    const int h = (day + (13 * (mo + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;  // 0 = Saturday
    const int dow = (h + 6) % 7;
    return SystemTime{static_cast<u16>(year), static_cast<u16>(month), static_cast<u16>(dow),
                      static_cast<u16>(day), static_cast<u16>(hour), static_cast<u16>(minute),
                      static_cast<u16>(second), 0};
}

/// The response/request header name an info level selects: the SDK macro
/// suffix lower-cased with '_' -> '-' (WINHTTP_QUERY_CONTENT_TYPE ->
/// "content-type"). Header matching is case-insensitive.
inline std::string header_name_for_level(const char* suffix) {
    std::string name;
    for (const char* p = suffix; *p; ++p)
        name.push_back(*p == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    return name;
}

} // namespace vitreo::winapi::http_headers
