// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/inf_file.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace vitreo::winapi::setupapi_inf {

namespace {

[[nodiscard]] char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\x1a';
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_blank(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_blank(s.back())) s.remove_suffix(1);
    return s;
}

[[nodiscard]] std::string decode(std::span<const u8> bytes,
                                 std::string (*ansi_to_utf8)(std::string_view)) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::vector<u16> wide((bytes.size() - 2) / 2);
        for (usize i = 0; i < wide.size(); ++i) {
            wide[i] = static_cast<u16>(bytes[2 + 2 * i] | (bytes[3 + 2 * i] << 8));
        }
        return wide.empty() ? std::string{}
                            : utf16_to_utf8(wide.data(), static_cast<int>(wide.size()));
    }
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) return std::string(text.substr(3));
    return ansi_to_utf8 != nullptr ? ansi_to_utf8(text) : std::string(text);
}

/// End of the content of @p line: the first ';' outside quotes, or its end.
[[nodiscard]] usize content_end(std::string_view line) noexcept {
    bool quoted = false;
    for (usize i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        else if (line[i] == ';' && !quoted) return i;
    }
    return line.size();
}

/// Split a logical line into key and fields. Separators ('=' once, then ',')
/// count only outside quotes.
void split_line(std::string_view content, InfLine& out) {
    bool quoted = false;
    usize start = 0;
    for (usize i = 0; i <= content.size(); ++i) {
        const char c = i < content.size() ? content[i] : ',';
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted && i < content.size()) continue;
        if (c == '=' && !out.has_key && out.raw_fields.empty()) {
            out.has_key = true;
            out.raw_key = std::string(trim(content.substr(start, i - start)));
            out.raw_value = std::string(trim(content.substr(i + 1)));
            start = i + 1;
        } else if (c == ',') {
            out.raw_fields.emplace_back(trim(content.substr(start, i - start)));
            start = i + 1;
        }
    }
}

}  // namespace

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return lower(x) == lower(y); });
}

Result<InfFile, InfParseError> InfFile::parse(
    std::span<const u8> bytes, std::string (*ansi_to_utf8)(std::string_view),
    u16 ui_language) {
    using R = Result<InfFile, InfParseError>;
    const std::string text = decode(bytes, ansi_to_utf8);
    InfFile inf;
    InfSection* current = nullptr;
    std::string logical;
    u32 logical_first = 0;
    u32 number = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string_view raw(text.data() + pos, eol - pos);
        const bool last = eol >= text.size();
        pos = eol + 1;
        ++number;
        std::string_view content = trim(raw.substr(0, content_end(raw)));
        if (logical.empty()) logical_first = number;
        // A '\' ending a line's content (outside quotes) joins the next line.
        if (!content.empty() && content.back() == '\\' && !last) {
            logical.append(content.substr(0, content.size() - 1));
            continue;
        }
        logical.append(content);
        const std::string_view line = trim(logical);
        if (line.empty()) {
            logical.clear();
            if (last) break;
            continue;
        }
        if (line.front() == '[') {
            const usize close = line.find(']');
            if (close == std::string_view::npos) {
                return R::err({ERROR_BAD_SECTION_NAME_LINE, logical_first});
            }
            const std::string_view name = line.substr(1, close - 1);
            if (name.size() > kMaxSectionNameLength) {
                return R::err({ERROR_SECTION_NAME_TOO_LONG, logical_first});
            }
            current = nullptr;
            for (auto& s : inf.sections_) {
                if (iequals(s.name, name)) current = &s;
            }
            if (current == nullptr) {
                inf.sections_.push_back(InfSection{std::string(name), {}});
                current = &inf.sections_.back();
            }
        } else {
            if (current == nullptr) {
                return R::err({ERROR_EXPECTED_SECTION_NAME, logical_first});
            }
            InfLine parsed;
            parsed.line_number = logical_first;
            split_line(line, parsed);
            current->lines.push_back(std::move(parsed));
        }
        logical.clear();
        if (last) break;
    }
    if (inf.sections_.empty()) return R::err({ERROR_GENERAL_SYNTAX, 0});

    // One Strings section serves every token, chosen by rank.
    int best_rank = 0;
    for (usize i = 0; i < inf.sections_.size(); ++i) {
        const std::string_view name = inf.sections_[i].name;
        int rank = 0;
        if (iequals(name, "Strings")) {
            rank = 1;
        } else if (name.size() == 12 && iequals(name.substr(0, 8), "Strings.")) {
            const std::string hex(name.substr(8));
            if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
            const u16 id = static_cast<u16>(std::stoul(hex, nullptr, 16));
            if (id == ui_language) rank = 4;
            else if (id == (ui_language & 0x3FFu)) rank = 3;  // SUBLANG_NEUTRAL
            else if ((id & 0x3FFu) == (ui_language & 0x3FFu)) rank = 2;
        }
        if (rank > best_rank) {
            best_rank = rank;
            inf.strings_index_ = i;
        }
    }
    inf.style_ = INF_STYLE_OLDNT;
    if (const auto signature = inf.value("Version", "Signature")) {
        if (iequals(*signature, "$Windows NT$") || iequals(*signature, "$Chicago$") ||
            iequals(*signature, "$Windows 95$")) {
            inf.style_ = INF_STYLE_WIN4;
        }
    }
    return R::ok(std::move(inf));
}

const InfSection* InfFile::section(std::string_view name) const {
    for (const auto& s : sections_) {
        if (iequals(s.name, name)) return &s;
    }
    return nullptr;
}

std::string InfFile::resolve(std::string_view raw) const {
    std::string out;
    out.reserve(raw.size());
    bool quoted = false;
    for (usize i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '"') {
            if (quoted && i + 1 < raw.size() && raw[i + 1] == '"') {
                out.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
            continue;
        }
        if (c == '%') {
            const usize close = raw.find('%', i + 1);
            if (close != std::string_view::npos) {
                const std::string_view key = raw.substr(i + 1, close - i - 1);
                if (key.empty()) {
                    out.push_back('%');
                    i = close;
                    continue;
                }
                if (strings_index_) {
                    const InfSection& strings = sections_[*strings_index_];
                    const auto hit = std::find_if(
                        strings.lines.begin(), strings.lines.end(),
                        [&](const InfLine& l) { return l.has_key && iequals(resolve(l.raw_key), key); });
                    if (hit != strings.lines.end()) {
                        // A [Strings] value is the whole text after '=',
                        // commas included, with its quotes removed.
                        const std::string& value = hit->raw_value;
                        std::string literal;
                        bool q = false;
                        for (usize k = 0; k < value.size(); ++k) {
                            if (value[k] == '"') {
                                if (q && k + 1 < value.size() && value[k + 1] == '"') {
                                    literal.push_back('"');
                                    ++k;
                                } else {
                                    q = !q;
                                }
                                continue;
                            }
                            literal.push_back(value[k]);
                        }
                        out += literal;
                        i = close;
                        continue;
                    }
                }
                if (dirid_resolver_ && key.find_first_not_of("-0123456789") == std::string_view::npos &&
                    key.find_first_of("0123456789") != std::string_view::npos) {
                    const i64 id = std::strtoll(std::string(key).c_str(), nullptr, 10);
                    if (auto path = dirid_resolver_(id)) {
                        out += *path;
                        i = close;
                        continue;
                    }
                }
            }
        }
        out.push_back(c);
    }
    return out;
}

std::string InfFile::key(const InfLine& line) const {
    return line.has_key ? resolve(line.raw_key) : std::string{};
}

std::optional<std::string> InfFile::field(const InfLine& line, usize index) const {
    if (index == 0) {
        // Measured (SetupGetStringField): a keyless line has a field 0 only
        // when it has exactly one field, which then doubles as its key.
        if (line.has_key) return resolve(line.raw_key);
        if (line.raw_fields.size() != 1) return std::nullopt;
        return resolve(line.raw_fields[0]);
    }
    if (index > line.raw_fields.size()) return std::nullopt;
    return resolve(line.raw_fields[index - 1]);
}

std::optional<std::string> InfFile::value(std::string_view section_name,
                                          std::string_view key_name) const {
    const InfSection* s = section(section_name);
    if (s == nullptr) return std::nullopt;
    for (const auto& line : s->lines) {
        if (line.has_key && iequals(resolve(line.raw_key), key_name)) {
            if (line.raw_fields.empty()) return std::string{};
            return resolve(line.raw_fields[0]);
        }
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> InfFile::version_pairs() const {
    std::vector<std::pair<std::string, std::string>> pairs;
    const InfSection* s = section("Version");
    if (s == nullptr) return pairs;
    for (const auto& line : s->lines) {
        if (!line.has_key) continue;
        pairs.emplace_back(resolve(line.raw_key),
                           line.raw_fields.empty() ? std::string{} : resolve(line.raw_fields[0]));
    }
    return pairs;
}

}  // namespace vitreo::winapi::setupapi_inf
