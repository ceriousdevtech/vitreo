/// @file urlmon.cpp
/// @brief CoInternetCreateSecurityManager and IInternetSecurityManager.

#include "urlmon.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace vitreo::winapi {
namespace {

constexpr u32 S_OK = 0x00000000u;
constexpr u32 S_FALSE = 0x00000001u;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 E_INVALIDARG = 0x80070057u;

constexpr u32 URLZONE_LOCAL_MACHINE = 0;
constexpr u32 URLZONE_INTERNET = 3;

constexpr u32 URLPOLICY_ALLOW = 0x00;
constexpr u32 URLPOLICY_DISALLOW = 0x03;

constexpr u32 URLACTION_SCRIPT_RUN = 0x1400;
constexpr u32 URLACTION_SHELL_MIN = 0x1800;
constexpr u32 URLACTION_SHELL_EXECUTE_HIGHRISK = 0x1806;
constexpr u32 URLACTION_SHELL_EXECUTE_MODRISK = 0x1807;
constexpr u32 URLACTION_SHELL_EXECUTE_LOWRISK = 0x1808;

constexpr u32 SECURITY_MANAGER_SLOTS = 11;
constexpr usize STORAGE_SIZE = 0x10000;
constexpr GuestAddr STORAGE_HINT = 0x64000000ull;

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};
static_assert(sizeof(Guid) == 16);

constexpr Guid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_IINTERNET_SECURITY_MANAGER{
    0x79EAC9EEu, 0xBAF9u, 0x11CEu,
    {0x8C, 0x82, 0x00, 0xAA, 0x00, 0x4B, 0xA9, 0x0B}};

bool guid_equal(const Guid& left, const Guid& right) {
    return std::memcmp(&left, &right, sizeof(Guid)) == 0;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(text[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool is_filesystem_path(std::string_view url) {
    if (url.size() >= 2) {
        const unsigned char drive = static_cast<unsigned char>(url[0]);
        if (std::isalpha(drive) != 0 && url[1] == ':') return true;
    }
    return !url.empty() && (url[0] == '\\' || url[0] == '/');
}

// Learn MapUrlToZone: zone index for the URL. Local file / about / res map to
// URLZONE_LOCAL_MACHINE; http(s)/ftp map to URLZONE_INTERNET.
u32 zone_for_url(std::string_view url) {
    if (starts_with_ci(url, "file:") || starts_with_ci(url, "about:") ||
        starts_with_ci(url, "res:") || is_filesystem_path(url)) {
        return URLZONE_LOCAL_MACHINE;
    }
    return URLZONE_INTERNET;
}

bool is_shell_execute_action(u32 action) {
    return action == URLACTION_SHELL_MIN ||
           action == URLACTION_SHELL_EXECUTE_HIGHRISK ||
           action == URLACTION_SHELL_EXECUTE_MODRISK ||
           action == URLACTION_SHELL_EXECUTE_LOWRISK;
}

bool iid_supported(GuestMemory& memory, GuestAddr iid_address) {
    Guid iid{};
    if (iid_address == 0 ||
        memory.read_bytes(
                  iid_address, reinterpret_cast<Byte*>(&iid), sizeof(iid))
            .is_err()) {
        return false;
    }
    return guid_equal(iid, IID_IUNKNOWN) ||
           guid_equal(iid, IID_IINTERNET_SECURITY_MANAGER);
}

}  // namespace

UrlmonManager::UrlmonManager(GuestMemory& memory) : m_memory(memory) {}

void UrlmonManager::set_method_thunks(MethodThunks thunks) {
    m_method_thunks = thunks;
    if (m_region_base != 0) write_vtables();
}

bool UrlmonManager::write_pointer(
    GuestAddr address, GuestAddr value, bool is_64bit) {
    if (is_64bit) return m_memory.write_u64(address, value).is_ok();
    return m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

void UrlmonManager::write_vtables() {
    const GuestAddr slots[SECURITY_MANAGER_SLOTS] = {
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.set_security_site,
        m_method_thunks.get_security_site,
        m_method_thunks.map_url_to_zone,
        m_method_thunks.get_security_id,
        m_method_thunks.process_url_action,
        m_method_thunks.query_custom_policy,
        m_method_thunks.set_zone_mapping,
        m_method_thunks.get_zone_mappings,
    };
    for (u32 slot = 0; slot < SECURITY_MANAGER_SLOTS; ++slot) {
        (void)write_pointer(m_vtable32 + slot * 4, slots[slot], false);
        (void)write_pointer(m_vtable64 + slot * 8, slots[slot], true);
    }
}

bool UrlmonManager::ensure_storage() {
    if (m_region_base != 0) return true;
    GuestAddr minimum = STORAGE_HINT;
    while (minimum != 0) {
        const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
        if (base == 0) return false;
        if (m_memory.map_region(
                base, STORAGE_SIZE, MemoryProtection::ReadWrite).is_ok()) {
            m_region_base = base;
            break;
        }
        minimum = base + STORAGE_SIZE;
    }
    if (m_region_base == 0) return false;
    m_vtable32 = m_region_base;
    m_vtable64 = m_region_base + 0x80;
    m_next_object = m_region_base + 0x100;
    write_vtables();
    return true;
}

GuestAddr UrlmonManager::create_security_manager(bool is_64bit) {
    if (!ensure_storage()) return 0;
    if (m_next_object + 0x20 > m_region_base + STORAGE_SIZE) return 0;
    const GuestAddr address = m_next_object;
    m_next_object += 0x20;
    const GuestAddr vtable = is_64bit ? m_vtable64 : m_vtable32;
    if (!write_pointer(address, vtable, is_64bit)) return 0;
    m_objects.emplace(address, Object{});
    return address;
}

void UrlmonManager::impl_CoInternetCreateSecurityManager(Marshaler& m) {
    (void)m.arg(0);  // pSP: Learn — not implemented / ignored.
    const GuestAddr manager_out = m.arg_ptr(1);
    (void)m.arg(2);  // dwReserved
    if (manager_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(manager_out, 0);
    const GuestAddr object = create_security_manager(m.cpu_is_64bit());
    if (object == 0 || m.write_ptr(manager_out, object).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}

void UrlmonManager::impl_QueryInterface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid = m.arg_ptr(1);
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(object_out, 0);
    auto object = m_objects.find(self);
    if (object == m_objects.end() || !iid_supported(m_memory, iid)) {
        m.set_return(E_NOINTERFACE);
        return;
    }
    ++object->second.ref_count;
    if (m.write_ptr(object_out, self).is_err()) {
        --object->second.ref_count;
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void UrlmonManager::impl_AddRef(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    m.set_return(++object->second.ref_count);
}

void UrlmonManager::impl_Release(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(0);
        return;
    }
    const u32 ref_count = --object->second.ref_count;
    if (ref_count == 0) m_objects.erase(object);
    m.set_return(ref_count);
}

void UrlmonManager::impl_SetSecuritySite(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    if (object == m_objects.end()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    object->second.security_site = m.arg_ptr(1);
    m.set_return(S_OK);
}

void UrlmonManager::impl_GetSecuritySite(Marshaler& m) {
    auto object = m_objects.find(m.arg_ptr(0));
    const GuestAddr site_out = m.arg_ptr(1);
    if (object == m_objects.end() || site_out == 0) {
        m.set_return(site_out == 0 ? E_POINTER : E_INVALIDARG);
        return;
    }
    if (m.write_ptr(site_out, object->second.security_site).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void UrlmonManager::impl_MapUrlToZone(Marshaler& m) {
    const GuestAddr url_address = m.arg_ptr(1);
    const GuestAddr zone_out = m.arg_ptr(2);
    (void)m.arg(3);
    if (url_address == 0 || zone_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto url = m.read_wide_string(url_address);
    if (url.is_err() || url.value().empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (m.write_u32(zone_out, zone_for_url(url.value())).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void UrlmonManager::impl_GetSecurityId(Marshaler& m) {
    (void)m.arg(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    (void)m.arg(4);
    m.set_return(E_NOTIMPL);
}

void UrlmonManager::impl_ProcessUrlAction(Marshaler& m) {
    const GuestAddr url_address = m.arg_ptr(1);
    const u32 action = static_cast<u32>(m.arg(2));
    const GuestAddr policy_out = m.arg_ptr(3);
    const u32 policy_bytes = static_cast<u32>(m.arg(4));
    (void)m.arg(5);
    (void)m.arg(6);
    (void)m.arg(7);
    (void)m.arg(8);
    if (policy_out == 0 || policy_bytes < 4) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (url_address == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto url = m.read_wide_string(url_address);
    if (url.is_err() || url.value().empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 zone = zone_for_url(url.value());
    const u32 policy =
        (zone == URLZONE_LOCAL_MACHINE || action == URLACTION_SCRIPT_RUN ||
         !is_shell_execute_action(action))
            ? URLPOLICY_ALLOW
            : URLPOLICY_DISALLOW;
    if (m.write_u32(policy_out, policy).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(policy == URLPOLICY_ALLOW ? S_OK : S_FALSE);
}

void UrlmonManager::impl_QueryCustomPolicy(Marshaler& m) {
    (void)m.arg(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    (void)m.arg(4);
    (void)m.arg(5);
    (void)m.arg(6);
    (void)m.arg(7);
    m.set_return(E_NOTIMPL);
}

void UrlmonManager::impl_SetZoneMapping(Marshaler& m) {
    (void)m.arg(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    m.set_return(E_NOTIMPL);
}

void UrlmonManager::impl_GetZoneMappings(Marshaler& m) {
    (void)m.arg(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    m.set_return(E_NOTIMPL);
}

DllExports create_urlmon_thunks(UrlmonManager& manager) {
    DllExports exports;
    exports.entries.push_back(
        {"CoInternetCreateSecurityManager", 0,
         [&manager](Marshaler& m) {
             manager.impl_CoInternetCreateSecurityManager(m);
         },
         12});
    // HRESULT CoInternetGetSession(DWORD dwSessionMode,
    //     IInternetSession** ppIInternetSession, DWORD dwReserved)
    // The session exists to register temporary asynchronous pluggable
    // protocol handlers and MIME filters that urlmon's URL binding then
    // consults. Vitreo's urlmon has no binding engine (no URL monikers,
    // no IInternetProtocol dispatch), so a session would accept
    // registrations nothing ever honours; the call is refused instead.
    // Measured (propsheet_theme_session_probe session_*): Windows ignores
    // dwSessionMode and dwReserved and returns one process-wide session, and
    // a NULL ppIInternetSession faults inside urlmon. Here a NULL out pointer
    // is E_POINTER, and otherwise *ppIInternetSession is set to NULL.
    exports.entries.push_back(
        {"CoInternetGetSession", 0,
         [](Marshaler& m) {
             (void)m.arg(0);
             const GuestAddr session_out = m.arg_ptr(1);
             (void)m.arg(2);
             if (session_out == 0) {
                 m.set_return(E_POINTER);
                 return;
             }
             (void)m.write_ptr(session_out, 0);
             static std::atomic_flag reported = ATOMIC_FLAG_INIT;
             if (!reported.test_and_set()) {
                 std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED urlmon!CoInternetGetSession: no "
                     "URL binding engine for pluggable protocol sessions\n");
             }
             m.set_return(E_NOTIMPL);
         },
         12});
    exports.entries.push_back(
        {"_Urlmon_QueryInterface", 0,
         [&manager](Marshaler& m) { manager.impl_QueryInterface(m); }, 12});
    exports.entries.push_back(
        {"_Urlmon_AddRef", 0,
         [&manager](Marshaler& m) { manager.impl_AddRef(m); }, 4});
    exports.entries.push_back(
        {"_Urlmon_Release", 0,
         [&manager](Marshaler& m) { manager.impl_Release(m); }, 4});
    exports.entries.push_back(
        {"_Urlmon_SetSecuritySite", 0,
         [&manager](Marshaler& m) { manager.impl_SetSecuritySite(m); }, 8});
    exports.entries.push_back(
        {"_Urlmon_GetSecuritySite", 0,
         [&manager](Marshaler& m) { manager.impl_GetSecuritySite(m); }, 8});
    exports.entries.push_back(
        {"_Urlmon_MapUrlToZone", 0,
         [&manager](Marshaler& m) { manager.impl_MapUrlToZone(m); }, 16});
    exports.entries.push_back(
        {"_Urlmon_GetSecurityId", 0,
         [&manager](Marshaler& m) { manager.impl_GetSecurityId(m); }, 20});
    exports.entries.push_back(
        {"_Urlmon_ProcessUrlAction", 0,
         [&manager](Marshaler& m) { manager.impl_ProcessUrlAction(m); }, 36});
    exports.entries.push_back(
        {"_Urlmon_QueryCustomPolicy", 0,
         [&manager](Marshaler& m) { manager.impl_QueryCustomPolicy(m); }, 32});
    exports.entries.push_back(
        {"_Urlmon_SetZoneMapping", 0,
         [&manager](Marshaler& m) { manager.impl_SetZoneMapping(m); }, 16});
    exports.entries.push_back(
        {"_Urlmon_GetZoneMappings", 0,
         [&manager](Marshaler& m) { manager.impl_GetZoneMappings(m); }, 16});
    return exports;
}

}  // namespace vitreo::winapi
