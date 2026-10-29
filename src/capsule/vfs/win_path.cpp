/// @file win_path.cpp
/// @brief Windows path representation and manipulation implementation

#include "capsule/vfs/win_path.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::capsule {

// =============================================================================
// Helper Functions
// =============================================================================

namespace {

/// Check if character is a path separator
constexpr bool is_separator(char c) noexcept {
    return c == '\\' || c == '/';
}

/// Check if character is a valid drive letter
constexpr bool is_drive_letter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// Normalize drive letter to uppercase
constexpr char normalize_drive(char c) noexcept {
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

/// Split path string into components
std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> components;
    std::string current;
    
    for (char c : path) {
        if (is_separator(c)) {
            if (!current.empty()) {
                components.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    
    if (!current.empty()) {
        components.push_back(std::move(current));
    }
    
    return components;
}

} // anonymous namespace

// =============================================================================
// WinPath Implementation
// =============================================================================

WinPath::WinPath(std::string_view path) {
    if (path.empty()) {
        m_valid = true;
        return;
    }
    
    std::string_view remaining = path;

    // Extended-length prefix: "\\?\C:\foo" means C:\foo with Win32 path
    // normalization disabled. The prefix is stripped and the rest is parsed
    // as a drive path. Normalization is harmless for the extended paths
    // guests pass, including log files. "\\?\UNC\..." and "\\?\Volume{...}"
    // remain unsupported and fall through to the UNC check below.
    if (remaining.size() >= 6 &&
        is_separator(remaining[0]) && is_separator(remaining[1]) &&
        remaining[2] == '?' && is_separator(remaining[3]) &&
        is_drive_letter(remaining[4]) && remaining[5] == ':') {
        remaining = remaining.substr(4);
    }

    // Check for UNC path (\\server\share or //server/share)
    if (is_unc_path(remaining)) {
        m_valid = false;
        return;
    }
    
    // Check for drive letter (C: or C:\ or C:/)
    if (remaining.size() >= 2 && 
        is_drive_letter(remaining[0]) && 
        remaining[1] == ':') {
        m_drive = normalize_drive(remaining[0]);
        remaining = remaining.substr(2);
        
        // Skip leading separator after drive
        if (!remaining.empty() && is_separator(remaining[0])) {
            remaining = remaining.substr(1);
        }
    } else if (!remaining.empty() && is_separator(remaining[0])) {
        // Absolute path without drive (e.g., \Users\file)
        // Use default drive C:
        m_drive = 'C';
        remaining = remaining.substr(1);
    } else {
        // Relative path - use default drive
        m_drive = 'C';
    }
    
    // Split remaining path into components
    m_components = split_path(remaining);
    m_valid = true;
}

WinPath WinPath::normalize() const {
    if (!m_valid) {
        return *this;
    }
    
    WinPath result;
    result.m_drive = m_drive;
    result.m_valid = m_valid;
    
    for (const auto& component : m_components) {
        if (component == ".") {
            // Current directory - skip
            continue;
        } else if (component == "..") {
            // Parent directory - pop if possible
            if (!result.m_components.empty()) {
                result.m_components.pop_back();
            }
            // At root, ".." does nothing
        } else {
            result.m_components.push_back(component);
        }
    }
    
    return result;
}

std::string WinPath::directory() const {
    if (m_components.empty()) {
        return "";
    }
    
    // Directory is all components except the last one
    if (m_components.size() == 1) {
        return "";
    }
    
    std::string dir;
    for (std::size_t i = 0; i < m_components.size() - 1; ++i) {
        if (!dir.empty()) {
            dir += '/';
        }
        dir += m_components[i];
    }
    return dir;
}

std::string_view WinPath::filename() const noexcept {
    if (m_components.empty()) {
        return {};
    }
    return m_components.back();
}

std::string_view WinPath::extension() const noexcept {
    std::string_view name = filename();
    if (name.empty()) {
        return {};
    }
    
    // Find last dot
    auto dot_pos = name.rfind('.');
    if (dot_pos == std::string_view::npos || dot_pos == 0) {
        return {};
    }
    
    return name.substr(dot_pos + 1);
}

std::string_view WinPath::stem() const noexcept {
    std::string_view name = filename();
    if (name.empty()) {
        return {};
    }
    
    // Find last dot
    auto dot_pos = name.rfind('.');
    if (dot_pos == std::string_view::npos || dot_pos == 0) {
        return name;
    }
    
    return name.substr(0, dot_pos);
}

WinPath WinPath::parent() const {
    if (!m_valid || m_components.empty()) {
        return *this;  // Root has no parent, return self
    }
    
    WinPath result;
    result.m_drive = m_drive;
    result.m_valid = m_valid;
    result.m_components = std::vector<std::string>(
        m_components.begin(), 
        m_components.end() - 1
    );
    
    return result;
}

WinPath WinPath::join(std::string_view component) const {
    if (!m_valid) {
        return *this;
    }
    
    WinPath result = *this;
    
    // Split the component in case it contains separators
    auto parts = split_path(component);
    for (auto& part : parts) {
        result.m_components.push_back(std::move(part));
    }
    
    return result;
}

std::string WinPath::to_windows_string() const {
    std::string result;
    result += m_drive;
    result += ':';
    
    for (const auto& component : m_components) {
        result += '\\';
        result += component;
    }
    
    // If no components, add trailing backslash for root
    if (m_components.empty()) {
        result += '\\';
    }
    
    return result;
}

std::string WinPath::to_unix_string() const {
    std::string result;
    result += to_lower(m_drive);
    
    for (const auto& component : m_components) {
        result += '/';
        result += component;
    }
    
    return result;
}

bool WinPath::equals(const WinPath& other) const noexcept {
    // Both must be valid or both invalid
    if (m_valid != other.m_valid) {
        return false;
    }
    
    // Compare drive letters (case-insensitive, both should be uppercase)
    if (to_upper(m_drive) != to_upper(other.m_drive)) {
        return false;
    }
    
    // Compare component counts
    if (m_components.size() != other.m_components.size()) {
        return false;
    }
    
    // Compare each component case-insensitively
    for (std::size_t i = 0; i < m_components.size(); ++i) {
        if (!case_insensitive_equals(m_components[i], other.m_components[i])) {
            return false;
        }
    }
    
    return true;
}

bool WinPath::is_unc_path(std::string_view path) noexcept {
    // UNC paths start with \\ or //
    return path.size() >= 2 && 
           is_separator(path[0]) && 
           is_separator(path[1]);
}

// =============================================================================
// Utility Functions
// =============================================================================

bool case_insensitive_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower(a[i]) != to_lower(b[i])) {
            return false;
        }
    }
    
    return true;
}

std::string make_absolute_win_path(std::string_view path,
                                   std::string_view current_directory) {
    if (path.empty()) return {};

    const bool has_drive =
        path.size() >= 2 && is_drive_letter(path[0]) && path[1] == ':';
    const bool root_relative = is_separator(path.front());
    std::string candidate;

    if (has_drive) {
        candidate = path;
    } else if (root_relative) {
        const char drive =
            current_directory.size() >= 2 &&
                    is_drive_letter(current_directory[0]) &&
                    current_directory[1] == ':'
                ? current_directory[0]
                : 'C';
        candidate = std::string(1, drive) + ":" + std::string(path);
    } else {
        candidate = current_directory.empty()
                        ? std::string("C:\\")
                        : std::string(current_directory);
        if (!is_separator(candidate.back())) candidate.push_back('\\');
        candidate.append(path);
    }

    WinPath parsed(candidate);
    if (!parsed.is_valid()) return {};
    return parsed.normalize().to_windows_string();
}

} // namespace vitreo::capsule
