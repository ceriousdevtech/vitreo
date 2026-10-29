// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_core_thunks_network.cpp
/// @brief ole32 thunks for INetworkListManager, INetworkConnection and INetwork.

#include "com_core.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_thunks_internal.hpp"

#include <atomic>
#include <cstdio>

namespace vitreo::winapi {

/// INetworkListManager, INetworkConnection and INetwork vtable methods.
void append_com_network_thunks(DllExports& exports, ComCoreManager& mgr) {
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_AddRef", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_add_ref(m));
        }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_Release", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_release(m));
        }, 4});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_GetTypeInfoCount", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 8});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_GetTypeInfo", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 16});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_GetIDsOfNames", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 24});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_Invoke", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 36});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_GetNetworks", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_get_networks(m));
        }, 12});
    exports.entries.push_back(ExportEntry{"Vitreo_NetworkListManager_GetNetwork", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 24});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_GetNetworkConnections", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_get_network_connections(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_GetNetworkConnection", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 24});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_get_IsConnectedToInternet", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_get_is_connected_to_internet(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_get_IsConnected", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_get_is_connected(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_GetConnectivity", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_get_connectivity(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_SetSimulatedProfileInfo", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_ClearSimulatedProfileInfo", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cpc_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_cpc_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cpc_EnumConnectionPoints", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cpc_FindConnectionPoint", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_cpc_find_connection_point(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cp_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_cp_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cp_GetConnectionInterface", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) {
                m.set_return(com::E_POINTER);
                return;
            }
            Guid iid{0xDCB00001u, 0x570Fu, 0x4A9Bu,
                     {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
            if (m.write_struct(out, iid).is_err()) {
                m.set_return(com::E_INVALIDARG);
                return;
            }
            m.set_return(com::S_OK);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cp_GetConnectionPointContainer", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_cp_get_container(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cp_Advise", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_cp_advise(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cp_Unadvise", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_cp_unadvise(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Cp_EnumConnections", 0,
        [](Marshaler& m) { m.set_return(com::E_NOTIMPL); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_AddRef", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_add_ref(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_Release", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_release(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_NewEnum", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) {
                m.set_return(com::E_POINTER);
                return;
            }
            (void)m.write_ptr(out, 0);
            m.set_return(com::E_NOTIMPL);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_Next", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_next(m));
        }, 16});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_Skip", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_skip(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_Reset", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_reset(m));
        }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkListManager_Enum_Clone", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_list_manager_enum_clone(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_QueryInterface", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_query_interface(m));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_connection_add_ref(m)); },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_connection_release(m)); },
        4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_GetNetwork", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_network(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_get_IsConnectedToInternet", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_is_connected_to_internet(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_get_IsConnected", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_is_connected(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_GetConnectivity", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_connectivity(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_GetConnectionId", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_connection_id(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_GetAdapterId", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_adapter_id(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_NetworkConnection_GetDomainType", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_connection_get_domain_type(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_QueryInterface", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_query_interface(m)); },
        12});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_AddRef", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_add_ref(m)); }, 4});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_Release", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_release(m)); }, 4});
    auto nlm_unimplemented = [](const char* which, u16 bytes) {
        return ExportEntry{which, 0, [which](Marshaler& m) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ole32!%s\n", which);
            }
            m.set_return(com::E_NOTIMPL);
        }, bytes};
    };
    exports.entries.push_back(nlm_unimplemented("Vitreo_Network_GetName", 8));
    exports.entries.push_back(nlm_unimplemented("Vitreo_Network_SetName", 8));
    exports.entries.push_back(nlm_unimplemented("Vitreo_Network_GetDescription", 8));
    exports.entries.push_back(nlm_unimplemented("Vitreo_Network_SetDescription", 8));
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_GetNetworkId", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_get_network_id(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_GetDomainType", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_get_domain_type(m)); }, 8});
    exports.entries.push_back(nlm_unimplemented("Vitreo_Network_GetNetworkConnections", 8));
    exports.entries.push_back(
        nlm_unimplemented("Vitreo_Network_GetTimeCreatedAndConnected", 20));
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_get_IsConnectedToInternet", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.network_get_is_connected_to_internet(m));
        }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_get_IsConnected", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_get_is_connected(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_GetConnectivity", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_get_connectivity(m)); }, 8});
    exports.entries.push_back(ExportEntry{
        "Vitreo_Network_GetCategory", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.network_get_category(m)); }, 8});
    exports.entries.push_back(nlm_unimplemented("Vitreo_Network_SetCategory", 8));
}

} // namespace vitreo::winapi
