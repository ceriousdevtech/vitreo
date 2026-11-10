// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_path_helpers.cpp
/// @brief Shared Windows path classifiers for FileManager TUs.

#include "file_path_internal.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::winapi {

bool is_named_pipe_path(std::string_view p) {
    static constexpr char kPrefix[] = "\\\\.\\pipe\\";
    constexpr usize n = sizeof(kPrefix) - 1;
    if (p.size() < n) return false;
    for (usize i = 0; i < n; ++i) {
        // The extended Win32 namespace also addresses the local pipe device.
        if (i == 2 && p[i] == '?') continue;
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(p[i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(kPrefix[i])));
        if (a != b) return false;
    }
    return true;
}

std::string ascii_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    });
    return result;
}

std::optional<std::string> dos_device_name(std::string_view path) {
    static constexpr std::string_view kPrefix = "\\\\.\\";
    if (path.size() <= kPrefix.size()) return std::nullopt;
    for (usize i = 0; i < kPrefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(path[i])) !=
            std::tolower(static_cast<unsigned char>(kPrefix[i]))) {
            return std::nullopt;
        }
    }
    const std::string_view suffix = path.substr(kPrefix.size());
    if (suffix.find('\\') != std::string_view::npos ||
        suffix.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return ascii_lower(suffix);
}

std::optional<u32> physical_drive_number(std::string_view device) {
    static constexpr std::string_view kPrefix = "physicaldrive";
    if (!device.starts_with(kPrefix) || device.size() == kPrefix.size() ||
        device.size() > kPrefix.size() + 9) {
        return std::nullopt;
    }
    u32 number = 0;
    for (const char c : device.substr(kPrefix.size())) {
        if (c < '0' || c > '9') return std::nullopt;
        number = number * 10 + static_cast<u32>(c - '0');
    }
    return number;
}

} // namespace vitreo::winapi
