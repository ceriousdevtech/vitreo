#pragma once
// SPDX-License-Identifier: LGPL-2.1-only

/// @file ini_profile.hpp
/// @brief The one INI ("profile") file model behind every kernel32
///        Get/WritePrivateProfile* and Get/WriteProfile* export, A and W.
///
/// A pure model: bytes in, bytes out, no guest memory and no file system.
/// The file is kept as its lines; an edit rewrites only the lines it touches
/// and serialization writes every untouched line back byte for byte.
///
/// Every rule here is measured on Windows 11 (tools/oracle_probe/
/// profile_api_probe.cpp, tests/oracle_data/profile_api_probe.x86/x64.expected)
/// and is documented in .docs/contracts/kernel32.md § Profile (INI) files.

#include <vitreo/common.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::ini {

/// How a profile file stores its text. A file that starts with the UTF-16LE
/// BOM, or whose first unit is an ASCII character followed by a zero byte, is
/// UTF-16LE and stays UTF-16LE; every other file (a UTF-8 BOM included) is
/// ANSI text in the process code page.
enum class Encoding : u8 { Ansi, Utf16Le };

/// Trim the spaces and tabs Windows strips around names and values.
[[nodiscard]] std::u16string trim(std::u16string_view text);

/// Section and key names compare case-insensitively for ASCII letters only
/// (measured: "\xc9SEC" does not find "[\xe9sec]").
[[nodiscard]] bool names_equal(std::u16string_view a, std::u16string_view b) noexcept;

/// The value text a lookup returns: trimmed, then one pair of matching
/// single or double quotes (length >= 2) removed.
[[nodiscard]] std::u16string unquote(std::u16string_view raw_value);

class Document {
public:
    /// Parse a file's bytes. @p code_page decodes an ANSI file.
    [[nodiscard]] static Document parse(std::span<const u8> bytes, u32 code_page);

    /// The file's bytes: untouched lines exactly as read, edited and new
    /// lines encoded in the file's encoding (ANSI through @p code_page, with
    /// the code page's default character for anything it cannot map).
    [[nodiscard]] std::vector<u8> serialize(u32 code_page) const;

    [[nodiscard]] Encoding encoding() const noexcept { return m_encoding; }

    // ---- Queries (first matching section, first matching key) ----

    /// The value of @p key in @p section, unquoted; nullopt when absent.
    [[nodiscard]] std::optional<std::u16string> value(std::u16string_view section,
                                                      std::u16string_view key) const;
    /// Every section header in file order, duplicates and "[]" included.
    [[nodiscard]] std::vector<std::u16string> section_names() const;
    /// Every key of the section in order (duplicates included); nullopt when
    /// the section is absent.
    [[nodiscard]] std::optional<std::vector<std::u16string>> key_names(
        std::u16string_view section) const;
    /// GetPrivateProfileSection's entries: "key=value" with the key and value
    /// trimmed (quotes kept), lines without '=' trimmed, comments and blank
    /// lines left out; nullopt when the section is absent.
    [[nodiscard]] std::optional<std::vector<std::u16string>> section_entries(
        std::u16string_view section) const;

    // ---- Edits ----

    /// Replace the first matching key's value (the line keeps its text up to
    /// and including '='), else add "key=value" after the section's last
    /// non-blank line, else append "[section]" and the key at the end.
    void set_value(std::u16string_view section, std::u16string_view key,
                   std::u16string_view value);
    void delete_key(std::u16string_view section, std::u16string_view key);
    void delete_section(std::u16string_view section);
    /// WritePrivateProfileSection: a non-empty list replaces the section body
    /// verbatim; an empty list removes the section's keys only; a missing
    /// section is appended.
    void replace_section(std::u16string_view section,
                         const std::vector<std::u16string>& lines);

private:
    enum class Kind : u8 { Blank, Section, Comment, Key, Text };
    struct Line {
        std::u16string text;      ///< decoded content, terminator excluded
        std::u16string eol;       ///< "\r\n", "\n", "\r" or "" (last line)
        std::vector<u8> raw;      ///< original content bytes
        bool modified = false;    ///< text no longer matches raw
    };
    struct Classified {
        Kind kind = Kind::Blank;
        std::u16string name;      ///< section or key name, trimmed
        usize equals = 0;         ///< index of '=' for Kind::Key
    };

    [[nodiscard]] static Classified classify(std::u16string_view text);
    [[nodiscard]] std::optional<usize> find_section(std::u16string_view section) const;
    /// One past the last line of the section whose header is at @p header.
    [[nodiscard]] usize section_end(usize header) const;
    [[nodiscard]] std::optional<usize> find_key(usize header, std::u16string_view key) const;
    void insert_lines(usize at, const std::vector<std::u16string>& texts);
    void append_lines(const std::vector<std::u16string>& texts);

    Encoding m_encoding = Encoding::Ansi;
    std::vector<u8> m_prefix;  ///< UTF-16LE BOM bytes, kept as read
    std::vector<u8> m_tail;    ///< a trailing odd byte of a UTF-16 file
    std::vector<Line> m_lines;
};

} // namespace vitreo::winapi::ini
