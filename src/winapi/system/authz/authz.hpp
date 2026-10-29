// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file authz.hpp
/// @brief AUTHZ.dll API stubs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// AuthzManager
// =============================================================================

class AuthzManager {
public:
    AuthzManager() = default;
    ~AuthzManager() = default;

    AuthzManager(const AuthzManager&) = delete;
    AuthzManager& operator=(const AuthzManager&) = delete;

    /// FreeClaimDefinitions — no-op
    void free_claim_definitions(u32 handle);

    /// InitializeClaimDictionary — return success (TRUE)
    [[nodiscard]] u32 initialize_claim_dictionary(u32 dict_ptr);

    /// GetClaimDefinitions — return 0
    [[nodiscard]] u32 get_claim_definitions(u32 token, u32 claims_ptr, u32 count_ptr);

    /// FreeClaimDictionary — no-op
    void free_claim_dictionary(u32 dict_ptr);
};

/// Create AUTHZ.dll export thunks
DllExports create_authz_thunks(AuthzManager& mgr);

} // namespace vitreo::winapi
