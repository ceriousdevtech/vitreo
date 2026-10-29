// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file netutils.hpp
/// @brief netutils.dll network-management buffer API (NetApiBuffer*)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;
class HeapManager;

// =============================================================================
// NetUtilsManager
// =============================================================================

/// Buffers returned by the Net* functions (NetGetJoinInformation,
/// NetUserGetInfo, DsRole*, ...) live on the process heap, so the ApiBuffer
/// functions allocate, size, reallocate and free there.
class NetUtilsManager {
public:
    NetUtilsManager() = default;
    ~NetUtilsManager() = default;

    NetUtilsManager(const NetUtilsManager&) = delete;
    NetUtilsManager& operator=(const NetUtilsManager&) = delete;

    void set_heap_manager(HeapManager* heap) noexcept { m_heap = heap; }

    /// NetApiBufferFree: NERR_Success for NULL and for a live buffer
    /// (tools/oracle_probe/iphlp_netapi_probe free-null / free-allocated).
    [[nodiscard]] u32 net_api_buffer_free(GuestAddr buffer);

    void impl_NetApiBufferAllocate(Marshaler& m);
    void impl_NetApiBufferSize(Marshaler& m);
    void impl_NetApiBufferReallocate(Marshaler& m);

private:
    HeapManager* m_heap = nullptr;
};

/// Create netutils.dll export thunks
DllExports create_netutils_thunks(NetUtilsManager& mgr);

} // namespace vitreo::winapi
