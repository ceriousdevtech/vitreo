#pragma once
/// @file iphlpapi.hpp
/// @brief IPHLPAPI.DLL API stubs (IP Helper — network adapter info)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace vitreo::winapi {

class Marshaler;
class HandleTable;
class SyncManager;
class HeapManager;
class ThreadManager;

/// NotifyIpInterfaceChange registrations (interface_notification.cpp).
struct InterfaceChangeState;
struct InterfaceChangeStateDeleter {
    void operator()(InterfaceChangeState* state) const;
};
[[nodiscard]] std::unique_ptr<InterfaceChangeState, InterfaceChangeStateDeleter>
make_interface_change_state();

class IphlpapiManager {
public:
    IphlpapiManager();
    ~IphlpapiManager();
    void set_notification_managers(HandleTable& handles, SyncManager& sync);
    /// Calls into guest code: @p guest_call runs a callback synchronously on
    /// the current guest thread (NotifyIpInterfaceChange's initial
    /// notification); @p threads creates the worker threads later change
    /// notifications run on.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_callbacks(GuestCall guest_call, ThreadManager* threads);
    /// NETIO_STATUS NotifyIpInterfaceChange(ADDRESS_FAMILY,
    /// PIPINTERFACE_CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE*).
    void impl_NotifyIpInterfaceChange(Marshaler& m);
    /// NETIO_STATUS CancelMibChangeNotify2(HANDLE).
    void impl_CancelMibChangeNotify2(Marshaler& m);
    /// NETIO_STATUS GetBestRoute2(NET_LUID*, NET_IFINDEX, const SOCKADDR_INET*,
    /// const SOCKADDR_INET*, ULONG, PMIB_IPFORWARD_ROW2, SOCKADDR_INET*).
    void impl_GetBestRoute2(Marshaler& m);
    /// Process heap for tables the caller releases with FreeMibTable.
    void set_heap_manager(HeapManager* heap) { m_heap = heap; }
    /// The DNS host name the rest of the guest reports (FIXED_INFO.HostName
    /// equals GetComputerNameEx(ComputerNameDnsHostname) on Windows).
    void set_host_name_provider(std::function<std::string()> provider) {
        m_host_name_provider = std::move(provider);
    }
    void impl_GetCurrentThreadCompartmentId(Marshaler& m);
    void impl_SetCurrentThreadCompartmentId(Marshaler& m);
    void impl_IcmpCreateFile(Marshaler& m);
    void impl_IcmpCloseHandle(Marshaler& m);
    void impl_IcmpSendEcho(Marshaler& m);
    void impl_NotifyAddrChange(Marshaler& m);
    void impl_CancelIPChangeNotify(Marshaler& m);

    /// Optional allowlist of guest-visible PIDs for GetExtendedTcpTable.
    /// Host libproc sees every macOS process; Vitreo OpenProcess only knows
    /// capsule-registered guest PIDs. Without this filter, Agent/HelperSvc can
    /// latch onto a foreign host PID (OpenProcess → ERROR_INVALID_PARAMETER).
    using PidAllowlistProvider = std::function<std::unordered_set<u32>()>;

    void set_pid_allowlist_provider(PidAllowlistProvider provider) {
        m_pid_allowlist_provider = std::move(provider);
    }

    void impl_GetIfEntry(Marshaler& m);
    void impl_GetIfTable(Marshaler& m);
    void impl_GetAdaptersInfo(Marshaler& m);
    /// DWORD GetExtendedTcpTable(...) — AF_INET OWNER_PID / BASIC table classes
    /// via host TCP sockets (owning-PID authorization).
    void impl_GetExtendedTcpTable(Marshaler& m);

    /// DWORD GetNetworkParams(PFIXED_INFO, PULONG) — hostname + DNS servers.
    void impl_GetNetworkParams(Marshaler& m);
    /// DWORD GetAdaptersAddresses(...) — at least one IPv4 adapter.
    void impl_GetAdaptersAddresses(Marshaler& m);
    /// NETIO_STATUS GetNetworkConnectivityHint(NL_NETWORK_CONNECTIVITY_HINT*).
    void impl_GetNetworkConnectivityHint(Marshaler& m);

    void impl_ConvertInterfaceIndexToLuid(Marshaler& m);
    void impl_ConvertInterfaceLuidToIndex(Marshaler& m);
    void impl_ConvertInterfaceLuidToNameW(Marshaler& m);
    void impl_ConvertInterfaceNameToLuidW(Marshaler& m);
    void impl_ConvertInterfaceLuidToGuid(Marshaler& m);
    /// NETIO_STATUS ConvertInterfaceGuidToLuid(const GUID*, PNET_LUID)
    void impl_ConvertInterfaceGuidToLuid(Marshaler& m);
    /// NETIO_STATUS GetIfEntry2(PMIB_IF_ROW2)
    void impl_GetIfEntry2(Marshaler& m);
    /// DWORD GetBestInterface(IPAddr, PDWORD) — IPv4 routing table lookup.
    void impl_GetBestInterface(Marshaler& m);

    /// NET_IFINDEX if_nametoindex(PCSTR) — NDIS name → local ifindex (0 on miss).
    void impl_if_nametoindex(Marshaler& m);
    /// PCHAR if_indextoname(NET_IFINDEX, PCHAR) — local ifindex → NDIS name.
    void impl_if_indextoname(Marshaler& m);
    /// NETIO_STATUS GetIfTable2(PMIB_IF_TABLE2*) and FreeMibTable(PVOID).
    void impl_GetIfTable2(Marshaler& m);
    void impl_FreeMibTable(Marshaler& m);
    /// DWORD GetInterfaceInfo(PIP_INTERFACE_INFO, PULONG).
    void impl_GetInterfaceInfo(Marshaler& m);
    /// DWORD IpReleaseAddress / IpRenewAddress(PIP_ADAPTER_INDEX_MAP).
    void impl_IpReleaseAddress(Marshaler& m);
    void impl_IpRenewAddress(Marshaler& m);

private:
    struct CompartmentState;
    std::unique_ptr<CompartmentState> m_compartments;
    struct IcmpState;
    std::unique_ptr<IcmpState> m_icmp;
    struct NotificationState;
    std::unique_ptr<NotificationState> m_notifications;
    std::unique_ptr<InterfaceChangeState, InterfaceChangeStateDeleter>
        m_interface_changes;
    GuestCall m_guest_call;
    ThreadManager* m_threads = nullptr;
    PidAllowlistProvider m_pid_allowlist_provider;
    HeapManager* m_heap = nullptr;
    std::function<std::string()> m_host_name_provider;
    void release_or_renew(Marshaler& m, const char* api);
};

DllExports create_iphlpapi_thunks(IphlpapiManager& mgr);
/// dhcpcsvc.dll: DHCP client API over the host's cached DHCPv4 lease.
DllExports create_dhcpcsvc_thunks();

} // namespace vitreo::winapi
