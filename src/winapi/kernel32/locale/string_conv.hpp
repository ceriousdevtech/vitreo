#pragma once

/// @file string_conv.hpp
/// @brief MultiByteToWideChar / WideCharToMultiByte implementation (WIN32-034)
///
/// Provides UTF-8 ↔ UTF-16LE string conversion following the Win32 API
/// semantics, including null-terminator handling, buffer-size queries,
/// and proper error codes.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

// Forward declaration
class Marshaler;

// =============================================================================
// Code Page Constants
// =============================================================================

constexpr u32 CP_ACP   = 0;      ///< ANSI code page (treated as CP-1252)
constexpr u32 CP_OEMCP = 1;      ///< OEM code page (treated as CP-1252)
constexpr u32 CP_1252  = 1252;   ///< Western European (Windows-1252)
constexpr u32 CP_UTF8  = 65001;  ///< UTF-8 code page

// =============================================================================
// StringConverter – stateless UTF-8 ↔ UTF-16LE conversion
// =============================================================================

/// Stateless converter implementing Win32 MultiByteToWideChar /
/// WideCharToMultiByte semantics on the host side.
class StringConverter {
public:
    /// Convert a multi-byte (UTF-8) string to a wide (UTF-16LE) string.
    ///
    /// @param code_page      Code page (CP_ACP or CP_UTF8 supported).
    /// @param flags          Conversion flags (reserved, should be 0).
    /// @param mb_str         Pointer to the UTF-8 input string.
    /// @param cb_multibyte   Size in bytes of mb_str, or -1 if null-terminated.
    /// @param wide_str       Destination buffer, or nullptr to query size.
    /// @param cch_wide_char  Size of wide_str in u16 code units, or 0 to query.
    /// @return Number of u16 code units written (or required), 0 on error.
    static int multi_byte_to_wide_char(u32 code_page, u32 flags,
                                       const char* mb_str, int cb_multibyte,
                                       u16* wide_str, int cch_wide_char);

    /// Convert a wide (UTF-16LE) string to a multi-byte (UTF-8) string.
    ///
    /// @param code_page         Code page (CP_ACP or CP_UTF8 supported).
    /// @param flags             Conversion flags (reserved, should be 0).
    /// @param wide_str          Pointer to the UTF-16LE input string.
    /// @param cch_wide_char     Number of u16 code units, or -1 if null-terminated.
    /// @param mb_str            Destination buffer, or nullptr to query size.
    /// @param cb_multibyte      Size of mb_str in bytes, or 0 to query.
    /// @param default_char      Fallback char for unmappable chars (ignored for UTF-8).
    /// @param used_default_char Set to true if a default char was used (ignored for UTF-8).
    /// @return Number of bytes written (or required), 0 on error.
    static int wide_char_to_multi_byte(u32 code_page, u32 flags,
                                       const u16* wide_str, int cch_wide_char,
                                       char* mb_str, int cb_multibyte,
                                       const char* default_char,
                                       bool* used_default_char);

    StringConverter() = delete;
};

// =============================================================================
// Host-Level Helpers
// =============================================================================

/// Convert a UTF-16LE buffer to a UTF-8 std::string.
/// @param wide  Pointer to UTF-16LE code units.
/// @param len   Number of code units, or -1 for null-terminated.
[[nodiscard]] std::string utf16_to_utf8(const u16* wide, int len = -1);

/// Convert a UTF-8 string_view to a UTF-16LE vector.
[[nodiscard]] std::vector<u16> utf8_to_utf16(std::string_view utf8);

/// Convert a host UTF-8 path/string to guest CP_ACP (CP1252) bytes.
/// Learn *A APIs write ANSI characters, not UTF-8. ® is 0xAE, not C2 AE.
[[nodiscard]] std::string utf8_to_acp(std::string_view utf8);

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

/// Thunk for MultiByteToWideChar
void impl_MultiByteToWideChar(Marshaler& m);

/// Thunk for WideCharToMultiByte
void impl_WideCharToMultiByte(Marshaler& m);

// =============================================================================
// DLL Export Table
// =============================================================================

/// Build the export table for string conversion functions.
[[nodiscard]] DllExports make_string_conv_exports();

} // namespace vitreo::winapi
