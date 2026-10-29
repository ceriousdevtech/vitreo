/// @file directory.cpp
/// @brief DirHandle and DirectoryOps implementation

#include "directory.hpp"

#include <string>

namespace vitreo::capsule {

// =============================================================================
// DirHandle
// =============================================================================

Result<std::optional<DirectoryEntry>, VfsError> DirHandle::next() noexcept {
    if (!m_open) {
        return Result<std::optional<DirectoryEntry>, VfsError>::err(
            VfsError::HandleClosed);
    }
    if (m_index >= m_entries.size()) {
        return Result<std::optional<DirectoryEntry>, VfsError>::ok(std::nullopt);
    }
    return Result<std::optional<DirectoryEntry>, VfsError>::ok(
        m_entries[m_index++]);
}

Result<void, VfsError> DirHandle::rewind() noexcept {
    if (!m_open) {
        return Result<void, VfsError>::err(VfsError::HandleClosed);
    }
    m_index = 0;
    return Result<void, VfsError>::ok();
}

void DirHandle::close() noexcept {
    m_open = false;
}

// =============================================================================
// DirectoryOps
// =============================================================================

Result<void, VfsError> DirectoryOps::create_directory(
    std::string_view path, bool recursive) {

    if (!recursive) {
        return m_fs.create_dir(path);
    }

    // Recursive: walk each component and create progressively longer paths.
    // Ignore PathAlreadyExists because parent components may already exist.
    std::string accumulated;
    std::string_view remaining = path;

    while (!remaining.empty()) {
        auto sep = remaining.find('/');
        std::string_view component;

        if (sep == std::string_view::npos) {
            component = remaining;
            remaining = {};
        } else {
            component = remaining.substr(0, sep);
            remaining = remaining.substr(sep + 1);
        }

        if (component.empty()) {
            continue;
        }

        if (!accumulated.empty()) {
            accumulated += '/';
        }
        accumulated += component;

        auto result = m_fs.create_dir(accumulated);
        if (result.is_err() && result.error() != VfsError::PathAlreadyExists) {
            return result;
        }
    }

    return Result<void, VfsError>::ok();
}

Result<void, VfsError> DirectoryOps::remove_directory(
    std::string_view path, bool recursive) {

    if (!recursive) {
        return m_fs.remove(path);
    }

    // Recursive: depth-first removal of all contents.
    auto listing = m_fs.list_dir(path);
    if (listing.is_err()) {
        return Result<void, VfsError>::err(listing.error());
    }

    std::string base(path);
    for (auto& entry : listing.value()) {
        std::string child = base + "/" + entry.name;

        if (has_attribute(entry.info.attributes, FileAttributes::Directory)) {
            auto r = remove_directory(child, true);
            if (r.is_err()) {
                return r;
            }
        } else {
            auto r = m_fs.remove(child);
            if (r.is_err()) {
                return r;
            }
        }
    }

    // Finally remove the (now-empty) directory itself.
    return m_fs.remove(path);
}

Result<bool, VfsError> DirectoryOps::is_directory(std::string_view path) const {
    auto info = m_fs.stat(path);
    if (info.is_err()) {
        if (info.error() == VfsError::PathNotFound ||
            info.error() == VfsError::FileNotFound) {
            return Result<bool, VfsError>::ok(false);
        }
        return Result<bool, VfsError>::err(info.error());
    }
    return Result<bool, VfsError>::ok(
        has_attribute(info.value().attributes, FileAttributes::Directory));
}

Result<DirHandle, VfsError> DirectoryOps::open_directory(std::string_view path) {
    auto listing = m_fs.list_dir(path);
    if (listing.is_err()) {
        return Result<DirHandle, VfsError>::err(listing.error());
    }
    return Result<DirHandle, VfsError>::ok(
        DirHandle(std::move(listing.value())));
}

Result<bool, VfsError> DirectoryOps::exists(std::string_view path) const {
    auto info = m_fs.stat(path);
    if (info.is_ok()) {
        return Result<bool, VfsError>::ok(true);
    }
    if (info.error() == VfsError::PathNotFound ||
            info.error() == VfsError::FileNotFound) {
        return Result<bool, VfsError>::ok(false);
    }
    return Result<bool, VfsError>::err(info.error());
}

} // namespace vitreo::capsule
