#pragma once

/// @file path_encoding.hpp
/// @brief Normalize guest Win32 ANSI (CP_ACP / CP1252) path bytes for the host.
///
/// macOS / APFS path APIs require UTF-8. ANSI (*A) callers pass CP1252 bytes
/// (for example ® = 0xAE). A lone 0xAE is invalid UTF-8 and yields
/// EILSEQ / InternalError from open/mkdir. Valid UTF-8 paths (ASCII, or already
/// converted) are left unchanged.

#include <string>
#include <string_view>

namespace vitreo::capsule {

/// If `path` is valid UTF-8, return it unchanged; otherwise treat bytes as
/// CP1252 and encode to UTF-8.
[[nodiscard]] std::string guest_path_to_host_utf8(std::string_view path);

/// Decode Win32 ANSI (CP_ACP = 1252, GetACP) bytes to UTF-8 unconditionally.
/// *A path APIs interpret names in the ANSI code page (Learn: Naming Files),
/// so bytes that happen to form valid UTF-8 ("\xC3\xA9") still name the
/// CP1252 characters ("\u00C3\u00A9"), never "\u00E9".
[[nodiscard]] std::string ansi_to_utf8(std::string_view ansi);

}  // namespace vitreo::capsule
