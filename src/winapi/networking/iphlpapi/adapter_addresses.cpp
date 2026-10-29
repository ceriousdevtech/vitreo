/// @file adapter_addresses.cpp
/// @brief GetAdaptersAddresses: IP_ADAPTER_ADDRESSES_LH for every host interface.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {

// GAA_FLAG_SKIP_DNS_INFO: IPTypes.h (SDK 10.0.26100) defines it as 0x0800;
// the MinGW header the probe builds with predates it. Windows then omits the
// DNS server list (iphlp_adapters_probe gaa-flag "unknown_bits" dns=0).
constexpr u32 kGaaFlagSkipDnsInfo = 0x0800u;

[[nodiscard]] bool family_matches(u32 requested, const iphlp::NetAddress& a) noexcept {
    return requested == 0 || requested == a.family;
}

} // namespace

void IphlpapiManager::impl_GetAdaptersAddresses(Marshaler& m) {
    // ULONG GetAdaptersAddresses(ULONG Family, ULONG Flags, PVOID Reserved,
    //                            PIP_ADAPTER_ADDRESSES AdapterAddresses,
    //                            PULONG SizePointer)
    // https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses
    // Field values and error cases: tools/oracle_probe/iphlp_adapters_probe.
    const u32 family = m.arg32(0);
    const u32 flags = m.arg32(1);
    (void)m.arg_ptr(2);  // Reserved: ignored (gaa-size-query reserved_nonnull_rc=0)
    const GuestAddr buffer = m.arg_ptr(3);
    const GuestAddr size_ptr = m.arg_ptr(4);
    const auto supplied = read_size(m, size_ptr);
    if (!supplied ||
        (family != 0 && family != iphlp::kGuestAfInet && family != iphlp::kGuestAfInet6)) {
        m.set_return(ERROR_INVALID_PARAMETER);  // gaa-null-size / gaa-bad-family
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    if (!snapshot.ok) { m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE); return; }
    // Learn: with GAA_FLAG_INCLUDE_ALL_INTERFACES "all NDIS adapters will be
    // retrieved even those ... not bound to an address family specified in
    // the Family parameter"; otherwise only adapters enabled for the family.
    // GAA_FLAG_INCLUDE_ALL_COMPARTMENTS is documented as not supported, and
    // there is a single compartment.
    const bool all_interfaces = (flags & kGAA_FLAG_INCLUDE_ALL_INTERFACES) != 0;
    if (flags & kGAA_FLAG_INCLUDE_TUNNEL_BINDINGORDER) {
        // The host has no tunnel binding order; the list stays in route-metric
        // order (the Windows 10 order, Learn remarks).
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!GetAdaptersAddresses: GAA_FLAG_INCLUDE_TUNNEL_BINDINGORDER (listed in route-metric order)\n");
        });
    }
    std::vector<const iphlp::HostInterface*> adapters;
    for (const auto* iface : iphlp::adapter_order(snapshot, all_interfaces)) {
        if (!all_interfaces && family == iphlp::kGuestAfInet && !iface->ipv4_enabled) continue;
        if (!all_interfaces && family == iphlp::kGuestAfInet6 && !iface->ipv6_enabled) continue;
        adapters.push_back(iface);
    }
    if (adapters.empty()) { m.set_return(ERROR_NO_DATA); return; }

    const bool wide = m.cpu_is_64bit();
    const AdapterAbi& L = adapter_abi(m);
    GuestBlob blob(wide);
    usize previous = 0;
    bool first = true;
    for (const auto* ap : adapters) {
        const auto& a = *ap;
        const usize e = blob.reserve(L.IP_ADAPTER_ADDRESSES_LH_size, 8);
        if (!first) blob.pointer(previous + L.IP_ADAPTER_ADDRESSES_LH_Next, e);
        first = false;
        previous = e;
        blob.put<u32>(e, L.IP_ADAPTER_ADDRESSES_LH_size);  // Length (x86 376, x64 448)
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_IfIndex, a.ipv4_enabled ? a.index : 0u);
        blob.pointer(e + L.IP_ADAPTER_ADDRESSES_LH_AdapterName, blob.ascii_string(iphlp::format_guid_braced(a.guid)));

        // Address lists share one node shape: Length/Flags, Next, SOCKET_ADDRESS.
        auto chain = [&](usize head_field, usize node_size, usize address_field,
                         const std::vector<iphlp::NetAddress>& items, auto&& extra) {
            usize link = head_field;
            for (const auto& address : items) {
                if (!family_matches(family, address)) continue;
                const usize node = blob.reserve(node_size, 8);
                blob.pointer(link, node);
                link = node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_Next;
                blob.put<u32>(node, static_cast<u32>(node_size));
                blob.pointer(node + address_field, blob.sockaddr(address));
                blob.put<u32>(node + address_field + L.SOCKET_ADDRESS_iSockaddrLength, GuestBlob::sockaddr_length(address));
                extra(node, address);
            }
        };
        auto no_extra = [](usize, const iphlp::NetAddress&) {};
        if (!(flags & kGAA_FLAG_SKIP_UNICAST)) {
            usize link = e + L.IP_ADAPTER_ADDRESSES_LH_FirstUnicastAddress;
            for (const auto& u : a.unicast) {
                if (!family_matches(family, u.address)) continue;
                const usize node = blob.reserve(L.IP_ADAPTER_UNICAST_ADDRESS_LH_size, 8);
                blob.pointer(link, node);
                link = node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_Next;
                blob.put<u32>(node, L.IP_ADAPTER_UNICAST_ADDRESS_LH_size);
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_Flags, u.flags);
                blob.pointer(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_Address, blob.sockaddr(u.address));
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_Address + L.SOCKET_ADDRESS_iSockaddrLength,
                              GuestBlob::sockaddr_length(u.address));
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_PrefixOrigin, u.prefix_origin);
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_SuffixOrigin, u.suffix_origin);
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_DadState, u.dad_state);
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_ValidLifetime, u.valid_lifetime);
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_PreferredLifetime, u.preferred_lifetime);
                blob.put<u32>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_LeaseLifetime, u.lease_lifetime);
                blob.put<u8>(node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_OnLinkPrefixLength, u.prefix_length);
            }
        }
        if (!(flags & kGAA_FLAG_SKIP_ANYCAST))
            chain(e + L.IP_ADAPTER_ADDRESSES_LH_FirstAnycastAddress, L.IP_ADAPTER_ANYCAST_ADDRESS_XP_size,
                  L.IP_ADAPTER_ANYCAST_ADDRESS_XP_Address, a.anycast, no_extra);
        if (!(flags & kGAA_FLAG_SKIP_MULTICAST))
            chain(e + L.IP_ADAPTER_ADDRESSES_LH_FirstMulticastAddress, L.IP_ADAPTER_MULTICAST_ADDRESS_XP_size,
                  L.IP_ADAPTER_MULTICAST_ADDRESS_XP_Address, a.multicast, no_extra);
        if (!(flags & (kGAA_FLAG_SKIP_DNS_SERVER | kGaaFlagSkipDnsInfo)))
            chain(e + L.IP_ADAPTER_ADDRESSES_LH_FirstDnsServerAddress, L.IP_ADAPTER_DNS_SERVER_ADDRESS_XP_size,
                  L.IP_ADAPTER_DNS_SERVER_ADDRESS_XP_Address, a.reported_dns_servers(), no_extra);
        // Strings: GAA_FLAG_SKIP_FRIENDLY_NAME has no effect on current Windows
        // (gaa-flag skip_friendly friendly_len=27).
        blob.pointer(e + L.IP_ADAPTER_ADDRESSES_LH_DnsSuffix, blob.wide_string(a.dns_suffix));
        blob.pointer(e + L.IP_ADAPTER_ADDRESSES_LH_Description, blob.wide_string(a.description));
        blob.pointer(e + L.IP_ADAPTER_ADDRESSES_LH_FriendlyName, blob.wide_string(a.friendly_name));
        const usize mac_len = std::min<usize>(a.mac.size(), kMAX_ADAPTER_ADDRESS_LENGTH);
        blob.put_bytes(e + L.IP_ADAPTER_ADDRESSES_LH_PhysicalAddress, a.mac.data(), mac_len);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_PhysicalAddressLength, static_cast<u32>(mac_len));
        // Flags: DDNS registration is the Windows default on every adapter and
        // NetBIOS over TCP/IP on every non-loopback IPv4 adapter (gaa-loopback
        // Flags=0x181, primary 0x1c5).
        u32 adapter_flags = kIP_ADAPTER_DDNS_ENABLED;
        if (a.dhcp_enabled) adapter_flags |= kIP_ADAPTER_DHCP_ENABLED;
        if (!(a.host_flags & IFF_MULTICAST)) adapter_flags |= kIP_ADAPTER_NO_MULTICAST;
        if (!a.loopback && a.ipv4_enabled) adapter_flags |= kIP_ADAPTER_NETBIOS_OVER_TCPIP_ENABLED;
        if (a.ipv4_enabled) adapter_flags |= kIP_ADAPTER_IPV4_ENABLED;
        if (a.ipv6_enabled) adapter_flags |= kIP_ADAPTER_IPV6_ENABLED;
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_Flags, adapter_flags);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_Mtu, a.ip_mtu);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_IfType, a.if_type);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_OperStatus, a.oper_status);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_Ipv6IfIndex, a.ipv6_enabled ? a.index : 0u);
        const auto zones = a.zone_indices();
        blob.put_bytes(e + L.IP_ADAPTER_ADDRESSES_LH_ZoneIndices, zones.data(), sizeof(zones));
        if (flags & kGAA_FLAG_INCLUDE_PREFIX) {
            usize link = e + L.IP_ADAPTER_ADDRESSES_LH_FirstPrefix;
            for (u16 f : {iphlp::kGuestAfInet6, iphlp::kGuestAfInet}) {
                if (family != 0 && family != f) continue;
                for (const auto& p : a.prefixes(f)) {
                    const usize node = blob.reserve(L.IP_ADAPTER_PREFIX_XP_size, 8);
                    blob.pointer(link, node);
                    link = node + L.IP_ADAPTER_UNICAST_ADDRESS_LH_Next;
                    blob.put<u32>(node, L.IP_ADAPTER_PREFIX_XP_size);
                    blob.pointer(node + L.IP_ADAPTER_PREFIX_XP_Address, blob.sockaddr(p.address));
                    blob.put<u32>(node + L.IP_ADAPTER_PREFIX_XP_Address + L.SOCKET_ADDRESS_iSockaddrLength,
                                  GuestBlob::sockaddr_length(p.address));
                    blob.put<u32>(node + L.IP_ADAPTER_PREFIX_XP_PrefixLength, p.length);
                }
            }
        }
        blob.put<u64>(e + L.IP_ADAPTER_ADDRESSES_LH_TransmitLinkSpeed, a.link_speed);
        blob.put<u64>(e + L.IP_ADAPTER_ADDRESSES_LH_ReceiveLinkSpeed, a.link_speed);
        // GAA_FLAG_INCLUDE_WINS_INFO: no WINS servers are configured.
        if (flags & kGAA_FLAG_INCLUDE_GATEWAYS)
            chain(e + L.IP_ADAPTER_ADDRESSES_LH_FirstGatewayAddress, L.IP_ADAPTER_GATEWAY_ADDRESS_LH_size,
                  L.IP_ADAPTER_GATEWAY_ADDRESS_LH_Address, a.gateways, no_extra);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_Ipv4Metric, a.ipv4_metric);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_Ipv6Metric, a.ipv6_metric);
        blob.put<u64>(e + L.IP_ADAPTER_ADDRESSES_LH_Luid, a.luid);
        if (a.dhcp_enabled && a.dhcp_server) {
            blob.pointer(e + L.IP_ADAPTER_ADDRESSES_LH_Dhcpv4Server, blob.sockaddr(*a.dhcp_server));
            blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_Dhcpv4Server + L.SOCKET_ADDRESS_iSockaddrLength,
                          GuestBlob::sockaddr_length(*a.dhcp_server));
        }
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_CompartmentId, kDefaultCompartmentId);
        blob.put_bytes(e + L.IP_ADAPTER_ADDRESSES_LH_NetworkGuid, a.network_guid.data(), 16);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_ConnectionType, a.connection_type);
        blob.put<u32>(e + L.IP_ADAPTER_ADDRESSES_LH_TunnelType, a.tunnel_type);
        // Dhcpv6Server / client DUID / IAID: the host exposes no DHCPv6 client
        // state, and FirstDnsSuffix is NULL on Windows (gaa-loopback).
    }
    const u32 need = static_cast<u32>(blob.size());
    if (buffer == 0 || *supplied < need) {
        (void)m.write_u32(size_ptr, need);
        winapi::set_last_error(NO_ERROR);  // gaa-size-query le=0x0
        m.set_return(ERROR_BUFFER_OVERFLOW);
        return;
    }
    const auto image = blob.finish(buffer);
    if (!guest_writable(m, buffer, image.size()) ||
        m.memory().write_bytes(buffer, image.data(), image.size()).is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    VITREO_TRACE("[iphlpapi] GetAdaptersAddresses(family=%u, flags=0x%x) -> %zu adapters, %u bytes\n",
                 family, flags, adapters.size(), need);
    winapi::set_last_error(NO_ERROR);  // gaa-size-query exact_le=0x0
    m.set_return(NO_ERROR);
}

} // namespace vitreo::winapi
