// SPDX-License-Identifier: LGPL-2.1-only

/// @file object_security.cpp
/// @brief advapi32!GetSecurityInfo / SetSecurityInfo on object handles.
///
/// Contract: Learn aclapi GetSecurityInfo / SetSecurityInfo, and what
/// tools/oracle_probe/advapi_sdapi_probe.c measures (gsi_* / ssi_* lines):
///  - handle 0 -> ERROR_INVALID_HANDLE (last error untouched); any other
///    handle that names no object -> ERROR_INVALID_HANDLE (also the last error);
///  - an unnamed synchronisation object has no security at all: Get returns
///    an empty descriptor, Set fails with ERROR_NO_SECURITY_ON_OBJECT;
///  - SACL access through a handle needs ACCESS_SYSTEM_SECURITY on that
///    handle (ERROR_ACCESS_DENIED); the mandatory label does not;
///  - Get writes only the requested out-parameters and leaves
///    ERROR_INSUFFICIENT_BUFFER as the last error; Set leaves it untouched;
///  - the descriptor belongs to the object (a duplicated handle sees it), an
///    empty DACL stays empty (deny all), PROTECTED_DACL_SECURITY_INFORMATION
///    sets SE_DACL_PROTECTED and it persists, generic rights in an event's
///    DACL are mapped through the event type's GENERIC_MAPPING.

#include "winapi/advapi32/security/named_security.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kErrorNoSecurityOnObject = 1350;   // ERROR_NO_SECURITY_ON_OBJECT
constexpr u32 kAccessSystemSecurity = 0x01000000u;

/// Descriptors keyed by the object a handle names (not the handle value),
/// with entries dropped once no handle to the object remains.
struct ObjectSecurityStore {
    using Key = std::tuple<const HandleTable*, const void*, HandleType>;
    struct Entry {
        named_sd::Parts parts;
        u64 identity{0};   ///< object_identity() when stored
    };
    std::mutex mutex;
    std::map<Key, Entry> by_object;
};

ObjectSecurityStore& store() {
    static auto* s = new ObjectSecurityStore();
    return *s;
}

void sweep_locked(ObjectSecurityStore& s, const HandleTable& handles) {
    for (auto it = s.by_object.begin(); it != s.by_object.end();) {
        // HandleType::Invalid keys name objects outside the handle table
        // (pseudo handles, window objects, predefined registry keys).
        const auto& [table, object, type] = it->first;
        if (table == &handles && type != HandleType::Invalid &&
            handles.count_object(const_cast<void*>(object), type) == 0) {
            it = s.by_object.erase(it);
        } else {
            ++it;
        }
    }
}

constexpr u32 kSeRegistryKey = 4, kSeKernelObject = 6, kSeWindowObject = 7,
              kSeRegistryWow6432Key = 12, kSeRegistryWow6464Key = 13;

/// The object a handle names, as the store's key. Handle-table objects key
/// on the object; objects that live outside the table key on the handle
/// value and the SE_OBJECT_TYPE:
///  - GetCurrentProcess() / GetCurrentThread() pseudo handles (the thread
///    one per calling thread) for SE_KERNEL_OBJECT;
///  - SE_WINDOW_OBJECT handles (HWINSTA / HDESK / HWND belong to user32,
///    whose tables advapi32 cannot see: accepted unvalidated);
///  - the predefined HKEY_* roots for the registry object types.
/// False: the handle names no object (ERROR_INVALID_HANDLE).
[[nodiscard]] bool resolve_object(const HandleTable& handles, Handle handle, u32 object_type,
                                  ObjectSecurityStore::Key& key, HandleType& type) {
    type = handles.get_type(handle);
    if (type != HandleType::Invalid) {
        key = {&handles, handles.get(handle), type};
        return true;
    }
    const bool registry = object_type == kSeRegistryKey || object_type == kSeRegistryWow6432Key ||
                          object_type == kSeRegistryWow6464Key;
    u64 id = 0;
    if (object_type == kSeKernelObject && handle == 0xFFFFFFFFu) {
        id = 0xFFFFFFFFu;
        type = HandleType::Process;
    } else if (object_type == kSeKernelObject && handle == 0xFFFFFFFEu) {
        id = (u64{0xFFFFFFFEu} << 32) ^ std::hash<std::thread::id>{}(std::this_thread::get_id());
        type = HandleType::Thread;
    } else if (object_type == kSeWindowObject) {
        id = handle;
    } else if (registry && handle >= 0x80000000u && handle <= 0x80000007u) {
        id = handle;
        type = HandleType::Registry;
    } else {
        return false;
    }
    key = {&handles, reinterpret_cast<const void*>(static_cast<uintptr_t>(id ^ (u64{object_type} << 56))),
           HandleType::Invalid};
    return true;
}

/// The security a kernel32 synchronisation object captured at creation
/// (owner + DACL; what OpenEvent/OpenMutex/OpenSemaphore check), or null.
[[nodiscard]] object_access::Descriptor* sync_descriptor(const HandleTable& handles, Handle h,
                                                          HandleType type) {
    switch (type) {
        case HandleType::Event:
            if (auto* e = handles.get_as<SyncEvent>(h, type)) return &e->security;
            return nullptr;
        case HandleType::Mutex:
            if (auto* x = handles.get_as<SyncMutex>(h, type)) return &x->security;
            return nullptr;
        default:   // semaphores capture no security in kernel32
            return nullptr;
    }
}

/// What distinguishes a kernel32 synchronisation object from a later one
/// allocated at the same address (its ntsync object id / shared block), so a
/// stored descriptor never outlives the object it was set on.
[[nodiscard]] u64 object_identity(const HandleTable& handles, Handle h, HandleType type) {
    switch (type) {
        case HandleType::Event:
            if (auto* e = handles.get_as<SyncEvent>(h, type)) {
                return e->nt_id ^ (static_cast<u64>(reinterpret_cast<uintptr_t>(e->shared.get())) * 31u);
            }
            return 0;
        case HandleType::Mutex:
            if (auto* x = handles.get_as<SyncMutex>(h, type)) {
                return x->nt_id ^ (static_cast<u64>(reinterpret_cast<uintptr_t>(x->shared.get())) * 31u);
            }
            return 0;
        case HandleType::Semaphore:
            if (auto* x = handles.get_as<SyncSemaphore>(h, type)) {
                return x->nt_id ^ (static_cast<u64>(reinterpret_cast<uintptr_t>(x->shared.get())) * 31u);
            }
            return 0;
        default:
            return 0;
    }
}

/// Owner and DACL from an object's captured descriptor, when it has one
/// (an owner is recorded whenever kernel32 captured security at creation).
void overlay_captured(named_sd::Parts& parts, const object_access::Descriptor* captured) {
    if (captured == nullptr || captured->owner.empty()) return;
    parts.owner = captured->owner;
    parts.dacl_present = true;
    parts.dacl_null = !captured->dacl.has_value();
    parts.dacl.clear();
    if (captured->dacl) {
        u8 revision = 2;
        if (named_sd::parse_acl(*captured->dacl, revision, parts.dacl)) parts.dacl_revision = revision;
    }
}

/// True for a synchronisation object created without a name.
[[nodiscard]] bool unnamed_sync_object(const HandleTable& handles, Handle h, HandleType type) {
    switch (type) {
        case HandleType::Event:
            if (const auto* e = handles.get_as<SyncEvent>(h, type)) return e->name.empty();
            return false;
        case HandleType::Mutex:
            if (const auto* x = handles.get_as<SyncMutex>(h, type)) return x->name.empty();
            return false;
        case HandleType::Semaphore:
            if (const auto* x = handles.get_as<SyncSemaphore>(h, type)) return x->name.empty();
            return false;
        default:
            return false;
    }
}

/// The descriptor an object has before anyone sets one: owned by the
/// caller's user and primary group, with the capsule's default grant DACL.
[[nodiscard]] named_sd::Parts default_parts() {
    named_sd::Parts p;
    p.owner = SecurityStubs::synthetic_sid();
    p.group = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 0x02, 0, 0};   // BUILTIN\Users
    p.dacl_present = true;
    u8 revision = 2;
    (void)named_sd::parse_acl(make_default_grant_dacl_bytes(), revision, p.dacl);
    p.dacl_revision = revision;
    return p;
}

/// Map GENERIC_* in each ACE through the object type's GENERIC_MAPPING.
/// Measured for events only (probe gsi_after_set: GA -> 0x1F0003).
void map_generic(std::vector<std::vector<u8>>& aces, HandleType type) {
    if (type != HandleType::Event) return;
    constexpr u32 kRead = 0x00020001u, kWrite = 0x00020002u, kExecute = 0x00120000u,
                  kAll = 0x001F0003u;
    for (auto& ace : aces) {
        if (ace.size() < 8) continue;
        u32 mask = 0;
        std::memcpy(&mask, ace.data() + 4, 4);
        u32 mapped = mask & 0x0FFFFFFFu;
        if (mask & 0x80000000u) mapped |= kRead;
        if (mask & 0x40000000u) mapped |= kWrite;
        if (mask & 0x20000000u) mapped |= kExecute;
        if (mask & 0x10000000u) mapped |= kAll;
        std::memcpy(ace.data() + 4, &mapped, 4);
    }
}

/// A handle's granted access, with the table's "everything" standing for the
/// object's all-access mask (which never includes ACCESS_SYSTEM_SECURITY).
/// Objects outside the table (pseudo handles: PROCESS/THREAD_ALL_ACCESS)
/// get the same all-access mask.
[[nodiscard]] u32 handle_access(const HandleTable& handles, Handle h) {
    if (handles.get_type(h) == HandleType::Invalid) return 0x00FFFFFFu;
    const u32 access = handles.get_access(h);
    return access == HandleAccess::All ? 0x00FFFFFFu : access;
}

} // namespace

void add_object_security_exports(DllExports& exports, SecurityStubs& stubs) {
    // DWORD GetSecurityInfo(HANDLE, SE_OBJECT_TYPE, SECURITY_INFORMATION,
    //   PSID*, PSID*, PACL*, PACL*, PSECURITY_DESCRIPTOR*) — stdcall 32.
    exports.entries.push_back({"GetSecurityInfo", 0, [&stubs](Marshaler& m) {
        const Handle handle = static_cast<Handle>(m.arg(0));
        const u32 info = m.arg32(2);
        const GuestAddr owner_out = m.arg_ptr(3);
        const GuestAddr group_out = m.arg_ptr(4);
        const GuestAddr dacl_out = m.arg_ptr(5);
        const GuestAddr sacl_out = m.arg_ptr(6);
        const GuestAddr sd_out = m.arg_ptr(7);
        HandleTable& handles = stubs.handle_table();
        if (handle == 0) {
            m.set_return(error_codes::ERROR_INVALID_HANDLE);
            return;
        }
        ObjectSecurityStore::Key key;
        HandleType type = HandleType::Invalid;
        if (!resolve_object(handles, handle, m.arg32(1), key, type)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(error_codes::ERROR_INVALID_HANDLE);
            return;
        }
        if ((info & named_sd::kSacl) != 0 &&
            (handle_access(handles, handle) & kAccessSystemSecurity) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(error_codes::ERROR_ACCESS_DENIED);
            return;
        }
        named_sd::Parts parts;
        if (!unnamed_sync_object(handles, handle, type)) {
            ObjectSecurityStore& s = store();
            std::lock_guard lock(s.mutex);
            sweep_locked(s, handles);
            auto it = s.by_object.find(key);
            if (it != s.by_object.end() && it->second.identity != object_identity(handles, handle, type)) {
                s.by_object.erase(it);
                it = s.by_object.end();
            }
            parts = it != s.by_object.end() ? it->second.parts : default_parts();
            overlay_captured(parts, sync_descriptor(handles, handle, type));
        }
        const std::vector<u8> sd = named_sd::build_sd(parts, info);
        const GuestAddr block = stubs.alloc_local_memory(static_cast<u32>(sd.size()));
        if (block == 0) {
            m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        (void)m.memory().write_bytes(block, sd.data(), sd.size());
        auto component = [&](usize field) -> GuestAddr {
            u32 off = 0;
            std::memcpy(&off, sd.data() + field, 4);
            return off == 0 ? 0 : block + off;
        };
        // Only the requested components are written (probe gsi_event_dacl:
        // owner/group/SACL out-parameters keep their values).
        if (owner_out != 0 && (info & named_sd::kOwner) != 0) (void)m.write_ptr(owner_out, component(4));
        if (group_out != 0 && (info & named_sd::kGroup) != 0) (void)m.write_ptr(group_out, component(8));
        if (dacl_out != 0 && (info & named_sd::kDacl) != 0) (void)m.write_ptr(dacl_out, component(16));
        if (sacl_out != 0 && (info & named_sd::kSaclClasses) != 0) {
            (void)m.write_ptr(sacl_out, component(12));
        }
        if (sd_out != 0) (void)m.write_ptr(sd_out, block);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 32});

    // DWORD SetSecurityInfo(HANDLE, SE_OBJECT_TYPE, SECURITY_INFORMATION,
    //   PSID, PSID, PACL, PACL) — stdcall 28.
    exports.entries.push_back({"SetSecurityInfo", 0, [&stubs](Marshaler& m) {
        constexpr u32 kWriteDac = 0x00040000u, kWriteOwner = 0x00080000u;
        const Handle handle = static_cast<Handle>(m.arg(0));
        const u32 info = m.arg32(2);
        const GuestAddr owner = m.arg_ptr(3);
        const GuestAddr group = m.arg_ptr(4);
        const GuestAddr dacl = m.arg_ptr(5);
        const GuestAddr sacl = m.arg_ptr(6);
        HandleTable& handles = stubs.handle_table();
        auto fail = [&m](u32 err) {
            set_last_error(err);
            m.set_return(err);
        };
        if (handle == 0) {
            m.set_return(error_codes::ERROR_INVALID_HANDLE);
            return;
        }
        ObjectSecurityStore::Key key;
        HandleType type = HandleType::Invalid;
        if (!resolve_object(handles, handle, m.arg32(1), key, type)) {
            return fail(error_codes::ERROR_INVALID_HANDLE);
        }
        const u32 access = handle_access(handles, handle);
        if ((info & named_sd::kSacl) != 0 && (access & kAccessSystemSecurity) == 0) {
            return fail(error_codes::ERROR_ACCESS_DENIED);
        }
        if ((info & named_sd::kDacl) != 0 && (access & kWriteDac) == 0) {
            return fail(error_codes::ERROR_ACCESS_DENIED);
        }
        if ((info & (named_sd::kOwner | named_sd::kLabel)) != 0 && (access & kWriteOwner) == 0) {
            return fail(error_codes::ERROR_ACCESS_DENIED);
        }
        if ((info & (named_sd::kOwner | named_sd::kGroup | named_sd::kDacl |
                     named_sd::kSaclClasses)) == 0) {
            m.set_return(error_codes::ERROR_SUCCESS);   // probe ssi_event_zero_info
            return;
        }
        if (unnamed_sync_object(handles, handle, type)) return fail(kErrorNoSecurityOnObject);

        auto read_sid = [&m](GuestAddr at, std::vector<u8>& out) -> bool {
            if (at == 0 || SecurityStubs::is_valid_sid(m, at) == 0) return false;
            out.resize(SecurityStubs::get_length_sid(m, at));
            return m.memory().read_bytes(at, out.data(), out.size()).is_ok();
        };
        auto read_acl = [&m](GuestAddr at, u8& revision, std::vector<std::vector<u8>>& aces) -> bool {
            const auto size = m.read_u16(at + 2);
            if (size.is_err() || size.value() < 8) return false;
            std::vector<u8> bytes(size.value());
            if (m.memory().read_bytes(at, bytes.data(), bytes.size()).is_err()) return false;
            return named_sd::parse_acl(bytes, revision, aces);
        };
        named_sd::Parts in;
        if ((info & named_sd::kOwner) != 0 && !read_sid(owner, in.owner)) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        if ((info & named_sd::kGroup) != 0 && !read_sid(group, in.group)) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        if ((info & named_sd::kDacl) != 0) {
            in.dacl_present = true;
            in.dacl_null = dacl == 0;
            if (dacl != 0 && !read_acl(dacl, in.dacl_revision, in.dacl)) {
                return fail(error_codes::ERROR_INVALID_ACL);
            }
            map_generic(in.dacl, type);
        }
        if ((info & named_sd::kSaclClasses) != 0) {
            in.sacl_present = true;
            in.sacl_null = sacl == 0;
            if (sacl != 0 && !read_acl(sacl, in.sacl_revision, in.sacl)) {
                return fail(error_codes::ERROR_INVALID_ACL);
            }
        }
        if ((info & named_sd::kOwner) != 0) {
            const named_sd::Principal who = stubs.principal();
            const bool allowed =
                in.owner == who.user || who.has_restore_privilege ||
                std::find(who.owner_groups.begin(), who.owner_groups.end(), in.owner) !=
                    who.owner_groups.end();
            if (!allowed) return fail(error_codes::ERROR_INVALID_OWNER);
        }

        ObjectSecurityStore& s = store();
        std::lock_guard lock(s.mutex);
        sweep_locked(s, handles);
        const u64 identity = object_identity(handles, handle, type);
        auto it = s.by_object.find(key);
        if (it != s.by_object.end() && it->second.identity != identity) {
            s.by_object.erase(it);
            it = s.by_object.end();
        }
        named_sd::Parts cur = it != s.by_object.end() ? it->second.parts : default_parts();
        object_access::Descriptor* captured = sync_descriptor(handles, handle, type);
        overlay_captured(cur, captured);
        if ((info & named_sd::kOwner) != 0) cur.owner = in.owner;
        if ((info & named_sd::kGroup) != 0) cur.group = in.group;
        if ((info & named_sd::kDacl) != 0) {
            cur.dacl_present = true;
            cur.dacl_null = in.dacl_null;
            cur.dacl_revision = in.dacl_revision;
            cur.dacl = in.dacl;
            if ((info & named_sd::kProtectedDacl) != 0) cur.control |= named_sd::kSeDaclProtected;
            else if ((info & named_sd::kUnprotectedDacl) != 0) {
                cur.control = static_cast<u16>(cur.control & ~named_sd::kSeDaclProtected);
            }
        }
        if ((info & named_sd::kSaclClasses) != 0) {
            std::vector<std::vector<u8>> keep;
            for (const auto& a : cur.sacl) {
                if ((info & named_sd::sacl_class_of(a[0])) == 0) keep.push_back(a);
            }
            for (const auto& a : in.sacl) {
                if ((info & named_sd::sacl_class_of(a[0])) != 0) keep.push_back(a);
            }
            cur.sacl = std::move(keep);
            cur.sacl_present = true;
            if ((info & named_sd::kSacl) != 0) {
                cur.sacl_null = in.sacl_null;
                if ((info & named_sd::kProtectedSacl) != 0) cur.control |= named_sd::kSeSaclProtected;
                else if ((info & named_sd::kUnprotectedSacl) != 0) {
                    cur.control = static_cast<u16>(cur.control & ~named_sd::kSeSaclProtected);
                }
            }
        }
        // The new owner / DACL is what later opens of the object are checked
        // against (kernel32 authorize_object), not just what Get reports.
        if (captured != nullptr && (info & (named_sd::kOwner | named_sd::kDacl)) != 0) {
            captured->owner = cur.owner;
            if (cur.dacl_null) captured->dacl.reset();
            else captured->dacl = named_sd::build_acl(cur.dacl_revision, cur.dacl);
        }
        s.by_object[key] = ObjectSecurityStore::Entry{std::move(cur), identity};
        m.set_return(error_codes::ERROR_SUCCESS);
    }, 28});
}

} // namespace vitreo::winapi
