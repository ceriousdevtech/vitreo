#pragma once
/// @file shell_paths_internal.hpp
/// @brief Shared surface between shell_paths.cpp and shell_paths_registration.cpp.
///
/// Private to shell32's path implementation. One entry, because one entry is
/// all the export table reaches for.

#include <string>

namespace vitreo::winapi {

/// Narrow a UTF-16 path to the UTF-8 the capsule VFS speaks.
[[nodiscard]] std::string wstring_to_utf8(const std::wstring& ws);

} // namespace vitreo::winapi
