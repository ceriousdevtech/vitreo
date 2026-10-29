#pragma once

/// @file hive.hpp
/// @brief Registry hive tree structure (REG-001)
///
/// Provides the in-memory tree data structures used by the capsule
/// registry. Each hive root (HKLM, HKCU, etc.) owns a tree of RegNode
/// instances.

#include <vitreo/common.hpp>

#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vitreo::capsule {

// =============================================================================
// Registry Value Types
// =============================================================================

/// Windows registry value type identifiers
enum class RegType : u32 {
    None      = 0,
    Sz        = 1,   // REG_SZ
    ExpandSz  = 2,   // REG_EXPAND_SZ
    Binary    = 3,   // REG_BINARY
    Dword     = 4,   // REG_DWORD
    MultiSz   = 7,   // REG_MULTI_SZ
    Qword     = 11,  // REG_QWORD
    /// Exact bytes of a value whose Win32 type (RegValue::raw_type) the typed
    /// forms above cannot hold byte-for-byte: REG_NONE with data,
    /// REG_DWORD_BIG_ENDIAN, REG_LINK, the resource-list types, any other
    /// type code, a REG_DWORD/REG_QWORD whose size is not 4/8, or a
    /// REG_MULTI_SZ that is not a well-formed list. RegSetValueEx stores the
    /// caller's bytes and RegQueryValueEx returns them unchanged (Learn,
    /// "Registry Value Types"). Payload: vector<u8>.
    Raw       = 0xFFFFFFFFu,
};

/// A single registry value with type tag and payload.
struct RegValue {
    RegType type = RegType::None;

    /// Payload storage. The active variant depends on `type`:
    ///   Sz / ExpandSz  -> wstring
    ///   Binary         -> vector<u8>
    ///   Dword          -> u32
    ///   Qword          -> u64
    ///   MultiSz        -> vector<wstring>
    ///   Raw            -> vector<u8> (Win32 type in raw_type)
    ///   None           -> monostate
    std::variant<
        std::monostate,
        std::wstring,
        std::vector<u8>,
        u32,
        u64,
        std::vector<std::wstring>
    > data;

    /// Win32 REG_* code of a RegType::Raw value.
    u32 raw_type = 0;

    /// Creation sequence within its key. Windows enumerates a key's values in
    /// the order they were created (an overwrite keeps the slot), not by name
    /// (s4_registry_probe enumw_all_*). Maintained by Registry; 0 for values
    /// seeded directly into the tree, which then enumerate first, by name.
    u64 order = 0;
};

// =============================================================================
// Case-Insensitive Wide String Comparator (for keys & value names)
// =============================================================================

struct RegCaseInsensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const;
};

// =============================================================================
// RegNode  – a single key in the hive tree
// =============================================================================

struct RegNode {
    /// Display name of this key (original case preserved)
    std::wstring name;

    /// Subkeys, ordered case-insensitively
    std::map<std::wstring, std::unique_ptr<RegNode>, RegCaseInsensitive> children;

    /// Values stored under this key, ordered case-insensitively
    std::map<std::wstring, RegValue, RegCaseInsensitive> values;

    explicit RegNode(std::wstring key_name = {})
        : name(std::move(key_name)) {}
};

// =============================================================================
// Path Helpers
// =============================================================================

/// Split a backslash-delimited registry path into its components.
/// Leading/trailing backslashes are ignored.
std::vector<std::wstring> split_reg_path(std::wstring_view path);

} // namespace vitreo::capsule
