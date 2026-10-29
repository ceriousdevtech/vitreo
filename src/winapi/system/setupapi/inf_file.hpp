// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file inf_file.hpp
/// @brief The INF file format SetupAPI reads: sections, lines, fields and
///        %strkey% substitution. Bytes in, parsed text out; no guest memory,
///        no VFS.
///
/// Syntax rules: "General Syntax Rules for INF Files" (Learn,
/// windows-hardware/drivers/install/general-syntax-rules-for-inf-files).
/// Error codes and the INF-style decision were measured with
/// tests/winapi/setupapi/oracle/setupapi_inf_probe.c (SetupOpenInfFileA):
/// a file with no section is ERROR_GENERAL_SYNTAX, an unterminated section
/// header is ERROR_BAD_SECTION_NAME_LINE at its 1-based line, and a file is
/// INF_STYLE_WIN4 when its [Version] Signature is $Windows NT$, $Chicago$ or
/// $Windows 95$ (any case), else INF_STYLE_OLDNT.

#include <vitreo/common.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vitreo::winapi::setupapi_inf {

/// INF_STYLE_* (SetupAPI.h).
constexpr u32 INF_STYLE_NONE = 0x0;
constexpr u32 INF_STYLE_OLDNT = 0x1;
constexpr u32 INF_STYLE_WIN4 = 0x2;

/// INF syntax errors (SetupAPI.h / winerror.h).
constexpr u32 ERROR_EXPECTED_SECTION_NAME = 0xE0000000u;
constexpr u32 ERROR_BAD_SECTION_NAME_LINE = 0xE0000001u;
constexpr u32 ERROR_SECTION_NAME_TOO_LONG = 0xE0000002u;
constexpr u32 ERROR_GENERAL_SYNTAX = 0xE0000003u;

/// Longest section name SetupAPI accepts (MAX_SECT_NAME_LEN, SetupAPI.h).
constexpr usize kMaxSectionNameLength = 255;

/// One line of a section. Fields are kept as written (quotes and %strkey%
/// tokens intact, surrounding blanks trimmed); InfFile::resolve turns one
/// into its value. A line without an unquoted '=' has no key.
struct InfLine {
    bool has_key = false;
    std::string raw_key;
    std::vector<std::string> raw_fields;
    std::string raw_value;  ///< everything after '=', trimmed (a [Strings] value)
    u32 line_number = 0;  ///< 1-based, first physical line of the logical line
};

struct InfSection {
    std::string name;  ///< as first written
    std::vector<InfLine> lines;
};

struct InfParseError {
    u32 error = 0;
    u32 line = 0;  ///< 1-based line of the error; 0 when not line-specific
};

class InfFile {
public:
    /// Parse a whole INF. UTF-16LE with a BOM is decoded; a UTF-8 BOM is
    /// stripped; anything else is ANSI, decoded through @p ansi_to_utf8.
    /// @p ui_language picks the one Strings section every token comes from
    /// (Learn, INF Strings section): [Strings.LLLL] for that LANGID, then
    /// the primary language with SUBLANG_NEUTRAL, then any section of that
    /// primary language, then undecorated [Strings].
    [[nodiscard]] static Result<InfFile, InfParseError> parse(
        std::span<const u8> bytes,
        std::string (*ansi_to_utf8)(std::string_view) = nullptr,
        u16 ui_language = 0x0409);

    [[nodiscard]] u32 style() const noexcept { return style_; }

    /// %dirid% tokens (Learn, Using Dirids: "anywhere a string is expected
    /// except the [Strings] section"): a numeric token with no [Strings]
    /// entry is replaced by @p resolver's path; nullopt keeps it as written
    /// (probe: %99999% stays).
    using DiridResolver = std::function<std::optional<std::string>(i64)>;
    void set_dirid_resolver(DiridResolver resolver) { dirid_resolver_ = std::move(resolver); }

    /// Case-insensitive section lookup; sections written more than once are
    /// one section whose lines are in file order.
    [[nodiscard]] const InfSection* section(std::string_view name) const;

    /// A field's value: quotes removed ("" inside quotes is one quote) and
    /// %strkey% replaced from the selected Strings section, inside quotes
    /// too (Learn's "%%SystemRoot%%\System32" example is quoted); %% is a
    /// literal %, and an undefined key, which Learn leaves undocumented, is
    /// kept as written.
    [[nodiscard]] std::string resolve(std::string_view raw) const;

    /// SetupGetStringField numbering: 0 is the key (a keyless line has one
    /// only when it holds a single field), 1.. are the fields after '=' (or
    /// all fields of a keyless line). nullopt past the end.
    [[nodiscard]] std::optional<std::string> field(const InfLine& line,
                                                   usize index) const;
    [[nodiscard]] usize field_count(const InfLine& line) const noexcept {
        return line.raw_fields.size();
    }
    [[nodiscard]] std::string key(const InfLine& line) const;

    /// First field of the first line keyed @p key in @p section.
    [[nodiscard]] std::optional<std::string> value(std::string_view section,
                                                   std::string_view key) const;

    /// The [Version] lines as (key, first field) pairs, in file order: what
    /// SetupGetInfInformation records (probe: DriverVer "02-04-2010, 5.3.1"
    /// is stored as "02-04-2010"). Keyless lines are not recorded.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> version_pairs() const;

    [[nodiscard]] const std::vector<InfSection>& sections() const noexcept {
        return sections_;
    }

private:
    std::vector<InfSection> sections_;
    u32 style_ = INF_STYLE_NONE;
    std::optional<usize> strings_index_;
    DiridResolver dirid_resolver_;
};

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

}  // namespace vitreo::winapi::setupapi_inf
