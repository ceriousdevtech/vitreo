// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file wkscli.hpp
/// @brief wkscli.dll / netapi32 join and DS role APIs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;
class HeapManager;

// =============================================================================
// WksCliManager
// =============================================================================

class WksCliManager {
public:
    WksCliManager() = default;
    ~WksCliManager() = default;

    WksCliManager(const WksCliManager&) = delete;
    WksCliManager& operator=(const WksCliManager&) = delete;

    void set_heap_manager(HeapManager* h) noexcept { m_heap = h; }
    [[nodiscard]] HeapManager* heap_manager() const noexcept { return m_heap; }

    /// NetGetJoinInformation — workgroup membership (NERR_Success)
    void net_get_join_information(Marshaler& m);

    /// DsRoleGetPrimaryDomainInformation — standalone workstation + WORKGROUP
    void ds_role_get_primary_domain_information(Marshaler& m);

    /// DsRoleFreeMemory — free buffer from DsRoleGetPrimaryDomainInformation
    void ds_role_free_memory(Marshaler& m);

    /// NetGetAadJoinInformation — not AAD-joined (S_OK, *ppJoinInfo = NULL)
    void net_get_aad_join_information(Marshaler& m);

    /// NetFreeAadJoinInformation — free buffer from NetGetAadJoinInformation
    void net_free_aad_join_information(Marshaler& m);

    /// NetUserGetInfo — levels 0/1 for the capsule local user
    void net_user_get_info(Marshaler& m);

    /// NetShareEnum — empty local share list (NERR_Success)
    void net_share_enum(Marshaler& m);

private:
    HeapManager* m_heap = nullptr;
};

/// Create wkscli.dll export thunks (also merged into netapi32.dll)
DllExports create_wkscli_thunks(WksCliManager& mgr);

} // namespace vitreo::winapi
