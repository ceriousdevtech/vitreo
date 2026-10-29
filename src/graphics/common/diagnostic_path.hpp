#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vitreo::graphics {

// Diagnostic paths are literals, never printf formats. Expand only the named
// PID token and (when supplied) frame token. Reject overlong paths, not truncate.
inline std::optional<std::string> diagnostic_path(
    std::string_view path, unsigned long long pid,
    std::optional<unsigned long long> frame = std::nullopt) {
    constexpr size_t max_bytes = 1023;
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 1 < path.size() && path[i + 1] == 'p') {
            result += std::to_string(pid);
            ++i;
        } else if (frame && path[i] == '%' && i + 1 < path.size() && path[i + 1] == 'u') {
            result += std::to_string(*frame);
            ++i;
        } else {
            result += path[i];
        }
        if (result.size() > max_bytes) return std::nullopt;
    }
    return result;
}

} // namespace vitreo::graphics
