#pragma once

/// @file registry.hpp
/// @brief Capsule registry — Windows registry emulation (REG-001)
///
/// Provides an in-memory registry with the four standard root hives
/// (HKCR, HKCU, HKLM, HKU). All key lookups are case-insensitive and use
/// backslash separators, matching native Windows behaviour.

#include <vitreo/common.hpp>
#include "capsule/registry/hive.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::capsule {

// =============================================================================
// Handle type
// =============================================================================

/// Opaque handle to an open registry key.
using RegHandle = u64;

/// The registry view an open, create or delete addresses (Learn, "Registry
/// Redirector", "Accessing an Alternate Registry View"). Default takes the
/// view of the parent handle; a predefined root handle has the view of the
/// process (32-bit for a WOW64 process). Native64 is KEY_WOW64_64KEY and
/// Wow32 is KEY_WOW64_32KEY.
enum class RegView : u8 { Default = 0, Native64 = 1, Wow32 = 2 };

/// Well-known root key handles (match Win32 constants).
constexpr RegHandle HKEY_CLASSES_ROOT  = 0x80000000u;
constexpr RegHandle HKEY_CURRENT_USER  = 0x80000001u;
constexpr RegHandle HKEY_LOCAL_MACHINE = 0x80000002u;
constexpr RegHandle HKEY_USERS         = 0x80000003u;

// =============================================================================
// Registry
// =============================================================================

/// Full in-memory Windows registry emulation.
///
/// Usage:
/// @code
///     Registry reg;
///     auto h = reg.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", true);
///     reg.set_value(h.value(), L"Version", RegValue{RegType::Sz, L"1.0"s});
///     auto val = reg.query_value(h.value(), L"Version");
/// @endcode
class Registry {
public:
    Registry();

    /// Open or create a key under `parent`. If `create` is true the key (and
    /// all intermediate keys) are created when they do not exist. Returns the
    /// handle on success, or `Error::NotFound` / `Error::InvalidHandle`.
    [[nodiscard]] Result<RegHandle, Error> open_key(RegHandle parent,
                                                     std::wstring_view subkey,
                                                     bool create);

    /// open_key in an explicit registry view. The new handle carries the
    /// view, so later opens relative to it without a view flag stay in it
    /// (tests/oracle_data/wow64_registry_probe*.expected, from_s32_* /
    /// from_s64_*). `created` reports whether the key was created.
    [[nodiscard]] Result<RegHandle, Error> open_key(RegHandle parent,
                                                     std::wstring_view subkey,
                                                     bool create, RegView view,
                                                     bool* created = nullptr);

    /// A handle to predefined root `root` that carries `view`, for host code
    /// that writes on behalf of a component of a given bitness (a Windows
    /// Installer component, an InprocServer32 of either bitness). Owned by
    /// the registry; close_key ignores it.
    [[nodiscard]] RegHandle view_root(RegHandle root, RegView view);

    /// This process is a 32-bit (WOW64) process: predefined roots and
    /// handles opened without a view flag use the 32-bit view. The capsule
    /// itself always stores the 64-bit Windows layout, with the 32-bit view
    /// in the WOW6432Node keys, so 32- and 64-bit processes share one store.
    void set_wow64_process(bool wow64);
    [[nodiscard]] bool wow64_process() const;

    /// Close a previously opened handle. Root handles are silently ignored.
    void close_key(RegHandle handle);

    /// Query a named value under `key`. Use an empty name for the default
    /// value. Returns `Error::NotFound` when the value does not exist.
    [[nodiscard]] Result<RegValue, Error> query_value(RegHandle key,
                                                      std::wstring_view name);

    /// Set (create or overwrite) a named value under `key`.
    VoidResult set_value(RegHandle key,
                         std::wstring_view name,
                         const RegValue& value);

    /// Delete a named value under `key`.
    VoidResult delete_value(RegHandle key,
                            std::wstring_view name);

    /// Delete the subkey `subkey` beneath `parent`.
    ///
    /// Matches RegDeleteKey: the subkey must exist, and — as on Windows NT —
    /// it must have no subkeys of its own. Returns NotFound when absent and
    /// AccessDenied when it still has children, so callers can report the
    /// Win32 error the guest expects instead of claiming a delete happened.
    [[nodiscard]] Result<void, Error> delete_key(RegHandle parent,
                                                 std::wstring_view subkey,
                                                 RegView view = RegView::Default);

    /// List the subkey names immediately under `key`.
    [[nodiscard]] Result<std::vector<std::wstring>, Error>
    enum_keys(RegHandle key);

    /// List the value names stored under `key`.
    [[nodiscard]] Result<std::vector<std::wstring>, Error>
    enum_values(RegHandle key);

    /// Return a stable, case-folded full path for an open key. This is used by
    /// Win32 registry-change notification matching without exposing RegNode
    /// pointers across the capsule/WinAPI boundary.
    [[nodiscard]] std::optional<std::wstring>
    canonical_key_path(RegHandle key) const;

    // -------------------------------------------------------------------------
    // Persistence
    // -------------------------------------------------------------------------

    /// Save all hives to JSON files in the given directory.
    /// Creates HKCR.json, HKCU.json, HKLM.json, HKU.json.
    void save(const std::filesystem::path& registry_dir);

    /// Load all hives from JSON files in the given directory.
    /// Missing files are silently skipped (hive keeps init_defaults data).
    void load(const std::filesystem::path& registry_dir);

    /// The WOW64 registry redirector: the physical path (root name first,
    /// e.g. {"HKEY_LOCAL_MACHINE", "SOFTWARE", "WOW6432Node", ...}) that
    /// `path` names in the 32-bit (`wow32`) or 64-bit view. Public for tests.
    [[nodiscard]] static std::vector<std::wstring> redirect_path(
        std::vector<std::wstring> path, bool wow32);

private:
    using Path = std::vector<std::wstring>;

    struct MergedKeyBinding {
        RegNode* user{nullptr};
        RegNode* machine{nullptr};
        std::wstring relative_path;
        bool wow32{false};
        Path user_path;     ///< physical path of the per-user classes key
        Path machine_path;  ///< physical path of the machine classes key
    };

    /// View and physical path of a non-merged handle.
    struct HandleInfo {
        RegView view{RegView::Default};
        Path path;
    };

    [[nodiscard]] bool handle_is_wow32(RegHandle handle) const;
    [[nodiscard]] RegNode* root_node(const std::wstring& name) const;
    RegNode* walk_physical(const Path& path, bool create);
    [[nodiscard]] static std::wstring path_string(const Path& path);
    /// `path` joined with its case kept, for live-journal records.
    [[nodiscard]] static std::wstring journal_path(const Path& path);
    void forget_handles_to(RegNode* node);
    Result<void, Error> delete_physical(const Path& path);
    void add_link_children(const Path& path,
                           std::map<std::wstring, std::wstring,
                                    RegCaseInsensitive>& names);

    /// Resolve a handle to its RegNode. Returns nullptr on invalid handle.
    RegNode* resolve(RegHandle handle) const;

    /// Walk (and optionally create) a path from `root` node.
    RegNode* walk_path(RegNode* root,
                       const std::vector<std::wstring>& parts,
                       bool create);

    void sync_live_journal();
    void record_live_key(std::wstring_view canonical_path);
    void record_live_value(std::wstring_view canonical_path,
                           std::wstring_view name,
                           const RegValue& value);
    void record_live_value_delete(std::wstring_view canonical_path,
                                  std::wstring_view name);
    void record_live_key_delete(std::wstring_view canonical_path);
    void append_live_record(std::string record);
    void apply_live_record(std::string_view record);
    RegNode* live_write_node(std::wstring_view canonical_path, bool create);
    void live_delete_node(std::wstring_view canonical_path);

    // Root hive nodes --------------------------------------------------------
    std::unique_ptr<RegNode> m_hkcr;
    std::unique_ptr<RegNode> m_hkcu;
    std::unique_ptr<RegNode> m_hklm;
    std::unique_ptr<RegNode> m_hku;

    // Handle bookkeeping -----------------------------------------------------
    RegHandle m_next_handle{0x1};  // first non-root handle
    std::map<RegHandle, RegNode*> m_handles;
    std::map<RegHandle, MergedKeyBinding> m_merged_handles;
    std::map<RegHandle, HandleInfo> m_handle_info;
    std::map<std::pair<RegHandle, RegView>, RegHandle> m_view_roots;
    bool m_wow64_process{false};
    std::vector<std::unique_ptr<RegNode>> m_live_retired_nodes;

    mutable std::recursive_mutex m_mutex;
    std::filesystem::path m_registry_dir;
    u64 m_live_journal_offset{0};

    /// Pre-populate default registry entries.
    void init_defaults();
};

} // namespace vitreo::capsule
