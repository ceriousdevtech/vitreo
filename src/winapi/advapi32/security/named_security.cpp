// SPDX-License-Identifier: LGPL-2.1-only

/// @file named_security.cpp
/// @brief Security-descriptor model behind Get/SetNamedSecurityInfoW.
///
/// See named_security.hpp for the measured Windows contract. ACE layouts and
/// inheritance flags follow winnt.h (ACE_HEADER, *_INHERIT_ACE) and Learn
/// "ACE Inheritance Rules" (CREATOR OWNER/GROUP substitution, generic-right
/// mapping on the effective copy, inherit-only copy kept for containers).

#include "winapi/advapi32/security/named_security.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::named_sd {

namespace {

constexpr u8 OBJECT_INHERIT_ACE = 0x01;
constexpr u8 CONTAINER_INHERIT_ACE = 0x02;
constexpr u8 NO_PROPAGATE_INHERIT_ACE = 0x04;
constexpr u8 INHERIT_ONLY_ACE = 0x08;
constexpr u8 INHERITED_ACE = 0x10;

constexpr u8 ACCESS_ALLOWED_ACE_TYPE = 0x00;
constexpr u8 ACCESS_DENIED_ACE_TYPE = 0x01;

constexpr u32 FILE_ALL_ACCESS = 0x001F01FFu;

u32 rd32(std::span<const u8> b, usize off) {
    u32 v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
void wr32(std::vector<u8>& b, usize off, u32 v) { std::memcpy(b.data() + off, &v, 4); }
u16 rd16(std::span<const u8> b, usize off) {
    return static_cast<u16>(b[off] | (b[off + 1] << 8));
}

usize sid_size(std::span<const u8> b, usize off) {
    if (off + 8 > b.size()) return 0;
    const usize n = 8 + 4u * b[off + 1];
    return off + n <= b.size() ? n : 0;
}

/// Offset of the SID inside an ACE (object ACEs carry a Flags DWORD and
/// optional GUIDs before it).
usize ace_sid_offset(std::span<const u8> ace) {
    const u8 type = ace[0];
    const bool object_ace = type == 0x05 || type == 0x06 || type == 0x07 ||
                            type == 0x08 || type == 0x0B || type == 0x0C ||
                            type == 0x0F || type == 0x10;
    if (!object_ace) return 8;
    const u32 flags = rd32(ace, 8);
    usize off = 12;
    if (flags & 1) off += 16; // ACE_OBJECT_TYPE_PRESENT
    if (flags & 2) off += 16; // ACE_INHERITED_OBJECT_TYPE_PRESENT
    return off;
}

std::vector<u8> ace_sid(const std::vector<u8>& ace) {
    const usize off = ace_sid_offset(ace);
    const usize n = sid_size(ace, off);
    return n == 0 ? std::vector<u8>{}
                  : std::vector<u8>(ace.begin() + off, ace.begin() + off + n);
}

std::vector<u8> ace_with_sid(const std::vector<u8>& ace, const std::vector<u8>& sid) {
    const usize off = ace_sid_offset(ace);
    const usize old = sid_size(ace, off);
    std::vector<u8> out(ace.begin(), ace.begin() + off);
    out.insert(out.end(), sid.begin(), sid.end());
    out.insert(out.end(), ace.begin() + off + old, ace.end());
    const u16 size = static_cast<u16>(out.size());
    out[2] = static_cast<u8>(size);
    out[3] = static_cast<u8>(size >> 8);
    return out;
}

std::vector<u8> encode_sid(u8 authority, std::initializer_list<u32> subs) {
    std::vector<u8> s(8 + 4 * subs.size(), 0);
    s[0] = 1;
    s[1] = static_cast<u8>(subs.size());
    s[7] = authority;
    usize i = 0;
    for (u32 v : subs) {
        std::memcpy(s.data() + 8 + 4 * i, &v, 4);
        ++i;
    }
    return s;
}

std::vector<u8> make_ace(u8 type, u8 flags, u32 mask, const std::vector<u8>& sid) {
    std::vector<u8> ace(8 + sid.size(), 0);
    ace[0] = type;
    ace[1] = flags;
    const u16 size = static_cast<u16>(ace.size());
    ace[2] = static_cast<u8>(size);
    ace[3] = static_cast<u8>(size >> 8);
    wr32(ace, 4, mask);
    std::memcpy(ace.data() + 8, sid.data(), sid.size());
    return ace;
}

/// FILE_GENERIC_* mapping (winnt.h) applied to an effective inherited ACE.
u32 map_file_generic(u32 mask) {
    constexpr u32 GR = 0x80000000u, GW = 0x40000000u, GE = 0x20000000u, GA = 0x10000000u;
    u32 out = mask & ~(GR | GW | GE | GA);
    if (mask & GR) out |= 0x00120089u;
    if (mask & GW) out |= 0x00120116u;
    if (mask & GE) out |= 0x001200A0u;
    if (mask & GA) out |= FILE_ALL_ACCESS;
    return out;
}

bool mask_bearing(u8 type) {
    // Every standard ACE type stores its ACCESS_MASK at +4.
    return type <= 0x14;
}

/// Learn "ACE Inheritance Rules": the ACEs a new child inherits.
std::vector<std::vector<u8>> inherit_aces(const std::vector<std::vector<u8>>& parent,
                                          bool child_is_container,
                                          const std::vector<u8>& owner,
                                          const std::vector<u8>& group) {
    const auto creator_owner = encode_sid(3, {0});
    const auto creator_group = encode_sid(3, {1});
    std::vector<std::vector<u8>> out;
    for (const auto& ace : parent) {
        const u8 flags = ace[1];
        if ((flags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == 0) continue;
        bool effective = false;
        bool propagate = false; // container keeps an inheritable copy
        u8 keep_flags = 0;
        if (!child_is_container) {
            if ((flags & OBJECT_INHERIT_ACE) == 0) continue;
            effective = true;
        } else if (flags & CONTAINER_INHERIT_ACE) {
            effective = true;
            if ((flags & NO_PROPAGATE_INHERIT_ACE) == 0) {
                propagate = true;
                keep_flags = flags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
            }
        } else { // OBJECT_INHERIT only: an inherit-only copy for grandchildren
            if (flags & NO_PROPAGATE_INHERIT_ACE) continue;
            propagate = true;
            keep_flags = OBJECT_INHERIT_ACE;
        }

        const auto sid = ace_sid(ace);
        const bool creator = sid == creator_owner || sid == creator_group;
        const u32 mask = mask_bearing(ace[0]) ? rd32(ace, 4) : 0;
        const bool generic = mask_bearing(ace[0]) && map_file_generic(mask) != mask;

        if (effective && !creator && !generic && propagate) {
            // One ACE serves both purposes.
            auto copy = ace;
            copy[1] = static_cast<u8>(keep_flags | INHERITED_ACE);
            out.push_back(std::move(copy));
            continue;
        }
        if (effective) {
            auto eff = creator ? ace_with_sid(ace, sid == creator_owner ? owner : group) : ace;
            eff[1] = INHERITED_ACE;
            if (mask_bearing(eff[0])) wr32(eff, 4, map_file_generic(mask));
            out.push_back(std::move(eff));
        }
        if (propagate) {
            auto io = ace;
            io[1] = static_cast<u8>(keep_flags | INHERIT_ONLY_ACE | INHERITED_ACE);
            out.push_back(std::move(io));
        }
    }
    return out;
}

std::vector<std::vector<u8>> explicit_only(const std::vector<std::vector<u8>>& aces) {
    std::vector<std::vector<u8>> out;
    for (const auto& a : aces) {
        if ((a[1] & INHERITED_ACE) == 0) out.push_back(a);
    }
    return out;
}

std::string normalize(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    while (path.size() > 3 && path.back() == '\\') path.pop_back();
    return path;
}

std::string parent_of(const std::string& path) {
    const usize slash = path.find_last_of('\\');
    if (slash == std::string::npos) return {};
    if (slash + 1 == path.size()) return {}; // "C:\" is a root
    if (slash == 2 && path.size() > 1 && path[1] == ':') return path.substr(0, 3);
    return path.substr(0, slash);
}

bool is_directory(capsule::Vfs& vfs, const std::string& path) {
    auto st = vfs.stat(path);
    return st.is_ok() &&
           (st.value().attributes & capsule::FileAttributes::Directory) !=
               capsule::FileAttributes::None;
}

/// Volume root with no stored descriptor: SYSTEM, Administrators and the
/// user with full, inheritable (OI|CI) access — the three ACEs Windows 10
/// profile folders pass down (advapi_acl_probe get_fresh: 3 x FILE_ALL_ACCESS).
Parts root_parts(const Principal& who) {
    Parts p;
    p.owner = who.user;
    p.group = who.user;
    p.dacl_present = true;
    const u8 oi_ci = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    p.dacl.push_back(make_ace(ACCESS_ALLOWED_ACE_TYPE, oi_ci, FILE_ALL_ACCESS, encode_sid(5, {18})));
    p.dacl.push_back(make_ace(ACCESS_ALLOWED_ACE_TYPE, oi_ci, FILE_ALL_ACCESS, encode_sid(5, {32, 544})));
    p.dacl.push_back(make_ace(ACCESS_ALLOWED_ACE_TYPE, oi_ci, FILE_ALL_ACCESS, who.user));
    return p;
}

u32 load_parts(capsule::Vfs& vfs, const std::string& path, Parts& out, bool& found) {
    std::vector<u8> bytes;
    u32 error = error_codes::ERROR_SUCCESS;
    if (!load_security_descriptor(vfs, path, bytes, found, error)) return error;
    if (found && !parse_sd(bytes, out)) return error_codes::ERROR_INVALID_SECURITY_DESCR;
    return error_codes::ERROR_SUCCESS;
}

u32 store_parts(capsule::Vfs& vfs, const std::string& path, const Parts& parts) {
    u32 error = error_codes::ERROR_SUCCESS;
    const auto bytes = build_sd(parts, kOwner | kGroup | kDacl | kSaclClasses);
    if (!store_security_descriptor(vfs, path, bytes, error)) return error;
    return error_codes::ERROR_SUCCESS;
}

void reinherit(Parts& child, const Parts& parent, bool child_is_container) {
    if (child.dacl_present && !child.dacl_null &&
        (child.control & kSeDaclProtected) == 0) {
        auto aces = explicit_only(child.dacl);
        auto inh = inherit_aces(parent.dacl, child_is_container, child.owner, child.group);
        aces.insert(aces.end(), inh.begin(), inh.end());
        child.dacl = std::move(aces);
        child.control |= kSeDaclAutoInherited;
    }
    if ((child.control & kSeSaclProtected) == 0) {
        std::vector<std::vector<u8>> keep;
        std::vector<std::vector<u8>> parent_audit;
        for (const auto& a : child.sacl) {
            if (!(sacl_class_of(a[0]) == kSacl && (a[1] & INHERITED_ACE))) keep.push_back(a);
        }
        for (const auto& a : parent.sacl) {
            if (sacl_class_of(a[0]) == kSacl) parent_audit.push_back(a);
        }
        auto inh = inherit_aces(parent_audit, child_is_container, child.owner, child.group);
        keep.insert(keep.end(), inh.begin(), inh.end());
        if (!inh.empty()) child.sacl_present = true;
        child.sacl = std::move(keep);
    }
}

void propagate(capsule::Vfs& vfs, const std::string& dir, const Principal& who,
               usize& budget) {
    auto listing = vfs.list_dir(dir);
    if (listing.is_err()) return;
    Parts parent;
    if (effective_file_sd(vfs, dir, who, parent) != error_codes::ERROR_SUCCESS) return;
    for (const auto& entry : listing.value()) {
        if (entry.name == "." || entry.name == ".." || budget == 0) continue;
        --budget;
        const std::string child = dir.back() == '\\' ? dir + entry.name : dir + "\\" + entry.name;
        const bool child_dir = (entry.info.attributes & capsule::FileAttributes::Directory) !=
                               capsule::FileAttributes::None;
        Parts stored;
        bool found = false;
        if (load_parts(vfs, child, stored, found) == error_codes::ERROR_SUCCESS && found) {
            reinherit(stored, parent, child_dir);
            (void)store_parts(vfs, child, stored);
        }
        // Children without a stored descriptor inherit dynamically, but their
        // own descendants may have stored ones.
        if (child_dir) propagate(vfs, child, who, budget);
    }
}

} // namespace

u32 sacl_class_of(u8 ace_type) noexcept {
    switch (ace_type) {
    case 0x11: return kLabel;       // SYSTEM_MANDATORY_LABEL_ACE_TYPE
    case 0x12: return kAttribute;   // SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE
    case 0x13: return kScope;       // SYSTEM_SCOPED_POLICY_ID_ACE_TYPE
    case 0x14: return kTrustLabel;  // SYSTEM_PROCESS_TRUST_LABEL_ACE_TYPE
    default:   return kSacl;
    }
}

u32 expand_info(u32 info) noexcept {
    if (info & kBackup) {
        info |= kOwner | kGroup | kDacl | kSacl | kLabel | kAttribute | kScope;
    }
    return info;
}

bool parse_acl(std::span<const u8> acl, u8& revision,
               std::vector<std::vector<u8>>& aces) {
    if (acl.size() < 8) return false;
    revision = acl[0];
    const u16 size = rd16(acl, 2);
    const u16 count = rd16(acl, 4);
    if (size < 8 || size > acl.size()) return false;
    usize off = 8;
    aces.clear();
    for (u16 i = 0; i < count; ++i) {
        if (off + 4 > size) return false;
        const u16 ace_size = rd16(acl, off + 2);
        if (ace_size < 8 || off + ace_size > size) return false;
        aces.emplace_back(acl.begin() + off, acl.begin() + off + ace_size);
        off += ace_size;
    }
    return true;
}

std::vector<u8> build_acl(u8 revision, const std::vector<std::vector<u8>>& aces) {
    std::vector<u8> acl(8, 0);
    acl[0] = revision;
    for (const auto& a : aces) acl.insert(acl.end(), a.begin(), a.end());
    const u16 size = static_cast<u16>(acl.size());
    acl[2] = static_cast<u8>(size);
    acl[3] = static_cast<u8>(size >> 8);
    const u16 count = static_cast<u16>(aces.size());
    acl[4] = static_cast<u8>(count);
    acl[5] = static_cast<u8>(count >> 8);
    return acl;
}

bool parse_sd(std::span<const u8> sd, Parts& out) {
    if (sd.size() < 20 || sd[0] != 1) return false;
    out = Parts{};
    const u16 control = rd16(sd, 2);
    if ((control & kSeSelfRelative) == 0) return false;
    out.control = control & (kSeDaclAutoInherited | kSeSaclAutoInherited |
                             kSeDaclProtected | kSeSaclProtected);
    const u32 owner = rd32(sd, 4), group = rd32(sd, 8), sacl = rd32(sd, 12), dacl = rd32(sd, 16);
    if (owner) {
        const usize n = sid_size(sd, owner);
        if (n == 0) return false;
        out.owner.assign(sd.begin() + owner, sd.begin() + owner + n);
    }
    if (group) {
        const usize n = sid_size(sd, group);
        if (n == 0) return false;
        out.group.assign(sd.begin() + group, sd.begin() + group + n);
    }
    if (control & kSeDaclPresent) {
        out.dacl_present = true;
        if (dacl == 0) out.dacl_null = true;
        else if (dacl >= sd.size() ||
                 !parse_acl(sd.subspan(dacl), out.dacl_revision, out.dacl)) return false;
    }
    if (control & kSeSaclPresent) {
        out.sacl_present = true;
        if (sacl == 0) out.sacl_null = true;
        else if (sacl >= sd.size() ||
                 !parse_acl(sd.subspan(sacl), out.sacl_revision, out.sacl)) return false;
    }
    return true;
}

std::vector<u8> build_sd(const Parts& parts, u32 info) {
    info = expand_info(info);
    std::vector<u8> sd(20, 0);
    sd[0] = 1;
    u16 control = kSeSelfRelative;
    auto append = [&](const std::vector<u8>& bytes, usize field) {
        while (sd.size() & 3u) sd.push_back(0);
        wr32(sd, field, static_cast<u32>(sd.size()));
        sd.insert(sd.end(), bytes.begin(), bytes.end());
    };
    if ((info & kOwner) && !parts.owner.empty()) append(parts.owner, 4);
    if ((info & kGroup) && !parts.group.empty()) append(parts.group, 8);
    if ((info & kSaclClasses) && parts.sacl_present) {
        std::vector<std::vector<u8>> selected;
        for (const auto& a : parts.sacl) {
            if (info & sacl_class_of(a[0])) selected.push_back(a);
        }
        // Only report a SACL when a selected class has ACEs, or the audit
        // SACL itself was asked for (advapi_acl_probe get_sacl: a file with
        // no SACL reports none).
        if (!selected.empty() || (info & kSacl)) {
            control |= kSeSaclPresent;
            control |= parts.control & (kSeSaclAutoInherited | kSeSaclProtected);
            if (!parts.sacl_null) append(build_acl(parts.sacl_revision, selected), 12);
        }
    }
    if ((info & kDacl) && parts.dacl_present) {
        control |= kSeDaclPresent;
        control |= parts.control & (kSeDaclAutoInherited | kSeDaclProtected);
        if (!parts.dacl_null) append(build_acl(parts.dacl_revision, parts.dacl), 16);
    }
    sd[2] = static_cast<u8>(control);
    sd[3] = static_cast<u8>(control >> 8);
    return sd;
}

u32 effective_file_sd(capsule::Vfs& vfs, const std::string& raw_path,
                      const Principal& who, Parts& out) {
    const std::string path = normalize(raw_path);
    bool found = false;
    if (const u32 e = load_parts(vfs, path, out, found); e != error_codes::ERROR_SUCCESS) {
        return e;
    }
    if (found) return error_codes::ERROR_SUCCESS;
    const std::string parent = parent_of(path);
    if (parent.empty() || parent == path) {
        out = root_parts(who);
        return error_codes::ERROR_SUCCESS;
    }
    Parts p;
    if (const u32 e = effective_file_sd(vfs, parent, who, p); e != error_codes::ERROR_SUCCESS) {
        return e;
    }
    out = Parts{};
    out.owner = who.user;
    out.group = who.user;
    out.dacl_present = true;
    reinherit(out, p, is_directory(vfs, path));
    // An object that only ever inherited at creation carries the inherited
    // ACEs but not SE_DACL_AUTO_INHERITED: control 0x8004 for a fresh file
    // (advapi_acl_probe get_fresh) and a fresh directory
    // (advapi_named_null_dacl_probe fresh_dir_owner_dacl, x86 = x64).
    out.control &= static_cast<u16>(~kSeDaclAutoInherited);
    return error_codes::ERROR_SUCCESS;
}

u32 merge_components(Parts& cur, u32 info, const Parts& in, const Principal& who) {
    info = expand_info(info);
    if (info & kOwner) {
        if (in.owner.empty()) return error_codes::ERROR_INVALID_PARAMETER;
        const bool allowed =
            in.owner == who.user || who.has_restore_privilege ||
            std::find(who.owner_groups.begin(), who.owner_groups.end(), in.owner) !=
                who.owner_groups.end();
        if (!allowed) return 1307u; // ERROR_INVALID_OWNER
    }
    if ((info & kSacl) && !who.has_security_privilege) {
        return 1314u; // ERROR_PRIVILEGE_NOT_HELD
    }
    if (info & kOwner) cur.owner = in.owner;
    if (info & kGroup) cur.group = in.group;
    if (info & kDacl) {
        cur.dacl_present = true;
        cur.dacl_null = in.dacl_null;
        cur.dacl_revision = in.dacl_revision;
        cur.dacl = in.dacl;
        cur.control |= kSeDaclAutoInherited;
        // A NULL DACL is stored protected: SetNamedSecurityInfoW(DACL, NULL)
        // reads back control 0x9404 with no DACL
        // (advapi_named_null_dacl_probe after_null_named_dacl, x86 = x64).
        if ((info & kProtectedDacl) || (in.dacl_null && !(info & kUnprotectedDacl))) {
            cur.control |= kSeDaclProtected;
        } else if (info & kUnprotectedDacl) {
            cur.control &= ~kSeDaclProtected;
        }
    }
    if (info & kSaclClasses) {
        std::vector<std::vector<u8>> keep;
        for (const auto& a : cur.sacl) {
            if ((info & sacl_class_of(a[0])) == 0) keep.push_back(a);
        }
        for (const auto& a : in.sacl) {
            if (info & sacl_class_of(a[0])) keep.push_back(a);
        }
        cur.sacl = std::move(keep);
        cur.sacl_present = true;
        cur.sacl_null = (info & kSacl) ? in.sacl_null : cur.sacl_null && cur.sacl.empty();
        if (info & kSacl) {
            cur.control |= kSeSaclAutoInherited;
            if (info & kProtectedSacl) cur.control |= kSeSaclProtected;
            else if (info & kUnprotectedSacl) cur.control &= ~kSeSaclProtected;
        }
    }
    return error_codes::ERROR_SUCCESS;
}

u32 set_file_sd(capsule::Vfs& vfs, const std::string& raw_path, u32 info,
                const Parts& in, const Principal& who) {
    const std::string path = normalize(raw_path);
    info = expand_info(info);
    Parts cur;
    if (const u32 e = effective_file_sd(vfs, path, who, cur); e != error_codes::ERROR_SUCCESS) {
        return e;
    }
    if (const u32 e = merge_components(cur, info, in, who); e != error_codes::ERROR_SUCCESS) {
        return e;
    }
    const bool dir = is_directory(vfs, path);
    if (info & (kDacl | kSacl)) {
        const std::string parent = parent_of(path);
        if (!parent.empty() && parent != path) {
            Parts p;
            if (effective_file_sd(vfs, parent, who, p) == error_codes::ERROR_SUCCESS) {
                Parts before = cur;
                reinherit(cur, p, dir);
                if (!(info & kDacl)) {
                    cur.dacl = before.dacl;
                    cur.control = static_cast<u16>(
                        (cur.control & ~kSeDaclAutoInherited) |
                        (before.control & kSeDaclAutoInherited));
                }
                if (!(info & kSacl)) { cur.sacl = before.sacl; }
            }
        }
    }
    if (const u32 e = store_parts(vfs, path, cur); e != error_codes::ERROR_SUCCESS) {
        return e;
    }
    // Learn: inheritable ACEs are propagated to existing child objects.
    if (dir && (info & (kDacl | kSacl))) {
        usize budget = 200000;
        propagate(vfs, path, who, budget);
    }
    return error_codes::ERROR_SUCCESS;
}

u32 effective_rights(const std::vector<std::vector<u8>>& aces,
                     std::span<const u8> trustee,
                     const std::vector<std::vector<u8>>& aliases) {
    const auto everyone = encode_sid(1, {0});
    auto applies = [&](const std::vector<u8>& sid) {
        if (sid.size() == trustee.size() &&
            std::equal(sid.begin(), sid.end(), trustee.begin())) return true;
        if (sid == everyone) return true;
        return std::find(aliases.begin(), aliases.end(), sid) != aliases.end();
    };
    u32 allowed = 0, denied = 0;
    for (const auto& ace : aces) {
        if (ace[1] & INHERIT_ONLY_ACE) continue;
        if (ace[0] != ACCESS_ALLOWED_ACE_TYPE && ace[0] != ACCESS_DENIED_ACE_TYPE) continue;
        if (!applies(ace_sid(ace))) continue;
        const u32 mask = rd32(ace, 4);
        if (ace[0] == ACCESS_DENIED_ACE_TYPE) denied |= mask & ~allowed;
        else allowed |= mask & ~denied;
    }
    return allowed;
}

} // namespace vitreo::winapi::named_sd
