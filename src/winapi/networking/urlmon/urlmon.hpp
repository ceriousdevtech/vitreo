#pragma once

/// @file urlmon.hpp
/// @brief urlmon.dll CoInternetCreateSecurityManager + IInternetSecurityManager.
///
/// Guest exports (x86 stdcall bytes = 4 * arg count, including this for COM):
///   CoInternetCreateSecurityManager          12  (pSP, ppSM, dwReserved)
///
/// Internal COM method thunks (not IAT exports; dll_registry looks these up):
///   _Urlmon_QueryInterface                   12  (this, riid, ppv)
///   _Urlmon_AddRef                            4  (this)
///   _Urlmon_Release                           4  (this)
///   _Urlmon_SetSecuritySite                   8  (this, pSite)
///   _Urlmon_GetSecuritySite                   8  (this, ppSite)
///   _Urlmon_MapUrlToZone                     16  (this, pwszUrl, pdwZone, dwFlags)
///   _Urlmon_GetSecurityId                    20  (this, pwszUrl, pbSecurityId,
///                                                 pcbSecurityId, dwReserved)
///   _Urlmon_ProcessUrlAction                 36  (this, pwszUrl, dwAction, pPolicy,
///                                                 cbPolicy, pContext, cbContext,
///                                                 dwFlags, dwReserved)
///   _Urlmon_QueryCustomPolicy                32  (this, pwszUrl, guidKey, ppPolicy,
///                                                 pcbPolicy, pContext, cbContext,
///                                                 dwReserved)
///   _Urlmon_SetZoneMapping                   16  (this, dwZone, lpszPattern, dwFlags)
///   _Urlmon_GetZoneMappings                  16  (this, dwZone, ppenumString, dwFlags)
///
/// dll_registry wiring (do not forget UrlmonManager in the WinAPI context):
///   (void)reg.register_dll(
///       "urlmon.dll", winapi::create_urlmon_thunks(ctx.urlmon_mgr));
///   winapi::UrlmonManager::MethodThunks methods{};
///   methods.query_interface =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_QueryInterface");
///   methods.add_ref =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_AddRef");
///   methods.release =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_Release");
///   methods.set_security_site =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_SetSecuritySite");
///   methods.get_security_site =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_GetSecuritySite");
///   methods.map_url_to_zone =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_MapUrlToZone");
///   methods.get_security_id =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_GetSecurityId");
///   methods.process_url_action =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_ProcessUrlAction");
///   methods.query_custom_policy =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_QueryCustomPolicy");
///   methods.set_zone_mapping =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_SetZoneMapping");
///   methods.get_zone_mappings =
///       reg.get_thunk_address("urlmon.dll", "_Urlmon_GetZoneMappings");
///   ctx.urlmon_mgr.set_method_thunks(methods);

#include "winapi/core/thunk_registry.hpp"

#include <unordered_map>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class Marshaler;

class UrlmonManager {
public:
    struct MethodThunks {
        GuestAddr query_interface{0};
        GuestAddr add_ref{0};
        GuestAddr release{0};
        GuestAddr set_security_site{0};
        GuestAddr get_security_site{0};
        GuestAddr map_url_to_zone{0};
        GuestAddr get_security_id{0};
        GuestAddr process_url_action{0};
        GuestAddr query_custom_policy{0};
        GuestAddr set_zone_mapping{0};
        GuestAddr get_zone_mappings{0};
    };

    explicit UrlmonManager(GuestMemory& memory);

    void set_method_thunks(MethodThunks thunks);

    void impl_CoInternetCreateSecurityManager(Marshaler& m);
    void impl_QueryInterface(Marshaler& m);
    void impl_AddRef(Marshaler& m);
    void impl_Release(Marshaler& m);
    void impl_SetSecuritySite(Marshaler& m);
    void impl_GetSecuritySite(Marshaler& m);
    void impl_MapUrlToZone(Marshaler& m);
    void impl_GetSecurityId(Marshaler& m);
    void impl_ProcessUrlAction(Marshaler& m);
    void impl_QueryCustomPolicy(Marshaler& m);
    void impl_SetZoneMapping(Marshaler& m);
    void impl_GetZoneMappings(Marshaler& m);

private:
    struct Object {
        u32 ref_count{1};
        GuestAddr security_site{0};
    };

    [[nodiscard]] bool ensure_storage();
    void write_vtables();
    [[nodiscard]] bool write_pointer(
        GuestAddr address, GuestAddr value, bool is_64bit);
    [[nodiscard]] GuestAddr create_security_manager(bool is_64bit);

    GuestMemory& m_memory;
    MethodThunks m_method_thunks{};
    GuestAddr m_region_base{0};
    GuestAddr m_vtable32{0};
    GuestAddr m_vtable64{0};
    GuestAddr m_next_object{0};
    std::unordered_map<GuestAddr, Object> m_objects;
};

[[nodiscard]] DllExports create_urlmon_thunks(UrlmonManager& manager);

}  // namespace vitreo::winapi
