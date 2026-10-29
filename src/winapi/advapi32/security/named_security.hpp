// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file named_security.hpp
/// @brief Security-descriptor model behind Get/SetNamedSecurityInfoW.
///
/// Semantics measured on Windows 10 (tools/oracle_probe/advapi_acl_probe.c):
///  - Set replaces ONLY the components named in SecurityInfo;
///  - an empty DACL is stored as-is (deny all);
///  - PROTECTED_/UNPROTECTED_DACL_SECURITY_INFORMATION drive SE_DACL_PROTECTED
///    and whether the parent's inheritable ACEs are merged in (after the
///    explicit ones), and a new inheritable ACE propagates to existing
///    children;
///  - a file with no stored descriptor carries the ACEs it inherited.
///  - SACL classes (audit, mandatory label, resource attribute, scoped policy,
///    trust label) are selected independently by their *_SECURITY_INFORMATION
///    bit.

#include <vitreo/common.hpp>

#include <span>
#include <string>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi::named_sd {

// SECURITY_INFORMATION bits (winnt.h).
inline constexpr u32 kOwner = 0x00000001;
inline constexpr u32 kGroup = 0x00000002;
inline constexpr u32 kDacl = 0x00000004;
inline constexpr u32 kSacl = 0x00000008;
inline constexpr u32 kLabel = 0x00000010;
inline constexpr u32 kAttribute = 0x00000020;
inline constexpr u32 kScope = 0x00000040;
inline constexpr u32 kTrustLabel = 0x00000080;
inline constexpr u32 kBackup = 0x00010000;
inline constexpr u32 kProtectedDacl = 0x80000000;
inline constexpr u32 kProtectedSacl = 0x40000000;
inline constexpr u32 kUnprotectedDacl = 0x20000000;
inline constexpr u32 kUnprotectedSacl = 0x10000000;
/// Every SACL class selector.
inline constexpr u32 kSaclClasses = kSacl | kLabel | kAttribute | kScope | kTrustLabel;

// SECURITY_DESCRIPTOR_CONTROL bits.
inline constexpr u16 kSeDaclPresent = 0x0004;
inline constexpr u16 kSeSaclPresent = 0x0010;
inline constexpr u16 kSeDaclAutoInherited = 0x0400;
inline constexpr u16 kSeSaclAutoInherited = 0x0800;
inline constexpr u16 kSeDaclProtected = 0x1000;
inline constexpr u16 kSeSaclProtected = 0x2000;
inline constexpr u16 kSeSelfRelative = 0x8000;

/// A self-relative security descriptor taken apart.
struct Parts {
    std::vector<u8> owner;              ///< empty = absent
    std::vector<u8> group;              ///< empty = absent
    bool dacl_present{false};
    bool dacl_null{false};              ///< present with a NULL ACL
    u8 dacl_revision{2};
    std::vector<std::vector<u8>> dacl;  ///< raw ACEs
    bool sacl_present{false};
    bool sacl_null{false};
    u8 sacl_revision{2};
    std::vector<std::vector<u8>> sacl;
    u16 control{0};                     ///< only the protected/auto-inherited bits
};

/// Who is asking: the effective token's user, the groups it may assign as
/// owner, and the privileges it holds (present, enabled or not — the aclapi
/// functions enable SE_SECURITY_NAME / SE_RESTORE_NAME themselves).
struct Principal {
    std::vector<u8> user;
    std::vector<std::vector<u8>> owner_groups;
    std::vector<std::vector<u8>> alias_groups; ///< SAM aliases the user is in
    bool has_security_privilege{false};
    bool has_restore_privilege{false};
};

[[nodiscard]] bool parse_sd(std::span<const u8> sd, Parts& out);
[[nodiscard]] bool parse_acl(std::span<const u8> acl, u8& revision,
                             std::vector<std::vector<u8>>& aces);
[[nodiscard]] std::vector<u8> build_acl(u8 revision,
                                        const std::vector<std::vector<u8>>& aces);
/// Serialise only the components selected by `info` (SACL ACEs filtered to
/// the selected classes).
[[nodiscard]] std::vector<u8> build_sd(const Parts& parts, u32 info);

/// SecurityInfo bit selecting an ACE of `type` inside a SACL.
[[nodiscard]] u32 sacl_class_of(u8 ace_type) noexcept;

/// Normalise BACKUP_SECURITY_INFORMATION into its component bits.
[[nodiscard]] u32 expand_info(u32 info) noexcept;

/// The descriptor a file or directory carries: the stored one, or the one it
/// inherited from its nearest ancestor. Returns a Win32 error.
[[nodiscard]] u32 effective_file_sd(capsule::Vfs& vfs, const std::string& path,
                                    const Principal& who, Parts& out);

/// Apply SetNamedSecurityInfo to a file object. `in` holds the components
/// the caller passed (only those selected by `info` are read). Returns a
/// Win32 error.
[[nodiscard]] u32 set_file_sd(capsule::Vfs& vfs, const std::string& path,
                              u32 info, const Parts& in, const Principal& who);

/// The same merge for objects without an inheritance hierarchy (registry
/// names, kernel objects): update `current` in place.
[[nodiscard]] u32 merge_components(Parts& current, u32 info, const Parts& in,
                                   const Principal& who);

/// Access a trustee is granted by `aces` (GetEffectiveRightsFromAcl rules
/// measured on Windows: inherit-only ACEs ignored, inherited denies applied,
/// Everyone and the trustee's SAM aliases count, token-only groups do not).
[[nodiscard]] u32 effective_rights(const std::vector<std::vector<u8>>& aces,
                                   std::span<const u8> trustee,
                                   const std::vector<std::vector<u8>>& trustee_aliases);

} // namespace vitreo::winapi::named_sd
