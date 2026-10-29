#pragma once
/// @file host_interfaces.hpp
/// @brief One snapshot of the host's network interfaces in Windows terms.
///
/// Every IP Helper view (GetAdaptersAddresses, GetAdaptersInfo,
/// GetInterfaceInfo, GetIfTable2/GetIfEntry2, the LUID/GUID/index/name
/// conversions) is serialized from this one model, so the identities they
/// expose agree the way they do on Windows (tools/oracle_probe/
/// iphlp_adapters_probe: gaa-identity, gai-relations, gii-relations, ift2).
///
/// Values come from the host: getifaddrs, the kernel routing table (default
/// gateways), per-address IPv6 flags and lifetimes, multicast memberships,
/// interface counters, and the SystemConfiguration dynamic store (DHCP lease,
/// per-service DNS servers, interface kind and display name). Where Windows
/// defines the value independently of any host (the software loopback
/// adapter's names, MTU and link speed, default zone indices, the automatic
/// interface metric table) the measured Windows value is used and cited.

#include <vitreo/common.hpp>
#include "interface_identity.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::iphlp {

/// ws2def.h address families as the guest sees them.
constexpr u16 kGuestAfInet = 2;
constexpr u16 kGuestAfInet6 = 23;

struct NetAddress {
    u16 family = 0;              ///< kGuestAfInet / kGuestAfInet6
    std::array<u8, 16> bytes{};  ///< network order; IPv4 uses bytes[0..3]
    u32 scope_id = 0;            ///< IPv6 zone index (Windows ZoneIndices value)
    bool operator==(const NetAddress&) const = default;
    [[nodiscard]] bool is_v4() const noexcept { return family == kGuestAfInet; }
    [[nodiscard]] u32 v4_network_order() const noexcept;  ///< in_addr.s_addr
    [[nodiscard]] std::string to_dotted() const;          ///< IPv4 dotted quad
};

struct UnicastAddress {
    NetAddress address;
    u8 prefix_length = 0;
    u32 prefix_origin = 0;
    u32 suffix_origin = 0;
    u32 dad_state = 0;
    u32 valid_lifetime = 0xFFFFFFFFu;
    u32 preferred_lifetime = 0xFFFFFFFFu;
    u32 lease_lifetime = 0xFFFFFFFFu;
    u32 flags = 0;               ///< IP_ADAPTER_ADDRESS_*
};

struct Prefix {
    NetAddress address;
    u32 length = 0;
};

/// if_data64 counters mapped onto MIB_IF_ROW2 names.
struct InterfaceCounters {
    u64 in_octets = 0, in_ucast = 0, in_nucast = 0, in_discards = 0, in_errors = 0, in_unknown = 0;
    u64 out_octets = 0, out_ucast = 0, out_nucast = 0, out_errors = 0;
};

struct HostInterface {
    std::string bsd_name;
    u32 index = 0;               ///< host interface index == Windows NET_IFINDEX
    u32 host_flags = 0;          ///< IFF_*
    bool loopback = false;
    bool hardware = false;       ///< a physical port the host enumerates

    // Windows classification (ipifcons.h / ifdef.h / ntddndis.h values).
    u32 if_type = 0;
    u32 tunnel_type = 0;
    u32 media_type = 0;
    u32 physical_medium = 0;
    u32 access_type = 0;
    u32 connection_type = 0;
    u32 oper_status = 0;         ///< IF_OPER_STATUS
    u32 admin_status = 0;        ///< NET_IF_ADMIN_STATUS
    u32 media_connect_state = 0; ///< NET_IF_MEDIA_CONNECT_STATE
    u8 status_flags = 0;         ///< InterfaceAndOperStatusFlags bits

    u32 ip_mtu = 0;              ///< IP_ADAPTER_ADDRESSES.Mtu
    u32 link_mtu = 0;            ///< MIB_IF_ROW2.Mtu
    u64 link_speed = 0;          ///< bits per second, both directions
    std::vector<u8> mac;         ///< current link-layer address

    std::array<u8, 16> guid{};           ///< interface GUID, struct byte order
    std::array<u8, 16> network_guid{};   ///< network identity GUID
    u64 luid = 0;
    std::string friendly_name;   ///< UTF-8
    std::string description;     ///< UTF-8

    bool ipv4_enabled = false;
    bool ipv6_enabled = false;
    bool dhcp_enabled = false;
    std::optional<NetAddress> dhcp_server;
    i64 lease_obtained = 0;      ///< unix time, 0 without a DHCP lease
    i64 lease_expires = 0;
    /// Options of the current DHCPv4 lease as the host's client cached them
    /// (RFC 2132 code -> raw option data).
    std::map<u8, std::vector<u8>> dhcp_options;
    u32 ipv4_metric = 0;
    u32 ipv6_metric = 0;
    std::string dns_suffix;      ///< UTF-8 connection-specific suffix

    std::vector<UnicastAddress> unicast;   ///< IPv6 first, then IPv4
    std::vector<NetAddress> anycast;
    std::vector<NetAddress> multicast;     ///< IPv6 ascending, then IPv4 ascending
    std::vector<NetAddress> dns_servers;   ///< as configured (may be empty)
    std::vector<NetAddress> gateways;      ///< default routes via this interface
    InterfaceCounters counters;

    /// Windows default zone indices: link/subnet zones are the interface,
    /// wider zones default to 1, global is 0 (iphlp_adapters_probe ZoneIndices).
    [[nodiscard]] std::array<u32, 16> zone_indices() const noexcept;
    /// On-link prefixes GAA_FLAG_INCLUDE_PREFIX reports for one family.
    [[nodiscard]] std::vector<Prefix> prefixes(u16 family) const;
    /// DNS servers GetAdaptersAddresses reports (with Windows' fec0::ffff
    /// defaults for an IPv6 interface that has no configured server).
    [[nodiscard]] std::vector<NetAddress> reported_dns_servers() const;
};

enum class SnapshotDetail {
    Identity,  ///< names, indices, types, GUID/LUID and addresses only
    Full,      ///< plus routes, DHCP, DNS, multicast, IPv6 lifetimes, counters
};

/// Every host interface, ordered by index. Empty only when the host
/// interface list itself cannot be read (`ok` = false).
struct Snapshot {
    bool ok = false;
    std::vector<HostInterface> interfaces;
    [[nodiscard]] const HostInterface* by_index(u32 index) const noexcept;
    [[nodiscard]] const HostInterface* by_luid(u64 luid) const noexcept;
    [[nodiscard]] const HostInterface* by_guid(const std::array<u8, 16>& guid) const noexcept;
};
[[nodiscard]] Snapshot snapshot_host_interfaces(SnapshotDetail detail);

/// Adapters in GetAdaptersAddresses order (automatic metric, loopback last):
/// the IP-enabled interfaces, or every interface when `include_all`
/// (GAA_FLAG_INCLUDE_ALL_INTERFACES).
[[nodiscard]] std::vector<const HostInterface*> adapter_order(const Snapshot& snapshot, bool include_all = false);

/// NET_LUID value (ifdef.h NET_LUID_LH: IfType << 48 | NetLuidIndex << 24).
[[nodiscard]] constexpr u64 make_luid(u32 if_type, u32 net_luid_index) noexcept {
    return (static_cast<u64>(if_type & 0xFFFFu) << 48) |
           (static_cast<u64>(net_luid_index & 0xFFFFFFu) << 24);
}
[[nodiscard]] constexpr u32 luid_if_type(u64 luid) noexcept { return static_cast<u32>(luid >> 48); }
[[nodiscard]] constexpr u32 luid_net_index(u64 luid) noexcept { return static_cast<u32>((luid >> 24) & 0xFFFFFFu); }

/// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" of a struct-order GUID.
[[nodiscard]] std::string format_guid_braced(const std::array<u8, 16>& guid);
// interface_guid_for_name(): interface_identity.hpp (shared with ole32's
// INetworkConnection::GetAdapterId).

/// NDIS interface name "<type>_<NetLuidIndex>" (ConvertInterfaceLuidToNameW).
[[nodiscard]] std::string ndis_interface_name(u64 luid);
/// Inverse of ndis_interface_name; std::nullopt for a malformed name.
[[nodiscard]] std::optional<u64> parse_ndis_interface_name(std::string_view name);

}  // namespace vitreo::winapi::iphlp
