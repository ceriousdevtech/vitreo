// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file synthetic_version_resource.hpp
/// @brief The VS_VERSIONINFO Vitreo reports for its built-in system DLLs.
///
/// One builder for both places that must agree on it: GetFileVersionInfo /
/// VerQueryValue for a built-in module, and the RT_VERSION resource inside
/// that module's synthetic PE image, which is also the file served at the
/// module's path (a process that parses the file sees what the API returns).

#pragma once

#include <vitreo/common.hpp>

#include <string_view>
#include <vector>

namespace vitreo::winapi {

/// The raw VS_VERSIONINFO block (wLength-prefixed, as GetFileVersionInfo
/// returns it) for the built-in system module @p module_name.
[[nodiscard]] std::vector<u8> make_synthetic_system_version_resource(
    std::string_view module_name);

}  // namespace vitreo::winapi
