#pragma once

/// @file registry.hpp
/// @brief advapi32 registry API thunks (WIN32-060)

#include <vitreo/common.hpp>
#include "winapi/kernel32/handle.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "capsule/registry/registry.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

class SyncManager;
class EnvironmentManager;

// =============================================================================
// Win32 Error Codes (registry-related)
// =============================================================================

constexpr u32 ERROR_SUCCESS        = 0;
constexpr u32 ERROR_FILE_NOT_FOUND = 2;
constexpr u32 ERROR_ACCESS_DENIED  = 5;
constexpr u32 ERROR_INVALID_HANDLE = 6;
constexpr u32 ERROR_INVALID_PARAMETER = 87;
constexpr u32 ERROR_CALL_NOT_IMPLEMENTED = 120;
constexpr u32 ERROR_MORE_DATA      = 234;
constexpr u32 ERROR_NO_MORE_ITEMS = 259;
constexpr u32 ERROR_NOACCESS      = 998;
constexpr u32 ERROR_UNSUPPORTED_TYPE = 1630;

// =============================================================================
// Win32 Registry Types
// =============================================================================

constexpr u32 REG_NONE      = 0;
constexpr u32 REG_SZ        = 1;
constexpr u32 REG_EXPAND_SZ = 2;
constexpr u32 REG_BINARY    = 3;
constexpr u32 REG_DWORD     = 4;
constexpr u32 REG_MULTI_SZ  = 7;
constexpr u32 REG_QWORD     = 11;

// =============================================================================
// Disposition constants for RegCreateKeyEx
// =============================================================================

constexpr u32 REG_CREATED_NEW_KEY     = 1;
constexpr u32 REG_OPENED_EXISTING_KEY = 2;

// =============================================================================
// RegistryThunks
// =============================================================================

/// Registry thunk manager — bridges Win32 API calls to capsule Registry.
///
/// Each method corresponds to one Win32 registry function and returns a
/// Win32 error code (ERROR_SUCCESS on success).
class RegistryThunks {
public:
    RegistryThunks(capsule::Registry& registry, HandleTable& handles);

    // No copy / no move (reference members)
    RegistryThunks(const RegistryThunks&) = delete;
    RegistryThunks& operator=(const RegistryThunks&) = delete;

    void set_sync_manager(SyncManager* manager) noexcept {
        m_sync_manager = manager;
    }
    void set_environment_manager(EnvironmentManager* manager) noexcept {
        m_environment_manager = manager;
    }

    // -------------------------------------------------------------------------
    // Win32 API implementations
    // -------------------------------------------------------------------------

    u32 reg_open_key_ex_w(u64 hkey, const std::wstring& subkey, u32 options,
                          u32 sam_desired, u64* result_handle);

    u32 reg_open_key_ex_a(u64 hkey, const std::string& subkey, u32 options,
                          u32 sam_desired, u64* result_handle);

    /// RegCreateKeyEx core. `sam_desired` carries the KEY_WOW64_* view.
    u32 reg_create_key_ex_w(u64 hkey, const std::wstring& subkey,
                            u64* result_handle, u32* disposition,
                            u32 sam_desired = 0);

    u32 reg_create_key_ex_a(u64 hkey, const std::string& subkey,
                            u64* result_handle, u32* disposition,
                            u32 sam_desired = 0);

    u32 reg_open_key_a(u64 hkey, const std::string& subkey,
                       u64* result_handle);

    u32 reg_connect_registry(u64 hkey, u64* result_handle);

    u32 reg_query_value_ex_w(u64 hkey, const std::wstring& value_name,
                             u32* type_out, u8* data_out, u32* data_size);

    u32 reg_query_value_ex_a(u64 hkey, const std::string& value_name,
                             u32* type_out, u8* data_out, u32* data_size);

    u32 reg_get_value_w(u64 hkey, const std::wstring& subkey,
                        const std::wstring& value_name, u32 flags,
                        u32* type_out, u8* data_out, u32* data_size);

    u32 reg_get_value_a(u64 hkey, const std::string& subkey,
                        const std::string& value_name, u32 flags,
                        u32* type_out, u8* data_out, u32* data_size);

    u32 reg_set_value_ex_w(u64 hkey, const std::wstring& value_name,
                           u32 type, const u8* data, u32 data_size);

    u32 reg_set_value_ex_a(u64 hkey, const std::string& value_name,
                           u32 type, const u8* data, u32 data_size);

    u32 reg_set_key_value_w(u64 hkey, const std::wstring& subkey,
                            const std::wstring& value_name, u32 type,
                            const u8* data, u32 data_size);

    u32 reg_delete_value_w(u64 hkey, const std::wstring& value_name);

    u32 reg_delete_key_value_w(u64 hkey, const std::wstring& subkey,
                               const std::wstring& value_name);

    u32 reg_close_key(u64 hkey);

    u32 reg_notify_change_key_value(u64 hkey, bool watch_subtree,
                                    u32 notify_filter, Handle event,
                                    bool asynchronous);

    u32 reg_enum_key_ex_w(u64 hkey, u32 index, std::wstring& name_out);

    u32 reg_enum_value_w(u64 hkey, u32 index, std::wstring& name_out,
                         u32* type_out, u8* data_out, u32* data_size);

    /// RegEnumValue core: the index-th value's name and stored value.
    /// ERROR_INVALID_HANDLE for an unknown key, ERROR_NO_MORE_ITEMS past the
    /// end. The entry points apply the W/A buffer rules
    /// (s4_registry_probe.expected enumw_* / enuma_*).
    u32 reg_enum_value_entry(u64 hkey, u32 index, std::wstring& name_out,
                             capsule::RegValue& value_out);

    u32 reg_query_value_w(u64 hkey, const std::wstring& subkey,
                          u8* data_out, u32* data_size);

    u32 reg_enum_key_ex_a(u64 hkey, u32 index, std::string& name_out);

    /// RegDeleteKey core. Deletes `subkey` under `hkey`, refusing (as Windows
    /// NT does) when the subkey still has subkeys of its own.
    u32 reg_delete_key(u64 hkey, const std::wstring& subkey,
                       capsule::RegView view = capsule::RegView::Default);

    /// RegDeleteKeyExW core: deletes `subkey` in the view samDesired names
    /// (KEY_WOW64_32KEY / KEY_WOW64_64KEY; other access bits are ignored).
    u32 reg_delete_key_ex(u64 hkey, const std::wstring& subkey,
                          u32 sam_desired, u32 reserved);

    /// RegDeleteTree: recursively delete subkeys and values. `subkey` empty
    /// means clear `hkey` in place (the key itself remains).
    u32 reg_delete_tree(u64 hkey, const std::wstring& subkey);

    /// Key metadata for RegQueryInfoKey. Despite the name this returns only
    /// counts and lengths — no strings — so both the ANSI and wide entry
    /// points share it. Lengths are in characters, excluding the terminator,
    /// which is what callers add before allocating.
    u32 reg_query_info_key_a(u64 hkey, u32* num_sub_keys, u32* max_subkey_len,
                             u32* num_values, u32* max_value_len,
                             u32* max_data_len);

    u32 reg_open_key_w(u64 hkey, const std::wstring& subkey,
                       u64* result_handle);

    /// The registry view samDesired's KEY_WOW64_* bits select; false when
    /// both are set (ERROR_INVALID_PARAMETER).
    [[nodiscard]] static bool wow64_view_from_sam(u32 sam_desired,
                                                  capsule::RegView& view) noexcept;

    /// True when the guest HKEY names a well-known root or a live registry
    /// handle. Lets callers report ERROR_INVALID_HANDLE without opening a key.
    [[nodiscard]] bool is_valid_key(u64 hkey) const;

    /// The full path of the key `hkey` names, case-folded to lower case and
    /// rooted at its hive ("hkey_current_user\\software\\..."); nullopt
    /// for a handle that names no key. The identity a key's security
    /// descriptor is held against (RegGetKeySecurity / RegSetKeySecurity).
    [[nodiscard]] std::optional<std::wstring> key_path(u64 hkey) const;

private:
    capsule::Registry& m_registry;
    HandleTable&       m_handles;
    SyncManager*       m_sync_manager{nullptr};
    EnvironmentManager* m_environment_manager{nullptr};

    struct SynchronousRegistryWait {
        std::mutex mutex;
        std::condition_variable cv;
        bool signaled{false};
    };
    struct RegistryWatch {
        u64 registration_hkey{0};
        std::wstring key_path;
        bool watch_subtree{false};
        u32 notify_filter{0};
        Handle event{NULL_HANDLE};
        std::shared_ptr<SynchronousRegistryWait> synchronous_wait;
    };
    std::mutex m_watch_mutex;
    std::vector<RegistryWatch> m_watches;

    void notify_registry_change(const std::wstring& changed_key_path,
                                u32 change_filter, bool name_change);
    void cancel_watches_for_handle(u64 hkey);

    /// Map a guest HKEY to a capsule RegHandle.
    /// Well-known root constants are returned directly; everything else is
    /// looked up in the handle table.
    [[nodiscard]] capsule::RegHandle resolve_hkey(u64 hkey) const;

public:
    /// Convert a capsule value's type to its Win32 REG_* constant.
    [[nodiscard]] static u32 reg_type_to_win32(const capsule::RegValue& v) noexcept;

    /// Serialise a RegValue into a byte buffer and return its size.
    /// If @p out is nullptr, only the required size is returned.
    [[nodiscard]] static u32 serialise_value(const capsule::RegValue& val,
                                             u8* out, u32 buf_size);

    [[nodiscard]] static u32 serialise_value_ansi(
        const capsule::RegValue& val, u8* out, u32 buf_size);
private:

    u32 query_get_value(u64 hkey, const std::wstring& subkey,
                        const std::wstring& value_name, u32 flags,
                        capsule::RegValue& value_out, u32& type_out);
};

// =============================================================================
// DLL Export Factory
// =============================================================================

/// Create the advapi32.dll registry DLL export table.
[[nodiscard]] DllExports create_advapi32_registry_thunks(RegistryThunks& thunks);

} // namespace vitreo::winapi
