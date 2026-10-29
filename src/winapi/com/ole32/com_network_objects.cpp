// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_network_objects.cpp
/// @brief INetworkConnection and INetwork objects handed out by the network list
/// manager enumerators.

#include "com_core.hpp"
#include "winapi/networking/iphlpapi/interface_identity.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

namespace vitreo::winapi {

namespace {

constexpr u32 NLM_DOMAIN_TYPE_NON_DOMAIN_NETWORK = 0;
constexpr u32 NLM_NETWORK_CATEGORY_PRIVATE = 1;
constexpr Guid IID_INETWORK{
    0xDCB00002u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr Guid IID_INETWORKCONNECTION{
    0xDCB00005u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
// Adapter ID when the host has no default route (no IP Helper adapter carries
// the connection); otherwise the default-route adapter's GUID is reported.
constexpr Guid kHostAdapterId{
    0x4E885C4Au, 0x7B1Au, 0x4C1Eu, {0x9A, 0x3D, 0x6F, 0x2B, 0x8C, 0x0E, 0x1A, 0x10}};
constexpr Guid kHostConnectionId{
    0x7C3A1E20u, 0x5B44u, 0x4F18u, {0xA1, 0xD2, 0x9E, 0x6B, 0x0C, 0x4F, 0x8A, 0x11}};
constexpr Guid kHostNetworkId{
    0x91D4B6E2u, 0x0C57u, 0x4A83u, {0xB8, 0x11, 0x2F, 0x6E, 0x90, 0xC3, 0x55, 0x7D}};
constexpr u32 kHostConnectionCount = 1;

} // namespace

u32 ComCoreManager::network_connection_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_connection_refs.find(object);
    if (found == m_nlm_connection_refs.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IDISPATCH) &&
         !guid_equal(iid.value(), IID_INETWORKCONNECTION))) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++found->second;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --found->second;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_connection_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_connection_refs.find(m.arg_ptr(0));
    if (found == m_nlm_connection_refs.end()) return 0;
    return ++found->second;
}

u32 ComCoreManager::network_connection_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_connection_refs.find(m.arg_ptr(0));
    if (found == m_nlm_connection_refs.end()) return 0;
    const u32 refs = --found->second;
    if (refs == 0) m_nlm_connection_refs.erase(found);
    return refs;
}

u32 ComCoreManager::network_connection_get_network(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(object)) return E_NOINTERFACE;
    static const char* names[] = {
        "Vitreo_Network_QueryInterface",
        "Vitreo_Network_AddRef",
        "Vitreo_Network_Release",
        "Vitreo_NetworkListManager_GetTypeInfoCount",
        "Vitreo_NetworkListManager_GetTypeInfo",
        "Vitreo_NetworkListManager_GetIDsOfNames",
        "Vitreo_NetworkListManager_Invoke",
        "Vitreo_Network_GetName",
        "Vitreo_Network_SetName",
        "Vitreo_Network_GetDescription",
        "Vitreo_Network_SetDescription",
        "Vitreo_Network_GetNetworkId",
        "Vitreo_Network_GetDomainType",
        "Vitreo_Network_GetNetworkConnections",
        "Vitreo_Network_GetTimeCreatedAndConnected",
        "Vitreo_Network_get_IsConnectedToInternet",
        "Vitreo_Network_get_IsConnected",
        "Vitreo_Network_GetConnectivity",
        "Vitreo_Network_GetCategory",
        "Vitreo_Network_SetCategory",
    };
    const GuestAddr network = create_network_object(m, names, std::size(names));
    if (network == 0) return com::E_NOTIMPL;
    m_nlm_network_refs[network] = 1;
    if (m.write_ptr(out, network).is_err()) {
        m_nlm_network_refs.erase(network);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_connection_get_is_connected_to_internet(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u16(out, VARIANT_TRUE_VALUE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_connection_get_is_connected(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u16(out, VARIANT_TRUE_VALUE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_connection_get_connectivity(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u32(out, NLM_CONNECTIVITY_CONNECTED).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_connection_get_connection_id(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_struct(out, kHostConnectionId).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_connection_get_adapter_id(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    // The connection's adapter is the one carrying the default route; its ID
    // is the same GUID IP Helper reports as IP_ADAPTER_ADDRESSES.AdapterName
    // and ConvertInterfaceLuidToGuid (winapi/networking/iphlpapi).
    if (const auto name = iphlp::default_route_interface()) {
        const auto guid = iphlp::interface_guid_for_name(*name);
        if (m.memory().write_bytes(out, guid.data(), guid.size()).is_err()) return com::E_INVALIDARG;
        return com::S_OK;
    }
    if (m.write_struct(out, kHostAdapterId).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_connection_get_domain_type(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_connection_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u32(out, NLM_DOMAIN_TYPE_NON_DOMAIN_NETWORK).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_network_refs.find(object);
    if (found == m_nlm_network_refs.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IDISPATCH) &&
         !guid_equal(iid.value(), IID_INETWORK))) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++found->second;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --found->second;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_network_refs.find(m.arg_ptr(0));
    if (found == m_nlm_network_refs.end()) return 0;
    return ++found->second;
}

u32 ComCoreManager::network_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_network_refs.find(m.arg_ptr(0));
    if (found == m_nlm_network_refs.end()) return 0;
    const u32 refs = --found->second;
    if (refs == 0) m_nlm_network_refs.erase(found);
    return refs;
}

u32 ComCoreManager::network_get_is_connected_to_internet(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_network_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u16(out, VARIANT_TRUE_VALUE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_get_is_connected(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_network_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u16(out, VARIANT_TRUE_VALUE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_get_connectivity(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_network_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u32(out, NLM_CONNECTIVITY_CONNECTED).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_get_category(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_network_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u32(out, NLM_NETWORK_CATEGORY_PRIVATE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_get_domain_type(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_network_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_u32(out, NLM_DOMAIN_TYPE_NON_DOMAIN_NETWORK).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_get_network_id(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_network_refs.contains(m.arg_ptr(0))) return E_NOINTERFACE;
    if (m.write_struct(out, kHostNetworkId).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

} // namespace vitreo::winapi
