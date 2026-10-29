// SPDX-License-Identifier: LGPL-2.1-only
/// @file external_app_mapping.hpp
/// @brief Capsule-side mapping for an executable stored outside the capsule.

#pragma once

#include <vitreo/common.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace vitreo::cli {

struct DetachedExternalAppMapping {
    std::string link_path;
    std::string target_path;
    std::string lease_path;
    int lease_fd{-1};
};

/// Why a capsule-side mapping could not be established.
enum class ExternalAppMappingError : u8 {
    None = 0,
    InvalidHostDirectory,   ///< The host directory is missing or not a directory
    AppsDirectoryUnusable,  ///< C:\\Vitreo\\Apps could not be created
    NoAvailableName,        ///< Every candidate name is occupied by unrelated state
    LinkCreationFailed,     ///< The filesystem refused to create the symlink
};

[[nodiscard]] const char* external_app_mapping_error_message(
    ExternalAppMappingError error) noexcept;

/// An exclusively owned symlink that exposes one host directory to the guest.
///
/// Running an executable from outside the capsule still requires the guest to
/// reach it — and its sibling DLLs, data files, and child executables — through
/// a Windows path. Vitreo publishes the executable's host directory as
/// `C:\Vitreo\Apps\<guest_directory_name()>`.
///
/// Ownership is exclusive and is established with an atomic `symlink(2)`:
///
/// - two concurrent launches can never believe they own the same entry;
/// - a name already used by a real directory (an installed portable app, for
///   example) is never removed to make room, it is simply skipped;
/// - two unrelated host directories sharing a basename receive distinct guest
///   names instead of overwriting each other;
/// - release removes the symlink only when it still points at the directory
///   this mapping published.
///
/// The mapping must outlive every guest process that resolves paths through it,
/// including child processes; callers that still own workers hand the link to
/// `ipc::ProcessSession::defer_symlink_cleanup` via `detach()`.
class ExternalAppMapping {
public:
    ExternalAppMapping() = default;

    /// Removes the mapping when it is still owned.
    ~ExternalAppMapping();

    ExternalAppMapping(const ExternalAppMapping&) = delete;
    ExternalAppMapping& operator=(const ExternalAppMapping&) = delete;
    ExternalAppMapping(ExternalAppMapping&& other) noexcept;
    ExternalAppMapping& operator=(ExternalAppMapping&& other) noexcept;

    /// Publish @p host_directory inside the capsule rooted at @p capsule_root.
    ///
    /// @param capsule_root   Host path of the capsule (…/.vitreo/capsules/<name>).
    /// @param host_directory Host directory holding the external executable.
    /// @param error          Receives the failure reason when the claim fails.
    /// @return An owning mapping, or an unowned mapping when @p error is set.
    [[nodiscard]] static ExternalAppMapping claim(
        std::string_view capsule_root,
        std::string_view host_directory,
        ExternalAppMappingError& error);

    /// True when this object owns a live capsule-side symlink.
    [[nodiscard]] bool is_owned() const noexcept { return m_owned; }

    /// Capsule-side directory name, e.g. `Solitaire` or `Solitaire.4f2a91c4`.
    [[nodiscard]] const std::string& guest_directory_name() const noexcept {
        return m_guest_directory_name;
    }

    /// Host path of the symlink itself.
    [[nodiscard]] const std::string& link_path() const noexcept {
        return m_link_path;
    }

    /// Host directory the symlink points at.
    [[nodiscard]] const std::string& target_path() const noexcept {
        return m_target_path;
    }

    /// Give up ownership so another component can remove the link later while
    /// retaining the advisory lease that prevents a concurrent claim.
    [[nodiscard]] DetachedExternalAppMapping detach();

    /// Remove the link now, but only when it still resolves to `target_path()`.
    void release();

private:
    std::string m_guest_directory_name;
    std::string m_link_path;
    std::string m_target_path;
    std::string m_lease_path;
    int m_lease_fd{-1};
    bool m_owned = false;
};

} // namespace vitreo::cli
