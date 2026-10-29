// SPDX-License-Identifier: LGPL-2.1-only
/// @file process_registry.hpp
/// @brief Capsule-scoped discovery of live Vitreo guest processes.

#pragma once

#include <vitreo/common.hpp>

#include <string>
#include <optional>
#include <vector>

namespace vitreo::ipc {

struct ProcessSnapshotEntry {
    u32 pid{0};
    u32 parent_pid{0};
    std::string image_name;
    std::string image_path;
    u32 primary_thread_id{0};
};

/// Publish one guest process in its capsule for the lifetime of this object.
class ProcessRegistration {
public:
    ProcessRegistration(const std::string& capsule_root,
                        const std::string& image_name,
                        const std::string& image_path = {},
                        u32 primary_thread_id = 0);
    ~ProcessRegistration();

    ProcessRegistration(const ProcessRegistration&) = delete;
    ProcessRegistration& operator=(const ProcessRegistration&) = delete;
    ProcessRegistration(ProcessRegistration&& other) noexcept;
    ProcessRegistration& operator=(ProcessRegistration&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return !m_path.empty(); }

private:
    std::string m_path;
};

/// Capture the live guest processes currently registered in one capsule.
[[nodiscard]] std::vector<ProcessSnapshotEntry> list_processes(
    const std::string& capsule_root);

/// Resolve a published primary TID to its capsule-scoped process owner.
[[nodiscard]] std::optional<u32> primary_thread_owner(u32 thread_id);

} // namespace vitreo::ipc
