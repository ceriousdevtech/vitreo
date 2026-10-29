// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_object_security_exports.cpp
/// @brief advapi32 string-SD conversion and file / kernel-object security exports:
/// ConvertStringSecurityDescriptorToSecurityDescriptorA/W, GetFileSecurityA/W,
/// GetKernelObjectSecurity and SetKernelObjectSecurity.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "capsule/vfs/vfs.hpp"
#include "winapi/advapi32/security/named_security.hpp"
#include "winapi/advapi32/security/sddl.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

/// Registers ConvertStringSecurityDescriptorToSecurityDescriptorA/W,
/// GetFileSecurityA/W, GetKernelObjectSecurity, SetKernelObjectSecurity.
void add_file_object_security_exports(DllExports& exports,
                                      SecurityStubs& stubs,
                                      capsule::Vfs* vfs) {
    // -----------------------------------------------------------------
    // Service Control Manager — Wide / Ex variants (default-fail stubs)
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Security descriptor / SID helper stubs
    // -----------------------------------------------------------------

    // BOOL ConvertStringSecurityDescriptorToSecurityDescriptor{A,W}(
    //     LPCTSTR StringSD, DWORD Revision, PSECURITY_DESCRIPTOR* SD, PULONG SDSize)
    // 4 args, stdcall_bytes=16. A real SDDL parser (sddl.cpp); every rule and
    // error code is measured by tools/oracle_probe/advapi_sddl_probe.c.
    //  - NULL string / NULL out -> ERROR_INVALID_PARAMETER, before
    //    Revision != SDDL_REVISION_1 -> ERROR_UNKNOWN_REVISION; neither
    //    touches the outputs.
    //  - Past those checks *SDSize is zeroed; on failure *SD is untouched.
    //  - Success: a self-relative SD from LocalAlloc(LMEM_FIXED, size) and
    //    last error ERROR_SUCCESS (probe: 0xDEAD -> 0).
    auto string_sd_to_sd = [&stubs](Marshaler& m, bool wide) {
        constexpr u32 ERROR_UNKNOWN_REVISION = 1305;
        const GuestAddr str_sd = m.arg_ptr(0);
        const u32 revision = m.arg32(1);
        const GuestAddr out_sd = m.arg_ptr(2);
        const GuestAddr out_size = m.arg_ptr(3);
        if (str_sd == 0 || out_sd == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (revision != 1) {
            set_last_error(ERROR_UNKNOWN_REVISION);
            m.set_return(0);
            return;
        }
        if (out_size != 0) (void)m.write_u32(out_size, 0);
        auto text = wide ? m.read_wide_string(str_sd) : m.read_ansi_string(str_sd);
        if (text.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        std::vector<u8> sd;
        if (const u32 err = sddl::string_to_sd(text.value(), sd); err != 0) {
            VITREO_TRACE("[advapi32] ConvertStringSecurityDescriptorToSecurityDescriptor%c"
                         "(\"%s\") -> FALSE %u\n", wide ? 'W' : 'A',
                         text.value().c_str(), err);
            set_last_error(err);
            m.set_return(0);
            return;
        }
        const GuestAddr block = stubs.alloc_local_memory(static_cast<u32>(sd.size()));
        if (block == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        if (m.memory().write_bytes(block, sd.data(), sd.size()).is_err() ||
            m.write_ptr(out_sd, block).is_err()) {
            stubs.free_local_memory(block);
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        if (out_size != 0) (void)m.write_u32(out_size, static_cast<u32>(sd.size()));
        VITREO_TRACE("[advapi32] ConvertStringSecurityDescriptorToSecurityDescriptor%c"
                     "(\"%s\") -> TRUE sd=0x%llx size=%zu\n", wide ? 'W' : 'A',
                     text.value().c_str(), static_cast<unsigned long long>(block),
                     sd.size());
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"ConvertStringSecurityDescriptorToSecurityDescriptorA", 0,
        [string_sd_to_sd](Marshaler& m) { string_sd_to_sd(m, false); }, 16});
    exports.entries.push_back({"ConvertStringSecurityDescriptorToSecurityDescriptorW", 65,
        [string_sd_to_sd](Marshaler& m) { string_sd_to_sd(m, true); }, 16});

    // BOOL GetFileSecurityA/W(LPCTSTR lpFileName,
    //                         SECURITY_INFORMATION RequestedInformation,
    //                         PSECURITY_DESCRIPTOR pSecurityDescriptor,
    //                         DWORD nLength, LPDWORD lpnLengthNeeded)
    // 5 args, stdcall_bytes=20.
    //
    // Was previously unregistered → generic missing-export stub, which returned
    // 0xFFFFFFFF (fake TRUE for a BOOL) while writing neither the descriptor nor
    // *lpnLengthNeeded, and leaked 20 bytes of stack. See
    // .docs/battlenet-bringup.md.
    //
    // Partial-but-honest: the capsule has a single principal and no cross-user
    // boundary, so the returned descriptor is a structurally-valid 20-byte
    // self-relative SECURITY_DESCRIPTOR with a NULL DACL (grant-all) — the same
    // rationale as ConvertStringSecurityDescriptorToSecurityDescriptorW above.
    // The parts callers actually branch on are exact: object existence, the
    // ERROR_INSUFFICIENT_BUFFER size-query protocol, and *lpnLengthNeeded.
    auto get_file_security = [vfs](Marshaler& m, bool ansi) {
            GuestAddr name_ptr = m.arg_ptr(0);
            u32       req_info = static_cast<u32>(m.arg(1));
            GuestAddr sd_ptr   = m.arg_ptr(2);
            u32       n_length = static_cast<u32>(m.arg(3));
            GuestAddr needed_p = m.arg_ptr(4);

            // lpnLengthNeeded is not optional on Windows.
            if (name_ptr == 0 || needed_p == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto name_result = ansi
                ? m.read_ansi_string(name_ptr)
                : m.read_wide_string(name_ptr);
            if (!name_result.is_ok()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const std::string& path = name_result.value();

            // Without a VFS we cannot tell a missing file from a present one,
            // and claiming either would be a guess. Fail honestly.
            if (vfs == nullptr) {
                VITREO_TRACE(
                    "[advapi32] GetFileSecurity%c(\"%s\"): no VFS\n",
                    ansi ? 'A' : 'W', path.c_str());
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0);
                return;
            }
            if (!vfs->stat(path).is_ok()) {
                VITREO_TRACE(
                    "[advapi32] GetFileSecurity%c(\"%s\") -> FALSE"
                    " (not found)\n",
                    ansi ? 'A' : 'W', path.c_str());
                set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                m.set_return(0);
                return;
            }

            // Use persisted capsule metadata when SetNamedSecurityInfoW has
            // stored a descriptor; otherwise expose the capsule's default
            // single-principal NULL-DACL descriptor with owner+group SIDs
            // (required by AccessCheck per Microsoft Learn).
            std::vector<u8> descriptor;
            bool found = false;
            u32 metadata_error = error_codes::ERROR_SUCCESS;
            if (!load_security_descriptor(*vfs, path, descriptor,
                                          found, metadata_error)) {
                set_last_error(metadata_error);
                m.set_return(0);
                return;
            }
            if (!found) {
                descriptor = make_default_null_dacl_sd(
                    /*include_owner=*/true, /*include_group=*/true,
                    /*dacl_present=*/true);
            } else {
                // A stored descriptor is returned with exactly the requested
                // components: after SetNamedSecurityInfoW(DACL, NULL) a
                // DACL-only query reads control 0x9404 with no owner or
                // group (advapi_named_null_dacl_probe.expected). A
                // stored empty DACL stays empty (advapi_sdapi_probe
                // sfs_dacl_empty reads aces=0).
                named_sd::Parts parts;
                if (!named_sd::parse_sd(descriptor, parts)) {
                    set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
                    m.set_return(0);
                    return;
                }
                descriptor = named_sd::build_sd(parts, req_info);
            }

            const u32 sd_size = static_cast<u32>(descriptor.size());
            (void)m.write_u32(needed_p, sd_size);
            if (sd_ptr == 0 || n_length < sd_size) {
                VITREO_TRACE(
                    "[advapi32] GetFileSecurity%c(\"%s\" info=0x%x)"
                    " -> FALSE need=%u have=%u\n",
                    ansi ? 'A' : 'W', path.c_str(), req_info, sd_size,
                    n_length);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            if (m.memory().write_bytes(sd_ptr, descriptor.data(),
                                       descriptor.size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            VITREO_TRACE(
                "[advapi32] GetFileSecurity%c(\"%s\" info=0x%x)"
                " -> TRUE size=%u source=%s\n",
                ansi ? 'A' : 'W', path.c_str(), req_info, sd_size,
                found ? "stored" : "default");
            m.set_return(1);
        };
    exports.entries.push_back(
        {"GetFileSecurityA", 0,
         [get_file_security](Marshaler& m) { get_file_security(m, true); },
         20});
    exports.entries.push_back(
        {"GetFileSecurityW", 0,
         [get_file_security](Marshaler& m) { get_file_security(m, false); },
         20});

    // BOOL GetKernelObjectSecurity(HANDLE Handle,
    //     SECURITY_INFORMATION RequestedInformation,
    //     PSECURITY_DESCRIPTOR pSecurityDescriptor,
    //     DWORD nLength, LPDWORD lpnLengthNeeded)
    // 5 args, stdcall_bytes=20.
    //
    // Per Microsoft Learn (securitybaseapi.h): returns a self-relative copy of
    // the security descriptor protecting a kernel object. Callers resolve it
    // with GetProcAddress; a missing export is fatal (intentional UD2,
    // STATUS_ILLEGAL_INSTRUCTION 0xc000001d).
    //
    // Capsule policy: NULL DACL grant-all for DACL queries. LABEL queries
    // (SECURITY_INFORMATION 0x10) must include a mandatory-label SACL.
    // Callers dereference the SACL after GetSecurityDescriptorSacl without
    // checking the present flag, so a NULL SACL faults at [sacl+4].
    exports.entries.push_back({"GetKernelObjectSecurity", 0,
        [&stubs](Marshaler& m) {
            const Handle handle = static_cast<Handle>(m.arg(0));
            const u32 req_info = static_cast<u32>(m.arg(1));
            const GuestAddr sd_ptr = m.arg_ptr(2);
            const u32 n_length = static_cast<u32>(m.arg(3));
            const GuestAddr needed_p = m.arg_ptr(4);

            if (needed_p == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            constexpr Handle CURRENT_PROCESS = static_cast<Handle>(-1);
            constexpr Handle CURRENT_THREAD = static_cast<Handle>(-2);
            const bool pseudo =
                handle == CURRENT_PROCESS || handle == CURRENT_THREAD;
            if (handle == NULL_HANDLE ||
                (!pseudo && !stubs.handle_table().is_valid(handle))) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }

            constexpr u32 READ_CONTROL = 0x00020000;
            if (!pseudo) {
                const u32 access = stubs.handle_table().get_access(handle);
                if (access != HandleAccess::All &&
                    (access & READ_CONTROL) == 0) {
                    set_last_error(error_codes::ERROR_ACCESS_DENIED);
                    m.set_return(0);
                    return;
                }
            }

            constexpr u32 LABEL_SECURITY_INFORMATION = 0x00000010;
            constexpr u16 SE_SELF_RELATIVE = 0x8000;
            constexpr u16 SE_SACL_PRESENT = 0x0010;

            std::vector<u8> descriptor;
            if ((req_info & LABEL_SECURITY_INFORMATION) != 0) {
                // Self-relative SD + SACL with one SYSTEM_MANDATORY_LABEL_ACE
                // for Medium integrity (S-1-16-8192).
                constexpr u32 kSdHeader = 20;
                constexpr u32 kAclHeader = 8;
                constexpr u32 kAceHeader = 4;
                constexpr u32 kAceMask = 4;
                constexpr u32 kSidSize = 12; // S-1-16-8192
                constexpr u32 kAceSize = kAceHeader + kAceMask + kSidSize;
                constexpr u32 kAclSize = kAclHeader + kAceSize;
                constexpr u32 kTotal = kSdHeader + kAclSize;
                descriptor.assign(kTotal, 0);

                descriptor[0] = 1; // Revision
                descriptor[2] = static_cast<u8>(SE_SELF_RELATIVE | SE_SACL_PRESENT);
                descriptor[3] = static_cast<u8>(
                    (SE_SELF_RELATIVE | SE_SACL_PRESENT) >> 8);
                // Owner/Group/Dacl offsets remain 0.
                descriptor[12] = static_cast<u8>(kSdHeader); // Sacl offset
                descriptor[13] = static_cast<u8>(kSdHeader >> 8);

                // ACL
                descriptor[kSdHeader + 0] = 2; // ACL_REVISION
                descriptor[kSdHeader + 2] = static_cast<u8>(kAclSize);
                descriptor[kSdHeader + 3] = static_cast<u8>(kAclSize >> 8);
                descriptor[kSdHeader + 4] = 1; // AceCount
                descriptor[kSdHeader + 5] = 0;

                // SYSTEM_MANDATORY_LABEL_ACE
                const u32 ace = kSdHeader + kAclHeader;
                descriptor[ace + 0] = 0x11; // SYSTEM_MANDATORY_LABEL_ACE_TYPE
                descriptor[ace + 2] = static_cast<u8>(kAceSize);
                descriptor[ace + 3] = static_cast<u8>(kAceSize >> 8);
                // Mask: SYSTEM_MANDATORY_LABEL_NO_WRITE_UP
                descriptor[ace + 4] = 0x01;
                // SID S-1-16-8192
                const u32 sid = ace + kAceHeader + kAceMask;
                descriptor[sid + 0] = 1; // Revision
                descriptor[sid + 1] = 1; // SubAuthorityCount
                descriptor[sid + 7] = 16; // SECURITY_MANDATORY_LABEL_AUTHORITY
                descriptor[sid + 8] = 0x00; // RID 0x2000 little-endian
                descriptor[sid + 9] = 0x20;
            } else {
                // NULL-DACL grant-all self-relative descriptor (same as files).
                descriptor.assign(20, 0);
                descriptor[0] = 1;
                descriptor[3] = 0x80; // SE_SELF_RELATIVE
            }

            const u32 sd_size = static_cast<u32>(descriptor.size());
            (void)m.write_u32(needed_p, sd_size);
            if (sd_ptr == 0 || n_length < sd_size) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            if (m.memory().write_bytes(sd_ptr, descriptor.data(),
                                       descriptor.size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 20});

    // BOOL SetKernelObjectSecurity(HANDLE Handle,
    //     SECURITY_INFORMATION SecurityInformation,
    //     PSECURITY_DESCRIPTOR SecurityDescriptor)
    // 3 args, stdcall_bytes=12.
    //
    // Resolved by GetProcAddress after GetKernelObjectSecurity. A
    // missing-export fail-thunk returns 0xFFFFFFFF, which callers treat as a
    // hard failure (intentional UD2).
    // Capsule: accept sets on valid handles and process/thread pseudos after
    // a basic self-relative SD sanity check. Descriptor contents are not
    // persisted yet (Get still returns the synthetic grant-all / Medium label).
    exports.entries.push_back({"SetKernelObjectSecurity", 0,
        [&stubs](Marshaler& m) {
            const Handle handle = static_cast<Handle>(m.arg(0));
            const u32 sec_info = static_cast<u32>(m.arg(1));
            const GuestAddr sd_ptr = m.arg_ptr(2);
            (void)sec_info;

            constexpr Handle CURRENT_PROCESS = static_cast<Handle>(-1);
            constexpr Handle CURRENT_THREAD = static_cast<Handle>(-2);
            const bool pseudo =
                handle == CURRENT_PROCESS || handle == CURRENT_THREAD;
            if (handle == NULL_HANDLE ||
                (!pseudo && !stubs.handle_table().is_valid(handle))) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            if (sd_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            constexpr u32 WRITE_DAC = 0x00040000;
            constexpr u32 WRITE_OWNER = 0x00080000;
            constexpr u32 ACCESS_SYSTEM_SECURITY = 0x01000000;
            if (!pseudo) {
                const u32 access = stubs.handle_table().get_access(handle);
                if (access != HandleAccess::All &&
                    (access & (WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY)) == 0) {
                    set_last_error(error_codes::ERROR_ACCESS_DENIED);
                    m.set_return(0);
                    return;
                }
            }

            // Minimal SECURITY_DESCRIPTOR header check (revision + self-relative).
            auto rev = m.memory().read_u8(sd_ptr);
            auto control_hi = m.memory().read_u8(sd_ptr + 3);
            if (rev.is_err() || control_hi.is_err() ||
                rev.value() != 1 ||
                (control_hi.value() & 0x80) == 0) { // SE_SELF_RELATIVE
                set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
                m.set_return(0);
                return;
            }

            m.set_return(1);
        }, 12});
}

} // namespace vitreo::winapi
