#pragma once
/// @file msi.hpp
/// @brief MSI.dll — Windows Installer client API.
///
/// MsiManager is the msi.dll instance state shared by the engine areas; the
/// records, databases, views and summary information live in its
/// HandleTable (msi_database.hpp).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_core.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo { namespace capsule { class Vfs; } }

namespace vitreo::capsule { class Registry; }

namespace vitreo::winapi {

class Marshaler;

/// Windows Installer error codes used by this module. These are the standard
/// system error values, not MSI-private ones.
enum : u32 {
    MSI_ERROR_SUCCESS              = 0u,
    MSI_ERROR_INVALID_HANDLE       = 6u,
    MSI_ERROR_INVALID_PARAMETER    = 87u,
    MSI_ERROR_CALL_NOT_IMPLEMENTED = 120u,
    MSI_ERROR_OPEN_FAILED          = 110u,
    MSI_ERROR_MORE_DATA            = 234u,
    MSI_ERROR_NO_MORE_ITEMS        = 259u,
    MSI_ERROR_UNKNOWN_PRODUCT      = 1605u,
    MSI_ERROR_UNKNOWN_PROPERTY     = 1608u,
    MSI_ERROR_BAD_QUERY_SYNTAX     = 1615u,
    MSI_ERROR_INSTALL_USEREXIT     = 1602u,
    MSI_ERROR_INSTALL_FAILURE      = 1603u,
    MSI_ERROR_INSTALL_PACKAGE_OPEN_FAILED = 1619u,
    MSI_ERROR_INVALID_TABLE        = 1628u,
    MSI_ERROR_INSTALL_PACKAGE_INVALID = 1620u,
    MSI_ERROR_FUNCTION_FAILED      = 1627u,
};

/// INSTALLSTATE values returned by MsiQueryProductState. Signed: the error
/// states are negative and callers compare against them directly.
enum : i32 {
    INSTALLSTATE_NOTUSED    = -7,
    INSTALLSTATE_BADCONFIG  = -6,
    INSTALLSTATE_INCOMPLETE = -5,
    INSTALLSTATE_SOURCEABSENT = -4,
    INSTALLSTATE_MOREDATA   = -3,
    INSTALLSTATE_INVALIDARG = -2,
    INSTALLSTATE_UNKNOWN    = -1,
    INSTALLSTATE_BROKEN     = 0,
    INSTALLSTATE_ADVERTISED = 1,
    INSTALLSTATE_ABSENT     = 2,
    INSTALLSTATE_LOCAL      = 3,
    INSTALLSTATE_SOURCE     = 4,
    INSTALLSTATE_DEFAULT    = 5,
};

class MsiManager {
public:
    MsiManager() = default;

    /// Execute an msiexec command line, returning the process exit code.
    /// Runs synchronously; the caller presents it as an already-exited
    /// process. Codes are the documented Windows Installer ones
    /// (0 = success, 1603 = install failure, 1619 = package could not be
    /// opened, 1620 = package invalid).
    [[nodiscard]] u32 run_msiexec(const std::string& command_line);

    /// Wire the capsule VFS so packages can be read from the guest's drives.
    /// Without it the database entry points cannot open anything and say so
    /// rather than pretending the package is absent.
    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    /// WriteRegistryValues and product registration need the capsule registry.
    void set_registry(capsule::Registry* registry) noexcept {
        m_registry = registry;
    }
    void set_user_sid(std::wstring sid) { m_user_sid = std::move(sid); }

    // ---- Shared state for the engine areas (msi_<area>.cpp) ----
    /// Every MSIHANDLE (msi_core.hpp). The legacy maps below predate it and
    /// are migrated into it by the database area.
    [[nodiscard]] msi::HandleTable& handles() noexcept { return m_handles; }
    [[nodiscard]] capsule::Vfs* vfs() const noexcept { return m_vfs; }
    [[nodiscard]] capsule::Registry* registry() const noexcept { return m_registry; }
    [[nodiscard]] const std::wstring& user_sid() const noexcept { return m_user_sid; }

    // ---- Engine glue (msi_sequence.cpp; the engine area owns these) ----
    /// The system state the built-in properties describe; default:
    /// msi::default_machine_state(registry, user_sid).
    void set_machine_state(msi::MachineState state) { m_machine_state = std::move(state); m_machine_state_set = true; }
    [[nodiscard]] msi::MachineState machine_state() const;
    /// The internal UI host the engine presents dialogs with (msi_ui.cpp).
    void set_ui_host(msi::UiHost* host) noexcept { m_ui_host = host; }
    /// Services for an engine run in this process: VFS, registry, this
    /// manager, the process's custom-action host, the UI host.
    [[nodiscard]] msi::Services engine_services();
    /// MsiSetInternalUI's level (INSTALLUILEVEL_*), for MsiOpenPackage sessions.
    [[nodiscard]] u32 engine_ui_level() const noexcept { return m_internal_ui_level; }
    /// MsiSetInternalUI (msi_ui.cpp) keeps engine_ui_level() current.
    void set_engine_ui_level(u32 level) noexcept { m_internal_ui_level = level; }
    [[nodiscard]] std::optional<std::vector<u8>> engine_read_file(const std::string& path) const { return read_guest_file(path); }
    [[nodiscard]] bool engine_write_file(const std::string& path, std::span<const u8> bytes) const { return write_guest_file(path, bytes); }
    /// Legacy InstallFiles (until the files area's action replaces it).
    [[nodiscard]] u32 engine_install_files(msi::Session& session, const std::string& package_path) {
        return install_files(session, package_path);
    }
    /// A copy of the record behind @p handle.
    [[nodiscard]] std::optional<msi::Record> engine_legacy_record(u32 handle);
    /// The package behind a database MSIHANDLE (MsiOpenPackage "#handle").
    [[nodiscard]] std::shared_ptr<msi::Package> engine_database_package(u32 handle);

    void impl_MsiFormatRecordW(Marshaler& m);
    void impl_MsiEnumRelatedProductsW(Marshaler& m);
    void impl_MsiSetInternalUI(Marshaler& m);
    void impl_MsiSetExternalUIW(Marshaler& m);
    void impl_MsiSetExternalUIRecord(Marshaler& m);
    void impl_MsiEnableLogW(Marshaler& m);
    void impl_MsiInstallProductW(Marshaler& m);
    void impl_MsiSourceListAddSourceExW(Marshaler& m);
    void impl_MsiGetProductInfoExW(Marshaler& m);
    void impl_MsiEnumProductsExW(Marshaler& m);
    void impl_MsiGetPatchInfoExW(Marshaler& m);
    void impl_MsiDetermineApplicablePatchesW(Marshaler& m);

    /// Installed-product registration, persisted in the capsule registry so
    /// the state outlives the (elevated) process that performed the install.
    void register_product(msi::Session& session,
                          const std::string& package_path);
    [[nodiscard]] bool is_product_registered(
        const std::string& product_code) const;
    [[nodiscard]] std::string product_property(
        const std::string& product_code, const std::string& property) const;
    void set_product_property(const std::string& product_code,
                              const std::string& property,
                              const std::string& value);
    [[nodiscard]] std::vector<std::string> registered_products() const;

    // -- Product query API -------------------------------------------------
    void impl_MsiQueryProductStateW(Marshaler& m);
    void impl_MsiEnumProductsW(Marshaler& m);
    void impl_MsiEnumProductsA(Marshaler& m);
    void impl_MsiGetProductInfoW(Marshaler& m);

private:
    msi::HandleTable m_handles;
    msi::MachineState m_machine_state;
    bool m_machine_state_set = false;
    msi::UiHost* m_ui_host = nullptr;
    std::wstring m_user_sid;
    [[nodiscard]] i32 product_state(const std::string& product_code) const;
    [[nodiscard]] std::wstring install_properties_path(const std::string& product_code) const;
    [[nodiscard]] std::optional<std::vector<std::string>> advertised_products(bool machine_only = false) const;
    /// Windows Installer engine UI state, exchanged by MsiSetInternalUI.
    /// INSTALLUILEVEL_DEFAULT until a caller changes it.
    u32       m_internal_ui_level = 1;
    GuestAddr m_internal_ui_owner = 0;
    /// External UI handlers, exchanged by MsiSetExternalUI{W,Record}.
    GuestAddr m_external_ui_handler = 0;
    GuestAddr m_external_ui_record_handler = 0;
    u32       m_external_ui_filter = 0;
    GuestAddr m_external_ui_context = 0;

    /// Run the file-copy half of InstallExecuteSequence for @p session.
    /// Returns a Windows Installer error code.
    [[nodiscard]] u32 install_files(msi::Session& session,
                                    const std::string& package_path);

    /// Read a whole file out of the capsule VFS.
    [[nodiscard]] std::optional<std::vector<u8>> read_guest_file(
        const std::string& path) const;

    /// Create every missing directory along @p path's parent chain. Returns
    /// false only when a level could not be created, not when it already
    /// exists.
    [[nodiscard]] bool ensure_parent_directories(const std::string& path) const;

    /// Write @p bytes to @p path, replacing any existing file, after creating
    /// its parent directories.
    [[nodiscard]] bool write_guest_file(const std::string& path,
                                        std::span<const u8> bytes) const;

    /// Copy @p value into the guest buffer following the standard MSI
    /// "buffer + pcchValueBuf" convention, returning the MSI error code.
    /// On entry *pcch is the buffer capacity in characters *including* the
    /// null terminator; on exit it is the character count *excluding* it.
    u32 write_sized_wide_string(Marshaler& m,
                                GuestAddr buffer,
                                GuestAddr pcch,
                                const std::string& value);

    capsule::Registry* m_registry = nullptr;
    capsule::Vfs* m_vfs = nullptr;
};

/// All msi.dll export names (the generated table).
[[nodiscard]] std::vector<std::string> msi_export_names();
/// Every registrar's handlers, by export name (tests call through this).
[[nodiscard]] msi::HandlerMap msi_handlers(MsiManager& mgr);
/// Build the msi.dll export table: every export with its ordinal (the same
/// on x86 and x64) and x86 cleanup bytes, wired to @p mgr.
DllExports create_msi_thunks(MsiManager& mgr);

} // namespace vitreo::winapi
