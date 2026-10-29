// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file named_object_directory.hpp
/// @brief Process-wide BaseNamedObjects kind registry for cross-type collisions.
///
/// Learn (CreateFileMappingW / CreateEventW / …): names of events, mutexes,
/// semaphores, waitable timers, jobs, and file mappings share one namespace.
/// Creating a mapping whose name already belongs to another kind fails with
/// ERROR_INVALID_HANDLE (Windows sec_attrs_probe).

#include <vitreo/common.hpp>

#include <string>
#include <string_view>

namespace vitreo::winapi {

enum class NamedObjectKind : u8 {
    FileMapping = 1,
    Event,
    Mutex,
    Semaphore,
    WaitableTimer,
    Job,
    /// A DXGI shared resource's name (IDXGIResource1::CreateSharedHandle).
    SharedResource,
};

/// Returns true when @p name is already registered as a *different* kind.
[[nodiscard]] bool named_object_kind_conflict(std::string_view name,
                                              NamedObjectKind want) noexcept;

/// Register @p name as @p kind. Fails (returns false) on cross-type conflict.
/// Same-kind re-registration is a no-op success (caller handles ALREADY_EXISTS).
[[nodiscard]] bool named_object_register(std::string_view name,
                                         NamedObjectKind kind) noexcept;

/// Drop the registration when the last live object of that name/kind is gone.
void named_object_unregister(std::string_view name,
                             NamedObjectKind kind) noexcept;

/// Strip Local\ the same way CreateFileMapping / sync creators do.
[[nodiscard]] std::string normalize_named_object_name(std::string name);

}  // namespace vitreo::winapi
