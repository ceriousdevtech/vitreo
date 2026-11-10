// SPDX-License-Identifier: LGPL-2.1-only

/// @file ini_profile.cpp
/// @brief INI profile file model (see ini_profile.hpp for the contract).

#include "winapi/kernel32/environment/ini_profile.hpp"

#include "winapi/kernel32/locale/codepage_convert.hpp"

namespace vitreo::winapi::ini {

namespace {

[[nodiscard]] bool is_blank(char16_t c) noexcept { return c == u' ' || c == u'\t'; }

[[nodiscard]] char16_t fold_ascii(char16_t c) noexcept {
    return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + (u'a' - u'A')) : c;
}

[[nodiscard]] std::u16string decode_ansi(const u8* bytes, usize size, u32 code_page) {
    if (size == 0) return {};
    std::u16string out(size, u'\0');  // single-byte and UTF-8 never expand
    const auto r = codepage::multi_byte_to_wide(
        code_page, 0, bytes, size, reinterpret_cast<u16*>(out.data()), out.size());
    out.resize(r.ok() ? r.count : 0);
    return out;
}

void encode(std::u16string_view text, Encoding encoding, u32 code_page,
            std::vector<u8>& out) {
    if (text.empty()) return;
    if (encoding == Encoding::Utf16Le) {
        for (char16_t c : text) {
            out.push_back(static_cast<u8>(c & 0xFF));
            out.push_back(static_cast<u8>(c >> 8));
        }
        return;
    }
    const auto* src = reinterpret_cast<const u16*>(text.data());
    const auto need = codepage::wide_to_multi_byte(code_page, 0, src, text.size(),
                                                   nullptr, 0, nullptr, false);
    if (!need.ok() || need.count == 0) return;
    const usize base = out.size();
    out.resize(base + need.count);
    const auto r = codepage::wide_to_multi_byte(code_page, 0, src, text.size(),
                                                out.data() + base, need.count,
                                                nullptr, false);
    out.resize(base + (r.ok() ? r.count : 0));
}

const std::u16string kCrLf = u"\r\n";

} // namespace

std::u16string trim(std::u16string_view text) {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && is_blank(text[begin])) ++begin;
    while (end > begin && is_blank(text[end - 1])) --end;
    return std::u16string(text.substr(begin, end - begin));
}

bool names_equal(std::u16string_view a, std::u16string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (fold_ascii(a[i]) != fold_ascii(b[i])) return false;
    }
    return true;
}

std::u16string unquote(std::u16string_view raw_value) {
    std::u16string v = trim(raw_value);
    if (v.size() >= 2 && (v.front() == u'"' || v.front() == u'\'') &&
        v.back() == v.front()) {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

Document Document::parse(std::span<const u8> bytes, u32 code_page) {
    Document doc;
    usize pos = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        doc.m_encoding = Encoding::Utf16Le;
        doc.m_prefix.assign(bytes.begin(), bytes.begin() + 2);
        pos = 2;
    } else if (bytes.size() >= 2 && bytes[0] != 0 && bytes[0] < 0x80 && bytes[1] == 0) {
        doc.m_encoding = Encoding::Utf16Le;
    }

    if (doc.m_encoding == Encoding::Utf16Le) {
        const usize units_end = pos + ((bytes.size() - pos) / 2) * 2;
        Line line;
        auto flush = [&](std::u16string eol) {
            line.eol = std::move(eol);
            doc.m_lines.push_back(std::move(line));
            line = Line{};
        };
        while (pos < units_end) {
            const char16_t c = static_cast<char16_t>(bytes[pos] | (bytes[pos + 1] << 8));
            if (c == u'\r' || c == u'\n') {
                const bool crlf = c == u'\r' && pos + 3 < units_end &&
                                  bytes[pos + 2] == '\n' && bytes[pos + 3] == 0;
                pos += crlf ? 4 : 2;
                flush(crlf ? kCrLf : std::u16string(1, c));
                continue;
            }
            line.text.push_back(c);
            line.raw.push_back(bytes[pos]);
            line.raw.push_back(bytes[pos + 1]);
            pos += 2;
        }
        if (!line.text.empty()) flush({});
        doc.m_tail.assign(bytes.begin() + units_end, bytes.end());
        return doc;
    }

    while (pos < bytes.size()) {
        usize end = pos;
        while (end < bytes.size() && bytes[end] != '\r' && bytes[end] != '\n') ++end;
        Line line;
        line.raw.assign(bytes.begin() + pos, bytes.begin() + end);
        line.text = decode_ansi(bytes.data() + pos, end - pos, code_page);
        if (end < bytes.size()) {
            if (bytes[end] == '\r' && end + 1 < bytes.size() && bytes[end + 1] == '\n') {
                line.eol = kCrLf;
                end += 2;
            } else {
                line.eol = std::u16string(1, static_cast<char16_t>(bytes[end]));
                end += 1;
            }
        }
        doc.m_lines.push_back(std::move(line));
        pos = end;
    }
    return doc;
}

std::vector<u8> Document::serialize(u32 code_page) const {
    std::vector<u8> out = m_prefix;
    for (const auto& line : m_lines) {
        if (line.modified) {
            encode(line.text, m_encoding, code_page, out);
        } else {
            out.insert(out.end(), line.raw.begin(), line.raw.end());
        }
        // Measured: a rewritten file always ends its last line, even when
        // the edit was elsewhere (writeA.replace_formatted adds CRLF after
        // the untouched final "t=1").
        encode(line.eol.empty() ? kCrLf : line.eol, m_encoding, code_page, out);
    }
    out.insert(out.end(), m_tail.begin(), m_tail.end());
    return out;
}

Document::Classified Document::classify(std::u16string_view text) {
    Classified c;
    usize i = 0;
    while (i < text.size() && is_blank(text[i])) ++i;
    if (i == text.size()) return c;
    if (text[i] == u'[') {
        c.kind = Kind::Section;
        const usize close = text.find(u']', i + 1);
        const usize end = close == std::u16string_view::npos ? text.size() : close;
        c.name = trim(text.substr(i + 1, end - i - 1));
        return c;
    }
    if (text[i] == u';') {
        c.kind = Kind::Comment;
        return c;
    }
    const usize equals = text.find(u'=', i);
    if (equals == std::u16string_view::npos) {
        c.kind = Kind::Text;
        return c;
    }
    c.kind = Kind::Key;
    c.equals = equals;
    c.name = trim(text.substr(i, equals - i));
    return c;
}

std::optional<usize> Document::find_section(std::u16string_view section) const {
    const std::u16string wanted = trim(section);
    for (usize i = 0; i < m_lines.size(); ++i) {
        const auto c = classify(m_lines[i].text);
        if (c.kind == Kind::Section && names_equal(c.name, wanted)) return i;
    }
    return std::nullopt;
}

usize Document::section_end(usize header) const {
    usize i = header + 1;
    while (i < m_lines.size() && classify(m_lines[i].text).kind != Kind::Section) ++i;
    return i;
}

std::optional<usize> Document::find_key(usize header, std::u16string_view key) const {
    const std::u16string wanted = trim(key);
    const usize end = section_end(header);
    for (usize i = header + 1; i < end; ++i) {
        const auto c = classify(m_lines[i].text);
        if (c.kind == Kind::Key && names_equal(c.name, wanted)) return i;
    }
    return std::nullopt;
}

std::optional<std::u16string> Document::value(std::u16string_view section,
                                              std::u16string_view key) const {
    const auto header = find_section(section);
    if (!header) return std::nullopt;
    const auto at = find_key(*header, key);
    if (!at) return std::nullopt;
    const auto& text = m_lines[*at].text;
    return unquote(std::u16string_view(text).substr(classify(text).equals + 1));
}

std::vector<std::u16string> Document::section_names() const {
    std::vector<std::u16string> names;
    for (const auto& line : m_lines) {
        auto c = classify(line.text);
        if (c.kind == Kind::Section) names.push_back(std::move(c.name));
    }
    return names;
}

std::optional<std::vector<std::u16string>> Document::key_names(
    std::u16string_view section) const {
    const auto header = find_section(section);
    if (!header) return std::nullopt;
    std::vector<std::u16string> keys;
    const usize end = section_end(*header);
    for (usize i = *header + 1; i < end; ++i) {
        auto c = classify(m_lines[i].text);
        if (c.kind == Kind::Key) keys.push_back(std::move(c.name));
    }
    return keys;
}

std::optional<std::vector<std::u16string>> Document::section_entries(
    std::u16string_view section) const {
    const auto header = find_section(section);
    if (!header) return std::nullopt;
    std::vector<std::u16string> entries;
    const usize end = section_end(*header);
    for (usize i = *header + 1; i < end; ++i) {
        const auto& text = m_lines[i].text;
        const auto c = classify(text);
        if (c.kind == Kind::Key) {
            entries.push_back(c.name + u"=" +
                              trim(std::u16string_view(text).substr(c.equals + 1)));
        } else if (c.kind == Kind::Text) {
            entries.push_back(trim(text));
        }
    }
    return entries;
}

void Document::insert_lines(usize at, const std::vector<std::u16string>& texts) {
    // The line before a new line must end; the file's last line may not.
    if (at > 0 && m_lines[at - 1].eol.empty()) m_lines[at - 1].eol = kCrLf;
    std::vector<Line> fresh;
    fresh.reserve(texts.size());
    for (const auto& text : texts) {
        Line line;
        line.text = text;
        line.eol = kCrLf;
        line.modified = true;
        fresh.push_back(std::move(line));
    }
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(at),
                   std::make_move_iterator(fresh.begin()),
                   std::make_move_iterator(fresh.end()));
}

void Document::append_lines(const std::vector<std::u16string>& texts) {
    insert_lines(m_lines.size(), texts);
}

void Document::set_value(std::u16string_view section, std::u16string_view key,
                         std::u16string_view value) {
    const std::u16string entry = trim(key) + u"=" + std::u16string(value);
    const auto header = find_section(section);
    if (!header) {
        append_lines({u"[" + std::u16string(section) + u"]", entry});
        return;
    }
    if (const auto at = find_key(*header, key)) {
        Line& line = m_lines[*at];
        line.text = line.text.substr(0, classify(line.text).equals + 1) +
                    std::u16string(value);
        line.modified = true;
        return;
    }
    usize insert_at = *header + 1;
    const usize end = section_end(*header);
    for (usize i = *header + 1; i < end; ++i) {
        if (classify(m_lines[i].text).kind != Kind::Blank) insert_at = i + 1;
    }
    insert_lines(insert_at, {entry});
}

void Document::delete_key(std::u16string_view section, std::u16string_view key) {
    const auto header = find_section(section);
    if (!header) return;
    if (const auto at = find_key(*header, key)) {
        m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(*at));
    }
}

void Document::delete_section(std::u16string_view section) {
    const auto header = find_section(section);
    if (!header) return;
    const usize end = section_end(*header);
    m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(*header),
                  m_lines.begin() + static_cast<std::ptrdiff_t>(end));
}

void Document::replace_section(std::u16string_view section,
                               const std::vector<std::u16string>& lines) {
    const auto header = find_section(section);
    if (!header) {
        std::vector<std::u16string> texts{u"[" + std::u16string(section) + u"]"};
        texts.insert(texts.end(), lines.begin(), lines.end());
        append_lines(texts);
        return;
    }
    const usize end = section_end(*header);
    if (lines.empty()) {
        // Measured: an empty list removes the section's keys and keeps its
        // other lines (writeSectionA.empty_list).
        for (usize i = end; i > *header + 1; --i) {
            if (classify(m_lines[i - 1].text).kind == Kind::Key) {
                m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }
        return;
    }
    m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(*header + 1),
                  m_lines.begin() + static_cast<std::ptrdiff_t>(end));
    insert_lines(*header + 1, lines);
}

} // namespace vitreo::winapi::ini
