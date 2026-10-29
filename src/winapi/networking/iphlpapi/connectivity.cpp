/// @file connectivity.cpp
/// @brief GetNetworkConnectivityHint and GetBestInterface (host routing lookup).

#include "iphlpapi_internal.hpp"

#include <sys/sysctl.h>

namespace vitreo::winapi {

void IphlpapiManager::impl_GetNetworkConnectivityHint(Marshaler& m) {
    // nldef.h: level 3 is InternetAccess, 1 is None. Cost 1 is Unrestricted.
    const GuestAddr hint = m.arg_ptr(0);
    if (hint == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    bool online = false;
    for (const auto& iface : iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity).interfaces) {
        if (iface.loopback || iface.oper_status != kIfOperStatusUp) continue;
        for (const auto& u : iface.unicast)
            if (u.address.is_v4()) online = true;
    }
    const u32 level = online ? 3u : 1u;
    const u32 cost = online ? 1u : 0u;
    if (m.write_u32(hint, level).is_err() || m.write_u32(hint + 4, cost).is_err() ||
        m.write_u8(hint + 8, 0).is_err() || m.write_u8(hint + 9, 0).is_err() ||
        m.write_u8(hint + 10, 0).is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    ipc::ipc_log("GetNetworkConnectivityHint -> level=%u cost=%u", level, cost);
    m.set_return(NO_ERROR);
}

void IphlpapiManager::impl_GetBestInterface(Marshaler& m) {
    // DWORD GetBestInterface(IPAddr dwDestAddr, PDWORD pdwBestIfIndex)
    const u32 destination = static_cast<u32>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    if (!out || !m.memory().guest_can_write(out, sizeof(u32))) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
#ifdef __APPLE__
    ifaddrs* entries = nullptr;
    if (::getifaddrs(&entries) != 0) { m.set_return(kErrorCannotComplete); return; }
    bool has_ipv4 = false;
    for (auto* entry = entries; entry; entry = entry->ifa_next) {
        if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET) { has_ipv4 = true; break; }
    }
    ::freeifaddrs(entries);
    if (!has_ipv4) { m.set_return(ERROR_NOT_SUPPORTED); return; }
    // Ask the kernel routing table, including host routes, default routes and
    // scoped interface routes. Selecting the first configured IPv4 address is
    // not a route lookup. RTM_GET never transmits an IP packet.
    const int fd = ::socket(PF_ROUTE, SOCK_RAW, AF_INET);
    if (fd < 0) {
        m.set_return(errno == EAFNOSUPPORT ? ERROR_NOT_SUPPORTED : kErrorCannotComplete);
        return;
    }
    struct RouteSocket {
        int fd;
        ~RouteSocket() { ::close(fd); }
    } socket_owner{fd};
    timeval timeout{1, 0};
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        m.set_return(kErrorCannotComplete); // ERROR_CAN_NOT_COMPLETE (winerror.h)
        return;
    }
    struct {
        rt_msghdr header;
        sockaddr_in destination;
    } request{};
    request.header.rtm_msglen = sizeof(request);
    request.header.rtm_version = RTM_VERSION;
    request.header.rtm_type = RTM_GET;
    request.header.rtm_addrs = RTA_DST;
    request.header.rtm_seq = 1;
    request.header.rtm_pid = ::getpid();
    request.destination.sin_len = sizeof(sockaddr_in);
    request.destination.sin_family = AF_INET;
    request.destination.sin_addr.s_addr = destination;
    if (::write(fd, &request, sizeof(request)) != sizeof(request)) {
        m.set_return(kErrorCannotComplete);
        return;
    }
    // A private socket and kernel PID/sequence filtering prevent concurrent
    // callers or routing broadcasts from satisfying the wrong request.
    for (;;) {
        alignas(rt_msghdr) std::array<u8, 4096> response{};
        const auto count = ::read(fd, response.data(), response.size());
        if (count < static_cast<ssize_t>(sizeof(rt_msghdr))) {
            m.set_return(kErrorCannotComplete);
            return;
        }
        const auto* header = reinterpret_cast<const rt_msghdr*>(response.data());
        if (header->rtm_version != RTM_VERSION || header->rtm_type != RTM_GET ||
            header->rtm_pid != request.header.rtm_pid || header->rtm_seq != 1) continue;
        if (header->rtm_errno != 0 || header->rtm_index == 0) {
            m.set_return(kErrorCannotComplete);
            return;
        }
        winapi::set_last_error(NO_ERROR);
        m.set_return(m.write_u32(out, header->rtm_index).is_ok()
            ? NO_ERROR : ERROR_INVALID_PARAMETER);
        return;
    }
#else
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!GetBestInterface: host routing table backend\n");
    });
    m.set_return(ERROR_NOT_SUPPORTED);
#endif
}



// -----------------------------------------------------------------------------
// GetBestRoute2
// -----------------------------------------------------------------------------
//
// Learn (netioapi.h) plus tools/oracle_probe/netio_notify_route_probe
// (x86 = x64):
//   - NULL DestinationAddress, BestRoute or BestSourceAddress, or a destination
//     family other than AF_INET/AF_INET6: ERROR_INVALID_PARAMETER, outputs
//     untouched.
//   - A non-zero InterfaceLuid selects the interface, else a non-zero
//     InterfaceIndex; both zero (or a NULL LUID) means any interface. An
//     unknown interface is ERROR_FILE_NOT_FOUND and no route is
//     ERROR_NETWORK_UNREACHABLE; both write the outputs (zeroed here).
//   - No stack for the destination family: ERROR_NOT_SUPPORTED.
//   - MIB_IPFORWARD_ROW2 is 104 bytes and SOCKADDR_INET 28 on both bitnesses.
//     A gateway route reports Protocol MIB_IPPROTO_NETMGMT, Immortal 1 and
//     Metric 0; an on-link or host route MIB_IPPROTO_LOCAL, Immortal 0 and a
//     non-zero metric; both AutoconfigureAddress 1, Publish 0, Origin
//     NlroManual, a non-zero Age. Route lifetimes are host DHCP/RA state
//     Windows keeps and macOS does not; they are reported as infinite.

namespace {

constexpr u32 kErrorNetworkUnreachable = 1231;   // winerror.h
constexpr usize kSockaddrInetSize = 28;
constexpr usize kForwardRow2Size = 104;
constexpr u32 kMibIpprotoLocal = 2;
constexpr u32 kMibIpprotoNetmgmt = 3;
constexpr u32 kRouteMetricOnLink = 256;   // Windows' default route metric for system routes

struct RouteLookup {
    u32 index = 0;
    int flags = 0;
    iphlp::NetAddress destination;
    u8 prefix_length = 0;
    std::optional<iphlp::NetAddress> gateway;
    std::optional<iphlp::NetAddress> source;
};

std::optional<iphlp::NetAddress> to_net_address(const sockaddr* address) {
    if (address == nullptr) return std::nullopt;
    iphlp::NetAddress result;
    if (address->sa_family == AF_INET && address->sa_len >= sizeof(sockaddr_in)) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        result.family = iphlp::kGuestAfInet;
        std::memcpy(result.bytes.data(), &v4->sin_addr, 4);
        return result;
    }
    if (address->sa_family == AF_INET6 && address->sa_len >= sizeof(sockaddr_in6)) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        result.family = iphlp::kGuestAfInet6;
        std::memcpy(result.bytes.data(), &v6->sin6_addr, 16);
        result.scope_id = v6->sin6_scope_id;
        return result;
    }
    return std::nullopt;
}

/// Prefix length of a routing-socket netmask, which the kernel may truncate
/// after its last non-zero byte.
u8 netmask_prefix_length(const sockaddr* mask, int family) {
    if (mask == nullptr) return family == AF_INET ? 32 : 128;
    const usize offset = family == AF_INET ? offsetof(sockaddr_in, sin_addr)
                                           : offsetof(sockaddr_in6, sin6_addr);
    const usize width = family == AF_INET ? 4 : 16;
    const auto* bytes = reinterpret_cast<const u8*>(mask);
    u8 bits = 0;
    for (usize i = 0; i < width && offset + i < mask->sa_len; ++i) {
        bits += static_cast<u8>(__builtin_popcount(bytes[offset + i]));
    }
    return bits;
}

/// RTM_GET for @p destination, scoped to @p scope_index when non-zero.
/// Returns nullopt with @p no_route set when the kernel has no route.
std::optional<RouteLookup> lookup_route(const iphlp::NetAddress& destination,
                                        u32 scope_index, bool& no_route) {
    no_route = false;
#ifdef __APPLE__
    const int family = destination.is_v4() ? AF_INET : AF_INET6;
    const int fd = ::socket(PF_ROUTE, SOCK_RAW, family);
    if (fd < 0) return std::nullopt;
    struct RouteSocket {
        int fd;
        ~RouteSocket() { ::close(fd); }
    } socket_owner{fd};
    timeval timeout{1, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // The routing message is packed: each sockaddr follows the previous one
    // (rounded to 4 bytes) directly after the 92-byte header. A struct with a
    // sockaddr_storage member would pad it to 96 and the kernel would read
    // the destination as zeros (the default route).
    alignas(rt_msghdr) std::array<u8, 512> buffer{};
    auto& header = *reinterpret_cast<rt_msghdr*>(buffer.data());
    header.rtm_version = RTM_VERSION;
    header.rtm_type = RTM_GET;
    // RTA_IFP asks for the outgoing interface and its address (RTA_IFA).
    header.rtm_addrs = RTA_DST | RTA_IFP;
    header.rtm_seq = 2;
    header.rtm_pid = ::getpid();
    if (scope_index != 0) {
        header.rtm_flags = RTF_IFSCOPE;
        header.rtm_index = static_cast<u16>(scope_index);
    }
    usize length = sizeof(rt_msghdr);
    if (destination.is_v4()) {
        sockaddr_in v4{};
        v4.sin_len = sizeof(sockaddr_in);
        v4.sin_family = AF_INET;
        std::memcpy(&v4.sin_addr, destination.bytes.data(), 4);
        std::memcpy(buffer.data() + length, &v4, sizeof(v4));
        length += sizeof(sockaddr_in);
    } else {
        sockaddr_in6 v6{};
        v6.sin6_len = sizeof(sockaddr_in6);
        v6.sin6_family = AF_INET6;
        std::memcpy(&v6.sin6_addr, destination.bytes.data(), 16);
        v6.sin6_scope_id = destination.scope_id;
        std::memcpy(buffer.data() + length, &v6, sizeof(v6));
        length += sizeof(sockaddr_in6);
    }
    sockaddr_dl link{};
    link.sdl_len = sizeof(sockaddr_dl);
    link.sdl_family = AF_LINK;
    std::memcpy(buffer.data() + length, &link, sizeof(link));
    length += sizeof(sockaddr_dl);
    header.rtm_msglen = static_cast<u16>(length);
    const auto request_pid = header.rtm_pid;
    const auto request_seq = header.rtm_seq;
    if (::write(fd, buffer.data(), length) != static_cast<ssize_t>(length)) {
        no_route = errno == ESRCH || errno == ENETUNREACH || errno == EHOSTUNREACH;
        return std::nullopt;
    }
    for (;;) {
        alignas(rt_msghdr) std::array<u8, 4096> response{};
        const auto count = ::read(fd, response.data(), response.size());
        if (count < static_cast<ssize_t>(sizeof(rt_msghdr))) return std::nullopt;
        const auto* header = reinterpret_cast<const rt_msghdr*>(response.data());
        if (header->rtm_version != RTM_VERSION || header->rtm_type != RTM_GET ||
            header->rtm_pid != request_pid || header->rtm_seq != request_seq) {
            continue;
        }
        if (header->rtm_errno != 0) {
            no_route = true;
            return std::nullopt;
        }
        const sockaddr* addresses[RTAX_MAX] = {};
        const u8* cursor = response.data() + sizeof(rt_msghdr);
        const u8* end = response.data() + std::min<usize>(count, header->rtm_msglen);
        for (int i = 0; i < RTAX_MAX && cursor < end; ++i) {
            if ((header->rtm_addrs & (1 << i)) == 0) continue;
            const auto* address = reinterpret_cast<const sockaddr*>(cursor);
            addresses[i] = address;
            const usize step = address->sa_len == 0
                ? sizeof(u32)
                : 1 + ((static_cast<usize>(address->sa_len) - 1) | (sizeof(u32) - 1));
            cursor += step;
        }
        RouteLookup result;
        result.index = header->rtm_index;
        result.flags = header->rtm_flags;
        const auto route_destination = to_net_address(addresses[RTAX_DST]);
        result.destination = route_destination.value_or(iphlp::NetAddress{
            static_cast<u16>(destination.is_v4() ? iphlp::kGuestAfInet : iphlp::kGuestAfInet6)});
        result.prefix_length = (header->rtm_flags & RTF_HOST) != 0
            ? static_cast<u8>(destination.is_v4() ? 32 : 128)
            : netmask_prefix_length(addresses[RTAX_NETMASK], family);
        if ((header->rtm_flags & RTF_GATEWAY) != 0) {
            result.gateway = to_net_address(addresses[RTAX_GATEWAY]);
        }
        result.source = to_net_address(addresses[RTAX_IFA]);
        return result;
    }
#else
    (void)destination; (void)scope_index;
    return std::nullopt;
#endif
}

void write_sockaddr_inet(Marshaler& m, GuestAddr at, const iphlp::NetAddress& address) {
    std::array<u8, kSockaddrInetSize> image{};
    image[0] = static_cast<u8>(address.family);
    image[1] = static_cast<u8>(address.family >> 8);
    if (address.is_v4()) {
        std::memcpy(image.data() + 4, address.bytes.data(), 4);
    } else {
        std::memcpy(image.data() + 8, address.bytes.data(), 16);
        std::memcpy(image.data() + 24, &address.scope_id, 4);
    }
    (void)m.memory().write_bytes(at, image.data(), image.size());
}

u32 seconds_since_boot() {
    timeval boot{};
    usize size = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (::sysctl(mib, 2, &boot, &size, nullptr, 0) != 0) return 1;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const i64 elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now).count() - boot.tv_sec;
    return elapsed > 0 ? static_cast<u32>(elapsed) : 1;
}

}  // namespace

void IphlpapiManager::impl_GetBestRoute2(Marshaler& m) {
    const GuestAddr luid_ptr = m.arg_ptr(0);
    const u32 interface_index = static_cast<u32>(m.arg(1));
    const GuestAddr source_ptr = m.arg_ptr(2);
    const GuestAddr destination_ptr = m.arg_ptr(3);
    // m.arg(4): AddressSortOptions, "not currently used" (Learn).
    const GuestAddr route_ptr = m.arg_ptr(5);
    const GuestAddr best_source_ptr = m.arg_ptr(6);
    (void)source_ptr;
    if (destination_ptr == 0 || route_ptr == 0 || best_source_ptr == 0 ||
        !m.memory().guest_can_write(route_ptr, kForwardRow2Size) ||
        !m.memory().guest_can_write(best_source_ptr, kSockaddrInetSize)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const u16 family = m.memory().read_u16(destination_ptr).value_or(0);
    iphlp::NetAddress destination;
    destination.family = family;
    if (family == iphlp::kGuestAfInet) {
        (void)m.memory().read_bytes(destination_ptr + 4, destination.bytes.data(), 4);
    } else if (family == iphlp::kGuestAfInet6) {
        (void)m.memory().read_bytes(destination_ptr + 8, destination.bytes.data(), 16);
        destination.scope_id = m.memory().read_u32(destination_ptr + 24).value_or(0);
    } else {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }

    // From here on Windows writes the outputs even when it fails.
    const std::array<u8, kForwardRow2Size> zero_row{};
    (void)m.memory().write_bytes(route_ptr, zero_row.data(), zero_row.size());
    (void)m.memory().write_bytes(best_source_ptr, zero_row.data(), kSockaddrInetSize);

    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const u64 luid = luid_ptr ? m.memory().read_u64(luid_ptr).value_or(0) : 0;
    const iphlp::HostInterface* scope = nullptr;
    if (luid != 0) {
        scope = snapshot.by_luid(luid);
        if (!scope) { m.set_return(ERROR_FILE_NOT_FOUND); return; }
    } else if (interface_index != 0) {
        scope = snapshot.by_index(interface_index);
        if (!scope) { m.set_return(ERROR_FILE_NOT_FOUND); return; }
    }
    const bool stack_present = std::any_of(
        snapshot.interfaces.begin(), snapshot.interfaces.end(), [&](const auto& iface) {
            return std::any_of(iface.unicast.begin(), iface.unicast.end(),
                               [&](const auto& u) { return u.address.family == family; });
        });
    if (!stack_present) {
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }

    bool no_route = false;
    const auto route = lookup_route(destination, scope ? scope->index : 0, no_route);
    if (!route) {
        m.set_return(no_route ? kErrorNetworkUnreachable : kErrorCannotComplete);
        return;
    }
    const iphlp::HostInterface* outgoing = snapshot.by_index(route->index);
    const bool via_gateway = route->gateway.has_value();
    const bool loopback = (outgoing && outgoing->loopback) || (route->flags & RTF_LOCAL) != 0;

    // DestinationPrefix holds the network address under its prefix length.
    iphlp::NetAddress prefix = route->destination;
    for (usize bit = route->prefix_length; bit < (prefix.is_v4() ? 32u : 128u); ++bit) {
        prefix.bytes[bit / 8] &= static_cast<u8>(~(0x80u >> (bit % 8)));
    }
    iphlp::NetAddress next_hop;
    next_hop.family = family;
    if (via_gateway) next_hop = *route->gateway;

    std::array<u8, kForwardRow2Size> row{};
    const auto put32 = [&row](usize at, u32 value) { std::memcpy(row.data() + at, &value, 4); };
    const u64 route_luid = outgoing ? outgoing->luid : 0;
    std::memcpy(row.data() + 0, &route_luid, 8);
    put32(8, route->index);
    (void)m.memory().write_bytes(route_ptr, row.data(), row.size());
    write_sockaddr_inet(m, route_ptr + 12, prefix);
    (void)m.memory().write_u8(route_ptr + 40, route->prefix_length);
    write_sockaddr_inet(m, route_ptr + 44, next_hop);
    (void)m.memory().write_u8(route_ptr + 72, 0);                       // SitePrefixLength
    (void)m.memory().write_u32(route_ptr + 76, 0xFFFFFFFFu);            // ValidLifetime
    (void)m.memory().write_u32(route_ptr + 80, 0xFFFFFFFFu);            // PreferredLifetime
    (void)m.memory().write_u32(route_ptr + 84, via_gateway ? 0 : kRouteMetricOnLink);
    (void)m.memory().write_u32(route_ptr + 88,
                               via_gateway ? kMibIpprotoNetmgmt : kMibIpprotoLocal);
    (void)m.memory().write_u8(route_ptr + 92, loopback ? 1 : 0);        // Loopback
    (void)m.memory().write_u8(route_ptr + 93, 1);                       // AutoconfigureAddress
    (void)m.memory().write_u8(route_ptr + 94, 0);                       // Publish
    (void)m.memory().write_u8(route_ptr + 95, via_gateway ? 1 : 0);     // Immortal
    (void)m.memory().write_u32(route_ptr + 96, seconds_since_boot());   // Age
    (void)m.memory().write_u32(route_ptr + 100, 0);                     // NlroManual

    iphlp::NetAddress best_source;
    best_source.family = family;
    if (route->source && route->source->family == family) {
        best_source = *route->source;
    } else if (outgoing) {
        for (const auto& u : outgoing->unicast) {
            if (u.address.family == family) { best_source = u.address; break; }
        }
    }
    write_sockaddr_inet(m, best_source_ptr, best_source);
    m.set_return(NO_ERROR);
}

} // namespace vitreo::winapi
