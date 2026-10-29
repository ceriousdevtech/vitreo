#pragma once
/// @file codepage_convert.hpp
/// @brief MultiByteToWideChar / WideCharToMultiByte semantics, measured on
///        Windows rather than recalled.
///
/// Sources, all under tools/oracle_probe/:
///  - codepage_table_dump.c: every installed single-byte code page's decode of
///    each byte and encode of every UTF-16 unit, with and without
///    WC_NO_BEST_FIT_CHARS (tools/gen_codepage_tables.py -> codepage_tables.inc)
///  - codepage_decode_flags_dump.c: MB_USEGLYPHCHARS / MB_COMPOSITE decodes
///  - utf8_corpus_probe.c: 40,000 UTF-8 conversions (the decoder below matches
///    all of them)
///  - string_conversion_probe.c / string_conversion_flags_probe.c: flags,
///    errors, buffer rules and the pseudo code pages.

#include <vitreo/common.hpp>

namespace vitreo::winapi::codepage {

inline constexpr u32 kCpAcp = 0;
inline constexpr u32 kCpOemcp = 1;
inline constexpr u32 kCpMaccp = 2;
inline constexpr u32 kCpThreadAcp = 3;
inline constexpr u32 kCpSymbol = 42;
inline constexpr u32 kCpUtf7 = 65000;
inline constexpr u32 kCpUtf8 = 65001;

/// The code pages a correct Windows installation answers for the pseudo code
/// pages, as measured on an en-US system (GetCPInfoEx on Windows). Default
/// ANSI is 1252; a process with fusion `activeCodePage=UTF-8` switches to 65001
/// via set_ansi_code_page (GetACP / CP_ACP).
inline constexpr u32 kAnsiCodePage = 1252;
inline constexpr u32 kOemCodePage = 437;
inline constexpr u32 kMacCodePage = 10000;

/// Process ANSI code page (GetACP). Defaults to 1252; set to 65001 when the
/// main image's manifest declares activeCodePage UTF-8.
[[nodiscard]] u32 ansi_code_page() noexcept;
void set_ansi_code_page(u32 code_page) noexcept;

struct Result {
    u32 count = 0;       ///< units/bytes produced (or required, for a query)
    u32 error = 0;       ///< Win32 error when count == 0 and the call failed
    bool used_default = false;
    u32 written = 0;     ///< units/bytes stored before ERROR_INSUFFICIENT_BUFFER
    [[nodiscard]] bool ok() const noexcept { return error == 0; }
};

/// MultiByteToWideChar on host buffers. @p dst_capacity == 0 is a size query.
/// Output that does not fit fails with ERROR_INSUFFICIENT_BUFFER after writing
/// the units that do, as Windows does.
[[nodiscard]] Result multi_byte_to_wide(u32 code_page, u32 flags,
                                        const u8* src, usize src_len,
                                        u16* dst, usize dst_capacity);

/// WideCharToMultiByte on host buffers. @p default_char is lpDefaultChar
/// (nullptr when the caller passed NULL); @p want_used_default says whether
/// the caller passed lpUsedDefaultChar.
[[nodiscard]] Result wide_to_multi_byte(u32 code_page, u32 flags,
                                        const u16* src, usize src_len,
                                        u8* dst, usize dst_capacity,
                                        const u8* default_char,
                                        bool want_used_default);

/// IsValidCodePage: TRUE for every code page Vitreo converts exactly -- the
/// measured single-byte pages and UTF-8. Code pages Windows installs but
/// Vitreo does not implement (UTF-7, the DBCS pages) report FALSE, so a caller
/// that checks first takes its fallback instead of receiving wrong text.
[[nodiscard]] bool is_valid_code_page(u32 code_page);

/// GetCPInfo data for an implemented code page (after pseudo-page resolution).
struct CpInfo {
    u32 code_page = 0;
    u32 max_char_size = 0;
    u8 default_char[2] = {0, 0};
    u8 lead_bytes[12] = {};
    u16 unicode_default_char = 0;
};
[[nodiscard]] bool code_page_info(u32 code_page, CpInfo& out);

/// Resolve CP_ACP / CP_OEMCP / CP_MACCP / CP_THREAD_ACP to a real code page.
[[nodiscard]] u32 resolve_pseudo(u32 code_page) noexcept;

/// GetStringTypeW CT_CTYPE1 flags (C1_UPPER 0x1 ... C1_DEFINED 0x200) of one
/// UTF-16 unit; defined with GetStringTypeW in codepage.cpp.
[[nodiscard]] u16 ctype1_flags(u16 ch) noexcept;

} // namespace vitreo::winapi::codepage
