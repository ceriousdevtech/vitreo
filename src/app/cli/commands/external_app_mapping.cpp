// SPDX-License-Identifier: LGPL-2.1-only
// Capsule-side mapping for an executable stored outside the capsule.

#include "external_app_mapping.hpp"

#include "ipc/process_session.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace vitreo::cli {

namespace fs = std::filesystem;

namespace {

/// Number of `<base>.<hash>-<n>` fallbacks tried before the claim fails.
/// Reaching this bound means many unrelated entries occupy the same names,
/// which is a capsule-state problem the guest must not silently inherit.
constexpr int kMaxNumberedCandidates = 16;

/// FNV-1a over the canonical host directory, so the same directory always maps
/// to the same guest name and unrelated directories sharing a basename do not.
[[nodiscard]] std::string directory_fingerprint(std::string_view path) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const char ch : path) {
        hash ^= static_cast<u8>(ch);
        hash *= 0x100000001b3ULL;
    }
    const u32 folded = static_cast<u32>(hash ^ (hash >> 32));
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<usize>(i)] = kDigits[(folded >> ((7 - i) * 4)) & 0xFu];
    }
    return out;
}

/// Reduce a host basename to something safe as a capsule-side directory entry
/// and as a Windows path component.
[[nodiscard]] std::string sanitize_component(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const bool safe =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '_' ||
            ch == '.' || ch == '(' || ch == ')' || ch == '+' || ch == '&';
        out.push_back(safe ? ch : '_');
    }
    // Leading/trailing dots and spaces are not addressable from Windows paths.
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.size() > 64) out.resize(64);
    if (out.empty()) out = "App";
    return out;
}

/// A symlink whose target no longer exists cannot serve any guest and is not
/// owned by a live launch; reclaiming it keeps repeated runs on a stable name.
[[nodiscard]] bool is_dangling_symlink(const fs::path& candidate) {
    std::error_code ec;
    const auto link_status = fs::symlink_status(candidate, ec);
    if (ec || !fs::is_symlink(link_status)) return false;
    const auto resolved_status = fs::status(candidate, ec);
    return ec || !fs::exists(resolved_status);
}

/// @return true when the claim succeeded, false when the caller should try the
///         next candidate. @p fatal is set when no candidate can succeed.
[[nodiscard]] bool try_claim(const fs::path& candidate,
                             const std::string& target,
                             std::string& lease_path,
                             int& lease_fd,
                             bool& fatal) {
    fatal = false;
    lease_path = candidate.string() + ".vitreo-lease";
    lease_fd = ::open(lease_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lease_fd < 0) {
        fatal = true;
        return false;
    }
    if (::flock(lease_fd, LOCK_EX | LOCK_NB) != 0) {
        (void)::close(lease_fd);
        lease_fd = -1;
        return false;
    }

    auto release_lease = [&] {
        (void)::unlink(lease_path.c_str());
        (void)::close(lease_fd);
        lease_fd = -1;
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (::symlink(target.c_str(), candidate.c_str()) == 0) return true;
        if (errno != EEXIST) {
            fatal = true;
            VITREO_TRACE(
                "[vitreo] app mapping: cannot create link '%s' -> '%s': %s\n",
                candidate.c_str(), target.c_str(), std::strerror(errno));
            release_lease();
            return false;
        }

        std::error_code ec;
        const auto status = fs::symlink_status(candidate, ec);
        fs::path current;
        if (!ec && fs::is_symlink(status)) {
            current = fs::read_symlink(candidate, ec);
        }
        const bool stale_same_target =
            !ec && current.string() == target;
        if (attempt == 0 &&
            (is_dangling_symlink(candidate) || stale_same_target)) {
            std::error_code remove_error;
            fs::remove(candidate, remove_error);
            if (!remove_error) {
                VITREO_TRACE(
                    "[vitreo] app mapping: reclaimed unleased link '%s'\n",
                    candidate.c_str());
                continue;
            }
        }
        break;
    }
    release_lease();
    VITREO_TRACE("[vitreo] app mapping: '%s' is occupied, trying next name\n",
                 candidate.c_str());
    return false;
}

} // namespace

const char* external_app_mapping_error_message(
    ExternalAppMappingError error) noexcept {
    switch (error) {
    case ExternalAppMappingError::None:
        return "no error";
    case ExternalAppMappingError::InvalidHostDirectory:
        return "the executable's containing directory does not exist";
    case ExternalAppMappingError::AppsDirectoryUnusable:
        return "the capsule directory C:\\Vitreo\\Apps could not be created";
    case ExternalAppMappingError::NoAvailableName:
        return "no free name is available under C:\\Vitreo\\Apps";
    case ExternalAppMappingError::LinkCreationFailed:
        return "the capsule filesystem refused to create the mapping";
    }
    return "unknown mapping error";
}

ExternalAppMapping::~ExternalAppMapping() { release(); }

ExternalAppMapping::ExternalAppMapping(ExternalAppMapping&& other) noexcept
    : m_guest_directory_name(std::move(other.m_guest_directory_name)),
      m_link_path(std::move(other.m_link_path)),
      m_target_path(std::move(other.m_target_path)),
      m_lease_path(std::move(other.m_lease_path)),
      m_lease_fd(other.m_lease_fd),
      m_owned(other.m_owned) {
    other.m_lease_fd = -1;
    other.m_owned = false;
}

ExternalAppMapping& ExternalAppMapping::operator=(
    ExternalAppMapping&& other) noexcept {
    if (this != &other) {
        release();
        m_guest_directory_name = std::move(other.m_guest_directory_name);
        m_link_path = std::move(other.m_link_path);
        m_target_path = std::move(other.m_target_path);
        m_lease_path = std::move(other.m_lease_path);
        m_lease_fd = other.m_lease_fd;
        m_owned = other.m_owned;
        other.m_lease_fd = -1;
        other.m_owned = false;
    }
    return *this;
}

ExternalAppMapping ExternalAppMapping::claim(
    std::string_view capsule_root,
    std::string_view host_directory,
    ExternalAppMappingError& error) {
    ExternalAppMapping mapping;
    error = ExternalAppMappingError::None;

    std::error_code ec;
    const fs::path canonical_target =
        fs::weakly_canonical(fs::path(host_directory), ec);
    const fs::path target =
        (ec || canonical_target.empty()) ? fs::path(host_directory)
                                         : canonical_target;
    if (!fs::is_directory(target, ec)) {
        error = ExternalAppMappingError::InvalidHostDirectory;
        VITREO_TRACE("[vitreo] app mapping: '%s' is not a directory\n",
                     target.c_str());
        return mapping;
    }

    const fs::path apps_dir =
        fs::path(std::string(capsule_root)) / "drives" / "c" / "Vitreo" / "Apps";
    fs::create_directories(apps_dir, ec);
    if (!fs::is_directory(apps_dir, ec)) {
        error = ExternalAppMappingError::AppsDirectoryUnusable;
        VITREO_TRACE("[vitreo] app mapping: cannot use apps directory '%s'\n",
                     apps_dir.c_str());
        return mapping;
    }

    const std::string target_string = target.string();
    const std::string base = sanitize_component(target.filename().string());
    const std::string fingerprint = directory_fingerprint(target_string);

    for (int index = 0; index <= kMaxNumberedCandidates; ++index) {
        std::string name = base;
        if (index == 1) {
            name += "." + fingerprint;
        } else if (index > 1) {
            name += "." + fingerprint + "-" + std::to_string(index);
        }
        const fs::path candidate = apps_dir / name;
        bool fatal = false;
        std::string lease_path;
        int lease_fd = -1;
        if (try_claim(candidate, target_string, lease_path, lease_fd, fatal)) {
            mapping.m_guest_directory_name = name;
            mapping.m_link_path = candidate.string();
            mapping.m_target_path = target_string;
            mapping.m_lease_path = std::move(lease_path);
            mapping.m_lease_fd = lease_fd;
            mapping.m_owned = true;
            VITREO_TRACE(
                "[vitreo] app mapping: C:\\Vitreo\\Apps\\%s -> %s\n",
                name.c_str(), target_string.c_str());
            return mapping;
        }
        if (fatal) {
            error = ExternalAppMappingError::LinkCreationFailed;
            return mapping;
        }
    }

    error = ExternalAppMappingError::NoAvailableName;
    VITREO_TRACE(
        "[vitreo] app mapping: exhausted candidates for '%s' under '%s'\n",
        base.c_str(), apps_dir.c_str());
    return mapping;
}

DetachedExternalAppMapping ExternalAppMapping::detach() {
    if (!m_owned) return {};
    m_owned = false;
    DetachedExternalAppMapping detached{
        std::move(m_link_path), std::move(m_target_path),
        std::move(m_lease_path), m_lease_fd};
    m_lease_fd = -1;
    return detached;
}

void ExternalAppMapping::release() {
    if (!m_owned) return;
    m_owned = false;
    if (ipc::remove_mapped_symlink(m_link_path, m_target_path)) {
        VITREO_TRACE("[vitreo] app mapping: removed '%s'\n",
                     m_link_path.c_str());
    }
    if (!m_lease_path.empty()) (void)::unlink(m_lease_path.c_str());
    if (m_lease_fd >= 0) (void)::close(m_lease_fd);
    m_lease_fd = -1;
}

} // namespace vitreo::cli
