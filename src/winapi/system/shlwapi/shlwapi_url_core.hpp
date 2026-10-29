#pragma once

/// @file shlwapi_url_core.hpp
/// @brief Host-side model of shlwapi's URL family (UrlCanonicalize, UrlEscape,
///        UrlUnescape, UrlCombine, UrlGetPart, UrlIs*, UrlGetLocation,
///        UrlCompare, UrlCreateFromPath, PathCreateFromUrl, UrlApplyScheme,
///        UrlFixupW, ParseURL). Every rule was measured on Windows 11 by
///        tools/oracle_probe/shlwapi_url_probe.c (x86 and x64); the thunks in
///        shlwapi_url.cpp marshal guest buffers around these functions.

#include <vitreo/common.hpp>

#include <functional>
#include <optional>
#include <string>

namespace vitreo::winapi::shlwapi_url {

using U16 = std::u16string;

// HRESULTs these functions return (winerror.h).
inline constexpr u32 kSOk = 0x00000000;
inline constexpr u32 kSFalse = 0x00000001;
inline constexpr u32 kEPointer = 0x80004003;
inline constexpr u32 kEFail = 0x80004005;
inline constexpr u32 kENotImpl = 0x80004001;
inline constexpr u32 kEUnexpected = 0x8000FFFF;
inline constexpr u32 kEInvalidArg = 0x80070057;
inline constexpr u32 kUrlEInvalidSyntax = 0x80041001;  // URL_E_INVALID_SYNTAX

// dwFlags bits (shlwapi.h).
inline constexpr u32 URL_UNESCAPE = 0x10000000;
inline constexpr u32 URL_ESCAPE_UNSAFE = 0x20000000;
inline constexpr u32 URL_PLUGGABLE_PROTOCOL = 0x40000000;
inline constexpr u32 URL_WININET_COMPATIBILITY = 0x80000000;
inline constexpr u32 URL_DONT_ESCAPE_EXTRA_INFO = 0x02000000;
inline constexpr u32 URL_ESCAPE_SPACES_ONLY = 0x04000000;
inline constexpr u32 URL_DONT_SIMPLIFY = 0x08000000;
inline constexpr u32 URL_UNESCAPE_INPLACE = 0x00100000;
inline constexpr u32 URL_CONVERT_IF_DOSPATH = 0x00200000;
inline constexpr u32 URL_UNESCAPE_HIGH_ANSI_ONLY = 0x00400000;
inline constexpr u32 URL_INTERNAL_PATH = 0x00800000;
inline constexpr u32 URL_FILE_USE_PATHURL = 0x00010000;
inline constexpr u32 URL_DONT_UNESCAPE = 0x00020000;
inline constexpr u32 URL_ESCAPE_AS_UTF8 = 0x00040000;
inline constexpr u32 URL_ESCAPE_ASCII_URI_COMPONENT = 0x00080000;
inline constexpr u32 URL_ESCAPE_PERCENT = 0x00001000;
inline constexpr u32 URL_ESCAPE_SEGMENT_ONLY = 0x00002000;
inline constexpr u32 URL_PARTFLAG_KEEPSCHEME = 0x00000001;
inline constexpr u32 URL_APPLY_DEFAULT = 0x00000001;
inline constexpr u32 URL_APPLY_GUESSSCHEME = 0x00000002;
inline constexpr u32 URL_APPLY_GUESSFILE = 0x00000004;
inline constexpr u32 URL_APPLY_FORCEAPPLY = 0x00000008;

/// A function result: the HRESULT and, when it succeeded (S_OK / S_FALSE),
/// the produced string without its terminator.
struct Result {
    u32 hr = kSOk;
    U16 text;
};

/// The parts of ParseURL's PARSEDURL (offsets into the input).
struct Parsed {
    usize protocol_len = 0;
    usize suffix_start = 0;
    usize suffix_len = 0;
    u32 scheme = 0;  // URL_SCHEME_*
};

/// ParseURL: nullopt for URL_E_INVALID_SYNTAX.
[[nodiscard]] std::optional<Parsed> parse_url(const U16& url);

[[nodiscard]] Result canonicalize(const U16& url, u32 flags);
[[nodiscard]] Result escape(const U16& url, u32 flags);
/// UrlUnescape's converted string; nullopt for E_INVALIDARG (an escaped NUL).
[[nodiscard]] std::optional<U16> unescape(const U16& url, u32 flags);
/// UrlUnescapeA's converted bytes; nullopt for E_INVALIDARG.
[[nodiscard]] std::optional<std::string> unescape_ansi(const std::string& url, u32 flags);
[[nodiscard]] Result combine(const U16& base, const U16& relative, u32 flags);
[[nodiscard]] Result get_part(const U16& url, u32 part, u32 flags);
/// UrlIs's BOOL (URLIS_NOHISTORY reports the scheme's flag bit, 4).
[[nodiscard]] u32 url_is(const U16& url, u32 which);
[[nodiscard]] bool is_opaque(const U16& url);
[[nodiscard]] u32 is_no_history(const U16& url);
[[nodiscard]] bool path_is_url(const U16& url);
/// UrlGetLocation's character offset, or nullopt for NULL.
[[nodiscard]] std::optional<usize> get_location(const U16& url);
/// UrlCompare's operands after its normalisation; the caller compares them
/// with CompareString (locale, case-sensitive).
[[nodiscard]] std::pair<U16, U16> compare_operands(const U16& a, const U16& b, bool ignore_slash);
[[nodiscard]] Result create_from_path(const U16& path, u32 flags);
[[nodiscard]] Result path_from_url(const U16& url, u32 flags);

/// Registry lookups UrlApplyScheme makes (HKLM\Software\Microsoft\Windows\
/// CurrentVersion\URL\Prefixes and ...\URL\DefaultPrefix).
struct ApplySchemeRegistry {
    /// The data of the first Prefixes value whose name starts @p url
    /// (case-insensitively) and is shorter than it.
    std::function<std::optional<U16>(const U16& url)> prefix;
    /// DefaultPrefix's default value.
    std::function<std::optional<U16>()> default_prefix;
};
[[nodiscard]] Result apply_scheme(const U16& url, u32 flags, const ApplySchemeRegistry& registry);
/// UrlFixupW's translated URL (S_OK) or S_FALSE with no text; nullopt when
/// the URL's scheme is not a known one -- Windows then tries to correct a
/// misspelling, which Vitreo does not model (the thunk reports it).
[[nodiscard]] std::optional<Result> fixup(const U16& url);

/// The A forms of UrlCanonicalize, UrlEscape, UrlCombine and UrlGetPart
/// report an empty S_OK result as S_FALSE, except under
/// URL_PLUGGABLE_PROTOCOL (measured).
[[nodiscard]] inline u32 ansi_hr(u32 hr, const U16& text, u32 flags) {
    return hr == kSOk && (text.empty() || text[0] == 0) && !(flags & URL_PLUGGABLE_PROTOCOL) ? kSFalse : hr;
}

/// UTF-16 <-> ANSI (CP_ACP) for the A entry points.
[[nodiscard]] U16 from_ansi(const std::string& s);
[[nodiscard]] std::string to_ansi(const U16& s);

} // namespace vitreo::winapi::shlwapi_url
