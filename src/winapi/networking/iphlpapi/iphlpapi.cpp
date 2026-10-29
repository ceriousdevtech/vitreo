/// @file iphlpapi.cpp
/// @brief IphlpapiManager construction, the thread compartment APIs and the
///        iphlpapi.dll export table.
///
/// The API families live in the sibling files: tcp_table.cpp, if_table.cpp,
/// adapter_info.cpp, adapter_addresses.cpp, connectivity.cpp,
/// address_notification.cpp, if_row2.cpp, interface_convert.cpp, icmp.cpp
/// and dhcpcsvc.cpp.

#include "iphlpapi_internal.hpp"
#include "iphlpapi_state.hpp"

namespace vitreo::winapi {

void IphlpapiManager::impl_GetCurrentThreadCompartmentId(Marshaler& m) {
    std::lock_guard lock(m_compartments->mutex);
    const auto tid = ThreadManager::get_current_thread_id_static();
    const auto [it, inserted] = m_compartments->thread_ids.try_emplace(tid, kDefaultCompartmentId);
    (void)inserted;
    set_last_error(0);
    m.set_return(it->second);
}

void IphlpapiManager::impl_SetCurrentThreadCompartmentId(Marshaler& m) {
    const auto requested = m.arg32(0);
    set_last_error(0);
    // Zero restores the default; IDs absent from this capsule do not exist.
    if (requested != 0 && requested != kDefaultCompartmentId) {
        m.set_return(kCompartmentNotFound);
        return;
    }
    std::lock_guard lock(m_compartments->mutex);
    m_compartments->thread_ids[ThreadManager::get_current_thread_id_static()] = kDefaultCompartmentId;
    m.set_return(0);
}

IphlpapiManager::IphlpapiManager() : m_compartments(std::make_unique<CompartmentState>()), m_icmp(std::make_unique<IcmpState>()), m_notifications(std::make_unique<NotificationState>()), m_interface_changes(make_interface_change_state()) {}
IphlpapiManager::~IphlpapiManager() = default;
void IphlpapiManager::set_notification_managers(HandleTable& handles, SyncManager& sync) {
    m_notifications->handles = &handles;
    m_notifications->sync = &sync;
}

DllExports create_iphlpapi_thunks(IphlpapiManager& mgr) {
    DllExports exports;
    exports.entries.reserve(16);

    exports.entries.push_back({"IcmpCreateFile", 0,
        [&mgr](Marshaler& m) { mgr.impl_IcmpCreateFile(m); }, 0});
    exports.entries.push_back({"IcmpCloseHandle", 0,
        [&mgr](Marshaler& m) { mgr.impl_IcmpCloseHandle(m); }, 4});
    exports.entries.push_back({"IcmpSendEcho", 0,
        [&mgr](Marshaler& m) { mgr.impl_IcmpSendEcho(m); }, 32});

    exports.entries.push_back({"GetIfTable", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIfTable(m); }, 12});
    exports.entries.push_back(ExportEntry{
        "GetIfEntry", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIfEntry(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        "GetAdaptersInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetAdaptersInfo(m); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "GetExtendedTcpTable", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetExtendedTcpTable(m); },
        24
    });

    exports.entries.push_back(ExportEntry{
        "GetNetworkParams", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetNetworkParams(m); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "GetAdaptersAddresses", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetAdaptersAddresses(m); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "GetNetworkConnectivityHint", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetNetworkConnectivityHint(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        "ConvertInterfaceIndexToLuid", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertInterfaceIndexToLuid(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ConvertInterfaceLuidToIndex", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertInterfaceLuidToIndex(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ConvertInterfaceLuidToNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertInterfaceLuidToNameW(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "ConvertInterfaceNameToLuidW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertInterfaceNameToLuidW(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ConvertInterfaceLuidToGuid", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertInterfaceLuidToGuid(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ConvertInterfaceGuidToLuid", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertInterfaceGuidToLuid(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetCurrentThreadCompartmentId", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentThreadCompartmentId(m); }, 0
    });
    exports.entries.push_back(ExportEntry{
        "SetCurrentThreadCompartmentId", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetCurrentThreadCompartmentId(m); }, 4
    });
    exports.entries.push_back(ExportEntry{
        "GetIfEntry2", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIfEntry2(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetBestInterface", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetBestInterface(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "if_nametoindex", 0,
        [&mgr](Marshaler& m) { mgr.impl_if_nametoindex(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        "if_indextoname", 0,
        [&mgr](Marshaler& m) { mgr.impl_if_indextoname(m); }, 8 });
    exports.entries.push_back(ExportEntry{
        "GetIfTable2", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetIfTable2(m); }, 4 });
    exports.entries.push_back(ExportEntry{
        "GetInterfaceInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetInterfaceInfo(m); }, 8 });
    exports.entries.push_back(ExportEntry{
        "IpReleaseAddress", 0,
        [&mgr](Marshaler& m) { mgr.impl_IpReleaseAddress(m); }, 4 });
    exports.entries.push_back(ExportEntry{
        "IpRenewAddress", 0,
        [&mgr](Marshaler& m) { mgr.impl_IpRenewAddress(m); }, 4 });
    exports.entries.push_back(ExportEntry{
        "NotifyAddrChange", 0,
        [&mgr](Marshaler& m) { mgr.impl_NotifyAddrChange(m); }, 8 });
    exports.entries.push_back(ExportEntry{
        "NotifyIpInterfaceChange", 0,
        [&mgr](Marshaler& m) { mgr.impl_NotifyIpInterfaceChange(m); }, 20 });
    exports.entries.push_back(ExportEntry{
        "CancelMibChangeNotify2", 0,
        [&mgr](Marshaler& m) { mgr.impl_CancelMibChangeNotify2(m); }, 4 });
    exports.entries.push_back(ExportEntry{
        "GetBestRoute2", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetBestRoute2(m); }, 28 });
    exports.entries.push_back(ExportEntry{
        "CancelIPChangeNotify", 0,
        [&mgr](Marshaler& m) { mgr.impl_CancelIPChangeNotify(m); }, 4 });
    exports.entries.push_back(ExportEntry{
        "FreeMibTable", 0,
        [&mgr](Marshaler& m) { mgr.impl_FreeMibTable(m); }, 4 });

    return exports;
}

} // namespace vitreo::winapi
