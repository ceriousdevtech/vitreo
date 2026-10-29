// SPDX-License-Identifier: LGPL-2.1-only

/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

#include <vitreo/diagnostics.hpp>

/// @file name_normalisation.cpp
/// @brief Name Normalisation
///
/// Closed cut from module.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Name Normalisation
// =============================================================================

std::string ModuleManager::normalize_name(std::string_view name) {
    std::string result(name);
    // Strip directory path (either \ or / separators)
    auto backslash_pos = result.rfind('\\');
    auto slash_pos     = result.rfind('/');
    auto sep_pos = std::string::npos;
    if (backslash_pos != std::string::npos && slash_pos != std::string::npos) {
        sep_pos = std::max(backslash_pos, slash_pos);
    } else if (backslash_pos != std::string::npos) {
        sep_pos = backslash_pos;
    } else if (slash_pos != std::string::npos) {
        sep_pos = slash_pos;
    }
    if (sep_pos != std::string::npos) {
        result = result.substr(sep_pos + 1);
    }
    // Lowercase
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Strip trailing ".dll" if present (case-insensitive after lower).
    // Keep ".exe": Windows can load foo.exe and foo.dll simultaneously, and
    // both are valid module names even though their stem is identical.
    // Keep ".drv"/".ocx"/etc.: those are distinct ThunkRegistry keys.
    constexpr std::string_view dll_suffix = ".dll";
    if (result.size() > dll_suffix.size()) {
        auto pos = result.size() - dll_suffix.size();
        if (result.compare(pos, dll_suffix.size(), dll_suffix) == 0) {
            result.erase(pos);
        }
    }
    return result;
}

bool ModuleManager::normalized_name_equals(std::string_view name,
                                           std::string_view key) noexcept {
    // The same three steps as normalize_name, in place: basename, lowercase,
    // then drop a trailing ".dll" from a name longer than the suffix.
    if (const auto sep = name.find_last_of("\\/"); sep != std::string_view::npos) {
        name.remove_prefix(sep + 1);
    }
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    constexpr std::string_view dll_suffix = ".dll";
    if (name.size() > dll_suffix.size()) {
        const std::string_view tail = name.substr(name.size() - dll_suffix.size());
        bool is_dll = true;
        for (usize i = 0; i < dll_suffix.size(); ++i) {
            if (lower(tail[i]) != dll_suffix[i]) {
                is_dll = false;
                break;
            }
        }
        if (is_dll) name.remove_suffix(dll_suffix.size());
    }
    if (name.size() != key.size()) return false;
    for (usize i = 0; i < name.size(); ++i) {
        if (lower(name[i]) != key[i]) return false;
    }
    return true;
}

} // namespace vitreo::winapi
