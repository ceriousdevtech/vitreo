// SPDX-License-Identifier: LGPL-2.1-only
/// @file shared_memory.hpp
/// @brief POSIX shared-memory object keyed by a Windows mapping name, so a
///        named file-mapping is visible across the forked Vitreo host
///        processes that back guest processes (Stage 2 of cross-process IPC).

#pragma once

#include <vitreo/common.hpp>

#include <memory>
#include <optional>
#include <string>

namespace vitreo::ipc {

/// A named POSIX shared-memory object (shm_open + ftruncate). Views are mmap'd
/// MAP_SHARED so writes are mutually visible across processes. A companion
/// lifetime record tracks per-host-process references. The last live reference
/// removes both names, and a later open/create prunes references left by a
/// process that terminated without running destructors.
class SharedMemory {
public:
    ~SharedMemory();

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    /// Create (create=true) or attach (create=false) the shm object for the
    /// given Windows mapping name. On create, the object is sized to @p size;
    /// on attach, @p size is ignored and the existing size is used. Returns
    /// nullptr on failure.
    [[nodiscard]] static std::shared_ptr<SharedMemory>
    create_or_open(const std::string& win_name, u64 size, bool create);

    // Immutable creator protection travels outside the guest-visible payload.
    [[nodiscard]] static std::shared_ptr<SharedMemory>
    create_section(const std::string& win_name, u64 size, bool create, u32 protection);
    [[nodiscard]] std::optional<u32> section_protection() const noexcept;

    [[nodiscard]] int  fd()         const noexcept { return m_fd; }
    [[nodiscard]] u64  size()       const noexcept { return m_size; }
    [[nodiscard]] bool is_creator() const noexcept { return m_creator; }

    /// mmap a MAP_SHARED view [offset, offset+len). @p offset must be
    /// page-aligned (mmap requirement). Returns nullptr on failure.
    [[nodiscard]] void* map_view(u64 offset, usize len);

    /// Unmap a view previously returned by map_view().
    static void unmap_view(void* p, usize len);

    /// Remove every object in the current IPC scope whose referencing host
    /// processes have all terminated. A host process killed without running
    /// destructors cannot release its sections, and names that are never
    /// reopened (unnamed sections, per-instance GUID names) are otherwise
    /// never pruned. Windows frees a section once its last handle and view are
    /// gone, which process termination guarantees. Runs automatically once per
    /// process and scope on first use; returns the number of objects removed.
    static usize reap_stale_objects();

private:
    SharedMemory() = default;
    static std::shared_ptr<SharedMemory> create_impl(
        const std::string&, u64, bool, std::optional<u32>);

    int         m_fd{-1};
    int         m_lifetime_fd{-1};
    u64         m_size{0};
    bool        m_creator{false};
    void*       m_lifetime_map{nullptr};
    usize       m_lifetime_map_len{0};
    std::string m_posix_name;
    std::string m_lifetime_posix_name;
};

} // namespace vitreo::ipc
