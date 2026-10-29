/// @file hive.cpp
/// @brief Registry hive helpers (REG-001)

#include "capsule/registry/hive.hpp"

#include <algorithm>
#include <cwctype>

namespace vitreo::capsule {

// =============================================================================
// RegCaseInsensitive
// =============================================================================

bool RegCaseInsensitive::operator()(const std::wstring& a, const std::wstring& b) const {
    // Preserve the existing uppercase lexical ordering without constructing
    // two temporary strings for every tree lookup during hive/journal replay.
    const auto count = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto left = static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(a[i])));
        const auto right = static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(b[i])));
        if (left != right) return left < right;
    }
    return a.size() < b.size();
}

// =============================================================================
// split_reg_path
// =============================================================================

std::vector<std::wstring> split_reg_path(std::wstring_view path) {
    std::vector<std::wstring> parts;
    std::wstring_view::size_type start = 0;

    // Skip leading backslash
    while (start < path.size() && path[start] == L'\\') {
        ++start;
    }

    while (start < path.size()) {
        auto pos = path.find(L'\\', start);
        if (pos == std::wstring_view::npos) {
            parts.emplace_back(path.substr(start));
            break;
        }
        if (pos > start) {
            parts.emplace_back(path.substr(start, pos - start));
        }
        start = pos + 1;
    }

    return parts;
}

} // namespace vitreo::capsule
