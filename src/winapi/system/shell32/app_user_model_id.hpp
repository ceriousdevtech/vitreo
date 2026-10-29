// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file app_user_model_id.hpp
/// @brief Application User Model IDs: the window property values
///        SHGetPropertyStoreForWindow stores, and the identity the taskbar
///        (here, the Dock) groups processes and windows by.
///
/// Sources: Learn "Application User Model IDs (AppUserModelIDs)",
/// SHGetPropertyStoreForWindow, System.AppUserModel.ID, STARTUPINFOW
/// (STARTF_TITLEISAPPID); SDK um/propkey.h for the keys; measured behaviour in
/// tools/oracle_probe/window_propstore_probe{32,64}.expected.
///
/// A window's store keeps every value as text and converts it to the
/// property's schema type when it is read:
///   * a string property is returned with leading and trailing white space
///     removed; when that changed the text GetValue returns
///     INPLACE_S_TRUNCATED, and text that trims to nothing reads as VT_EMPTY;
///   * a boolean property reads "true"/"false" or an integer (nonzero is
///     VARIANT_TRUE); other text is TYPE_E_TYPEMISMATCH with VT_EMPTY;
///   * a key the property system does not describe reads back as the stored
///     text, untrimmed, with S_FALSE.

#include <vitreo/common.hpp>

namespace vitreo::capsule { class Vfs; }
namespace vitreo::winapi { class Marshaler; class ShellPathManager; }

#include <array>
#include <optional>
#include <string>

namespace vitreo::winapi::shell {

using PropertyFmtid = std::array<u8, 16>;

/// PKEY_AppUserModel_* FMTID {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}.
inline constexpr PropertyFmtid kAppUserModelFmtid{
    0x55, 0x28, 0x4C, 0x9F, 0x79, 0x9F, 0x39, 0x4B,
    0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3};
/// PKEY_AppUserModel_ID property id.
inline constexpr u32 kAppUserModelIdPid = 5;

inline constexpr u32 kInplaceTruncated = 0x000401A0u;   // INPLACE_S_TRUNCATED
inline constexpr u32 kTypeMismatch = 0x80028CA0u;        // TYPE_E_TYPEMISMATCH

/// The schema type of a shell property, as far as Vitreo models it.
enum class ShellPropertyType : u8 {
    String,
    Boolean,
    /// Described by the property system with a type Vitreo does not convert
    /// to (System.AppUserModel.StartPinOption UInt32, ToastActivatorCLSID
    /// Guid). Read back as stored text, reported once as UNIMPLEMENTED.
    Unconverted,
    /// Not described by the property system.
    Unknown,
};

[[nodiscard]] ShellPropertyType shell_property_type(const PropertyFmtid& fmtid,
                                                    u32 pid) noexcept;

/// What IPropertyStore::GetValue returns for a stored value.
struct CanonicalShellValue {
    enum class Kind : u8 { Empty, String, Boolean };
    u32 hr{0};
    Kind kind{Kind::Empty};
    std::u16string text;
    bool flag{false};
};

[[nodiscard]] CanonicalShellValue canonical_shell_value(const PropertyFmtid& fmtid,
                                                        u32 pid,
                                                        const std::u16string& stored);

/// The explicit System.AppUserModel.ID stored on @p hwnd, as GetValue would
/// read it; nullopt when the window has none (or it trims to nothing).
[[nodiscard]] std::optional<std::u16string> window_app_user_model_id(u32 hwnd);

/// The identity the Dock groups by: the window-level ID of @p hwnd when it
/// has one, else @p process_explicit_id (SetCurrentProcessExplicitAppUserModelID
/// or STARTF_TITLEISAPPID), else the system-assigned ID, which Learn leaves to
/// heuristics; Vitreo derives it from the executable image, so processes of
/// one executable group together and a different executable stands alone.
/// Explicit IDs and image-derived IDs never collide. UTF-8.
[[nodiscard]] std::string effective_app_user_model_id(
    u32 hwnd, const std::optional<std::u16string>& process_explicit_id,
    std::string_view image_path);

/// SHAddToRecentDocs(UINT uFlags, LPCVOID pv). Records an item's use the way
/// the shell's Recent folder shows it (Learn shlobj_core SHAddToRecentDocs;
/// behaviour measured by tools/oracle_probe/shlwapi_regsec_shell_probe.c):
///   * SHARD_PIDL / SHARD_PATHA / SHARD_PATHW write "<leaf>.lnk" for the item
///     and "<folder>.lnk" for the folder holding it into CSIDL_RECENT, whether
///     or not the item exists; a relative path is taken against the current
///     directory;
///   * nothing is recorded for an .exe or for an item under %TEMP%;
///   * a NULL pv clears the usage data: the Recent folder's links go;
///   * an unknown uFlags is ignored.
/// Jump List usage data and the AppUserModelID / IShellLink / IShellItem
/// forms have no Vitreo destination store: they log UNIMPLEMENTED once.
void add_to_recent_docs(Marshaler& m, ShellPathManager& paths, capsule::Vfs& vfs);

} // namespace vitreo::winapi::shell
