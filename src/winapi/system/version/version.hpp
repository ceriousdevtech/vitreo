#pragma once

/// @file version.hpp
/// @brief version.dll API implementations (WIN32-087)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <span>
#include <string>
#include <vector>

namespace vitreo { namespace capsule { class Vfs; } }

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;

/// Error code for missing version resource
constexpr u32 ERROR_RESOURCE_TYPE_NOT_FOUND = 1813;

struct VersionQueryResult {
    u32 value_offset{0};
    /// Bytes for binary values; UTF-16 code units for string values.
    u32 length{0};
    /// wType == 1: the value is UTF-16 text (VerQueryValueA converts it).
    bool text{false};
};

namespace version_install_flags {
constexpr u32 VIFF_FORCEINSTALL = 0x00000001u;
constexpr u32 VIFF_DONTDELETEOLD = 0x00000002u;

constexpr u32 VIF_TEMPFILE = 0x00000001u;
constexpr u32 VIF_MISMATCH = 0x00000002u;
constexpr u32 VIF_ACCESSVIOLATION = 0x00000200u;
constexpr u32 VIF_SHARINGVIOLATION = 0x00000400u;
constexpr u32 VIF_CANNOTCREATE = 0x00000800u;
constexpr u32 VIF_OUTOFMEMORY = 0x00008000u;
constexpr u32 VIF_CANNOTREADSRC = 0x00010000u;
constexpr u32 VIF_OUTOFSPACE = 0x00000100u;
constexpr u32 VIF_BUFFTOOSMALL = 0x00040000u;
constexpr u32 VIF_CANNOTLOADLZ32 = 0x00080000u;
} // namespace version_install_flags

// =============================================================================
// VersionManager
// =============================================================================

class VersionManager {
public:
    VersionManager() = default;

    /// Wire the capsule VFS so the real VS_VERSION_INFO resource can be read
    /// out of the PE on disk. Without it every query fails, which makes apps
    /// report their own version as unknown.
    void set_vfs(capsule::Vfs* vfs) { m_vfs = vfs; }
    /// Wire the loaded-module table so Vitreo-owned synthetic Windows DLLs
    /// can expose version metadata without a backing PE in the capsule.
    void set_module_manager(ModuleManager* modules) {
        m_module_manager = modules;
    }
    /// DLLs Vitreo itself provides (no file in the capsule) — a bare name of
    /// one of these still resolves, as it would to System32 on Windows.
    void set_builtin_dll_query(std::function<bool(std::string_view)> query) {
        m_builtin_dll = std::move(query);
    }
    /// PATH, the last step of the LoadLibrary search order.
    void set_path_lookup(std::function<std::string()> lookup) {
        m_path_lookup = std::move(lookup);
    }

    /// GetFileVersionInfoSizeW/A — size of the file's RT_VERSION resource,
    /// or 0 with ERROR_RESOURCE_TYPE_NOT_FOUND when it has none.
    [[nodiscard]] u32 get_file_version_info_size(const std::string& filename,
                                                  u32& handle) const;

    /// GetFileVersionInfoW/A — copy the file's RT_VERSION resource out.
    [[nodiscard]] bool get_file_version_info(const std::string& filename,
                                              u32 handle, u32 len,
                                              void* data) const;

    /// GetFileVersionInfoExW — delegates to get_file_version_info
    [[nodiscard]] bool get_file_version_info_ex(u32 flags,
                                                 const std::string& filename,
                                                 u32 handle, u32 len,
                                                 void* data) const;

    /// VerQueryValueW/A — resolve a sub-block within a bounded version block.
    [[nodiscard]] std::optional<VersionQueryResult> ver_query_value(
        std::span<const u8> block,
        const std::string& sub_block) const;

    /// VerInstallFileA — install a plain or SZDD-compressed source file through
    /// the capsule VFS. Existing destinations conservatively produce a
    /// recoverable mismatch unless VIFF_FORCEINSTALL is requested.
    [[nodiscard]] u32 install_file(
        u32 flags,
        const std::string& source_filename,
        const std::string& destination_filename,
        const std::string& source_directory,
        const std::string& destination_directory,
        const std::string& current_directory,
        std::string& temporary_file) const;

private:
    capsule::Vfs* m_vfs = nullptr;
    ModuleManager* m_module_manager = nullptr;
    std::function<bool(std::string_view)> m_builtin_dll;
    std::function<std::string()> m_path_lookup;
    /// Extract the RT_VERSION resource bytes. Empty on miss with `error` set
    /// to the Win32 reason (ERROR_FILE_NOT_FOUND for a missing file,
    /// ERROR_RESOURCE_DATA_NOT_FOUND for a non-PE file,
    /// ERROR_RESOURCE_TYPE_NOT_FOUND for a PE without RT_VERSION —
    /// version_probe.expected).
    [[nodiscard]] std::vector<u8> read_version_resource(const std::string& path,
                                                        u32& error) const;
    [[nodiscard]] std::vector<u8> read_disk_version_resource(
        const std::string& path, u32& error) const;
    /// LoadLibrary search order for a name without a path: the application
    /// directory, System32, the Windows directory, the current directory,
    /// then PATH. Empty when no candidate exists.
    [[nodiscard]] std::string search_file(const std::string& name) const;
public:
    /// Last error of this thread's most recent version API call.
    [[nodiscard]] u32 last_error() const;
};

/// The RT_VERSION resource of a PE image held in memory (the part of
/// GetFileVersionInfo after the file is read). Empty on miss, with `error`
/// ERROR_RESOURCE_DATA_NOT_FOUND for a non-PE image or
/// ERROR_RESOURCE_TYPE_NOT_FOUND for a PE without RT_VERSION.
[[nodiscard]] std::vector<u8> version_resource_from_image(const std::vector<u8>& image,
                                                          u32& error);

/// VS_FIXEDFILEINFO dwFileVersionMS:LS of @p image, or nullopt when it has
/// no version resource (or one without fixed info).
[[nodiscard]] std::optional<u64> fixed_file_version(const std::vector<u8>& image);

/// Create version.dll export thunks
DllExports create_version_thunks(VersionManager& mgr);

} // namespace vitreo::winapi
