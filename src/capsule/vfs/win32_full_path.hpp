/// @file win32_full_path.hpp
/// @brief Win32 path normalization (GetFullPathNameW semantics) on UTF-8.
///
/// Sources: Learn "Naming Files, Paths, and Namespaces" (fully qualified vs
/// relative, "." / ".." segments, trailing periods and spaces) and
/// GetFullPathNameW (remarks: "U:" -> current directory on U:, UNC and
/// \\?\ forms kept), with every shape measured on Windows 11 in
/// tools/oracle_probe/files_ns_probe.expected (gfp_* lines).

#pragma once

#include <vitreo/common.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace vitreo::capsule {

struct Win32FullPath {
    /// Fully qualified path with '\' separators.
    std::string path;
    /// Byte offset of the final component in @c path, or npos when the path
    /// ends in a separator (a directory / a root) or names a DOS device —
    /// GetFullPathNameW then stores NULL in *lpFilePart.
    usize file_part = std::string::npos;
};

/// Normalize @p path the way GetFullPathNameW does. @p cwd is the process
/// current directory ("C:\dir", no trailing separator except at a root).
/// Returns nullopt for an empty @p path (ERROR_INVALID_NAME).
[[nodiscard]] std::optional<Win32FullPath> win32_full_path(std::string_view path,
                                                           std::string_view cwd);

}  // namespace vitreo::capsule
