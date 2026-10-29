/// @file path_translator.cpp
/// @brief Path translation between Windows paths and host filesystem implementation

#include "capsule/vfs/path_translator.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::capsule {

// =============================================================================
// PathTranslator Implementation
// =============================================================================

PathTranslator::PathTranslator(const std::filesystem::path& capsule_root)
    : m_capsule_root(capsule_root) {
}

std::filesystem::path PathTranslator::to_host(const WinPath& win_path) const {
    // Start with drives directory
    std::filesystem::path result = m_capsule_root / DRIVES_DIR;
    
    // Add drive letter as lowercase directory name
    char drive_lower = to_lower(win_path.drive_letter());
    result /= std::string(1, drive_lower);
    
    // Add each component
    for (const auto& component : win_path.components()) {
        result /= component;
    }
    
    return result;
}

std::optional<WinPath> PathTranslator::to_windows(
    const std::filesystem::path& host_path
) const {
    // Get the path relative to drives directory
    std::filesystem::path drives = m_capsule_root / DRIVES_DIR;
    
    // Make paths absolute for comparison
    std::filesystem::path abs_host = host_path;
    std::filesystem::path abs_drives = drives;
    
    // Check if host_path starts with drives directory
    auto host_str = abs_host.string();
    auto drives_str = abs_drives.string();
    
    if (host_str.size() <= drives_str.size()) {
        return std::nullopt;
    }
    
    // Check prefix match
    if (host_str.substr(0, drives_str.size()) != drives_str) {
        return std::nullopt;
    }
    
    // Get relative portion after drives/
    std::string relative = host_str.substr(drives_str.size());
    
    // Skip leading separator
    if (!relative.empty() && (relative[0] == '/' || relative[0] == '\\')) {
        relative = relative.substr(1);
    }
    
    if (relative.empty()) {
        return std::nullopt;  // Just the drives directory itself
    }
    
    // First component should be the drive letter
    char drive_letter = '\0';
    std::size_t sep_pos = relative.find('/');
    if (sep_pos == std::string::npos) {
        sep_pos = relative.find('\\');
    }
    
    std::string drive_component;
    std::string remaining;
    
    if (sep_pos == std::string::npos) {
        drive_component = relative;
    } else {
        drive_component = relative.substr(0, sep_pos);
        remaining = relative.substr(sep_pos + 1);
    }
    
    // Drive component should be a single lowercase letter
    if (drive_component.size() != 1) {
        return std::nullopt;
    }
    
    char c = drive_component[0];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        drive_letter = to_upper(c);
    } else {
        return std::nullopt;
    }
    
    // Construct Windows path
    std::string win_path_str;
    win_path_str += drive_letter;
    win_path_str += ':';
    if (!remaining.empty()) {
        win_path_str += '\\';
        // Replace forward slashes with backslashes
        for (char ch : remaining) {
            if (ch == '/') {
                win_path_str += '\\';
            } else {
                win_path_str += ch;
            }
        }
    }
    
    WinPath result(win_path_str);
    if (!result.is_valid()) {
        return std::nullopt;
    }
    
    return result;
}

bool PathTranslator::is_within_capsule(
    const std::filesystem::path& host_path
) const {
    auto host_str = host_path.string();
    auto root_str = m_capsule_root.string();
    
    if (host_str.size() < root_str.size()) {
        return false;
    }
    
    return host_str.substr(0, root_str.size()) == root_str;
}

std::optional<char> PathTranslator::get_drive_for_path(
    const std::filesystem::path& host_path
) const {
    auto win_path = to_windows(host_path);
    if (win_path) {
        return win_path->drive_letter();
    }
    return std::nullopt;
}

std::filesystem::path PathTranslator::drive_path(char drive) const {
    char drive_lower = to_lower(drive);
    return m_capsule_root / DRIVES_DIR / std::string(1, drive_lower);
}

} // namespace vitreo::capsule
