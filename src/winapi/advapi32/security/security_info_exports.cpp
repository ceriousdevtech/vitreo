// SPDX-License-Identifier: LGPL-2.1-only

/// @file security_info_exports.cpp
/// @brief advapi32 descriptor accessors and named security info:
/// GetSecurityDescriptorDacl/Owner/Control, GetNamedSecurityInfoW and
/// SetNamedSecurityInfoW, plus the owner/group reader shared with
/// GetSecurityDescriptorGroup (token_operations_exports.cpp).
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "capsule/vfs/vfs.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/advapi32/security/named_security.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstdio>
#include <optional>
#include <utility>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {

// Microsoft Learn (securitybaseapi!GetSecurityDescriptorOwner /
// GetSecurityDescriptorGroup): return a pointer into the descriptor's
// owner/group SID. Self-relative SDs store offsets at +4/+8; absolute
// SDs store pointers at +4/+8 (x86) or +8/+16 (x64). A missing SID
// writes NULL and does not touch *lpb*Defaulted.
void get_descriptor_principal(Marshaler& m, bool owner) {
    constexpr u16 SE_OWNER_DEFAULTED = 0x0001;
    constexpr u16 SE_GROUP_DEFAULTED = 0x0002;
    constexpr u16 SE_SELF_RELATIVE = 0x8000;

    const GuestAddr descriptor = m.arg_ptr(0);
    const GuestAddr p_sid = m.arg_ptr(1);
    const GuestAddr p_def = m.arg_ptr(2);
    if (descriptor == 0 || p_sid == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const auto revision = m.read_u8(descriptor);
    const auto control_result = m.read_u16(descriptor + 2);
    if (revision.is_err() || control_result.is_err() ||
        revision.value() != 1) {
        set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
        m.set_return(0);
        return;
    }

    const u16 control = control_result.value();
    GuestAddr sid = 0;
    if ((control & SE_SELF_RELATIVE) != 0) {
        const u32 field = owner ? 4u : 8u;
        const u32 offset = m.read_u32(descriptor + field).value_or(0);
        if (offset != 0) {
            sid = descriptor + offset;
        }
    } else {
        const u32 pointer_base = m.cpu_is_64bit() ? 8u : 4u;
        const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        const u32 pointer_index = owner ? 0u : 1u;
        sid = m.read_ptr(
                      descriptor + pointer_base +
                      pointer_index * pointer_size)
                  .value_or(0);
    }

    if (m.write_ptr(p_sid, sid).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    if (sid != 0 && p_def != 0) {
        const u16 bit = owner ? SE_OWNER_DEFAULTED : SE_GROUP_DEFAULTED;
        (void)m.write_u32(p_def, (control & bit) != 0 ? 1u : 0u);
    }
    m.set_return(1);
}

/// Registers GetSecurityDescriptorDacl/Owner/Control, GetNamedSecurityInfoW,
/// SetNamedSecurityInfoW.
void add_security_info_exports(DllExports& exports,
                               SecurityStubs& stubs,
                               capsule::Vfs* vfs) {
    // BOOL GetSecurityDescriptorDacl(PSD, LPBOOL, PACL*, LPBOOL)
    exports.entries.push_back({"GetSecurityDescriptorDacl", 0,
        [](Marshaler& m) {
            constexpr u16 SE_DACL_PRESENT = 0x0004;
            constexpr u16 SE_DACL_DEFAULTED = 0x0008;
            constexpr u16 SE_SELF_RELATIVE = 0x8000;

            const GuestAddr descriptor = m.arg_ptr(0);
            GuestAddr p_present = m.arg_ptr(1);
            GuestAddr p_dacl    = m.arg_ptr(2);
            GuestAddr p_def     = m.arg_ptr(3);
            if (descriptor == 0 || p_present == 0 || p_dacl == 0 ||
                p_def == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto control_result = m.read_u16(descriptor + 2);
            if (control_result.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_DATA);
                m.set_return(0);
                return;
            }
            const u16 control = control_result.value();
            const bool present = (control & SE_DACL_PRESENT) != 0;
            GuestAddr dacl = 0;
            if (present) {
                if ((control & SE_SELF_RELATIVE) != 0) {
                    auto offset = m.read_u32(descriptor + 16);
                    if (offset.is_err()) {
                        set_last_error(error_codes::ERROR_INVALID_DATA);
                        m.set_return(0);
                        return;
                    }
                    if (offset.value() != 0) {
                        dacl = descriptor + offset.value();
                    }
                } else {
                    auto pointer = m.read_ptr(
                        descriptor + (m.cpu_is_64bit() ? 32u : 16u));
                    if (pointer.is_err()) {
                        set_last_error(error_codes::ERROR_INVALID_DATA);
                        m.set_return(0);
                        return;
                    }
                    dacl = pointer.value();
                }
            }
            (void)m.write_u32(p_present, present ? 1u : 0u);
            if (present) (void)m.write_ptr(p_dacl, dacl);
            // *lpbDaclDefaulted is written either way (advapi_sdapi_probe
            // gsdd_after_false: not present, def 0x55 -> 0).
            (void)m.write_u32(p_def, (control & SE_DACL_DEFAULTED) != 0 ? 1u : 0u);
            m.set_return(1);
        }, 16});

    // BOOL GetSecurityDescriptorOwner(PSD, PSID*, LPBOOL)
    // Microsoft Learn (securitybaseapi!GetSecurityDescriptorOwner).
    exports.entries.push_back({"GetSecurityDescriptorOwner", 0,
        [](Marshaler& m) {
            get_descriptor_principal(m, true);
        }, 12});

    // BOOL GetSecurityDescriptorControl(PSD, PSECURITY_DESCRIPTOR_CONTROL, LPDWORD)
    exports.entries.push_back({"GetSecurityDescriptorControl", 0,
        [](Marshaler& m) {
            const GuestAddr descriptor = m.arg_ptr(0);
            const GuestAddr control_out = m.arg_ptr(1);
            const GuestAddr revision_out = m.arg_ptr(2);
            if (descriptor == 0 || control_out == 0 || revision_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const auto revision = m.read_u8(descriptor);
            const auto control = m.read_u16(descriptor + 2);
            if (revision.is_err() || control.is_err() ||
                revision.value() != 1) {
                set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
                m.set_return(0);
                return;
            }
            if (m.write_u16(control_out, control.value()).is_err() ||
                m.write_u32(revision_out, revision.value()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            VITREO_TRACE(
                "[advapi32] GetSecurityDescriptorControl(sd=0x%llx) "
                "-> TRUE control=0x%04x revision=%u\n",
                static_cast<unsigned long long>(descriptor),
                control.value(), revision.value());
            m.set_return(1);
        }, 12});

    // DWORD GetNamedSecurityInfoW(LPCWSTR, SE_OBJECT_TYPE, SI, PSID*, PSID*,
    //                             PACL*, PACL*, PSECURITY_DESCRIPTOR*)
    // Microsoft Learn (aclapi!GetNamedSecurityInfoW): ERROR_SUCCESS + LocalFree
    // buffer in *ppSecurityDescriptor; optional SID/ACL outs point into that SD.
    exports.entries.push_back({"GetNamedSecurityInfoW", 0,
        [&stubs, vfs](Marshaler& m) {
            constexpr u32 SE_FILE_OBJECT = 1;
            constexpr u32 SE_REGISTRY_KEY = 4;
            constexpr u32 SE_REGISTRY_WOW64_32KEY = 12;
            constexpr u32 SE_REGISTRY_WOW64_64KEY = 13;

            const GuestAddr name_ptr = m.arg_ptr(0);
            const u32 object_type = static_cast<u32>(m.arg(1));
            const u32 security_info = named_sd::expand_info(static_cast<u32>(m.arg(2)));
            const GuestAddr owner_out = m.arg_ptr(3);
            const GuestAddr group_out = m.arg_ptr(4);
            const GuestAddr dacl_out = m.arg_ptr(5);
            const GuestAddr sacl_out = m.arg_ptr(6);
            const GuestAddr descriptor_out = m.arg_ptr(7);

            if (name_ptr == 0 || (security_info & 0x0001FFFFu) == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            // Learn: ppSecurityDescriptor is required if any SID/ACL out is.
            if (descriptor_out == 0 &&
                (owner_out || group_out || dacl_out || sacl_out)) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            const bool is_registry =
                object_type == SE_REGISTRY_KEY ||
                object_type == SE_REGISTRY_WOW64_32KEY ||
                object_type == SE_REGISTRY_WOW64_64KEY;
            if (object_type != SE_FILE_OBJECT && !is_registry) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED advapi32!GetNamedSecurityInfoW: "
                        "SE_OBJECT_TYPE %u (no per-object descriptor store)\n",
                        object_type);
                }
                m.set_return(error_codes::ERROR_NOT_SUPPORTED);
                return;
            }
            auto path_result = m.read_wide_string(name_ptr);
            if (path_result.is_err()) {
                m.set_return(error_codes::ERROR_INVALID_NAME);
                return;
            }
            const std::string& path = path_result.value();
            const named_sd::Principal who = stubs.principal();
            // Reading the audit SACL needs SE_SECURITY_NAME held (aclapi
            // enables it itself when present).
            if ((security_info & named_sd::kSacl) != 0 && !who.has_security_privilege) {
                m.set_return(1314u /*ERROR_PRIVILEGE_NOT_HELD*/);
                return;
            }

            named_sd::Parts parts;
            if (is_registry) {
                if (!is_registry_object_name(path)) {
                    m.set_return(error_codes::ERROR_INVALID_NAME);
                    return;
                }
                bool found = false;
                {
                    auto& store = named_registry_sds();
                    std::lock_guard<std::mutex> lock(store.mutex);
                    auto it = store.by_name.find(normalize_registry_object_name(path));
                    if (it != store.by_name.end()) {
                        found = named_sd::parse_sd(it->second, parts);
                    }
                }
                if (!found) {
                    // Keys without an explicit descriptor: the owner, group
                    // and a NULL DACL the capsule registry grants everyone.
                    parts = named_sd::Parts{};
                    parts.owner = who.user;
                    parts.group = who.user;
                    parts.dacl_present = true;
                    parts.dacl_null = true;
                }
            } else {
                if (vfs == nullptr) {
                    m.set_return(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                    return;
                }
                if (vfs->stat(path).is_err()) {
                    m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
                    return;
                }
                if (const u32 e = named_sd::effective_file_sd(*vfs, path, who, parts);
                    e != error_codes::ERROR_SUCCESS) {
                    m.set_return(e);
                    return;
                }
            }

            // The returned copy holds exactly the requested components.
            const std::vector<u8> descriptor = named_sd::build_sd(parts, security_info);
            if (descriptor_out == 0) {
                m.set_return(error_codes::ERROR_SUCCESS);
                return;
            }
            const GuestAddr allocated =
                stubs.alloc_local_memory(static_cast<u32>(descriptor.size()));
            if (allocated == 0) {
                m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return;
            }
            if (m.memory().write_bytes(allocated, descriptor.data(),
                                       descriptor.size()).is_err()) {
                stubs.free_local_memory(allocated);
                m.set_return(error_codes::ERROR_NOACCESS);
                return;
            }
            const u32 owner_off = sd_u32_at(descriptor, 4);
            const u32 group_off = sd_u32_at(descriptor, 8);
            const u32 sacl_off = sd_u32_at(descriptor, 12);
            const u32 dacl_off = sd_u32_at(descriptor, 16);
            // Outs for components that were not requested are left untouched
            // (advapi_acl_probe get_dacl_only: o=1 g=1 s=1).
            if (owner_out && (security_info & named_sd::kOwner)) {
                (void)m.write_ptr(owner_out, owner_off ? allocated + owner_off : 0);
            }
            if (group_out && (security_info & named_sd::kGroup)) {
                (void)m.write_ptr(group_out, group_off ? allocated + group_off : 0);
            }
            if (dacl_out && (security_info & named_sd::kDacl)) {
                (void)m.write_ptr(dacl_out, dacl_off ? allocated + dacl_off : 0);
            }
            if (sacl_out && (security_info & named_sd::kSaclClasses)) {
                (void)m.write_ptr(sacl_out, sacl_off ? allocated + sacl_off : 0);
            }
            (void)m.write_ptr(descriptor_out, allocated);
            m.set_return(error_codes::ERROR_SUCCESS);
        }, 32});

    // DWORD SetNamedSecurityInfoW(LPWSTR, SE_OBJECT_TYPE,
    //     SECURITY_INFORMATION, PSID, PSID, PACL, PACL)
    // Only the components named in SecurityInfo change (advapi_acl_probe).
    exports.entries.push_back({"SetNamedSecurityInfoW", 0,
        [&stubs, vfs](Marshaler& m) {
            constexpr u32 SE_FILE_OBJECT = 1;
            constexpr u32 SE_REGISTRY_KEY = 4;
            constexpr u32 SE_REGISTRY_WOW64_32KEY = 12;
            constexpr u32 SE_REGISTRY_WOW64_64KEY = 13;

            const GuestAddr name_ptr = m.arg_ptr(0);
            const u32 object_type = static_cast<u32>(m.arg(1));
            const u32 information = named_sd::expand_info(static_cast<u32>(m.arg(2)));
            const GuestAddr owner = m.arg_ptr(3);
            const GuestAddr group = m.arg_ptr(4);
            const GuestAddr dacl = m.arg_ptr(5);
            const GuestAddr sacl = m.arg_ptr(6);

            if (name_ptr == 0 || (information & 0x0001FFFFu) == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            const bool is_registry =
                object_type == SE_REGISTRY_KEY ||
                object_type == SE_REGISTRY_WOW64_32KEY ||
                object_type == SE_REGISTRY_WOW64_64KEY;
            if (object_type != SE_FILE_OBJECT && !is_registry) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED advapi32!SetNamedSecurityInfoW: "
                        "SE_OBJECT_TYPE %u (no per-object descriptor store)\n",
                        object_type);
                }
                m.set_return(error_codes::ERROR_NOT_SUPPORTED);
                return;
            }
            auto path_result = m.read_wide_string(name_ptr);
            if (path_result.is_err()) {
                m.set_return(error_codes::ERROR_INVALID_NAME);
                return;
            }
            const std::string& path = path_result.value();

            auto read_sid = [&m](GuestAddr sid, std::vector<u8>& bytes) -> bool {
                if (sid == 0) return true;
                if (SecurityStubs::is_valid_sid(m, sid) == 0) return false;
                const usize size = 8 + static_cast<usize>(m.read_u8(sid + 1).value_or(0)) * 4;
                bytes.resize(size);
                return m.memory().read_bytes(sid, bytes.data(), size).is_ok();
            };
            auto read_acl = [&m](GuestAddr acl, bool& is_null, u8& revision,
                                 std::vector<std::vector<u8>>& aces) -> bool {
                is_null = acl == 0;
                if (acl == 0) return true;
                const u16 size = m.read_u16(acl + 2).value_or(0);
                if (size < 8) return false;
                std::vector<u8> bytes(size);
                if (m.memory().read_bytes(acl, bytes.data(), size).is_err()) return false;
                return named_sd::parse_acl(bytes, revision, aces);
            };
            named_sd::Parts in;
            if (((information & named_sd::kOwner) && !read_sid(owner, in.owner)) ||
                ((information & named_sd::kGroup) && !read_sid(group, in.group)) ||
                ((information & named_sd::kDacl) &&
                 !read_acl(dacl, in.dacl_null, in.dacl_revision, in.dacl)) ||
                ((information & named_sd::kSaclClasses) &&
                 !read_acl(sacl, in.sacl_null, in.sacl_revision, in.sacl))) {
                m.set_return(error_codes::ERROR_INVALID_DATA);
                return;
            }
            const named_sd::Principal who = stubs.principal();

            if (is_registry) {
                if (!is_registry_object_name(path)) {
                    m.set_return(error_codes::ERROR_INVALID_NAME);
                    return;
                }
                const std::string key = normalize_registry_object_name(path);
                auto& store = named_registry_sds();
                std::lock_guard<std::mutex> lock(store.mutex);
                named_sd::Parts cur;
                auto it = store.by_name.find(key);
                if (it == store.by_name.end() || !named_sd::parse_sd(it->second, cur)) {
                    cur = named_sd::Parts{};
                    cur.owner = who.user;
                    cur.group = who.user;
                    cur.dacl_present = true;
                    cur.dacl_null = true;
                }
                if (const u32 e = named_sd::merge_components(cur, information, in, who);
                    e != error_codes::ERROR_SUCCESS) {
                    m.set_return(e);
                    return;
                }
                store.by_name[key] = named_sd::build_sd(
                    cur, named_sd::kOwner | named_sd::kGroup | named_sd::kDacl |
                             named_sd::kSaclClasses);
                m.set_return(error_codes::ERROR_SUCCESS);
                return;
            }
            if (vfs == nullptr) {
                m.set_return(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                return;
            }
            if (vfs->stat(path).is_err()) {
                m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
                return;
            }
            const u32 e = named_sd::set_file_sd(*vfs, path, information, in, who);
            VITREO_TRACE("[advapi32] SetNamedSecurityInfoW(\"%s\", info=0x%x) -> %u\n",
                         path.c_str(), information, e);
            m.set_return(e);
        }, 28});
}

namespace {

/// The named-security object name ("CURRENT_USER\\SOFTWARE\\X") of the key an
/// HKEY names, or nullopt when the handle names no key.
std::optional<std::string> registry_key_object_name(const RegistryThunks& registry,
                                                    u64 hkey) {
    const auto path = registry.key_path(hkey);
    if (!path.has_value()) return std::nullopt;
    std::string name;
    for (wchar_t ch : *path) name.push_back(static_cast<char>(ch < 0x80 ? ch : '?'));
    name = normalize_registry_object_name(name);
    static constexpr std::pair<const char*, const char*> kRoots[] = {
        {"HKEY_CURRENT_USER", "CURRENT_USER"}, {"HKEY_LOCAL_MACHINE", "MACHINE"},
        {"HKEY_CLASSES_ROOT", "CLASSES_ROOT"}, {"HKEY_USERS", "USERS"}};
    for (const auto& [hive, object] : kRoots) {
        const std::string prefix = hive;
        if (name == prefix || name.rfind(prefix + "\\", 0) == 0) {
            return std::string(object) + name.substr(prefix.size());
        }
    }
    return name;
}

/// The key's descriptor: the stored one, else the default every capsule key
/// carries (the caller as owner and group, a NULL DACL) -- the same default
/// GetNamedSecurityInfoW reports.
named_sd::Parts registry_key_parts(const std::string& object, const named_sd::Principal& who) {
    named_sd::Parts parts;
    auto& store = named_registry_sds();
    std::lock_guard<std::mutex> lock(store.mutex);
    auto it = store.by_name.find(object);
    if (it != store.by_name.end() && named_sd::parse_sd(it->second, parts)) return parts;
    parts = named_sd::Parts{};
    parts.owner = who.user;
    parts.group = who.user;
    parts.dacl_present = true;
    parts.dacl_null = true;
    return parts;
}

} // namespace

void add_registry_key_security_exports(DllExports& exports, SecurityStubs& stubs,
                                       RegistryThunks& registry) {
    // Measured on Windows 11 (tools/oracle_probe/shlwapi_regsec_shell_probe.c,
    // getsec_* / setsec_*, identical on x86 and x64):
    //  - an HKEY naming no key is ERROR_INVALID_HANDLE; the predefined roots
    //    are keys like any other;
    //  - the last error is never touched (getsec_null lasterr=dead);
    //  - SACL access needs ACCESS_SYSTEM_SECURITY on the handle
    //    (ERROR_ACCESS_DENIED); the capsule registry grants no handle that
    //    right, so the SACL is always refused here;
    //  - SecurityInformation 0 succeeds and changes nothing.
    constexpr u32 kErrorAccessDenied = 5, kErrorInvalidHandle = 6,
                  kErrorInsufficientBuffer = 122, kErrorInvalidParameter = 87;

    // LSTATUS RegGetKeySecurity(HKEY, SECURITY_INFORMATION,
    //     PSECURITY_DESCRIPTOR, LPDWORD) -- stdcall 16. Returns a
    // self-relative copy holding exactly the requested components; a short
    // buffer is ERROR_INSUFFICIENT_BUFFER with the size needed.
    exports.entries.push_back({"RegGetKeySecurity", 0, [&stubs, &registry](Marshaler& m) {
        const u64 hkey = m.arg(0);
        const u32 info = named_sd::expand_info(m.arg32(1));
        const GuestAddr sd_out = m.arg_ptr(2);
        const GuestAddr size_ptr = m.arg_ptr(3);
        const auto object = registry_key_object_name(registry, hkey);
        if (!object.has_value()) {
            m.set_return(kErrorInvalidHandle);
            return;
        }
        if (size_ptr == 0) {
            m.set_return(kErrorInvalidParameter);   // getsec_nullcb
            return;
        }
        if ((info & named_sd::kSacl) != 0) {
            m.set_return(kErrorAccessDenied);       // getsec_sacl
            return;
        }
        const named_sd::Parts parts = registry_key_parts(*object, stubs.principal());
        const std::vector<u8> sd = named_sd::build_sd(parts, info);
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        if (sd_out == 0 || capacity < sd.size()) {
            (void)m.write_u32(size_ptr, static_cast<u32>(sd.size()));
            m.set_return(kErrorInsufficientBuffer); // getsec_null / getsec_small
            return;
        }
        if (m.memory().write_bytes(sd_out, sd.data(), sd.size()).is_err()) {
            m.set_return(998u);                     // ERROR_NOACCESS
            return;
        }
        (void)m.write_u32(size_ptr, static_cast<u32>(sd.size()));
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 16});

    // LSTATUS RegSetKeySecurity(HKEY, SECURITY_INFORMATION,
    //     PSECURITY_DESCRIPTOR) -- stdcall 12. The descriptor may be absolute
    // (component pointers; 20 bytes on x86, 40 on x64) or self-relative
    // (offsets). Only the components SecurityInformation names are applied;
    // DACL_SECURITY_INFORMATION with a descriptor that carries no DACL sets a
    // NULL DACL (setsec_no_dacl_in_sd / after_no_dacl).
    exports.entries.push_back({"RegSetKeySecurity", 0, [&stubs, &registry](Marshaler& m) {
        constexpr u16 kSeDaclPresent = 0x0004, kSeSaclPresent = 0x0010,
                      kSeDaclAutoInherited = 0x0400, kSeSaclAutoInherited = 0x0800,
                      kSeDaclProtected = 0x1000, kSeSaclProtected = 0x2000,
                      kSeSelfRelative = 0x8000;
        const u64 hkey = m.arg(0);
        const u32 raw_info = m.arg32(1);
        const u32 info = named_sd::expand_info(raw_info);
        const GuestAddr sd = m.arg_ptr(2);
        const auto object = registry_key_object_name(registry, hkey);
        if (!object.has_value()) {
            m.set_return(kErrorInvalidHandle);      // setsec_badkey
            return;
        }
        // RtlValidSecurityDescriptor: revision 1 and readable.
        if (sd == 0) {
            m.set_return(kErrorInvalidParameter);   // setsec_null_sd
            return;
        }
        const auto revision = m.read_u8(sd);
        const auto control_r = m.read_u16(sd + 2);
        if (revision.is_err() || control_r.is_err() || revision.value() != 1) {
            m.set_return(kErrorInvalidParameter);   // setsec_null_sd / setsec_invalid_sd
            return;
        }
        const u16 control = control_r.value();
        if ((info & (named_sd::kOwner | named_sd::kGroup | named_sd::kDacl |
                     named_sd::kSaclClasses)) == 0) {
            m.set_return(error_codes::ERROR_SUCCESS);   // setsec_zero_info
            return;
        }
        if ((info & named_sd::kSacl) != 0) {
            m.set_return(kErrorAccessDenied);
            return;
        }
        // Component addresses, whatever the layout.
        auto component = [&](u32 index) -> GuestAddr {
            if ((control & kSeSelfRelative) != 0) {
                const u32 off = m.read_u32(sd + 4 + index * 4).value_or(0);
                return off == 0 ? 0 : sd + off;
            }
            const u32 width = m.cpu_is_64bit() ? 8u : 4u;
            return m.read_ptr(sd + width + index * width).value_or(0);
        };
        const GuestAddr owner = component(0), group = component(1);
        const GuestAddr sacl = (control & kSeSaclPresent) != 0 ? component(2) : 0;
        const GuestAddr dacl = (control & kSeDaclPresent) != 0 ? component(3) : 0;
        auto read_sid = [&m](GuestAddr sid, std::vector<u8>& bytes) -> bool {
            if (sid == 0) return true;
            if (SecurityStubs::is_valid_sid(m, sid) == 0) return false;
            const usize size = 8 + static_cast<usize>(m.read_u8(sid + 1).value_or(0)) * 4;
            bytes.resize(size);
            return m.memory().read_bytes(sid, bytes.data(), size).is_ok();
        };
        auto read_acl = [&m](GuestAddr acl, bool& is_null, u8& acl_revision,
                             std::vector<std::vector<u8>>& aces) -> bool {
            is_null = acl == 0;
            if (acl == 0) return true;
            const u16 size = m.read_u16(acl + 2).value_or(0);
            if (size < 8) return false;
            std::vector<u8> bytes(size);
            if (m.memory().read_bytes(acl, bytes.data(), size).is_err()) return false;
            return named_sd::parse_acl(bytes, acl_revision, aces);
        };
        named_sd::Parts in;
        if (((info & named_sd::kOwner) && !read_sid(owner, in.owner)) ||
            ((info & named_sd::kGroup) && !read_sid(group, in.group)) ||
            ((info & named_sd::kDacl) &&
             !read_acl(dacl, in.dacl_null, in.dacl_revision, in.dacl)) ||
            ((info & named_sd::kSaclClasses) &&
             !read_acl(sacl, in.sacl_null, in.sacl_revision, in.sacl))) {
            m.set_return(kErrorInvalidParameter);
            return;
        }
        named_sd::Principal who = stubs.principal();
        // Unlike the aclapi functions, NtSetSecurityObject does not enable
        // SE_RESTORE_NAME itself: an arbitrary owner needs it enabled already
        // (setsec_foreign_owner 1307 in a session that holds it disabled).
        who.has_restore_privilege =
            stubs.privilege_enabled(static_cast<Handle>(0xFFFFFFFCu), 18u);
        auto& store = named_registry_sds();
        named_sd::Parts cur = registry_key_parts(*object, who);
        // merge_components applies the aclapi rules (auto-inherit marking,
        // PROTECTED_* selectors); this is the low-level call, whose control
        // bits are the descriptor's own.
        const u16 before = cur.control;
        if (const u32 e = named_sd::merge_components(cur, info, in, who);
            e != error_codes::ERROR_SUCCESS) {
            m.set_return(e);                        // setsec_foreign_owner 1307
            return;
        }
        if ((info & named_sd::kDacl) != 0) {
            cur.control = static_cast<u16>(
                (cur.control & ~(kSeDaclAutoInherited | kSeDaclProtected)) |
                (control & (kSeDaclAutoInherited | kSeDaclProtected)));
        }
        if ((info & named_sd::kSaclClasses) != 0) {
            cur.control = static_cast<u16>(
                (cur.control & ~(kSeSaclAutoInherited | kSeSaclProtected)) |
                (before & (kSeSaclAutoInherited | kSeSaclProtected)));
        }
        std::lock_guard<std::mutex> lock(store.mutex);
        store.by_name[*object] = named_sd::build_sd(
            cur, named_sd::kOwner | named_sd::kGroup | named_sd::kDacl |
                     named_sd::kSaclClasses);
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 12});
}

} // namespace vitreo::winapi
