// SPDX-License-Identifier: LGPL-2.1-only

/// @file impersonation_exports.cpp
/// @brief advapi32 impersonation and SID/ACL construction exports: the Impersonate*
/// family, OpenThreadToken, RevertToSelf, AllocateLocallyUniqueId,
/// AllocateAndInitializeSid, GetAclInformation, SetSecurityDescriptorGroup/Owner
/// and FreeSid.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <atomic>

namespace vitreo::winapi {

/// Registers the Impersonate* family, OpenThreadToken, RevertToSelf,
/// AllocateLocallyUniqueId, AllocateAndInitializeSid, GetAclInformation,
/// SetSecurityDescriptorGroup/Owner and FreeSid.
void add_impersonation_exports(DllExports& exports, SecurityStubs& stubs) {
    // BOOL ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL ImpersonationLevel)
    // 1 arg, stdcall_bytes=4
    exports.entries.push_back(
        {"ImpersonateSelf", 33, [&stubs](Marshaler& m) {
            const u32 level = static_cast<u32>(m.arg(0));
            u32 error = error_codes::ERROR_SUCCESS;
            const bool ok = stubs.impersonate_self(level, error);
            VITREO_TRACE("[advapi32] ImpersonateSelf(level=%u) -> %s\n",
                         level, ok ? "TRUE" : "FALSE");
            // Success leaves the last error untouched (measured on Windows 11:
            // last_error_on_success_advapi32_probe.expected).
            if (!ok) set_last_error(error);
            m.set_return(ok ? 1 : 0);
        }, 4});

    // BOOL ImpersonateLoggedOnUser(HANDLE hToken) — 1 arg, stdcall_bytes=4.
    // Must be a real export: the missing-export stub uses stdcall_bytes=0.
    exports.entries.push_back(
        {"ImpersonateLoggedOnUser", 0, [&stubs](Marshaler& m) {
            const Handle token = static_cast<Handle>(m.arg(0));
            u32 error = error_codes::ERROR_SUCCESS;
            const bool ok = stubs.impersonate_logged_on_user(token, error);
            VITREO_TRACE(
                "[advapi32] ImpersonateLoggedOnUser(token=0x%x) -> %s\n",
                static_cast<unsigned>(token), ok ? "TRUE" : "FALSE");
            if (!ok) set_last_error(error);  // success: untouched (measured)
            m.set_return(ok ? 1 : 0);
        }, 4});

    // BOOL ImpersonateAnonymousToken(HANDLE ThreadHandle) — stdcall 4.
    // https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-impersonateanonymoustoken
    // The thread impersonates ANONYMOUS LOGON (advapi_token2_probe anon_*:
    // S-1-5-7, OpenThreadToken succeeds, Everyone is not a member). A handle
    // that is not a thread -> ERROR_INVALID_HANDLE. Success leaves the last
    // error untouched.
    exports.entries.push_back(
        {"ImpersonateAnonymousToken", 0, [&stubs](Marshaler& m) {
            const Handle thread = static_cast<Handle>(m.arg(0));
            const u32 err = stubs.impersonate_anonymous(thread);
            if (err != 0) {
                set_last_error(err);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4});

    // BOOL ImpersonateNamedPipeClient(HANDLE hNamedPipe) — stdcall 4.
    // https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-impersonatenamedpipeclient
    // A handle that is not a named-pipe server end -> ERROR_INVALID_HANDLE
    // (probe inpc_event / inpc_badhandle). Every client of a capsule pipe is
    // the capsule's own user, so the thread impersonates that identity at
    // SecurityImpersonation — the level a client gets without
    // SECURITY_SQOS_PRESENT (probe inpc_default_after_read). The pipe layer
    // tracks neither whether the server has read yet (Windows:
    // ERROR_CANNOT_IMPERSONATE before the first read under dynamic tracking)
    // nor a client's SQOS level; see report_impl_advapi.md.
    exports.entries.push_back(
        {"ImpersonateNamedPipeClient", 0, [&stubs](Marshaler& m) {
            const Handle pipe = static_cast<Handle>(m.arg(0));
            if (stubs.handle_table().get_type(pipe) != HandleType::Pipe) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            u32 err = error_codes::ERROR_SUCCESS;
            if (!stubs.impersonate_self(2, err)) {
                set_last_error(err);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4});

    // BOOL OpenThreadToken(HANDLE ThreadHandle, DWORD DesiredAccess, BOOL OpenAsSelf, PHANDLE TokenHandle)
    // 4 args, stdcall_bytes=16
    exports.entries.push_back(
        {"OpenThreadToken", 34, [&stubs](Marshaler& m) {
            const Handle thread = static_cast<Handle>(m.arg(0));
            const u32 access = static_cast<u32>(m.arg(1));
            (void)m.arg(2); // OpenAsSelf affects the access check, not identity.
            const GuestAddr out = m.arg_ptr(3);
            if (out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (m.write_ptr(out, 0).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            Handle token = NULL_HANDLE;
            u32 error = error_codes::ERROR_SUCCESS;
            const bool ok =
                stubs.open_thread_token(thread, access, token, error);
            if (!ok) {
                VITREO_TRACE(
                    "[advapi32] OpenThreadToken(thread=0x%x) -> FALSE "
                    "(error=%u)\n",
                    static_cast<unsigned>(thread), error);
                set_last_error(error);
                m.set_return(0);
                return;
            }
            if (m.write_ptr(out, token).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            VITREO_TRACE(
                "[advapi32] OpenThreadToken(thread=0x%x access=0x%x) "
                "-> 0x%x\n",
                static_cast<unsigned>(thread), access,
                static_cast<unsigned>(token));
            m.set_return(1);
        }, 16});

    // BOOL RevertToSelf(void)
    // 0 args, stdcall_bytes=0
    exports.entries.push_back(
        {"RevertToSelf", 35, [&stubs](Marshaler& m) {
            (void)stubs.revert_to_self();
            VITREO_TRACE("[advapi32] RevertToSelf -> TRUE\n");
            m.set_return(1);
        }, 0});

    // BOOL AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority,
    //   BYTE nSubAuthorityCount, DWORD sa0..sa7, PSID *pSid)
    // 11 args, stdcall_bytes=44
    exports.entries.push_back(
        // Learn (AllocateLocallyUniqueId): returns a LUID unique only for
        // this boot of this system. A monotonically increasing 64-bit counter
        // satisfies exactly that, and uniqueness is the entire contract —
        // returning a constant would make two "distinct" LUIDs compare equal.
        {"AllocateLocallyUniqueId", 0, [](Marshaler& m) {
            const GuestAddr luid_out = m.arg_ptr(0);
            if (luid_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            static std::atomic<u64> next_luid{0x10000};
            const u64 value = next_luid.fetch_add(1, std::memory_order_relaxed);
            // LUID is { DWORD LowPart; LONG HighPart; }.
            (void)m.write_u32(luid_out, static_cast<u32>(value & 0xFFFFFFFFu));
            (void)m.write_u32(luid_out + 4, static_cast<u32>(value >> 32));
            m.set_return(1);
        }, 4});

    exports.entries.push_back(
        {"AllocateAndInitializeSid", 36, [&stubs](Marshaler& m) {
            GuestAddr auth_ptr = m.arg_ptr(0);
            u8 sub_count = static_cast<u8>(m.arg(1));
            // args 2-9 are sub-authority DWORDs (sa0..sa7)
            GuestAddr psid_out = m.arg_ptr(10);

            if (auth_ptr == 0 || psid_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            // RtlAllocateAndInitializeSid: STATUS_INVALID_SID ->
            // ERROR_INVALID_SID (advapi_token_probe aais_9: gle=0x539).
            if (sub_count > 8) {
                set_last_error(1337u /*ERROR_INVALID_SID*/);
                m.set_return(0);
                return;
            }

            // SID size: revision(1) + sub_auth_count(1) + authority(6) + sub_auths(count*4)
            u32 sid_size = 8 + sub_count * 4;
            GuestAddr sid_addr = stubs.alloc_sid_memory(sid_size);
            if (sid_addr == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                m.set_return(0);
                return;
            }

            // Write revision = 1
            (void)m.write_u8(sid_addr, 1);
            // Write sub-authority count
            (void)m.write_u8(sid_addr + 1, sub_count);
            // Copy 6-byte identifier authority
            for (u32 i = 0; i < 6; ++i) {
                auto b = m.read_u8(auth_ptr + i);
                (void)m.write_u8(sid_addr + 2 + i, b.is_ok() ? b.value() : 0);
            }
            // Write sub-authorities from args 2..9
            for (u8 i = 0; i < sub_count && i < 8; ++i) {
                u32 sa = static_cast<u32>(m.arg(2 + i));
                (void)m.write_u32(sid_addr + 8 + i * 4, sa);
            }

            // *pSid is a pointer-sized PSID variable (8 bytes on x64).
            if (m.write_ptr(psid_out, sid_addr).is_err()) {
                stubs.free_local_memory(sid_addr);
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            VITREO_TRACE("[advapi32] AllocateAndInitializeSid(count=%u) "
                         "-> 0x%llx\n",
                         sub_count,
                         static_cast<unsigned long long>(sid_addr));
            m.set_return(1);
        }, 44});


    // BOOL GetAclInformation(PACL, LPVOID, DWORD, ACL_INFORMATION_CLASS)
    // AclRevisionInformation is a single DWORD. AclSizeInformation contains
    // AceCount, bytes occupied by the ACL header and ACEs, and unused bytes.
    exports.entries.push_back(
        {"GetAclInformation", 0, [](Marshaler& m) {
            const GuestAddr acl = m.arg_ptr(0);
            const GuestAddr info = m.arg_ptr(1);
            const u32 info_length = static_cast<u32>(m.arg(2));
            const u32 info_class = static_cast<u32>(m.arg(3));

            ParsedAcl parsed;
            if (acl == 0 || !parse_acl(m, acl, parsed)) {
                set_last_error(error_codes::ERROR_INVALID_ACL);
                m.set_return(0);
                return;
            }

            const u32 required = info_class == 1 ? 4u
                               : info_class == 2 ? 12u
                                                 : 0u;
            if (required == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (info_length < required) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }

            bool wrote = false;
            if (info_class == 1) { // AclRevisionInformation
                wrote = info != 0 &&
                        m.write_u32(info, parsed.revision).is_ok();
            } else { // AclSizeInformation
                wrote = info != 0 &&
                        m.write_u32(info, parsed.ace_count).is_ok() &&
                        m.write_u32(info + 4, parsed.bytes_in_use).is_ok() &&
                        m.write_u32(info + 8,
                                    parsed.size - parsed.bytes_in_use).is_ok();
            }
            if (!wrote) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            VITREO_TRACE(
                "[advapi32] GetAclInformation(acl=0x%llx class=%u) "
                "-> TRUE revision=%u count=%u used=%u free=%u\n",
                static_cast<unsigned long long>(acl), info_class,
                parsed.revision, parsed.ace_count, parsed.bytes_in_use,
                parsed.size - parsed.bytes_in_use);
            m.set_return(1);
        }, 16});



    const auto set_descriptor_principal = [](Marshaler& m, bool owner) {
        constexpr u16 SE_GROUP_DEFAULTED = 0x0002;
        constexpr u16 SE_OWNER_DEFAULTED = 0x0001;
        constexpr u16 SE_SELF_RELATIVE = 0x8000;

        const GuestAddr descriptor = m.arg_ptr(0);
        const GuestAddr principal = m.arg_ptr(1);
        const bool defaulted = m.arg(2) != 0;
        if (descriptor == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        const auto revision = m.read_u8(descriptor);
        const auto control_result = m.read_u16(descriptor + 2);
        if (revision.is_err() || control_result.is_err() ||
            revision.value() != 1 ||
            (control_result.value() & SE_SELF_RELATIVE) != 0) {
            set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
            m.set_return(0);
            return;
        }

        const u32 pointer_base = m.cpu_is_64bit() ? 8u : 4u;
        const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        const u32 pointer_index = owner ? 0u : 1u;
        const GuestAddr pointer_field =
            descriptor + pointer_base + pointer_index * pointer_size;
        const bool pointer_written = m.cpu_is_64bit()
            ? m.write_u64(pointer_field, principal).is_ok()
            : m.write_u32(pointer_field, static_cast<u32>(principal)).is_ok();

        u16 control = control_result.value();
        const u16 defaulted_flag = owner ? SE_OWNER_DEFAULTED
                                         : SE_GROUP_DEFAULTED;
        if (defaulted) {
            control |= defaulted_flag;
        } else {
            control &= static_cast<u16>(~defaulted_flag);
        }
        if (!pointer_written || m.write_u16(descriptor + 2, control).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }

        VITREO_TRACE(
            "[advapi32] SetSecurityDescriptor%s(sd=0x%llx sid=0x%llx "
            "defaulted=%u) -> TRUE\n",
            owner ? "Owner" : "Group",
            static_cast<unsigned long long>(descriptor),
            static_cast<unsigned long long>(principal), defaulted ? 1u : 0u);
        m.set_return(1);
    };

    // SetSecurityDescriptorOwner/Group require an absolute descriptor. They
    // store the caller's SID pointer and update the matching defaulted bit.
    exports.entries.push_back(
        {"SetSecurityDescriptorGroup", 39,
         [set_descriptor_principal](Marshaler& m) {
             set_descriptor_principal(m, false);
         }, 12});
    exports.entries.push_back(
        {"SetSecurityDescriptorOwner", 40,
         [set_descriptor_principal](Marshaler& m) {
             set_descriptor_principal(m, true);
         }, 12});



    // PVOID FreeSid(PSID pSid)
    // 1 arg, stdcall_bytes=4
    exports.entries.push_back(
        {"FreeSid", 43, [&stubs](Marshaler& m) {
            // Frees a SID from AllocateAndInitializeSid; returns NULL and
            // leaves the last error alone (advapi_token_probe freesid).
            const GuestAddr sid = m.arg_ptr(0);
            if (sid != 0) stubs.free_local_memory(sid);
            m.set_return(0);
        }, 4});
}

} // namespace vitreo::winapi
