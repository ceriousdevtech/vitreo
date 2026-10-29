// SPDX-License-Identifier: LGPL-2.1-only

/// @file security_registration.cpp
/// @brief Registration order for advapi32's security surface.
///
/// Composition only: create_advapi32_security_thunks calls one add_*_exports
/// function per group of exports, in the order below. Each of those maps
/// export names to SecurityStubs handlers and records each one's stdcall
/// byte count; they live in the sibling *_exports.cpp files and are declared
/// in security_exports.hpp. No security behavior lives here.
///
/// The order is ABI-visible: thunk addresses follow entries[] order, and the
/// by-name table keeps the last of any duplicate names (which is why the
/// ACL / access-check / object-security groups go last). Groups whose
/// exports straddle another group in that order are registered by two
/// functions, so no export changes position.
///
/// Split out of security.cpp, which was 7,137 lines and roughly three-quarters
/// registration. Adding an export no longer touches the translation unit where
/// token, SID, and ACL behavior lives.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.

#include "winapi/advapi32/security/credentials.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

// =============================================================================
// Thunk Exports
// =============================================================================

DllExports create_advapi32_security_thunks(SecurityStubs& stubs,
                                           capsule::Vfs* vfs,
                                           Kernel32ExtManager* process_launcher) {
    DllExports exports;
    add_token_exports(exports, stubs);
    add_account_lookup_exports(exports, stubs);
    add_set_file_security_exports(exports, vfs);
    add_event_source_exports(exports);
    add_sd_layout_exports(exports);
    add_lookup_privilege_value_a_export(exports, stubs);
    add_impersonation_exports(exports, stubs);
    add_crypto_provider_exports(exports, stubs);
    add_crypto_hash_exports(exports);
    add_crypto_key_exports(exports);
    add_crypto_param_exports(exports);
    add_crypto_signature_exports(exports);
    add_crypto_cipher_exports(exports);
    add_file_object_security_exports(exports, stubs, vfs);
    add_sid_conversion_exports(exports, stubs);
    add_security_info_exports(exports, stubs, vfs);
    add_token_duplication_exports(exports, stubs, process_launcher);

    // Credential Manager: CredReadW / CredWriteW / CredDeleteW / CredFree.
    add_credential_exports(exports, stubs);

    add_token_check_exports(exports, stubs, process_launcher);
    add_explicit_access_build_exports(exports, stubs);
    add_explicit_access_query_exports(exports, stubs);
    add_event_tracing_exports(exports, stubs, vfs);
    add_set_token_information_exports(exports, stubs);
    add_decrypt_file_exports(exports, vfs);

    // ACL / security-descriptor primitives (acl_api.cpp) go last so that
    // these are the registrations the by-name table keeps.
    add_acl_api_exports(exports);
    add_access_check_export(exports, stubs);
    add_object_security_exports(exports, stubs);
    return exports;
}

} // namespace vitreo::winapi
