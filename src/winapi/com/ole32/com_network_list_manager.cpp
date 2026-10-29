// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_network_list_manager.cpp
/// @brief INetworkListManager and its connection-point container, connection point
/// and network/connection enumerators.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

namespace vitreo::winapi {

namespace {

constexpr Guid IID_INETWORKLISTMANAGEREVENTS{
    0xDCB00001u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr Guid IID_IENUMNETWORKS{
    0xDCB00003u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr Guid IID_IENUMNETWORKCONNECTIONS{
    0xDCB00006u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr u32 NLM_ENUM_NETWORK_CONNECTED = 0x1;
constexpr u32 NLM_ENUM_NETWORK_DISCONNECTED = 0x2;
constexpr Guid IID_ICONNECTIONPOINT{
    0xB196B286u, 0xBAB4u, 0x101Au, {0xB6, 0x9C, 0x00, 0xAA, 0x00, 0x34, 0x1D, 0x07}};
constexpr u32 CONNECT_E_NOCONNECTION = 0x80040207u;

} // namespace

u32 ComCoreManager::network_list_manager_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(object);
    if (id_it == m_nlm_iface_to_id.end()) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    GuestAddr result = 0;
    if (guid_equal(iid.value(), IID_IUNKNOWN) ||
        guid_equal(iid.value(), IID_IDISPATCH) ||
        guid_equal(iid.value(), IID_INETWORKLISTMANAGER)) {
        result = state_it->second.nlm;
    } else if (guid_equal(iid.value(), IID_ICONNECTIONPOINTCONTAINER)) {
        result = state_it->second.cpc;
    } else {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++state_it->second.refs;
    if (m.write_ptr(out_ptr, result).is_err()) {
        --state_it->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(m.arg_ptr(0));
    if (id_it == m_nlm_iface_to_id.end()) return 0;
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) return 0;
    return ++state_it->second.refs;
}

u32 ComCoreManager::network_list_manager_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(m.arg_ptr(0));
    if (id_it == m_nlm_iface_to_id.end()) return 0;
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) return 0;
    const u32 refs = --state_it->second.refs;
    if (refs == 0) {
        m_nlm_iface_to_id.erase(state_it->second.nlm);
        m_nlm_iface_to_id.erase(state_it->second.cpc);
        m_nlm_iface_to_id.erase(state_it->second.cp);
        m_nlm_states.erase(state_it);
    }
    return refs;
}

u32 ComCoreManager::network_list_manager_get_network_connections(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_iface_to_id.contains(object)) return E_NOINTERFACE;
    if (m_thunks == nullptr) return com::E_NOTIMPL;

    // IEnumNetworkConnections derives from IDispatch (netlistmgr.h), so the
    // enumerator methods start at slot 7, after IUnknown + IDispatch. The
    // IDispatch slots reuse the stateless E_NOTIMPL thunks shared with the
    // network list manager itself.
    const char* enum_names[] = {
        "Vitreo_NetworkListManager_Enum_QueryInterface",
        "Vitreo_NetworkListManager_Enum_AddRef",
        "Vitreo_NetworkListManager_Enum_Release",
        "Vitreo_NetworkListManager_GetTypeInfoCount",
        "Vitreo_NetworkListManager_GetTypeInfo",
        "Vitreo_NetworkListManager_GetIDsOfNames",
        "Vitreo_NetworkListManager_Invoke",
        "Vitreo_NetworkListManager_Enum_NewEnum",
        "Vitreo_NetworkListManager_Enum_Next",
        "Vitreo_NetworkListManager_Enum_Skip",
        "Vitreo_NetworkListManager_Enum_Reset",
        "Vitreo_NetworkListManager_Enum_Clone",
    };
    constexpr usize kSlots = sizeof(enum_names) / sizeof(enum_names[0]);
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr enum_obj = reserve_com_scratch((1 + kSlots) * pointer_size);
    if (enum_obj == 0) return E_OUTOFMEMORY;
    const GuestAddr vtable = enum_obj + pointer_size;
    if (m.write_ptr(enum_obj, vtable).is_err()) {
        return com::E_INVALIDARG;
    }
    for (usize i = 0; i < kSlots; ++i) {
        auto fn = m_thunks->get_thunk_address("ole32.dll", enum_names[i]);
        if (fn.is_err() ||
            m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        fn.value())
                .is_err()) {
            return com::E_NOTIMPL;
        }
    }
    m_nlm_enums[enum_obj] = {};
    if (m.write_ptr(out, enum_obj).is_err()) {
        m_nlm_enums.erase(enum_obj);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_get_networks(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    if ((flags & (NLM_ENUM_NETWORK_CONNECTED | NLM_ENUM_NETWORK_DISCONNECTED)) == 0 ||
        (flags & ~(NLM_ENUM_NETWORK_CONNECTED | NLM_ENUM_NETWORK_DISCONNECTED)) != 0) {
        return com::E_INVALIDARG;
    }
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_iface_to_id.contains(object)) return E_NOINTERFACE;
    static const char* enum_names[] = {
        "Vitreo_NetworkListManager_Enum_QueryInterface",
        "Vitreo_NetworkListManager_Enum_AddRef",
        "Vitreo_NetworkListManager_Enum_Release",
        "Vitreo_NetworkListManager_GetTypeInfoCount",
        "Vitreo_NetworkListManager_GetTypeInfo",
        "Vitreo_NetworkListManager_GetIDsOfNames",
        "Vitreo_NetworkListManager_Invoke",
        "Vitreo_NetworkListManager_Enum_NewEnum",
        "Vitreo_NetworkListManager_Enum_Next",
        "Vitreo_NetworkListManager_Enum_Skip",
        "Vitreo_NetworkListManager_Enum_Reset",
        "Vitreo_NetworkListManager_Enum_Clone",
    };
    const GuestAddr enum_obj =
        create_network_object(m, enum_names, std::size(enum_names));
    if (enum_obj == 0) return com::E_NOTIMPL;
    NlmEnumState state;
    state.yields_networks = true;
    state.count = (flags & NLM_ENUM_NETWORK_CONNECTED) != 0 ? 1u : 0u;
    m_nlm_enums[enum_obj] = state;
    if (m.write_ptr(out, enum_obj).is_err()) {
        m_nlm_enums.erase(enum_obj);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_get_is_connected_to_internet(
    Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_iface_to_id.contains(object)) return E_NOINTERFACE;
    if (m.write_u16(out, VARIANT_TRUE_VALUE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_get_is_connected(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_iface_to_id.contains(object)) return E_NOINTERFACE;
    if (m.write_u16(out, VARIANT_TRUE_VALUE).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_get_connectivity(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_iface_to_id.contains(object)) return E_NOINTERFACE;
    if (m.write_u32(out, NLM_CONNECTIVITY_CONNECTED).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_cpc_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(object);
    if (id_it == m_nlm_iface_to_id.end()) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    GuestAddr result = 0;
    if (guid_equal(iid.value(), IID_IUNKNOWN) ||
        guid_equal(iid.value(), IID_ICONNECTIONPOINTCONTAINER)) {
        result = state_it->second.cpc;
    } else if (guid_equal(iid.value(), IID_IDISPATCH) ||
               guid_equal(iid.value(), IID_INETWORKLISTMANAGER)) {
        result = state_it->second.nlm;
    } else {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++state_it->second.refs;
    if (m.write_ptr(out_ptr, result).is_err()) {
        --state_it->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_cpc_find_connection_point(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_POINTER;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(object);
    if (id_it == m_nlm_iface_to_id.end()) return E_NOINTERFACE;
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) return E_NOINTERFACE;
    if (!guid_equal(iid.value(), IID_INETWORKLISTMANAGEREVENTS)) {
        (void)m.write_ptr(out_ptr, 0);
        return CONNECT_E_NOCONNECTION;
    }
    ++state_it->second.refs;
    if (m.write_ptr(out_ptr, state_it->second.cp).is_err()) {
        --state_it->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_cp_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(object);
    if (id_it == m_nlm_iface_to_id.end()) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    GuestAddr result = 0;
    if (guid_equal(iid.value(), IID_IUNKNOWN) ||
        guid_equal(iid.value(), IID_ICONNECTIONPOINT)) {
        result = state_it->second.cp;
    } else if (guid_equal(iid.value(), IID_ICONNECTIONPOINTCONTAINER)) {
        result = state_it->second.cpc;
    } else {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++state_it->second.refs;
    if (m.write_ptr(out_ptr, result).is_err()) {
        --state_it->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_cp_get_container(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(object);
    if (id_it == m_nlm_iface_to_id.end()) return E_NOINTERFACE;
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) return E_NOINTERFACE;
    ++state_it->second.refs;
    if (m.write_ptr(out, state_it->second.cpc).is_err()) {
        --state_it->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_cp_advise(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr sink = m.arg_ptr(1);
    const GuestAddr cookie_out = m.arg_ptr(2);
    if (sink == 0 || cookie_out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto id_it = m_nlm_iface_to_id.find(object);
    if (id_it == m_nlm_iface_to_id.end()) return E_NOINTERFACE;
    auto state_it = m_nlm_states.find(id_it->second);
    if (state_it == m_nlm_states.end()) return E_NOINTERFACE;
    const u32 cookie = state_it->second.next_advise_cookie++;
    if (m.write_u32(cookie_out, cookie).is_err()) return com::E_INVALIDARG;
    // Sink is accepted but events are not delivered yet — enough for
    // WindowsNetworkNotifier::StartListening to succeed.
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_cp_unadvise(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    (void)m.arg(1);
    std::lock_guard lock(m_com_mutex);
    if (!m_nlm_iface_to_id.contains(object)) return E_NOINTERFACE;
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_enum_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(object);
    if (found == m_nlm_enums.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IDISPATCH) &&
         !(found->second.yields_networks
               ? guid_equal(iid.value(), IID_IENUMNETWORKS)
               : guid_equal(iid.value(), IID_IENUMNETWORKCONNECTIONS)))) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++found->second.refs;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --found->second.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_enum_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(m.arg_ptr(0));
    if (found == m_nlm_enums.end()) return 0;
    return ++found->second.refs;
}

u32 ComCoreManager::network_list_manager_enum_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(m.arg_ptr(0));
    if (found == m_nlm_enums.end()) return 0;
    const u32 refs = --found->second.refs;
    if (refs == 0) m_nlm_enums.erase(found);
    return refs;
}

GuestAddr ComCoreManager::create_network_object(Marshaler& m, const char* const* names,
                                                usize slot_count) {
    if (m_thunks == nullptr) return 0;
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = reserve_com_scratch(
        pointer_size + static_cast<GuestAddr>(slot_count) * pointer_size);
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize i = 0; i < slot_count; ++i) {
        auto fn = m_thunks->get_thunk_address("ole32.dll", names[i]);
        if (fn.is_err() ||
            m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size, fn.value())
                .is_err()) {
            return 0;
        }
    }
    return object;
}

u32 ComCoreManager::network_list_manager_enum_next(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 celt = static_cast<u32>(m.arg(1));
    const GuestAddr rgelt = m.arg_ptr(2);
    const GuestAddr fetched = m.arg_ptr(3);
    if (celt > 1 && fetched == 0) return com::E_INVALIDARG;
    if (celt > 0 && rgelt == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(object);
    if (found == m_nlm_enums.end()) return E_NOINTERFACE;
    if (celt == 0) {
        if (fetched != 0) (void)m.write_u32(fetched, 0);
        return com::S_OK;
    }
    u32 produced = 0;
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    while (produced < celt && found->second.index < found->second.count) {
        static const char* connection_names[] = {
            "Vitreo_NetworkConnection_QueryInterface",
            "Vitreo_NetworkConnection_AddRef",
            "Vitreo_NetworkConnection_Release",
            "Vitreo_NetworkListManager_GetTypeInfoCount",
            "Vitreo_NetworkListManager_GetTypeInfo",
            "Vitreo_NetworkListManager_GetIDsOfNames",
            "Vitreo_NetworkListManager_Invoke",
            "Vitreo_NetworkConnection_GetNetwork",
            "Vitreo_NetworkConnection_get_IsConnectedToInternet",
            "Vitreo_NetworkConnection_get_IsConnected",
            "Vitreo_NetworkConnection_GetConnectivity",
            "Vitreo_NetworkConnection_GetConnectionId",
            "Vitreo_NetworkConnection_GetAdapterId",
            "Vitreo_NetworkConnection_GetDomainType",
        };
        static const char* network_names[] = {
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
        const bool as_network = found->second.yields_networks;
        const char* const* names = as_network ? network_names : connection_names;
        const usize slots = as_network ? std::size(network_names)
                                       : std::size(connection_names);
        const GuestAddr connection = create_network_object(m, names, slots);
        if (connection == 0) return com::E_NOTIMPL;
        if (as_network) m_nlm_network_refs[connection] = 1;
        else m_nlm_connection_refs[connection] = 1;
        if (m.write_ptr(rgelt + produced * pointer_size, connection).is_err()) {
            if (as_network) m_nlm_network_refs.erase(connection);
            else m_nlm_connection_refs.erase(connection);
            return com::E_INVALIDARG;
        }
        ++found->second.index;
        ++produced;
    }
    if (fetched != 0 && m.write_u32(fetched, produced).is_err()) {
        return com::E_INVALIDARG;
    }
    return produced == celt ? com::S_OK : com::S_FALSE;
}

u32 ComCoreManager::network_list_manager_enum_skip(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 celt = static_cast<u32>(m.arg(1));
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(object);
    if (found == m_nlm_enums.end()) return E_NOINTERFACE;
    const u32 remain = found->second.count - found->second.index;
    if (celt > remain) {
        found->second.index = found->second.count;
        return com::S_FALSE;
    }
    found->second.index += celt;
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_enum_reset(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(m.arg_ptr(0));
    if (found == m_nlm_enums.end()) return E_NOINTERFACE;
    found->second.index = 0;
    return com::S_OK;
}

u32 ComCoreManager::network_list_manager_enum_clone(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    std::lock_guard lock(m_com_mutex);
    auto found = m_nlm_enums.find(object);
    if (found == m_nlm_enums.end()) return E_NOINTERFACE;
    const u32 index = found->second.index;
    // Reuse the same vtable builder as GetNetworkConnections by copying the
    // existing object's vtable pointer into a new identity.
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const auto vtable = m.read_ptr(object);
    if (vtable.is_err()) return com::E_INVALIDARG;
    const GuestAddr clone = reserve_com_scratch(pointer_size);
    if (clone == 0) return E_OUTOFMEMORY;
    if (m.write_ptr(clone, vtable.value()).is_err()) return com::E_INVALIDARG;
    m_nlm_enums[clone] = NlmEnumState{
        .refs = 1,
        .index = index,
        .count = found->second.count,
        .yields_networks = found->second.yields_networks,
    };
    if (m.write_ptr(out, clone).is_err()) {
        m_nlm_enums.erase(clone);
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

} // namespace vitreo::winapi
