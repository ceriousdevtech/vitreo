// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shared_resource_handle.hpp
/// @brief NT handles to DXGI shared resources (IDXGIResource1::
///        CreateSharedHandle) and the names they carry.
///
/// Learn CreateSharedHandle: "Because the handle that CreateSharedHandle
/// returns is an NT handle, you can use the handle with CloseHandle,
/// DuplicateHandle, and so on." It is a kernel handle in the process handle
/// table like an event's -- not the shared storage's global id, which is what
/// IDXGIResource::GetSharedHandle hands out. Measured on Windows 11
/// (tools/oracle_probe/d3d11_share_probe): GetHandleInformation succeeds on
/// it, WaitForSingleObject fails (not waitable), a same-process duplicate
/// opens, the raw value in another process does not (E_INVALIDARG) while a
/// handle DuplicateHandle put there does, the handle keeps the resource alive
/// after its creator released it, and a name lives exactly as long as the
/// handles to the object that carries it.

#include "winapi/kernel32/handle.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vitreo::winapi {

/// One shared-resource kernel object. Every handle to it in this process
/// (DuplicateHandle copies) shares the object; a handle transferred to
/// another process gets that process's own object for the same storage.
struct SharedResourceObject {
    /// Backend shared-storage id (graphics::BackendTexture2D::shared_handle).
    u32 surface_id{0};
    /// The creator's identity for surface_id: storage ids are recycled.
    u64 nonce{0};
    /// Holds the storage alive while the object lives (Learn: the NT handle
    /// extends the lifetime of the shared resource).
    std::shared_ptr<void> keepalive;
    /// The name registration this object carries, if it was created named.
    std::shared_ptr<void> name;
    /// The registry key of that name (empty when unnamed).
    std::string name_key;
};

/// Keeps a transferred object's storage alive in the importing process: the
/// graphics layer installs one at startup (it owns the storage type).
using SharedResourceRetainer =
    std::function<std::shared_ptr<void>(u32 surface_id)>;
void set_shared_resource_retainer(SharedResourceRetainer retainer);

/// A new handle to @p object. SECURITY_ATTRIBUTES::bInheritHandle becomes
/// HANDLE_FLAG_INHERIT.
[[nodiscard]] Handle allocate_shared_resource_handle(
    HandleTable& table, std::shared_ptr<SharedResourceObject> object,
    u32 access, bool inherit);

/// The object @p handle refers to, or null when it is not a shared-resource
/// handle of this process. @p access_out receives the handle's access mask.
[[nodiscard]] std::shared_ptr<SharedResourceObject> shared_resource_object(
    const HandleTable& table, Handle handle, u32* access_out = nullptr);

/// HandleTable close callback for HandleType::SharedResource: drops this
/// handle's reference to the object.
void release_shared_resource_handle_object(void* object) noexcept;

/// DuplicateHandle within this process: another handle to the same object.
[[nodiscard]] Handle duplicate_shared_resource_handle(HandleTable& table,
                                                      Handle source,
                                                      u32 access,
                                                      bool inherit);

/// DuplicateHandle into another process: the descriptor the target imports
/// ("D,<access>,<flags>,<surface>,<nonce>,<name key hex or ->").
[[nodiscard]] std::optional<std::string> prepare_shared_resource_transfer(
    const HandleTable& table, Handle source, u32 access, bool inherit);
[[nodiscard]] Handle import_shared_resource_transfer(
    HandleTable& table, std::string_view descriptor);

/// Names. Measured: shared-resource names are one namespace with the other
/// named kernel objects (a name an event holds is DXGI_ERROR_NAME_ALREADY_
/// EXISTS, and CreateEvent on a shared resource's name fails
/// ERROR_INVALID_HANDLE), "Local\" is the default namespace, comparison is
/// case-insensitive, and a second CreateSharedHandle with a live name fails
/// even for the same resource. The registry is visible to every Vitreo host
/// process of the capsule.
enum class SharedResourceNameResult : u8 { Registered, AlreadyExists, Failed };

/// Register @p name for (surface_id, nonce). The registration lives while
/// @p registration_out (and any copy of it) is held.
[[nodiscard]] SharedResourceNameResult register_shared_resource_name(
    std::string_view name, u32 surface_id, u64 nonce,
    std::shared_ptr<void>& registration_out, std::string& key_out);

struct SharedResourceNameTarget {
    u32 surface_id{0};
    u64 nonce{0};
};
[[nodiscard]] std::optional<SharedResourceNameTarget>
lookup_shared_resource_name(std::string_view name);

}  // namespace vitreo::winapi
