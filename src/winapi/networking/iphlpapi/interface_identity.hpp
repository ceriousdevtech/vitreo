#pragma once
/// @file interface_identity.hpp
/// @brief Header-only interface identity shared by IP Helper and the Network
///        List Manager (ole32), which sits below the networking module.
///
/// Windows reports one GUID per adapter everywhere: IP_ADAPTER_ADDRESSES.
/// AdapterName, MIB_IF_ROW2.InterfaceGuid, ConvertInterfaceLuidToGuid and
/// INetworkConnection::GetAdapterId (tools/oracle_probe/iphlp_adapters_probe
/// gaa-identity, ift2 guid_eq_adaptername).

#include <vitreo/common.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <net/route.h>
#include <netinet/in.h>
#endif

namespace vitreo::winapi::iphlp {

/// Stable GUID (struct byte order) for a host interface name: a name-based
/// UUID (RFC 4122 section 4.3 layout over SHA-256), identical across launches
/// and reboots for the same interface.
[[nodiscard]] inline std::array<u8, 16> interface_guid_for_name(std::string_view bsd_name) {
    std::string input = "vitreo.netif:";
    input.append(bsd_name.data(), bsd_name.size());
    std::array<u8, 32> digest{};
#ifdef __APPLE__
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), digest.data());
#else
    u64 h = 1469598103934665603ull;
    for (char c : input) { h ^= static_cast<u8>(c); h *= 1099511628211ull; }
    std::memcpy(digest.data(), &h, 8);
#endif
    std::array<u8, 16> wire{};
    std::memcpy(wire.data(), digest.data(), 16);
    wire[6] = static_cast<u8>((wire[6] & 0x0F) | 0x50);  // version 5 (name-based)
    wire[8] = static_cast<u8>((wire[8] & 0x3F) | 0x80);  // RFC 4122 variant
    // Wire order -> struct order: Data1/Data2/Data3 are little-endian in memory.
    return {wire[3], wire[2], wire[1], wire[0], wire[5], wire[4], wire[7], wire[6],
            wire[8], wire[9], wire[10], wire[11], wire[12], wire[13], wire[14], wire[15]};
}

/// Name of the interface that carries the IPv4 default route (a kernel
/// routing-table RTM_GET for 0.0.0.0; nothing is transmitted).
[[nodiscard]] inline std::optional<std::string> default_route_interface() {
#ifdef __APPLE__
    const int fd = ::socket(PF_ROUTE, SOCK_RAW, AF_INET);
    if (fd < 0) return std::nullopt;
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
    timeval timeout{1, 0};
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return std::nullopt;
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
    if (::write(fd, &request, sizeof(request)) != static_cast<ssize_t>(sizeof(request))) return std::nullopt;
    for (int reads = 0; reads < 64; ++reads) {
        alignas(rt_msghdr) std::array<u8, 2048> response{};
        const ssize_t count = ::read(fd, response.data(), response.size());
        if (count < static_cast<ssize_t>(sizeof(rt_msghdr))) return std::nullopt;
        const auto* header = reinterpret_cast<const rt_msghdr*>(response.data());
        if (header->rtm_type != RTM_GET || header->rtm_pid != request.header.rtm_pid || header->rtm_seq != 1) continue;
        if (header->rtm_errno != 0 || header->rtm_index == 0) return std::nullopt;
        char name[IF_NAMESIZE] = {};
        if (!::if_indextoname(header->rtm_index, name)) return std::nullopt;
        return std::string(name);
    }
#endif
    return std::nullopt;
}

}  // namespace vitreo::winapi::iphlp
