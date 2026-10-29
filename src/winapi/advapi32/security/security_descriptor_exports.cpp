// SPDX-License-Identifier: LGPL-2.1-only

/// @file security_descriptor_exports.cpp
/// @brief advapi32 security-descriptor exports: SetFileSecurityA/W,
/// GetSecurityDescriptorLength, MakeSelfRelativeSD and MakeAbsoluteSD.
///
/// The two functions are separate because the event-source exports sit
/// between them in the registration order (see security_registration.cpp);
/// regrouping them would move thunk addresses.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/advapi32/security/named_security.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

/// Registers SetFileSecurityA/W.
void add_set_file_security_exports(DllExports& exports, capsule::Vfs* vfs) {
    // BOOL SetFileSecurityA/W(LPCTSTR lpFileName,
    //                         SECURITY_INFORMATION SecurityInformation,
    //                         PSECURITY_DESCRIPTOR pSecurityDescriptor)
    // Microsoft Learn (securitybaseapi!SetFileSecurityW): persist the requested
    // SD fields on the named file. Previously always returned TRUE without
    // writing capsule xattrs, so Agent's fix_file_permissions left empty
    // present-DACLs and marked builds non-playable (2113).
    auto set_file_security = [vfs](Marshaler& m, bool ansi) {
            constexpr u32 OWNER_SECURITY_INFORMATION = 0x1;
            constexpr u32 GROUP_SECURITY_INFORMATION = 0x2;
            constexpr u32 DACL_SECURITY_INFORMATION = 0x4;
            constexpr u32 SACL_SECURITY_INFORMATION = 0x8;
            constexpr u32 SE_DACL_PRESENT = 0x4;
            constexpr u32 SE_SACL_PRESENT = 0x10;
            constexpr u32 SE_SELF_RELATIVE = 0x8000;
            constexpr u32 component_information =
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION | 0x10u;

            const GuestAddr name_ptr = m.arg_ptr(0);
            const u32 information = static_cast<u32>(m.arg(1));
            const GuestAddr sd = m.arg_ptr(2);

            if (name_ptr == 0 || sd == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            // Measured (advapi_sdapi_probe sfs_*): no component at all is
            // ERROR_ACCESS_DENIED; unknown bits and the (UN)PROTECTED_* flags
            // are accepted and change nothing; LABEL_SECURITY_INFORMATION
            // replaces the mandatory label inside the SACL.
            constexpr u32 LABEL_SECURITY_INFORMATION = 0x10;
            if (information == 0) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                m.set_return(0);
                return;
            }
            const bool changes_nothing = (information & component_information) == 0;
            if (vfs == nullptr) {
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0);
                return;
            }
            auto path_result = ansi
                ? m.read_ansi_string(name_ptr)
                : m.read_wide_string(name_ptr);
            if (path_result.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_NAME);
                m.set_return(0);
                return;
            }
            const std::string& path = path_result.value();
            if (vfs->stat(path).is_err()) {
                set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                m.set_return(0);
                return;
            }

            const auto revision = m.read_u8(sd);
            const auto control_r = m.read_u16(sd + 2);
            if (revision.is_err() || control_r.is_err() ||
                revision.value() != 1) {
                set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
                m.set_return(0);
                return;
            }
            const u16 control = control_r.value();
            const bool self_relative = (control & SE_SELF_RELATIVE) != 0;
            if (changes_nothing) {   // probe sfs_bogus_info: TRUE, nothing stored
                m.set_return(1);
                return;
            }

            auto read_sid_at = [&](GuestAddr sid,
                                   std::vector<u8>& bytes) -> bool {
                if (sid == 0) return true;
                if (SecurityStubs::is_valid_sid(m, sid) == 0) return false;
                const u32 size = SecurityStubs::get_length_sid(m, sid);
                if (size < 8) return false;
                bytes.resize(size);
                return m.memory().read_bytes(sid, bytes.data(), size).is_ok();
            };
            auto read_acl_at = [&](GuestAddr acl,
                                   std::vector<u8>& bytes) -> bool {
                if (acl == 0) return true;
                ParsedAcl parsed;
                if (!parse_acl(m, acl, parsed)) return false;
                bytes.resize(parsed.size);
                return m.memory().read_bytes(acl, bytes.data(), bytes.size())
                    .is_ok();
            };

            std::vector<u8> owner_bytes;
            std::vector<u8> group_bytes;
            std::vector<u8> dacl_bytes;
            std::vector<u8> sacl_bytes;
            bool ok = true;
            if (self_relative) {
                const u32 owner_off = m.read_u32(sd + 4).value_or(0);
                const u32 group_off = m.read_u32(sd + 8).value_or(0);
                const u32 sacl_off = m.read_u32(sd + 12).value_or(0);
                const u32 dacl_off = m.read_u32(sd + 16).value_or(0);
                if ((information & OWNER_SECURITY_INFORMATION) != 0) {
                    ok = read_sid_at(owner_off ? sd + owner_off : 0,
                                     owner_bytes);
                }
                if (ok && (information & GROUP_SECURITY_INFORMATION) != 0) {
                    ok = read_sid_at(group_off ? sd + group_off : 0,
                                     group_bytes);
                }
                if (ok && (information & (SACL_SECURITY_INFORMATION |
                                          LABEL_SECURITY_INFORMATION)) != 0) {
                    ok = read_acl_at(
                        ((control & SE_SACL_PRESENT) != 0 && sacl_off != 0)
                            ? sd + sacl_off
                            : 0,
                        sacl_bytes);
                }
                if (ok && (information & DACL_SECURITY_INFORMATION) != 0) {
                    ok = read_acl_at(
                        ((control & SE_DACL_PRESENT) != 0 && dacl_off != 0)
                            ? sd + dacl_off
                            : 0,
                        dacl_bytes);
                }
            } else {
                const u32 owner_field = m.cpu_is_64bit() ? 8u : 4u;
                const u32 group_field = m.cpu_is_64bit() ? 16u : 8u;
                const u32 sacl_field = m.cpu_is_64bit() ? 24u : 12u;
                const u32 dacl_field = m.cpu_is_64bit() ? 32u : 16u;
                if ((information & OWNER_SECURITY_INFORMATION) != 0) {
                    ok = read_sid_at(m.read_ptr(sd + owner_field).value_or(0),
                                     owner_bytes);
                }
                if (ok && (information & GROUP_SECURITY_INFORMATION) != 0) {
                    ok = read_sid_at(m.read_ptr(sd + group_field).value_or(0),
                                     group_bytes);
                }
                if (ok && (information & (SACL_SECURITY_INFORMATION |
                                          LABEL_SECURITY_INFORMATION)) != 0) {
                    ok = read_acl_at(m.read_ptr(sd + sacl_field).value_or(0),
                                     sacl_bytes);
                }
                if (ok && (information & DACL_SECURITY_INFORMATION) != 0) {
                    ok = read_acl_at(m.read_ptr(sd + dacl_field).value_or(0),
                                     dacl_bytes);
                }
            }
            if (!ok) {
                set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
                m.set_return(0);
                return;
            }

            std::vector<u8> prior;
            bool found = false;
            u32 metadata_error = error_codes::ERROR_SUCCESS;
            if (!load_security_descriptor(*vfs, path, prior, found,
                                          metadata_error)) {
                set_last_error(metadata_error);
                m.set_return(0);
                return;
            }
            auto extract_sid = [](const std::vector<u8>& prev, u32 off_field,
                                  std::vector<u8>& out) {
                if (prev.size() < 20) return;
                const u32 off = sd_u32_at(prev, off_field);
                if (off == 0 || off + 2 > prev.size()) return;
                const u8 n = prev[off + 1];
                const usize sz = 8 + static_cast<usize>(n) * 4;
                if (off + sz > prev.size()) return;
                out.assign(prev.begin() + static_cast<std::ptrdiff_t>(off),
                           prev.begin() +
                               static_cast<std::ptrdiff_t>(off + sz));
            };
            auto extract_acl = [](const std::vector<u8>& prev, u32 off_field,
                                  std::vector<u8>& out) {
                if (prev.size() < 20) return;
                const u32 off = sd_u32_at(prev, off_field);
                if (off == 0 || off + 4 > prev.size()) return;
                const u16 sz = static_cast<u16>(
                    prev[off + 2] | (static_cast<u16>(prev[off + 3]) << 8));
                if (sz < 8 || off + sz > prev.size()) return;
                out.assign(prev.begin() + static_cast<std::ptrdiff_t>(off),
                           prev.begin() +
                               static_cast<std::ptrdiff_t>(off + sz));
            };
            if (found) {
                if ((information & OWNER_SECURITY_INFORMATION) == 0) {
                    extract_sid(prior, 4, owner_bytes);
                }
                if ((information & GROUP_SECURITY_INFORMATION) == 0) {
                    extract_sid(prior, 8, group_bytes);
                }
                if ((information & (SACL_SECURITY_INFORMATION |
                                    LABEL_SECURITY_INFORMATION)) == 0) {
                    extract_acl(prior, 12, sacl_bytes);
                } else if ((information & SACL_SECURITY_INFORMATION) == 0 ||
                           (information & LABEL_SECURITY_INFORMATION) == 0) {
                    // Only one SACL class is being replaced: keep the other
                    // class's ACEs from the stored SACL.
                    std::vector<u8> old_sacl;
                    extract_acl(prior, 12, old_sacl);
                    u8 rev_old = 2, rev_new = 2;
                    std::vector<std::vector<u8>> kept, fresh;
                    (void)named_sd::parse_acl(old_sacl, rev_old, kept);
                    (void)named_sd::parse_acl(sacl_bytes, rev_new, fresh);
                    const u32 replaced = information & (SACL_SECURITY_INFORMATION |
                                                        LABEL_SECURITY_INFORMATION);
                    std::vector<std::vector<u8>> merged;
                    for (const auto& a : kept) {
                        if ((named_sd::sacl_class_of(a[0]) & replaced) == 0) merged.push_back(a);
                    }
                    for (const auto& a : fresh) {
                        if ((named_sd::sacl_class_of(a[0]) & replaced) != 0) merged.push_back(a);
                    }
                    sacl_bytes = named_sd::build_acl(std::max(rev_old, rev_new), merged);
                }
                if ((information & DACL_SECURITY_INFORMATION) == 0) {
                    extract_acl(prior, 16, dacl_bytes);
                }
            }

            u16 out_control = SE_SELF_RELATIVE;
            if (!dacl_bytes.empty() ||
                (information & DACL_SECURITY_INFORMATION) != 0) {
                out_control |= SE_DACL_PRESENT;
            }
            if (!sacl_bytes.empty() ||
                (information & (SACL_SECURITY_INFORMATION |
                                LABEL_SECURITY_INFORMATION)) != 0) {
                out_control |= SE_SACL_PRESENT;
            }
            std::vector<u8> descriptor(20, 0);
            descriptor[0] = 1;
            descriptor[2] = static_cast<u8>(out_control);
            descriptor[3] = static_cast<u8>(out_control >> 8);
            auto append_component = [&descriptor](const std::vector<u8>& bytes,
                                                   usize offset_field) {
                if (bytes.empty()) return;
                while ((descriptor.size() & 3u) != 0) descriptor.push_back(0);
                const u32 offset = static_cast<u32>(descriptor.size());
                sd_write_u32_at(descriptor, offset_field, offset);
                descriptor.insert(descriptor.end(), bytes.begin(), bytes.end());
            };
            append_component(owner_bytes, 4);
            append_component(group_bytes, 8);
            append_component(sacl_bytes, 12);
            append_component(dacl_bytes, 16);
            descriptor = ensure_sd_has_owner_group(std::move(descriptor));
            // An empty DACL is kept as written: it denies everyone
            // (advapi_sdapi_probe sfs_dacl_empty: aces=0).

            u32 error = error_codes::ERROR_SUCCESS;
            if (!store_security_descriptor(*vfs, path, descriptor, error)) {
                set_last_error(error);
                m.set_return(0);
                return;
            }
            ipc::ipc_log(
                "SetFileSecurity%c path=\"%s\" info=0x%x sd=%zu -> TRUE",
                ansi ? 'A' : 'W', path.c_str(), information,
                descriptor.size());
            m.set_return(1);
        };
    exports.entries.push_back(
        {"SetFileSecurityA", 0,
         [set_file_security](Marshaler& m) { set_file_security(m, true); },
         12});
    exports.entries.push_back(
        {"SetFileSecurityW", 18,
         [set_file_security](Marshaler& m) { set_file_security(m, false); },
         12});
}

/// Registers GetSecurityDescriptorLength, MakeSelfRelativeSD,
/// MakeAbsoluteSD.
void add_sd_layout_exports(DllExports& exports) {
    // DWORD GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR)
    // Absolute descriptors reference their components through pointers, so
    // their serialized length is the fixed header plus each referenced SID or
    // ACL. Self-relative descriptors already contain those components, and
    // their length is the greatest referenced component end offset.
    exports.entries.push_back(
        {"GetSecurityDescriptorLength", 0, [](Marshaler& m) {
            constexpr u16 SE_SELF_RELATIVE = 0x8000;
            const GuestAddr descriptor = m.arg_ptr(0);
            const auto revision = m.read_u8(descriptor);
            const auto control_result = m.read_u16(descriptor + 2);
            if (descriptor == 0 || revision.is_err() ||
                control_result.is_err() || revision.value() != 1) {
                m.set_return(0);
                return;
            }

            auto sid_length = [&](GuestAddr sid) -> std::optional<u32> {
                if (sid == 0) return 0u;
                const auto count = m.read_u8(sid + 1);
                if (count.is_err()) return std::nullopt;
                return 8u + 4u * static_cast<u32>(count.value());
            };
            auto acl_length = [&](GuestAddr acl) -> std::optional<u32> {
                if (acl == 0) return 0u;
                const auto size = m.read_u16(acl + 2);
                if (size.is_err() || size.value() < 8) return std::nullopt;
                return size.value();
            };

            const u16 control = control_result.value();
            u32 length = (control & SE_SELF_RELATIVE) != 0
                             ? 20u
                             : (m.cpu_is_64bit() ? 40u : 20u);
            if ((control & SE_SELF_RELATIVE) != 0) {
                for (u32 index = 0; index < 4; ++index) {
                    const auto offset = m.read_u32(descriptor + 4 + index * 4);
                    if (offset.is_err()) {
                        m.set_return(0);
                        return;
                    }
                    if (offset.value() == 0) continue;
                    const auto component = index < 2
                        ? sid_length(descriptor + offset.value())
                        : acl_length(descriptor + offset.value());
                    if (!component.has_value()) {
                        m.set_return(0);
                        return;
                    }
                    length = std::max(length, offset.value() + *component);
                }
            } else {
                const u32 pointer_base = m.cpu_is_64bit() ? 8u : 4u;
                const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
                for (u32 index = 0; index < 4; ++index) {
                    const auto pointer =
                        m.read_ptr(descriptor + pointer_base +
                                   index * pointer_size);
                    if (pointer.is_err()) {
                        m.set_return(0);
                        return;
                    }
                    const auto component = index < 2
                        ? sid_length(pointer.value())
                        : acl_length(pointer.value());
                    if (!component.has_value()) {
                        m.set_return(0);
                        return;
                    }
                    length += *component;
                }
            }

            VITREO_TRACE(
                "[advapi32] GetSecurityDescriptorLength(sd=0x%llx "
                "self_relative=%u) -> %u\n",
                static_cast<unsigned long long>(descriptor),
                (control & SE_SELF_RELATIVE) != 0 ? 1u : 0u, length);
            m.set_return(length);
        }, 4});

    // BOOL MakeSelfRelativeSD(PSECURITY_DESCRIPTOR pAbsoluteSD,
    //                         PSECURITY_DESCRIPTOR pSelfRelativeSD,
    //                         LPDWORD lpdwBufferLength)
    exports.entries.push_back(
        {"MakeSelfRelativeSD", 0, [](Marshaler& m) {
            constexpr u16 SE_SELF_RELATIVE = 0x8000;
            const GuestAddr absolute_sd = m.arg_ptr(0);
            const GuestAddr relative_sd = m.arg_ptr(1);
            const GuestAddr size_ptr = m.arg_ptr(2);
            if (absolute_sd == 0 || size_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const auto revision = m.read_u8(absolute_sd);
            const auto sbz1 = m.read_u8(absolute_sd + 1);
            const auto control_result = m.read_u16(absolute_sd + 2);
            const auto capacity = m.read_u32(size_ptr);
            if (revision.is_err() || sbz1.is_err() ||
                control_result.is_err() || capacity.is_err() ||
                revision.value() != 1 ||
                (control_result.value() & SE_SELF_RELATIVE) != 0) {
                set_last_error(error_codes::ERROR_INVALID_DATA);
                m.set_return(0);
                return;
            }

            const u32 pointer_base = m.cpu_is_64bit() ? 8u : 4u;
            const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
            std::array<std::vector<u8>, 4> components;
            u32 required = 20;
            for (u32 index = 0; index < components.size(); ++index) {
                const auto pointer =
                    m.read_ptr(absolute_sd + pointer_base +
                               index * pointer_size);
                if (pointer.is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                if (pointer.value() == 0) continue;

                u32 size = 0;
                if (index < 2) {
                    const auto sid_revision = m.read_u8(pointer.value());
                    const auto count = m.read_u8(pointer.value() + 1);
                    if (sid_revision.is_err() || count.is_err() ||
                        sid_revision.value() != 1 || count.value() > 15) {
                        set_last_error(error_codes::ERROR_INVALID_SID);
                        m.set_return(0);
                        return;
                    }
                    size = 8u + 4u * static_cast<u32>(count.value());
                } else {
                    const auto acl_size = m.read_u16(pointer.value() + 2);
                    if (acl_size.is_err() || acl_size.value() < 8) {
                        set_last_error(error_codes::ERROR_INVALID_ACL);
                        m.set_return(0);
                        return;
                    }
                    size = acl_size.value();
                }

                components[index].resize(size);
                if (m.memory().read_bytes(pointer.value(),
                                          components[index].data(),
                                          size).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                required += size;
            }

            if (m.write_u32(size_ptr, required).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            if (relative_sd == 0 || capacity.value() < required) {
                VITREO_TRACE(
                    "[advapi32] MakeSelfRelativeSD absolute=0x%llx "
                    "relative=0x%llx size_ptr=0x%llx capacity=%u "
                    "required=%u -> FALSE ERROR_INSUFFICIENT_BUFFER\n",
                    static_cast<unsigned long long>(absolute_sd),
                    static_cast<unsigned long long>(relative_sd),
                    static_cast<unsigned long long>(size_ptr),
                    capacity.value(), required);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }

            std::vector<u8> relative(required, 0);
            relative[0] = revision.value();
            relative[1] = sbz1.value();
            const u16 relative_control =
                control_result.value() | SE_SELF_RELATIVE;
            std::memcpy(relative.data() + 2, &relative_control,
                        sizeof(relative_control));
            u32 next = 20;
            for (u32 index = 0; index < components.size(); ++index) {
                if (components[index].empty()) continue;
                std::memcpy(relative.data() + 4 + index * sizeof(u32),
                            &next, sizeof(next));
                std::memcpy(relative.data() + next,
                            components[index].data(),
                            components[index].size());
                next += static_cast<u32>(components[index].size());
            }
            if (m.memory().write_bytes(relative_sd, relative.data(),
                                       relative.size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            VITREO_TRACE(
                "[advapi32] MakeSelfRelativeSD absolute=0x%llx "
                "relative=0x%llx size_ptr=0x%llx capacity=%u size=%u -> "
                "TRUE\n",
                static_cast<unsigned long long>(absolute_sd),
                static_cast<unsigned long long>(relative_sd),
                static_cast<unsigned long long>(size_ptr), capacity.value(),
                required);
            m.set_return(1);
        }, 12});

    // BOOL MakeAbsoluteSD(PSECURITY_DESCRIPTOR pSelfRelativeSD,
    //     PSECURITY_DESCRIPTOR pAbsoluteSD, LPDWORD pAbsoluteSDSize,
    //     PACL pDacl, LPDWORD pDaclSize, PACL pSacl, LPDWORD pSaclSize,
    //     PSID pOwner, LPDWORD pOwnerSize, PSID pGroup, LPDWORD pGroupSize)
    exports.entries.push_back(
        {"MakeAbsoluteSD", 0, [](Marshaler& m) {
            constexpr u16 SE_SELF_RELATIVE = 0x8000;

            const GuestAddr relative_sd = m.arg_ptr(0);
            const GuestAddr absolute_sd = m.arg_ptr(1);
            const GuestAddr absolute_size_ptr = m.arg_ptr(2);
            const std::array<GuestAddr, 4> output_buffers = {
                m.arg_ptr(7), m.arg_ptr(9), m.arg_ptr(5), m.arg_ptr(3)
            };
            const std::array<GuestAddr, 4> output_size_ptrs = {
                m.arg_ptr(8), m.arg_ptr(10), m.arg_ptr(6), m.arg_ptr(4)
            };

            if (relative_sd == 0 || absolute_size_ptr == 0 ||
                std::ranges::any_of(output_size_ptrs,
                    [](GuestAddr ptr) { return ptr == 0; })) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const auto revision = m.read_u8(relative_sd);
            const auto sbz1 = m.read_u8(relative_sd + 1);
            const auto control_result = m.read_u16(relative_sd + 2);
            if (revision.is_err() || sbz1.is_err() ||
                control_result.is_err() || revision.value() != 1 ||
                (control_result.value() & SE_SELF_RELATIVE) == 0) {
                set_last_error(error_codes::ERROR_INVALID_DATA);
                m.set_return(0);
                return;
            }

            std::array<u32, 4> component_offsets{};
            for (u32 index = 0; index < component_offsets.size(); ++index) {
                const auto offset =
                    m.read_u32(relative_sd + 4 + index * sizeof(u32));
                if (offset.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_DATA);
                    m.set_return(0);
                    return;
                }
                component_offsets[index] = offset.value();
            }

            std::array<std::vector<u8>, 4> components;
            for (u32 index = 0; index < components.size(); ++index) {
                if (component_offsets[index] == 0) continue;
                const GuestAddr component =
                    relative_sd + component_offsets[index];
                u32 size = 0;
                if (index < 2) {
                    const auto sid_revision = m.read_u8(component);
                    const auto subauthority_count = m.read_u8(component + 1);
                    if (sid_revision.is_err() ||
                        subauthority_count.is_err() ||
                        sid_revision.value() != 1 ||
                        subauthority_count.value() > 15) {
                        set_last_error(error_codes::ERROR_INVALID_SID);
                        m.set_return(0);
                        return;
                    }
                    size = 8 + 4 * static_cast<u32>(
                        subauthority_count.value());
                } else {
                    const auto acl_size = m.read_u16(component + 2);
                    if (acl_size.is_err() || acl_size.value() < 8) {
                        set_last_error(error_codes::ERROR_INVALID_ACL);
                        m.set_return(0);
                        return;
                    }
                    size = acl_size.value();
                }

                components[index].resize(size);
                if (m.memory().read_bytes(
                        component, components[index].data(), size).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }

            const u32 absolute_size = m.cpu_is_64bit() ? 40u : 20u;
            const auto absolute_capacity =
                m.read_u32(absolute_size_ptr);
            if (absolute_capacity.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            bool insufficient =
                absolute_sd == 0 || absolute_capacity.value() < absolute_size;
            std::array<u32, 4> component_capacities{};
            for (u32 index = 0; index < components.size(); ++index) {
                const auto capacity = m.read_u32(output_size_ptrs[index]);
                if (capacity.is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                component_capacities[index] = capacity.value();
                const u32 required =
                    static_cast<u32>(components[index].size());
                if (required != 0 &&
                    (output_buffers[index] == 0 ||
                     component_capacities[index] < required)) {
                    insufficient = true;
                }
            }

            if (m.write_u32(absolute_size_ptr, absolute_size).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            for (u32 index = 0; index < components.size(); ++index) {
                if (m.write_u32(
                        output_size_ptrs[index],
                        static_cast<u32>(components[index].size())).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }

            if (insufficient) {
                VITREO_TRACE(
                    "[advapi32] MakeAbsoluteSD size query -> "
                    "sd=%u owner=%zu group=%zu sacl=%zu dacl=%zu\n",
                    absolute_size, components[0].size(),
                    components[1].size(), components[2].size(),
                    components[3].size());
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }

            for (u32 index = 0; index < components.size(); ++index) {
                if (!components[index].empty() &&
                    m.memory().write_bytes(
                        output_buffers[index], components[index].data(),
                        components[index].size()).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }

            std::vector<u8> absolute(absolute_size, 0);
            absolute[0] = revision.value();
            absolute[1] = sbz1.value();
            const u16 absolute_control =
                control_result.value() &
                static_cast<u16>(~SE_SELF_RELATIVE);
            std::memcpy(absolute.data() + 2, &absolute_control,
                        sizeof(absolute_control));
            const usize pointer_offset = m.cpu_is_64bit() ? 8u : 4u;
            const usize pointer_size = m.cpu_is_64bit() ? 8u : 4u;
            for (usize index = 0; index < output_buffers.size(); ++index) {
                const u64 pointer = components[index].empty()
                    ? 0
                    : output_buffers[index];
                std::memcpy(absolute.data() + pointer_offset +
                                index * pointer_size,
                            &pointer, pointer_size);
            }
            if (m.memory().write_bytes(
                    absolute_sd, absolute.data(), absolute.size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            VITREO_TRACE(
                "[advapi32] MakeAbsoluteSD relative=0x%llx "
                "absolute=0x%llx -> TRUE\n",
                static_cast<unsigned long long>(relative_sd),
                static_cast<unsigned long long>(absolute_sd));
            m.set_return(1);
        }, 44});
}

} // namespace vitreo::winapi
