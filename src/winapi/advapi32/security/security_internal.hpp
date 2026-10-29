#pragma once
/// @file security_internal.hpp
/// @brief Shared surface between security.cpp and security_registration.cpp.
///
/// Private to advapi32's security implementation. These were file-local to
/// security.cpp before its export table was split out; this header exists so
/// the two halves can still see each other, not to publish them.
///
/// Windows security descriptors are a self-describing byte layout — a header
/// with offsets to an owner SID, a group SID, and two ACLs — so most of what
/// follows is layout manipulation rather than policy.

#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "capsule/vfs/vfs.hpp"

#include <vitreo/common.hpp>

#include <span>
#include <string>
#include <string_view>
#include <map>
#include <mutex>
#include <vector>

namespace vitreo::winapi {

/// Sentinel index meaning "append at the end" for ACE insertion.
inline constexpr u32 kAppendAce = 0xFFFFFFFFu;

/// A parsed ACL header plus the offset of each ACE within it.
struct ParsedAcl {
    u8 revision{0};
    u16 size{0};
    u16 ace_count{0};
    u32 bytes_in_use{0};
    std::vector<u32> ace_offsets;
};

/// Security descriptors held against a registry key path rather than a file.
struct NamedRegistrySdStore {
    std::mutex mutex;
    std::map<std::string, std::vector<u8>> by_name;
};

[[nodiscard]] NamedRegistrySdStore& named_registry_sds();

/// Parse a guest ACL header and the offsets of its ACEs.
[[nodiscard]] bool parse_acl(Marshaler& m, GuestAddr acl, ParsedAcl& parsed);

/// True when the descriptor's DACL is marked present but contains no ACEs —
/// a state that denies everything and is almost never what a guest intends.
[[nodiscard]] bool sd_has_empty_present_dacl(const std::vector<u8>& descriptor);

/// Build the ACL bytes for a DACL granting full access to the synthetic SID.
[[nodiscard]] std::vector<u8> make_default_grant_dacl_bytes();

/// Insert ACEs into a guest ACL, updating its header. @p starting_index may be
/// kAppendAce.
[[nodiscard]] bool add_ace_bytes(Marshaler& m, GuestAddr acl, u32 revision,
                                 u32 starting_index, std::span<const u8> ace_list,
                                 u32& error);

/// Expand GENERIC_* bits in @p access_mask using the caller's mapping.
void apply_generic_mapping(u32& access_mask, u32 generic_read, u32 generic_write,
                           u32 generic_execute, u32 generic_all);

/// Read and write a u32 at a fixed offset in a descriptor's header.
[[nodiscard]] u32 sd_u32_at(const std::vector<u8>& descriptor, usize field);
void sd_write_u32_at(std::vector<u8>& descriptor, usize field, u32 value);

/// Give a descriptor an owner and group if it lacks them. Callers expect both
/// to be present even when the guest supplied neither.
[[nodiscard]] std::vector<u8> ensure_sd_has_owner_group(std::vector<u8> descriptor);

/// A DACL marked present but empty denies everything, which is almost never
/// what a guest means; this restores a permissive one.
[[nodiscard]] std::vector<u8> repair_empty_present_dacl(std::vector<u8> descriptor);

/// Build a descriptor whose DACL is null — meaning full access to everyone.
[[nodiscard]] std::vector<u8> make_default_null_dacl_sd(bool include_owner,
                                                        bool include_group,
                                                        bool dacl_present);

/// Whether @p path names a registry object rather than a filesystem one, and
/// the canonical form used as the store key.
[[nodiscard]] bool is_registry_object_name(const std::string& path);
[[nodiscard]] std::string normalize_registry_object_name(std::string path);

/// Resolve a TRUSTEE_IS_NAME account (CURRENT_USER, Administrators, SYSTEM, …)
/// to a well-known or synthetic SID. Names are matched case-insensitively.
[[nodiscard]] bool sid_from_account_name(std::string_view name,
                                         std::vector<u8>& sid_out);

/// Inverse of sid_from_account_name for LookupAccountSidW. Well-known SIDs
/// return the documented NT account name, domain, and SID_NAME_USE.
[[nodiscard]] bool account_from_sid(std::span<const u8> sid,
                                    std::wstring& name_out,
                                    std::wstring& domain_out,
                                    u32& use_out);

/// Register the ACL / security-descriptor primitives (acl_api.cpp). Called
/// last so these registrations are the ones the by-name table keeps.
void add_acl_api_exports(DllExports& exports);

class SecurityStubs;
/// Register AccessCheck (access_check.cpp).
void add_access_check_export(DllExports& exports, SecurityStubs& stubs);
/// Register GetSecurityInfo / SetSecurityInfo (object_security.cpp).
void add_object_security_exports(DllExports& exports, SecurityStubs& stubs);

/// Load and store a descriptor from the capsule's persisted security data.
[[nodiscard]] bool load_security_descriptor(capsule::Vfs& vfs, const std::string& path,
                                            std::vector<u8>& descriptor, bool& found,
                                            u32& error);
[[nodiscard]] bool store_security_descriptor(capsule::Vfs& vfs, const std::string& path,
                                             const std::vector<u8>& descriptor,
                                             u32& error);

} // namespace vitreo::winapi
