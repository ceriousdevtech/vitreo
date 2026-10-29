// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file security_exports.hpp
/// @brief Registration functions for advapi32's security exports.
///
/// Private to advapi32's security implementation, a sibling of
/// security_internal.hpp (which declares add_acl_api_exports,
/// add_access_check_export and add_object_security_exports the same way).
/// create_advapi32_security_thunks (security_registration.cpp) calls these
/// in a fixed order, which is ABI-visible: thunk addresses follow entries[]
/// order and the by-name table keeps the last of any duplicate names. A
/// group whose exports sit on both sides of another group in that order is
/// therefore registered by two functions.

#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::capsule {
class Vfs;
}

namespace vitreo::winapi {

class Kernel32ExtManager;
class SecurityStubs;

/// Read the owner (or group) SID pointer out of a guest security descriptor
/// for GetSecurityDescriptorOwner / GetSecurityDescriptorGroup. Defined in
/// security_info_exports.cpp.
void get_descriptor_principal(Marshaler& m, bool owner);

/// Register GetUserNameW/A, OpenProcessToken, GetTokenInformation,
/// AdjustTokenPrivileges.
void add_token_exports(DllExports& exports, SecurityStubs& stubs);

/// Register LookupPrivilegeValueW, LookupAccountSidW/NameW,
/// GetCurrentHwProfileW, the SID accessors (GetSidIdentifierAuthority ..
/// EqualSid), LookupPrivilegeNameA/W and LookupPrivilegeDisplayNameW.
void add_account_lookup_exports(DllExports& exports, SecurityStubs& stubs);

/// Register SetFileSecurityA/W.
void add_set_file_security_exports(DllExports& exports, capsule::Vfs* vfs);

/// Register DeregisterEventSource, RegisterEventSourceW, ReportEventW.
void add_event_source_exports(DllExports& exports);

/// Register GetSecurityDescriptorLength, MakeSelfRelativeSD, MakeAbsoluteSD.
void add_sd_layout_exports(DllExports& exports);

/// Register LookupPrivilegeValueA.
void add_lookup_privilege_value_a_export(DllExports& exports,
                                         SecurityStubs& stubs);

/// Register the Impersonate* family, OpenThreadToken, RevertToSelf,
/// AllocateLocallyUniqueId, AllocateAndInitializeSid, GetAclInformation,
/// SetSecurityDescriptorGroup/Owner and FreeSid.
void add_impersonation_exports(DllExports& exports, SecurityStubs& stubs);

/// Register CryptEnumProvidersA/W, CryptAcquireContextA/W,
/// CryptReleaseContext, CryptContextAddRef.
void add_crypto_provider_exports(DllExports& exports, SecurityStubs& stubs);

/// Register CryptCreateHash, CryptHashData, CryptGetHashParam,
/// CryptDestroyHash, CryptDestroyKey.
void add_crypto_hash_exports(DllExports& exports);

/// Register CryptGenKey, CryptImportKey, CryptGetUserKey.
void add_crypto_key_exports(DllExports& exports);

/// Register CryptGetProvParam, CryptSetHashParam, CryptExportKey.
void add_crypto_param_exports(DllExports& exports);

/// Register CryptSignHashA/W, CryptVerifySignatureA/W.
void add_crypto_signature_exports(DllExports& exports);

/// Register CryptEncrypt, CryptDecrypt, CryptDeriveKey, CryptGetKeyParam,
/// CryptGenRandom, SystemFunction036.
void add_crypto_cipher_exports(DllExports& exports);

/// Register ConvertStringSecurityDescriptorToSecurityDescriptorA/W,
/// GetFileSecurityA/W, GetKernelObjectSecurity, SetKernelObjectSecurity.
void add_file_object_security_exports(DllExports& exports,
                                      SecurityStubs& stubs,
                                      capsule::Vfs* vfs);

/// Register BuildExplicitAccessWithNameW, CreateWellKnownSid,
/// IsWellKnownSid, GetAce, the SID / security-descriptor to string
/// conversions and DeriveCapabilitySidsFromName.
void add_sid_conversion_exports(DllExports& exports, SecurityStubs& stubs);

/// Register GetSecurityDescriptorDacl/Owner/Control, GetNamedSecurityInfoW,
/// SetNamedSecurityInfoW.
void add_security_info_exports(DllExports& exports,
                               SecurityStubs& stubs,
                               capsule::Vfs* vfs);

/// Register MapGenericMask, DuplicateToken, DuplicateTokenEx,
/// CreateProcessAsUserW.
void add_token_duplication_exports(DllExports& exports,
                                   SecurityStubs& stubs,
                                   Kernel32ExtManager* process_launcher);

/// Register PrivilegeCheck, CreateProcessWithLogonW/TokenW,
/// CheckTokenMembership, IsTokenRestricted, SetThreadToken,
/// CreateRestrictedToken, RegDisablePredefinedCache,
/// GetSecurityDescriptorGroup.
void add_token_check_exports(DllExports& exports,
                             SecurityStubs& stubs,
                             Kernel32ExtManager* process_launcher);

/// Register BuildTrusteeWithSidW, SetEntriesInAclA/W,
/// BuildSecurityDescriptorA/W.
void add_explicit_access_build_exports(DllExports& exports,
                                       SecurityStubs& stubs);

/// Register GetExplicitEntriesFromAclA/W, GetEffectiveRightsFromAclW.
void add_explicit_access_query_exports(DllExports& exports,
                                       SecurityStubs& stubs);

/// Register the ETW / trace APIs (RegisterTraceGuidsW ..
/// GetTraceEnableFlags), OpenEventLogA, CloseEventLog and ReadEventLogW.
void add_event_tracing_exports(DllExports& exports,
                               SecurityStubs& stubs,
                               capsule::Vfs* vfs);

/// Register SetTokenInformation.
void add_set_token_information_exports(DllExports& exports,
                                       SecurityStubs& stubs);

/// Register DecryptFileA/W.
void add_decrypt_file_exports(DllExports& exports, capsule::Vfs* vfs);

} // namespace vitreo::winapi
