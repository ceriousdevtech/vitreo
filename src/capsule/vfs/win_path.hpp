#pragma once

/// @file win_path.hpp
/// @brief Windows path representation and manipulation
///
/// WinPath provides a structured representation of Windows paths with support
/// for drive letters, case-insensitive comparison, and normalization.

#include <string>
#include <string_view>
#include <vector>

namespace vitreo::capsule {

// =============================================================================
// WinPath Class
// =============================================================================

/// Represents a Windows-style path with drive letter and components
///
/// Supports paths like:
/// - "C:\Users\file.txt" (absolute with drive)
/// - "D:/Data/file.txt" (forward slashes)
/// - "\Users\file.txt" (absolute without drive, uses default C:)
/// - "subdir\file.txt" (relative)
///
/// UNC paths (\\server\share) are not supported and will fail parsing.
///
/// Example:
/// @code
///     WinPath path("C:\\Users\\John\\file.txt");
///     char drive = path.drive_letter();      // 'C'
///     auto dir = path.directory();           // "Users/John"
///     auto name = path.filename();           // "file.txt"
///     auto ext = path.extension();           // "txt"
///     
///     auto parent = path.parent();           // "C:\Users\John"
///     auto joined = parent.join("other.txt");// "C:\Users\John\other.txt"
/// @endcode
class WinPath {
public:
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------
    
    /// Default constructor - creates empty path with drive C:
    WinPath() = default;
    
    /// Construct from a Windows path string
    /// @param path Windows-style path (backslash or forward slash)
    explicit WinPath(std::string_view path);
    
    // -------------------------------------------------------------------------
    // Normalization
    // -------------------------------------------------------------------------
    
    /// Normalize path by resolving . and .. components
    /// @return Normalized path
    [[nodiscard]] WinPath normalize() const;
    
    // -------------------------------------------------------------------------
    // Component Access
    // -------------------------------------------------------------------------
    
    /// Get the drive letter (uppercase)
    /// @return Drive letter ('A'-'Z')
    [[nodiscard]] char drive_letter() const noexcept { return m_drive; }
    
    /// Get the directory portion (without drive, without filename)
    /// @return Directory path with forward slashes
    [[nodiscard]] std::string directory() const;
    
    /// Get the final path component (filename with extension)
    /// @return Filename or empty if path is directory-only
    [[nodiscard]] std::string_view filename() const noexcept;
    
    /// Get the file extension (without the dot)
    /// @return Extension or empty if no extension
    [[nodiscard]] std::string_view extension() const noexcept;
    
    /// Get the filename without extension
    /// @return Stem portion of filename
    [[nodiscard]] std::string_view stem() const noexcept;
    
    /// Get all path components
    /// @return Vector of path components
    [[nodiscard]] const std::vector<std::string>& components() const noexcept {
        return m_components;
    }
    
    /// Check if path is empty (no components)
    [[nodiscard]] bool empty() const noexcept { return m_components.empty(); }
    
    /// Check if path is root (drive only, no components)
    [[nodiscard]] bool is_root() const noexcept { return m_components.empty(); }
    
    // -------------------------------------------------------------------------
    // Operations
    // -------------------------------------------------------------------------
    
    /// Get the parent path
    /// @return Parent path, or root if already at root
    [[nodiscard]] WinPath parent() const;
    
    /// Join a component to this path
    /// @param component Path component to append
    /// @return New path with component appended
    [[nodiscard]] WinPath join(std::string_view component) const;
    
    // -------------------------------------------------------------------------
    // Conversion
    // -------------------------------------------------------------------------
    
    /// Convert to Windows-style string (with backslashes)
    /// @return Path like "C:\Users\file.txt"
    [[nodiscard]] std::string to_windows_string() const;
    
    /// Convert to Unix-style relative path (for capsule storage)
    /// @return Path like "c/Users/file.txt"
    [[nodiscard]] std::string to_unix_string() const;
    
    // -------------------------------------------------------------------------
    // Comparison
    // -------------------------------------------------------------------------
    
    /// Case-insensitive equality comparison
    /// @param other Path to compare with
    /// @return true if paths are equivalent (case-insensitive)
    [[nodiscard]] bool equals(const WinPath& other) const noexcept;
    
    /// Equality operator (case-insensitive)
    [[nodiscard]] bool operator==(const WinPath& other) const noexcept {
        return equals(other);
    }
    
    /// Inequality operator (case-insensitive)
    [[nodiscard]] bool operator!=(const WinPath& other) const noexcept {
        return !equals(other);
    }
    
    // -------------------------------------------------------------------------
    // Validation
    // -------------------------------------------------------------------------
    
    /// Check if the path was parsed successfully
    [[nodiscard]] bool is_valid() const noexcept { return m_valid; }
    
    /// Check if path is a UNC path (not supported)
    [[nodiscard]] static bool is_unc_path(std::string_view path) noexcept;
    
private:
    char m_drive = 'C';                    // Drive letter (uppercase)
    std::vector<std::string> m_components; // Path components
    bool m_valid = true;                   // Parse success flag
};

// =============================================================================
// Utility Functions
// =============================================================================

/// Case-insensitive string comparison
/// @return true if strings are equal (case-insensitive)
[[nodiscard]] bool case_insensitive_equals(
    std::string_view a,
    std::string_view b
) noexcept;

/// Resolve a Windows image/path argument against a process current directory.
/// The result is normalized, drive-qualified, and uses backslashes. Returns an
/// empty string for invalid/UNC input.
[[nodiscard]] std::string make_absolute_win_path(
    std::string_view path,
    std::string_view current_directory
);

/// Convert character to lowercase
[[nodiscard]] constexpr char to_lower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

/// Convert character to uppercase
[[nodiscard]] constexpr char to_upper(char c) noexcept {
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

} // namespace vitreo::capsule
