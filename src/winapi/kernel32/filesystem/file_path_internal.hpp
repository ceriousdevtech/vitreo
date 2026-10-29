// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file file_path_internal.hpp
/// @brief Shared Windows path classifiers for FileManager TUs.

#include <vitreo/common.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace vitreo::winapi {

[[nodiscard]] bool is_named_pipe_path(std::string_view p);
[[nodiscard]] std::string ascii_lower(std::string_view value);
[[nodiscard]] std::optional<std::string> dos_device_name(std::string_view path);

} // namespace vitreo::winapi
