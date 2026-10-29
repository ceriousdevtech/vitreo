#pragma once

/// @file path_translator.hpp
/// @brief Path translation between Windows paths and host filesystem
///
/// PathTranslator converts Windows-style paths to host filesystem paths
/// within a capsule's directory structure.

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "capsule/vfs/win_path.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "vitreo/result.hpp"

namespace vitreo::capsule {

// =============================================================================
// PathTranslator Class
// =============================================================================

/// Translates paths between Windows format and host filesystem
///
/// The translator maps Windows drive letters to subdirectories within
/// the capsule root:
///
///   C:\Users\file.txt → capsule_root/drives/c/Users/file.txt
///   D:\Data\info.txt  → capsule_root/drives/d/Data/info.txt
///
/// Example:
/// @code
///     PathTranslator translator("/home/user/.vitreo/capsules/myapp");
///     
///     WinPath win_path("C:\\Users\\file.txt");
///     auto host = translator.to_host(win_path);
///     // host = "/home/user/.vitreo/capsules/myapp/drives/c/Users/file.txt"
///     
///     auto back = translator.to_windows(host);
///     // back = WinPath("C:\Users\file.txt")
/// @endcode
class PathTranslator {
public:
    // -------------------------------------------------------------------------
    // Constants
    // -------------------------------------------------------------------------
    
    /// Subdirectory name for drive storage
    static constexpr const char* DRIVES_DIR = "drives";
    
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------
    
    /// Default constructor - invalid translator
    PathTranslator() = default;
    
    /// Construct with capsule root directory
    /// @param capsule_root Path to the capsule's root directory
    explicit PathTranslator(const std::filesystem::path& capsule_root);
    
    // -------------------------------------------------------------------------
    // Path Translation
    // -------------------------------------------------------------------------
    
    /// Convert a Windows path to a host filesystem path
    /// @param win_path Windows path to convert
    /// @return Host filesystem path within the capsule
    [[nodiscard]] std::filesystem::path to_host(const WinPath& win_path) const;
    
    /// Convert a host filesystem path back to a Windows path
    /// @param host_path Host path within the capsule
    /// @return Windows path if host_path is within the capsule, nullopt otherwise
    [[nodiscard]] std::optional<WinPath> to_windows(
        const std::filesystem::path& host_path
    ) const;
    
    // -------------------------------------------------------------------------
    // Validation
    // -------------------------------------------------------------------------
    
    /// Check if a host path is within this capsule
    /// @param host_path Path to check
    /// @return true if path is within capsule root
    [[nodiscard]] bool is_within_capsule(
        const std::filesystem::path& host_path
    ) const;
    
    /// Check if a host path is within a drive directory
    /// @param host_path Path to check
    /// @return Drive letter if path is within a drive, nullopt otherwise
    [[nodiscard]] std::optional<char> get_drive_for_path(
        const std::filesystem::path& host_path
    ) const;
    
    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------
    
    /// Get the capsule root directory
    [[nodiscard]] const std::filesystem::path& capsule_root() const noexcept {
        return m_capsule_root;
    }
    
    /// Get the drives directory path
    [[nodiscard]] std::filesystem::path drives_root() const {
        return m_capsule_root / DRIVES_DIR;
    }
    
    /// Get the host path for a specific drive
    /// @param drive Drive letter (A-Z, case-insensitive)
    /// @return Path to the drive's directory within the capsule
    [[nodiscard]] std::filesystem::path drive_path(char drive) const;
    
    /// Check if translator is valid (has capsule root set)
    [[nodiscard]] bool is_valid() const noexcept {
        return !m_capsule_root.empty();
    }
    
    /// Check if translator is valid
    [[nodiscard]] explicit operator bool() const noexcept {
        return is_valid();
    }
    
private:
    std::filesystem::path m_capsule_root;
};

// =============================================================================
// Result Types
// =============================================================================

/// Result type for path translation operations
using PathResult = Result<std::filesystem::path, VfsError>;

/// Result type for WinPath creation
using WinPathResult = Result<WinPath, VfsError>;

} // namespace vitreo::capsule
