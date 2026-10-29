// SPDX-License-Identifier: LGPL-2.1-only

/// @file token_operations_exports.cpp
/// @brief advapi32 token operations: MapGenericMask, DuplicateToken/Ex,
/// CreateProcessAsUserW, PrivilegeCheck, CreateProcessWithLogonW/TokenW,
/// CheckTokenMembership, IsTokenRestricted, SetThreadToken,
/// CreateRestrictedToken, RegDisablePredefinedCache and
/// GetSecurityDescriptorGroup.
///
/// The two functions are separate because the Credential Manager exports
/// (add_credential_exports) sit between them in the registration order (see
/// security_registration.cpp); regrouping them would move thunk addresses.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "ipc/ipc_gate.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"

#include <utility>
#include <vector>

namespace vitreo::winapi {

/// Registers MapGenericMask, DuplicateToken, DuplicateTokenEx,
/// CreateProcessAsUserW.
void add_token_duplication_exports(DllExports& exports,
                                   SecurityStubs& stubs,
                                   Kernel32ExtManager* process_launcher) {
    // VOID MapGenericMask(PDWORD AccessMask, PGENERIC_MAPPING gm)
    // Microsoft Learn (securitybaseapi!MapGenericMask): replace GENERIC_* bits
    // using GENERIC_MAPPING; clear the four generic bits afterward.
    exports.entries.push_back({"MapGenericMask", 0,
        [](Marshaler& m) {
            const GuestAddr mask_ptr = m.arg_ptr(0);
            const GuestAddr mapping = m.arg_ptr(1);
            if (mask_ptr == 0 || mapping == 0) {
                m.set_return(0);
                return;
            }
            auto mask_r = m.read_u32(mask_ptr);
            if (mask_r.is_err()) {
                m.set_return(0);
                return;
            }
            u32 mask = mask_r.value();
            const u32 generic_read = m.read_u32(mapping + 0).value_or(0);
            const u32 generic_write = m.read_u32(mapping + 4).value_or(0);
            const u32 generic_execute = m.read_u32(mapping + 8).value_or(0);
            const u32 generic_all = m.read_u32(mapping + 12).value_or(0);
            apply_generic_mapping(mask, generic_read, generic_write,
                                  generic_execute, generic_all);
            (void)m.write_u32(mask_ptr, mask);
            m.set_return(0);
        }, 8});


    // BOOL DuplicateToken(HANDLE ExistingTokenHandle,
    //                     SECURITY_IMPERSONATION_LEVEL ImpersonationLevel,
    //                     PHANDLE DuplicateTokenHandle)
    // 3 args, stdcall_bytes=12.
    //
    // A missing-export stub returns 0xFFFFFFFF (a nonzero BOOL) without
    // writing *DuplicateTokenHandle and, with stdcall_bytes=0, leaves 12
    // bytes of the caller's stack. This export writes the duplicate handle
    // and pops the three stdcall arguments.
    exports.entries.push_back({"DuplicateToken", 0,
        [&stubs](Marshaler& m) {
            const Handle existing = static_cast<Handle>(m.arg(0));
            const u32 level = m.arg32(1);
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            // Learn: DuplicateTokenEx(TOKEN_IMPERSONATE | TOKEN_QUERY, level,
            // TokenImpersonation) (advapi_token_probe duptoken; token2 dt_*:
            // failure leaves *out alone on x64, NULL on x86).
            constexpr u32 TOKEN_IMPERSONATE = 0x0004, TOKEN_QUERY = 0x0008;
            Handle dup = 0;
            const u32 err = stubs.duplicate_token_ex(
                existing, TOKEN_IMPERSONATE | TOKEN_QUERY, level, 2, dup);
            if (err != 0) {
                if (!m.cpu_is_64bit()) (void)m.write_ptr(out, 0);
                set_last_error(err);
                m.set_return(0);
                return;
            }
            (void)m.write_ptr(out, static_cast<u64>(dup));
            VITREO_TRACE("[advapi32] DuplicateToken(token=0x%llx level=%u)"
                         " -> 0x%llx\n",
                         static_cast<unsigned long long>(existing), level,
                         static_cast<unsigned long long>(dup));
            m.set_return(1);
        }, 12});

    // BOOL DuplicateTokenEx(HANDLE, DWORD, LPSA, SECURITY_IMPERSONATION_LEVEL,
    //                      TOKEN_TYPE, PHANDLE) — 6 args, stdcall_bytes=24.
    // Every check and the access mapping are SecurityStubs::duplicate_token_ex
    // (advapi_token2_probe dte_*). lpTokenAttributes' descriptor is not
    // modelled (tokens carry no object DACL here). On failure x64 leaves
    // *phNewToken alone and the x86 (WOW64) path stores NULL (probe
    // dte_*.expected vs .x86.expected).
    exports.entries.push_back({"DuplicateTokenEx", 0,
        [&stubs](Marshaler& m) {
            const Handle existing = static_cast<Handle>(m.arg(0));
            const u32 access = m.arg32(1);
            const u32 level = m.arg32(3);
            const u32 type = m.arg32(4);
            const GuestAddr out = m.arg_ptr(5);
            if (out == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            Handle dup = 0;
            const u32 err = stubs.duplicate_token_ex(existing, access, level, type, dup);
            if (err != 0) {
                if (!m.cpu_is_64bit()) (void)m.write_ptr(out, 0);
                set_last_error(err);
                m.set_return(0);
                return;
            }
            (void)m.write_ptr(out, static_cast<u64>(dup));
            VITREO_TRACE("[advapi32] DuplicateTokenEx(token=0x%llx access=0x%x level=%u"
                         " type=%u) -> 0x%llx\n",
                         static_cast<unsigned long long>(existing), access, level, type,
                         static_cast<unsigned long long>(dup));
            m.set_return(1);
        }, 24});

    // BOOL CreateProcessAsUserW(HANDLE,LPCWSTR,LPWSTR,LPSA,LPSA,BOOL,DWORD,
    //                          LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION)
    exports.entries.push_back({"CreateProcessAsUserW", 0,
        [process_launcher](Marshaler& m) {
            if (process_launcher == nullptr) {
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0);
                return;
            }
            process_launcher->impl_CreateProcessAsUserW(m);
        }, 44});
}

/// Registers PrivilegeCheck, CreateProcessWithLogonW/TokenW,
/// CheckTokenMembership, IsTokenRestricted, SetThreadToken,
/// CreateRestrictedToken, RegDisablePredefinedCache,
/// GetSecurityDescriptorGroup.
void add_token_check_exports(DllExports& exports,
                             SecurityStubs& stubs,
                             Kernel32ExtManager* process_launcher) {
    // BOOL PrivilegeCheck(HANDLE ClientToken, PPRIVILEGE_SET RequiredPrivileges,
    //                     LPBOOL pfResult) — 3 args, stdcall_bytes=12.
    // Measured on Windows 10 (tools/oracle_probe/cred_privilege_probe.expected
    // pc_*): works on primary and impersonation tokens, needs TOKEN_QUERY
    // (else ERROR_ACCESS_DENIED), marks every enabled requested privilege
    // SE_PRIVILEGE_USED_FOR_ACCESS, and with PRIVILEGE_SET_ALL_NECESSARY
    // needs all of them enabled, otherwise any one.
    exports.entries.push_back({"PrivilegeCheck", 0,
        [&stubs](Marshaler& m) {
            constexpr u32 TOKEN_QUERY = 0x0008;
            constexpr u32 PRIVILEGE_SET_ALL_NECESSARY = 1;
            constexpr u32 SE_PRIVILEGE_USED_FOR_ACCESS = 0x80000000u;
            const Handle token = static_cast<Handle>(m.arg(0));
            const GuestAddr set = m.arg_ptr(1);
            const GuestAddr result = m.arg_ptr(2);
            if (!stubs.is_token_handle(token)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            if (stubs.is_real_token_handle(token) &&
                !stubs.token_has_access(token, TOKEN_QUERY)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                m.set_return(0);
                return;
            }
            if (set == 0 || result == 0) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            const u32 count = m.read_u32(set).value_or(0);
            const u32 control = m.read_u32(set + 4).value_or(0);
            if (count > 0x1000) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            u32 enabled = 0;
            for (u32 i = 0; i < count; ++i) {
                const GuestAddr entry = set + 8 + i * 12;
                const u64 luid = m.read_u64(entry).value_or(0);
                if (stubs.privilege_enabled(token, luid)) {
                    ++enabled;
                    const u32 attrs = m.read_u32(entry + 8).value_or(0);
                    (void)m.write_u32(entry + 8, attrs | SE_PRIVILEGE_USED_FOR_ACCESS);
                }
            }
            const bool granted = (control & PRIVILEGE_SET_ALL_NECESSARY) != 0
                                     ? enabled == count
                                     : enabled != 0;
            (void)m.write_u32(result, granted ? 1u : 0u);
            m.set_return(1);
        }, 12});

    // BOOL CreateProcessWithLogonW(LPCWSTR user, LPCWSTR domain,
    //   LPCWSTR password, DWORD logonFlags, LPCWSTR app, LPWSTR cmd,
    //   DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW,
    //   LPPROCESS_INFORMATION) — 11 args, stdcall_bytes=44.
    exports.entries.push_back({"CreateProcessWithLogonW", 0,
        [process_launcher](Marshaler& m) {
            if (process_launcher == nullptr) {
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0);
                return;
            }
            process_launcher->impl_CreateProcessWithLogonW(m);
        }, 44});

    // BOOL CreateProcessWithTokenW(HANDLE,DWORD,LPCWSTR,LPWSTR,DWORD,LPVOID,
    //                              LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION)
    // 9 args, stdcall_bytes=36. Was a missing-export stub (BOOL-looking
    // 0xFFFFFFFF without writing PROCESS_INFORMATION).
    exports.entries.push_back({"CreateProcessWithTokenW", 0,
        [process_launcher](Marshaler& m) {
            if (process_launcher == nullptr) {
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0);
                return;
            }
            process_launcher->impl_CreateProcessWithTokenW(m);
        }, 36});

    // NOTE: GetTokenInformation is registered once, above (the real
    // SecurityStubs-backed implementation). A duplicate fail-stub here used to
    // shadow it in the by_name map (later registration wins), making every
    // GetTokenInformation call fail with ERROR_INVALID_HANDLE — which aborted
    // crash-reporting security-descriptor setup and killed the helper process
    // with ExitProcess(0xE0000008). Do not re-add duplicate export names.

    // BOOL CheckTokenMembership(HANDLE TokenHandle, PSID SidToCheck,
    //                           PBOOL IsMember) — 3 args, stdcall_bytes=12.
    // Learn + tools/oracle_probe/advapi_token2_probe ctm_*: the SID must be
    // present *and enabled* (deny-only does not count; a restricted token also
    // needs it among the restricting SIDs). Failure writes *IsMember = FALSE;
    // success leaves the last error untouched.
    exports.entries.push_back({"CheckTokenMembership", 0,
        [&stubs](Marshaler& m) {
            const Handle token = static_cast<Handle>(m.arg(0));
            const GuestAddr sid_ptr = m.arg_ptr(1);
            const GuestAddr is_member = m.arg_ptr(2);
            std::vector<u8> sid;
            if (sid_ptr != 0 && SecurityStubs::is_valid_sid(m, sid_ptr) != 0) {
                sid.resize(SecurityStubs::get_length_sid(m, sid_ptr));
                (void)m.memory().read_bytes(sid_ptr, sid.data(), sid.size());
            }
            bool member = false;
            const u32 err = stubs.check_token_membership(token, sid, member);
            if (is_member != 0) (void)m.write_u32(is_member, member ? 1u : 0u);
            VITREO_TRACE("[advapi32] CheckTokenMembership(token=0x%x) -> err=%u member=%u\n",
                         static_cast<unsigned>(token), err, member ? 1u : 0u);
            if (err != 0) {
                set_last_error(err);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 12});

    // BOOL IsTokenRestricted(HANDLE TokenHandle)
    // Learn: nonzero only when the token has a restricting-SID list.
    exports.entries.push_back({"IsTokenRestricted", 0,
        [&stubs](Marshaler& m) {
            const Handle token = static_cast<Handle>(m.arg(0));
            if (!stubs.is_token_handle(token)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            const bool restricted = stubs.token_is_restricted(token);
            m.set_return(restricted ? 1 : 0);
        }, 4});

    // ------------------------------------------------------------------
    // Remaining ADVAPI32 exports. They return ERROR_NOT_SUPPORTED or the
    // documented default and pop their stdcall arguments. A missing-export
    // stub with stdcall_bytes=0 leaves the caller's stack unbalanced.
    // ------------------------------------------------------------------

    // BOOL SetThreadToken(PHANDLE Thread, HANDLE Token)  — 2 args, 8 bytes
    exports.entries.push_back({"SetThreadToken", 0,
        [&stubs](Marshaler& m) {
            constexpr Handle CURRENT_THREAD_PSEUDO = 0xFFFFFFFEu;
            const GuestAddr thread_ptr = m.arg_ptr(0);
            const Handle token = static_cast<Handle>(m.arg(1));
            Handle thread = CURRENT_THREAD_PSEUDO;
            if (thread_ptr != 0) {
                const auto value = m.read_ptr(thread_ptr);
                if (value.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                thread = static_cast<Handle>(value.value());
            }

            u32 error = error_codes::ERROR_SUCCESS;
            const bool ok = stubs.set_thread_token(thread, token, error);
            ipc::ipc_log(
                "SetThreadToken thread=0x%x token=0x%x -> %s error=%u",
                static_cast<unsigned>(thread),
                static_cast<unsigned>(token), ok ? "TRUE" : "FALSE", error);
            if (!ok) set_last_error(error);  // success: untouched (measured)
            m.set_return(ok ? 1 : 0);
        }, 8});

    // BOOL CreateRestrictedToken(HANDLE,DWORD,DWORD,PSID_AND_ATTRIBUTES,
    //   DWORD,PLUID_AND_ATTRIBUTES,DWORD,PSID_AND_ATTRIBUTES,PHANDLE)
    exports.entries.push_back({"CreateRestrictedToken", 0,
        [&stubs](Marshaler& m) {
            constexpr u32 DISABLE_MAX_PRIVILEGE = 0x01;
            constexpr u32 KNOWN_FLAGS = 0x0F;
            constexpr u32 MAX_LIST_ENTRIES = 4096;

            const Handle existing = static_cast<Handle>(m.arg(0));
            const u32 flags = static_cast<u32>(m.arg(1));
            const u32 disable_count = static_cast<u32>(m.arg(2));
            const GuestAddr disable_sids = m.arg_ptr(3);
            const u32 delete_count = static_cast<u32>(m.arg(4));
            const GuestAddr delete_privileges = m.arg_ptr(5);
            const u32 restrict_count = static_cast<u32>(m.arg(6));
            const GuestAddr restrict_sids = m.arg_ptr(7);
            const GuestAddr token_out = m.arg_ptr(8);

            if (token_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            (void)m.write_ptr(token_out, 0);
            if (!stubs.is_token_handle(existing)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            if ((flags & ~KNOWN_FLAGS) != 0 ||
                disable_count > MAX_LIST_ENTRIES ||
                delete_count > MAX_LIST_ENTRIES ||
                restrict_count > MAX_LIST_ENTRIES ||
                (disable_count != 0 && disable_sids == 0) ||
                ((flags & DISABLE_MAX_PRIVILEGE) == 0 &&
                 delete_count != 0 && delete_privileges == 0) ||
                (restrict_count != 0 && restrict_sids == 0)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const bool is64 = m.cpu_is_64bit();
            const u32 entry_size = is64 ? 16u : 8u;
            auto parse_sids = [&](GuestAddr array, u32 count,
                                  std::vector<SecurityStubs::RestrictedSid>& out)
                -> bool {
                out.clear();
                out.reserve(count);
                for (u32 index = 0; index < count; ++index) {
                    const GuestAddr entry = array + index * entry_size;
                    const GuestAddr sid = is64
                        ? m.read_u64(entry).value_or(0)
                        : m.read_u32(entry).value_or(0);
                    const u32 attributes =
                        m.read_u32(entry + (is64 ? 8u : 4u)).value_or(0);
                    if (sid == 0 ||
                        SecurityStubs::is_valid_sid(m, sid) == 0) {
                        return false;
                    }
                    const u32 sid_size =
                        SecurityStubs::get_length_sid(m, sid);
                    SecurityStubs::RestrictedSid parsed;
                    parsed.bytes.resize(sid_size);
                    parsed.attributes = attributes;
                    if (m.memory().read_bytes(
                            sid, parsed.bytes.data(),
                            parsed.bytes.size()).is_err()) {
                        return false;
                    }
                    out.push_back(std::move(parsed));
                }
                return true;
            };

            std::vector<SecurityStubs::RestrictedSid> disabled;
            std::vector<SecurityStubs::RestrictedSid> restricting;
            if (!parse_sids(disable_sids, disable_count, disabled) ||
                !parse_sids(restrict_sids, restrict_count, restricting)) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            std::vector<u64> deleted_privileges;
            if ((flags & DISABLE_MAX_PRIVILEGE) == 0) {
                deleted_privileges.reserve(delete_count);
                for (u32 index = 0; index < delete_count; ++index) {
                    const auto luid = m.read_u64(
                        delete_privileges + index * 12u);
                    if (luid.is_err()) {
                        set_last_error(error_codes::ERROR_NOACCESS);
                        m.set_return(0);
                        return;
                    }
                    deleted_privileges.push_back(luid.value());
                }
            }

            Handle restricted = 0;
            if (!stubs.create_restricted_token(
                    existing, flags, std::move(disabled),
                    std::move(deleted_privileges),
                    std::move(restricting), restricted)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (m.write_ptr(token_out, restricted).is_err()) {
                (void)stubs.is_token_handle(restricted);
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            ipc::ipc_log(
                "CreateRestrictedToken existing=0x%x flags=0x%x "
                "disable=%u delete=%u restrict=%u -> h=0x%x",
                static_cast<unsigned>(existing), flags, disable_count,
                delete_count, restrict_count,
                static_cast<unsigned>(restricted));
            m.set_return(1);
        }, 36});

    // BOOL RegDisablePredefinedCache(void) — 0 args
    exports.entries.push_back({"RegDisablePredefinedCache", 0,
        [](Marshaler& m) {
            (void)m;
            m.set_return(error_codes::ERROR_SUCCESS);
        }, 0});

    // BOOL GetSecurityDescriptorGroup(PSD, PSID*, LPBOOL) — 3 args, 12 bytes
    // Microsoft Learn (securitybaseapi!GetSecurityDescriptorGroup).
    exports.entries.push_back({"GetSecurityDescriptorGroup", 0,
        [](Marshaler& m) {
            get_descriptor_principal(m, false);
        }, 12});
}

} // namespace vitreo::winapi
