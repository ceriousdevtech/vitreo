// SPDX-License-Identifier: LGPL-2.1-only

/// @file sid_conversion_exports.cpp
/// @brief advapi32 SID and security-descriptor conversions: BuildExplicitAccessWithNameW,
/// CreateWellKnownSid, IsWellKnownSid, GetAce, ConvertSidToStringSidA/W,
/// ConvertSecurityDescriptorToStringSecurityDescriptorA/W,
/// ConvertStringSidToSidA/W and DeriveCapabilitySidsFromName.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "winapi/advapi32/security/sddl.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <CommonCrypto/CommonDigest.h>

namespace vitreo::winapi {

/// Registers BuildExplicitAccessWithNameW, CreateWellKnownSid,
/// IsWellKnownSid, GetAce, the SID / security-descriptor to string
/// conversions and DeriveCapabilitySidsFromName.
void add_sid_conversion_exports(DllExports& exports, SecurityStubs& stubs) {
    // VOID BuildExplicitAccessWithNameW(PEXPLICIT_ACCESS_W pExplicitAccess,
    //     LPWSTR pTrusteeName, DWORD AccessPermissions, ACCESS_MODE AccessMode,
    //     DWORD Inheritance)
    // 5 args, stdcall_bytes=20. No return value. A sandbox helper calls this
    // via GetProcAddress to grant CURRENT_USER GENERIC_ALL; a null resolution
    // previously produced a call-through-null (RIP=0) crash.
    // Pure struct fill: populate the EXPLICIT_ACCESS_W and its embedded TRUSTEE_W
    // exactly as the documented contract specifies (TRUSTEE_IS_NAME, name ptr).
    exports.entries.push_back({"BuildExplicitAccessWithNameW", 0,
        [](Marshaler& m) {
            GuestAddr ea          = m.arg_ptr(0);
            GuestAddr trustee_name = m.arg_ptr(1);
            u32 access_perms      = static_cast<u32>(m.arg(2));
            u32 access_mode       = static_cast<u32>(m.arg(3));
            u32 inheritance       = static_cast<u32>(m.arg(4));
            if (ea == 0) return;

            constexpr u32 NO_MULTIPLE_TRUSTEE = 0;
            constexpr u32 TRUSTEE_IS_NAME     = 1;
            constexpr u32 TRUSTEE_IS_UNKNOWN  = 0;

            if (m.cpu_is_64bit()) {
                // EXPLICIT_ACCESS_W (x64, 48 bytes); TRUSTEE_W begins at +16.
                (void)m.write_u32(ea + 0,  access_perms);   // grfAccessPermissions
                (void)m.write_u32(ea + 4,  access_mode);    // grfAccessMode
                (void)m.write_u32(ea + 8,  inheritance);    // grfInheritance
                // +12 padding
                (void)m.write_u64(ea + 16, 0);              // Trustee.pMultipleTrustee
                (void)m.write_u32(ea + 24, NO_MULTIPLE_TRUSTEE);
                (void)m.write_u32(ea + 28, TRUSTEE_IS_NAME);   // TrusteeForm
                (void)m.write_u32(ea + 32, TRUSTEE_IS_UNKNOWN);// TrusteeType
                // +36 padding
                (void)m.write_u64(ea + 40, static_cast<u64>(trustee_name));
            } else {
                // EXPLICIT_ACCESS_W (x86, 32 bytes); TRUSTEE_W begins at +12.
                (void)m.write_u32(ea + 0,  access_perms);
                (void)m.write_u32(ea + 4,  access_mode);
                (void)m.write_u32(ea + 8,  inheritance);
                (void)m.write_u32(ea + 12, 0);              // pMultipleTrustee
                (void)m.write_u32(ea + 16, NO_MULTIPLE_TRUSTEE);
                (void)m.write_u32(ea + 20, TRUSTEE_IS_NAME);
                (void)m.write_u32(ea + 24, TRUSTEE_IS_UNKNOWN);
                (void)m.write_u32(ea + 28, static_cast<u32>(trustee_name));
            }
        }, 20});


    // BOOL CreateWellKnownSid(WELL_KNOWN_SID_TYPE WellKnownSidType,
    //   PSID DomainSid, PSID pSid, DWORD* cbSid) — stdcall 16.
    // https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-createwellknownsid
    // Table and every rule from tools/oracle_probe/advapi_wksid_probe.expected:
    // an unknown type or a domain type without a usable DomainSid ->
    // ERROR_INVALID_PARAMETER with *cbSid untouched; otherwise *cbSid becomes
    // the required size and a short buffer is ERROR_INSUFFICIENT_BUFFER, a
    // NULL pSid with room is ERROR_INVALID_PARAMETER (probe
    // world_nullsid_cb100). Success leaves the last error untouched.
    exports.entries.push_back({"CreateWellKnownSid", 66,
        [](Marshaler& m) {
            const u32 sid_type = m.arg32(0);
            const GuestAddr domain_ptr = m.arg_ptr(1);
            const GuestAddr sid = m.arg_ptr(2);
            const GuestAddr size_ptr = m.arg_ptr(3);
            if (size_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            std::vector<u8> domain;
            if (domain_ptr != 0) {
                const u8 count = m.read_u8(domain_ptr + 1).value_or(0);
                domain.resize(8u + count * 4u);
                if (m.memory().read_bytes(domain_ptr, domain.data(), domain.size()).is_err()) {
                    domain.clear();
                }
            }
            std::vector<u8> bytes;
            if (sddl::create_well_known_sid(sid_type, domain_ptr != 0 ? &domain : nullptr,
                                            bytes) != 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const u32 required = static_cast<u32>(bytes.size());
            const u32 available = m.read_u32(size_ptr).value_or(0);
            (void)m.write_u32(size_ptr, required);
            if (available < required) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            if (sid == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            (void)m.memory().write_bytes(sid, bytes.data(), bytes.size());
            VITREO_TRACE("[advapi32] CreateWellKnownSid(type=%u) -> %u bytes\n",
                         sid_type, required);
            m.set_return(1);
        }, 16});

    // BOOL IsWellKnownSid(PSID pSid, WELL_KNOWN_SID_TYPE WellKnownSidType) —
    // stdcall 8. Same table (probe "isWk" and iswk_* lines).
    exports.entries.push_back({"IsWellKnownSid", 0,
        [](Marshaler& m) {
            const GuestAddr sid = m.arg_ptr(0);
            const u32 type = m.arg32(1);
            std::vector<u8> bytes;
            if (sid != 0 && SecurityStubs::is_valid_sid(m, sid) != 0) {
                bytes.resize(SecurityStubs::get_length_sid(m, sid));
                (void)m.memory().read_bytes(sid, bytes.data(), bytes.size());
            }
            m.set_return(sddl::is_well_known_sid(bytes, type) ? 1 : 0);
        }, 8});

    // ========================================================================
    // Wave 8: additional security contracts
    // ========================================================================
    // BOOL GetAce(PACL pAcl, DWORD dwAceIndex, LPVOID *pAce)
    // Microsoft Learn: return a pointer to the indexed ACE within the caller's
    // ACL. The pointer remains owned by the ACL; no copy is allocated.
    exports.entries.push_back({"GetAce", 0,
        [](Marshaler& m) {
            const GuestAddr acl = m.arg_ptr(0);
            const u32 index = static_cast<u32>(m.arg(1));
            const GuestAddr out = m.arg_ptr(2);
            ParsedAcl parsed;
            if (acl == 0 || out == 0 || !parse_acl(m, acl, parsed)) {
                if (out != 0) (void)m.write_ptr(out, 0);
                set_last_error(error_codes::ERROR_INVALID_ACL);
                m.set_return(0);
                return;
            }
            if (index >= parsed.ace_count) {
                (void)m.write_ptr(out, 0);
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const GuestAddr ace = acl + parsed.ace_offsets[index];
            if (m.write_ptr(out, ace).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            VITREO_TRACE(
                "[advapi32] GetAce(acl=0x%llx index=%u) -> 0x%llx\n",
                static_cast<unsigned long long>(acl), index,
                static_cast<unsigned long long>(ace));
            m.set_return(1);
        }, 12});

    auto format_sid = [](Marshaler& m, GuestAddr sid,
                         std::string& text_out) -> bool {
        if (SecurityStubs::is_valid_sid(m, sid) == 0) {
            return false;
        }

        auto revision = m.read_u8(sid);
        auto count = m.read_u8(sid + 1);
        if (revision.is_err() || count.is_err()) {
            return false;
        }

        u64 authority = 0;
        for (u32 i = 0; i < 6; ++i) {
            auto byte = m.read_u8(sid + 2 + i);
            if (byte.is_err()) {
                return false;
            }
            authority = (authority << 8) | byte.value();
        }

        std::ostringstream text;
        text << "S-" << static_cast<u32>(revision.value()) << '-';
        if (authority <= 0xFFFFFFFFull) {
            text << authority;
        } else {
            // No zero padding (probe sidtostr_bigauth: "S-1-0x100000000-1").
            text << "0x" << std::hex << std::uppercase << authority << std::dec;
        }
        for (u32 i = 0; i < count.value(); ++i) {
            auto sub_authority = m.read_u32(sid + 8 + i * 4);
            if (sub_authority.is_err()) {
                return false;
            }
            text << '-' << sub_authority.value();
        }
        text_out = text.str();
        return true;
    };

    auto convert_sid_to_string = [&stubs, format_sid](Marshaler& m,
                                                       bool wide) {
        constexpr u32 ERROR_INVALID_SID = 1337;
        const GuestAddr sid = m.arg_ptr(0);
        const GuestAddr string_out = m.arg_ptr(1);
        if (string_out == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (m.write_ptr(string_out, 0).is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        std::string text;
        if (!format_sid(m, sid, text)) {
            set_last_error(ERROR_INVALID_SID);
            m.set_return(0);
            return;
        }

        const u32 char_width = wide ? 2u : 1u;
        const u32 byte_count = (static_cast<u32>(text.size()) + 1u) * char_width;
        const GuestAddr result = stubs.alloc_local_memory(byte_count);
        if (result == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }

        for (u32 i = 0; i < text.size(); ++i) {
            if (wide) {
                (void)m.write_u16(result + i * 2, static_cast<u8>(text[i]));
            } else {
                (void)m.write_u8(result + i, static_cast<u8>(text[i]));
            }
        }
        if (wide) {
            (void)m.write_u16(result + text.size() * 2, 0);
        } else {
            (void)m.write_u8(result + text.size(), 0);
        }
        if (m.write_ptr(string_out, result).is_err()) {
            stubs.free_local_memory(result);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        VITREO_TRACE("[advapi32] ConvertSidToStringSid%c(0x%llx) -> '%s' at 0x%llx\n",
                     wide ? 'W' : 'A', static_cast<unsigned long long>(sid),
                     text.c_str(), static_cast<unsigned long long>(result));
        m.set_return(1);
    };

    // The returned fixed allocation is owned by the caller and released with
    // LocalFree, matching the Windows advapi32 contract.
    exports.entries.push_back({"ConvertSidToStringSidA", 0,
        [convert_sid_to_string](Marshaler& m) {
            convert_sid_to_string(m, false);
        }, 8});
    exports.entries.push_back({"ConvertSidToStringSidW", 0,
        [convert_sid_to_string](Marshaler& m) {
            convert_sid_to_string(m, true);
        }, 8});

    // BOOL ConvertSecurityDescriptorToStringSecurityDescriptor{A,W}(
    //     PSECURITY_DESCRIPTOR, DWORD Revision, SECURITY_INFORMATION,
    //     LPTSTR* StringSD, PULONG StringSDLen)
    // 5 args, stdcall_bytes=20. Formatter in sddl.cpp; measured by
    // tools/oracle_probe/advapi_sddl_probe.c:
    //  - NULL out -> ERROR_INVALID_PARAMETER; Revision != 1 or a descriptor
    //    revision != 1 -> ERROR_UNKNOWN_REVISION; outputs untouched.
    //  - A NULL descriptor succeeds with an empty string.
    //  - W: *Len is Windows' budgeted length (sddl::sd_to_string), the
    //    buffer (Len + 1) WCHARs. A: *Len and the buffer are W Len + 1 bytes
    //    (probe ansi_len / ansi_nullsd).
    //  - Success sets ERROR_SUCCESS; the string is LocalAlloc'd.
    auto sd_to_string_sd = [&stubs](Marshaler& m, bool wide) {
        constexpr u32 ERROR_UNKNOWN_REVISION = 1305;
        const GuestAddr descriptor = m.arg_ptr(0);
        const u32 revision = m.arg32(1);
        const u32 requested = m.arg32(2);
        const GuestAddr string_out = m.arg_ptr(3);
        const GuestAddr length_out = m.arg_ptr(4);
        if (string_out == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (revision != 1) {
            set_last_error(ERROR_UNKNOWN_REVISION);
            m.set_return(0);
            return;
        }
        std::string text;
        u32 wide_length = 0;
        if (descriptor != 0) {
            sddl::SdView view;
            u32 err = sddl::capture_guest_sd(m, descriptor, view);
            if (err == 0) err = sddl::sd_to_string(view, requested, text, &wide_length);
            if (err != 0) {
                set_last_error(err);
                m.set_return(0);
                return;
            }
        }
        const u32 unit = wide ? 2u : 1u;
        const u32 reported = wide ? wide_length : wide_length + 1;
        const u32 bytes = wide ? (wide_length + 1) * 2 : wide_length + 1;
        const GuestAddr result = stubs.alloc_local_memory(bytes);
        if (result == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        std::vector<u8> encoded(bytes, 0);
        for (usize i = 0; i < text.size(); ++i) encoded[i * unit] = static_cast<u8>(text[i]);
        if (m.memory().write_bytes(result, encoded.data(), encoded.size()).is_err() ||
            m.write_ptr(string_out, result).is_err()) {
            stubs.free_local_memory(result);
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        if (length_out != 0) (void)m.write_u32(length_out, reported);
        VITREO_TRACE("[advapi32] ConvertSecurityDescriptorToStringSecurityDescriptor%c"
                     " -> \"%s\"\n", wide ? 'W' : 'A', text.c_str());
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"ConvertSecurityDescriptorToStringSecurityDescriptorA", 0,
        [sd_to_string_sd](Marshaler& m) { sd_to_string_sd(m, false); }, 20});
    exports.entries.push_back({"ConvertSecurityDescriptorToStringSecurityDescriptorW", 0,
        [sd_to_string_sd](Marshaler& m) { sd_to_string_sd(m, true); }, 20});

    // ConvertStringSidToSid{A,W}: standard notation or a two-letter SDDL alias
    // (probe advapi_sddl_probe "sid" lines); success sets ERROR_SUCCESS.
    auto convert_string_sid_to_sid = [&stubs](Marshaler& m, bool wide) {
        constexpr u32 ERROR_INVALID_SID = 1337;
        const GuestAddr string_ptr = m.arg_ptr(0);
        const GuestAddr sid_out = m.arg_ptr(1);
        if (sid_out == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        // Failure leaves *Sid unchanged (advapi_sddl_probe "sid" lines: out=1).
        if (string_ptr == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        Result<std::string> text_result = wide
            ? m.read_wide_string(string_ptr)
            : m.read_ansi_string(string_ptr);
        if (text_result.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        std::vector<u8> sid;
        if (sddl::string_to_sid(text_result.value(), sid) != 0) {
            set_last_error(ERROR_INVALID_SID);
            m.set_return(0);
            return;
        }

        const GuestAddr result = stubs.alloc_local_memory(static_cast<u32>(sid.size()));
        if (result == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        if (m.memory().write_bytes(result, sid.data(), sid.size()).is_err() ||
            m.write_ptr(sid_out, result).is_err()) {
            stubs.free_local_memory(result);
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }

        VITREO_TRACE("[advapi32] ConvertStringSidToSid%c('%s') -> 0x%llx\n",
                     wide ? 'W' : 'A', text_result.value().c_str(),
                     static_cast<unsigned long long>(result));
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };

    // The returned SID is a fixed local allocation owned by the caller.
    exports.entries.push_back({"ConvertStringSidToSidA", 0,
        [convert_string_sid_to_sid](Marshaler& m) {
            convert_string_sid_to_sid(m, false);
        }, 8});
    exports.entries.push_back({"ConvertStringSidToSidW", 0,
        [convert_string_sid_to_sid](Marshaler& m) {
            convert_string_sid_to_sid(m, true);
        }, 8});

    // BOOL DeriveCapabilitySidsFromName(LPCWSTR CapName,
    //   PSID** CapabilityGroupSids, DWORD* CapabilityGroupSidCount,
    //   PSID** CapabilitySids, DWORD* CapabilitySidCount)
    // KernelBase / api-ms-win-security-base-l1-2-2. LPAC directory grants
    // resolve this via GetProcAddress. A missing export fails the grant
    // and leaves LastError at 0.
    //
    // Contract (public docs / Old New Thing):
    //   - Uppercase the UTF-16 name (no NUL), SHA-256 it.
    //   - Group SID: S-1-5-32-<8 LE dwords of hash>
    //   - Capability SID: well-known short form S-1-15-3-<rid> for legacy
    //     names, otherwise S-1-15-3-1024-<8 LE dwords of hash>
    // Caller LocalFree's each SID and each PSID array.
    exports.entries.push_back({"DeriveCapabilitySidsFromName", 0,
        [&stubs](Marshaler& m) {
            const GuestAddr name_ptr = m.arg_ptr(0);
            const GuestAddr group_sids_out = m.arg_ptr(1);
            const GuestAddr group_count_out = m.arg_ptr(2);
            const GuestAddr cap_sids_out = m.arg_ptr(3);
            const GuestAddr cap_count_out = m.arg_ptr(4);

            auto fail = [&](u32 err) {
                set_last_error(err);
                m.set_return(0);
            };

            if (name_ptr == 0 || group_sids_out == 0 || group_count_out == 0 ||
                cap_sids_out == 0 || cap_count_out == 0) {
                fail(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }

            // Read UTF-16LE code units and uppercase ASCII letters for the
            // hash input (RtlUpcaseUnicodeString shape for BMP/ASCII names).
            std::vector<u16> units;
            units.reserve(64);
            for (usize i = 0; i < 32768; ++i) {
                auto cu = m.read_u16(name_ptr + i * 2);
                if (cu.is_err()) {
                    fail(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                if (cu.value() == 0) {
                    break;
                }
                u16 c = cu.value();
                if (c >= u16{'a'} && c <= u16{'z'}) {
                    c = static_cast<u16>(c - (u16{'a'} - u16{'A'}));
                }
                units.push_back(c);
            }
            if (units.empty()) {
                fail(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }

            std::vector<u8> hash_input(units.size() * 2);
            for (usize i = 0; i < units.size(); ++i) {
                hash_input[i * 2] = static_cast<u8>(units[i] & 0xFF);
                hash_input[i * 2 + 1] = static_cast<u8>((units[i] >> 8) & 0xFF);
            }

            u8 digest[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(hash_input.data(),
                      static_cast<CC_LONG>(hash_input.size()), digest);

            u32 hash_dwords[8];
            for (int i = 0; i < 8; ++i) {
                hash_dwords[i] =
                    static_cast<u32>(digest[i * 4]) |
                    (static_cast<u32>(digest[i * 4 + 1]) << 8) |
                    (static_cast<u32>(digest[i * 4 + 2]) << 16) |
                    (static_cast<u32>(digest[i * 4 + 3]) << 24);
            }

            // Narrow uppercase name for well-known capability RID lookup.
            std::string upper_ascii;
            upper_ascii.reserve(units.size());
            bool ascii_name = true;
            for (u16 c : units) {
                if (c > 0x7F) {
                    ascii_name = false;
                    break;
                }
                upper_ascii.push_back(static_cast<char>(c));
            }

            static constexpr struct {
                const char* name;
                u32 rid;
            } kWellKnownCaps[] = {
                {"INTERNETCLIENT", 1},
                {"INTERNETCLIENTSERVER", 2},
                {"PRIVATENETWORKCLIENTSERVER", 3},
                {"PICTURESLIBRARY", 4},
                {"VIDEOSLIBRARY", 5},
                {"MUSICLIBRARY", 6},
                {"DOCUMENTSLIBRARY", 7},
                {"ENTERPRISEAUTHENTICATION", 8},
                {"SHAREDUSERCERTIFICATES", 9},
                {"REMOVABLESTORAGE", 10},
                {"APPOINTMENTS", 11},
                {"CONTACTS", 12},
                {"INTERNETEXPLORER", 4096},
            };

            std::optional<u32> well_known_rid;
            if (ascii_name) {
                for (const auto& entry : kWellKnownCaps) {
                    if (upper_ascii == entry.name) {
                        well_known_rid = entry.rid;
                        break;
                    }
                }
            }

            auto write_sid_bytes = [](std::vector<u8>& out, u8 authority,
                                      const u32* subauths, u8 count) {
                out.clear();
                out.resize(8u + 4u * count);
                out[0] = 1; // Revision
                out[1] = count;
                out[2] = out[3] = out[4] = out[5] = out[6] = 0;
                out[7] = authority;
                for (u8 i = 0; i < count; ++i) {
                    const u32 sa = subauths[i];
                    out[8 + i * 4 + 0] = static_cast<u8>(sa & 0xFF);
                    out[8 + i * 4 + 1] = static_cast<u8>((sa >> 8) & 0xFF);
                    out[8 + i * 4 + 2] = static_cast<u8>((sa >> 16) & 0xFF);
                    out[8 + i * 4 + 3] = static_cast<u8>((sa >> 24) & 0xFF);
                }
            };

            // Group: S-1-5-32-<hash[0..7]>  (9 sub-authorities)
            u32 group_subs[9];
            group_subs[0] = 32; // SECURITY_BUILTIN_DOMAIN_RID /
                                // SECURITY_INSTALLER_GROUP_CAPABILITY_BASE
            for (int i = 0; i < 8; ++i) {
                group_subs[1 + i] = hash_dwords[i];
            }
            std::vector<u8> group_sid;
            write_sid_bytes(group_sid, 5, group_subs, 9);

            // Capability SID
            std::vector<u8> cap_sid;
            if (well_known_rid.has_value()) {
                u32 subs[2] = {3, well_known_rid.value()};
                write_sid_bytes(cap_sid, 15, subs, 2);
            } else {
                u32 subs[10];
                subs[0] = 3;     // SECURITY_CAPABILITY_BASE_RID
                subs[1] = 1024;  // SECURITY_CAPABILITY_APP_RID
                for (int i = 0; i < 8; ++i) {
                    subs[2 + i] = hash_dwords[i];
                }
                write_sid_bytes(cap_sid, 15, subs, 10);
            }

            const u32 ptr_size = m.cpu_is_64bit() ? 8u : 4u;

            auto alloc_sid = [&](const std::vector<u8>& bytes) -> GuestAddr {
                GuestAddr addr =
                    stubs.alloc_local_memory(static_cast<u32>(bytes.size()));
                if (addr == 0) {
                    return 0;
                }
                if (m.memory()
                        .write_bytes(addr, bytes.data(), bytes.size())
                        .is_err()) {
                    stubs.free_local_memory(addr);
                    return 0;
                }
                return addr;
            };

            auto alloc_ptr_array = [&](GuestAddr sid) -> GuestAddr {
                GuestAddr arr = stubs.alloc_local_memory(ptr_size);
                if (arr == 0) {
                    return 0;
                }
                if (m.write_ptr(arr, sid).is_err()) {
                    stubs.free_local_memory(arr);
                    return 0;
                }
                return arr;
            };

            const GuestAddr group_sid_addr = alloc_sid(group_sid);
            const GuestAddr cap_sid_addr = alloc_sid(cap_sid);
            if (group_sid_addr == 0 || cap_sid_addr == 0) {
                if (group_sid_addr != 0) {
                    stubs.free_local_memory(group_sid_addr);
                }
                if (cap_sid_addr != 0) {
                    stubs.free_local_memory(cap_sid_addr);
                }
                fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return;
            }

            const GuestAddr group_arr = alloc_ptr_array(group_sid_addr);
            const GuestAddr cap_arr = alloc_ptr_array(cap_sid_addr);
            if (group_arr == 0 || cap_arr == 0) {
                stubs.free_local_memory(group_sid_addr);
                stubs.free_local_memory(cap_sid_addr);
                if (group_arr != 0) {
                    stubs.free_local_memory(group_arr);
                }
                if (cap_arr != 0) {
                    stubs.free_local_memory(cap_arr);
                }
                fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return;
            }

            if (m.write_ptr(group_sids_out, group_arr).is_err() ||
                m.write_u32(group_count_out, 1).is_err() ||
                m.write_ptr(cap_sids_out, cap_arr).is_err() ||
                m.write_u32(cap_count_out, 1).is_err()) {
                stubs.free_local_memory(group_sid_addr);
                stubs.free_local_memory(cap_sid_addr);
                stubs.free_local_memory(group_arr);
                stubs.free_local_memory(cap_arr);
                fail(error_codes::ERROR_NOACCESS);
                return;
            }

            fprintf(stderr,
                    "[advapi32] DeriveCapabilitySidsFromName name_units=%zu "
                    "well_known=%d cap_sid_bytes=%zu -> TRUE\n",
                    units.size(), well_known_rid.has_value() ? 1 : 0,
                    cap_sid.size());
            m.set_return(1);
        },
        20});
}

} // namespace vitreo::winapi
