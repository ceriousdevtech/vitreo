#pragma once

/// @file env_store.hpp
/// @brief Capsule-level environment variable store (ENV-001)
///
/// Provides a case-insensitive wide-string key-value store for Windows
/// environment variables within a capsule. This is the capsule-level
/// primitive that manages the full Unicode (wide) environment block.

#include <vitreo/common.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::capsule {

// =============================================================================
// Case-Insensitive Wide String Comparator
// =============================================================================

/// Comparator that orders wide strings case-insensitively by converting to
/// uppercase, matching Windows environment variable semantics.
struct WideStringCaseInsensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const;
};

// =============================================================================
// EnvStore
// =============================================================================

/// A case-insensitive wide-string environment variable store.
///
/// Stores environment variables as KEY=VALUE pairs using wide strings
/// (std::wstring), matching the native Windows representation. Variable names
/// are compared case-insensitively, so L"PATH" and L"path" refer to the same
/// variable.
class EnvStore {
public:
    EnvStore();

    /// Get a variable value. Returns empty optional if not found.
    [[nodiscard]] std::optional<std::wstring> get(std::wstring_view name) const;

    /// Set a variable. Empty value removes the variable.
    void set(std::wstring_view name, std::wstring_view value);

    /// Remove a variable. Returns true if it existed.
    bool remove(std::wstring_view name);

    /// Check if a variable exists.
    [[nodiscard]] bool contains(std::wstring_view name) const;

    /// Get count of variables.
    [[nodiscard]] usize count() const;

    /// Get all variables as a double-null-terminated wide-char block.
    /// Format: L"KEY1=VALUE1\0KEY2=VALUE2\0\0"
    /// An empty store produces a block containing a single null character.
    [[nodiscard]] std::vector<wchar_t> to_block() const;

    /// Initialize with default Windows environment variables.
    void init_defaults();

private:
    std::map<std::wstring, std::wstring, WideStringCaseInsensitive> m_vars;
};

} // namespace vitreo::capsule
