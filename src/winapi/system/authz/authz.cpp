// SPDX-License-Identifier: LGPL-2.1-only
/// @file authz.cpp
/// @brief Implementation of AUTHZ.dll API stubs

#include "authz.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

// =============================================================================
// AuthzManager Implementation
// =============================================================================

void AuthzManager::free_claim_definitions(u32 /*handle*/) {
    // no-op
}

u32 AuthzManager::initialize_claim_dictionary(u32 /*dict_ptr*/) {
    return 1; // TRUE — success
}

u32 AuthzManager::get_claim_definitions(u32 /*token*/, u32 /*claims_ptr*/,
                                        u32 /*count_ptr*/) {
    return 0;
}

void AuthzManager::free_claim_dictionary(u32 /*dict_ptr*/) {
    // no-op
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_FreeClaimDefinitions(Marshaler& m, AuthzManager& mgr) {
    mgr.free_claim_definitions(static_cast<u32>(m.arg(0)));
    m.set_return(0);
}

static void impl_InitializeClaimDictionary(Marshaler& m, AuthzManager& mgr) {
    u32 rc = mgr.initialize_claim_dictionary(static_cast<u32>(m.arg(0)));
    m.set_return(rc);
}

static void impl_GetClaimDefinitions(Marshaler& m, AuthzManager& mgr) {
    u32 rc = mgr.get_claim_definitions(
        static_cast<u32>(m.arg(0)),
        static_cast<u32>(m.arg(1)),
        static_cast<u32>(m.arg(2)));
    m.set_return(rc);
}

static void impl_FreeClaimDictionary(Marshaler& m, AuthzManager& mgr) {
    mgr.free_claim_dictionary(static_cast<u32>(m.arg(0)));
    m.set_return(0);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_authz_thunks(AuthzManager& mgr) {
    DllExports exports;
    exports.entries.reserve(4);

    exports.entries.push_back(ExportEntry{
        "FreeClaimDefinitions", 1,
        [&mgr](Marshaler& m) { impl_FreeClaimDefinitions(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "InitializeClaimDictionary", 2,
        [&mgr](Marshaler& m) { impl_InitializeClaimDictionary(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetClaimDefinitions", 3,
        [&mgr](Marshaler& m) { impl_GetClaimDefinitions(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "FreeClaimDictionary", 4,
        [&mgr](Marshaler& m) { impl_FreeClaimDictionary(m, mgr); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
