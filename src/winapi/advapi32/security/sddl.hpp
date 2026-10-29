// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file sddl.hpp
/// @brief Security Descriptor Definition Language: string <-> descriptor.
///
/// Contract sources:
///  - Learn: "Security Descriptor String Format", "ACE Strings", "SID Strings",
///    ConvertStringSecurityDescriptorToSecurityDescriptorW,
///    ConvertSecurityDescriptorToStringSecurityDescriptorW,
///    ConvertStringSidToSidW.
///  - sddl.h (10.0.26100.0) for every alias string.
///  - tools/oracle_probe/advapi_sddl_probe.c (.expected / .x86.expected) for
///    everything Learn leaves open: the SID each alias maps to, byte layout
///    (SACL, DACL, owner, group; ACL revision 4 plus 4 slack bytes for an
///    object ACE converted to its plain form), whitespace and trailing-text
///    rules, the numeric-SID grammar, and each error code.
///
/// Pure host code: the thunks read/write guest memory and call in here.

#include <vitreo/common.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {
class Marshaler;
}

namespace vitreo::winapi::sddl {

/// A security descriptor taken apart (from either the absolute or the
/// self-relative form).
struct SdView {
    u8 revision{1};
    u16 control{0};
    std::vector<u8> owner;       ///< empty = absent
    std::vector<u8> group;       ///< empty = absent
    bool dacl_present{false};
    bool dacl_null{false};
    std::vector<u8> dacl;        ///< whole ACL (header + ACEs); empty when null
    bool sacl_present{false};
    bool sacl_null{false};
    std::vector<u8> sacl;
};

/// ConvertStringSidToSid: "S-R-I-S..." or a two-letter SDDL alias. Returns
/// ERROR_SUCCESS or ERROR_INVALID_SID.
[[nodiscard]] u32 string_to_sid(std::string_view text, std::vector<u8>& sid);

/// ConvertStringSecurityDescriptorToSecurityDescriptor after the NULL and
/// revision checks: a self-relative descriptor, or the Win32 error Windows
/// reports for this string.
[[nodiscard]] u32 string_to_sd(std::string_view text, std::vector<u8>& sd);

/// ConvertSecurityDescriptorToStringSecurityDescriptor after the NULL and
/// revision checks. `si` is the SECURITY_INFORMATION mask. `reported_length`
/// receives the length the W function reports: the string length plus one
/// per printed ACE plus the 10-character budget shortfall of each hex mask
/// (probe: "O:BA" -> 4, "D:(A;;GA;;;WD)" -> 15, "D:(A;;0x200;;;WD)" -> 23).
[[nodiscard]] u32 sd_to_string(const SdView& sd, u32 si, std::string& out,
                               u32* reported_length = nullptr);

/// A SID as SDDL writes it: an alias when Windows uses one, otherwise the
/// standard notation. Empty when the SID is not valid.
[[nodiscard]] std::string sid_to_sddl(std::span<const u8> sid);

/// ConvertSidToStringSid notation ("S-1-5-18", "S-1-0x100000000-1"). Does not
/// validate.
[[nodiscard]] std::string sid_to_string(std::span<const u8> sid);

/// CreateWellKnownSid's SID for `type` (`domain` for the account-domain
/// types). ERROR_SUCCESS or ERROR_INVALID_PARAMETER (unknown type, a domain
/// type without a usable DomainSid). tools/oracle_probe/advapi_wksid_probe.
[[nodiscard]] u32 create_well_known_sid(u32 type, const std::vector<u8>* domain,
                                        std::vector<u8>& sid);
/// IsWellKnownSid.
[[nodiscard]] bool is_well_known_sid(std::span<const u8> sid, u32 type);

/// Read a guest SECURITY_DESCRIPTOR (absolute at the guest's pointer width, or
/// self-relative) into a view. Returns ERROR_SUCCESS, ERROR_UNKNOWN_REVISION
/// for a revision other than 1, or ERROR_INVALID_SECURITY_DESCR / ERROR_NOACCESS
/// when a component cannot be read.
[[nodiscard]] u32 capture_guest_sd(Marshaler& m, u64 sd, SdView& out);

/// Serialise a view as a self-relative descriptor in the order Windows'
/// SDDL conversion uses (SACL, DACL, owner, group).
[[nodiscard]] std::vector<u8> build_self_relative(const SdView& sd);

} // namespace vitreo::winapi::sddl
