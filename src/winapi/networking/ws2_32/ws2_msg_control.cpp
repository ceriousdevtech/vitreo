// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_msg_control.cpp
/// @brief WSAMSG control data (WSACMSGHDR) in both directions, and the
///        NTSTATUS <-> Winsock error mapping of overlapped completions.
///
/// Windows and Darwin agree on what ancillary data means but not on how it is
/// spelled: different option numbers (IP_PKTINFO is 19 on Windows, 26 on
/// Darwin), different payloads (Windows reports the TOS byte as an INT under
/// IP_TOS, Darwin as a u_char under IP_RECVTOS), and a different header
/// (WSACMSGHDR carries a SIZE_T length, so it is 16 bytes on x64 and 12 on
/// x86). Every rule below is measured by tools/oracle_probe/ws2_msg_probe.c
/// (ws2_msg_probe.expected / .x86.expected) unless it cites Learn.

// IPV6_RECVPKTINFO / IPV6_PKTINFO / IPV6_HOPLIMIT / IPV6_TCLASS: RFC 3542 names.
#define __APPLE_USE_RFC_3542 1
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

namespace vitreo::winapi {

namespace {

// Windows option numbers (SDK ws2ipdef.h / ws2def.h).
constexpr int kWinIpprotoIp = 0;
constexpr int kWinIpprotoIpv6 = 41;
constexpr int kWinIpprotoUdp = 17;
constexpr int kWinIpTos = 3;
constexpr int kWinIpTtl = 4;
constexpr int kWinIpPktinfo = 19;
constexpr int kWinIpHoplimit = 21;
constexpr int kWinIpRecvif = 24;
constexpr int kWinIpRecvdstaddr = 25;
constexpr int kWinIpRecvtos = 40;
constexpr int kWinIpOriginalArrivalIf = 47;
constexpr int kWinIpEcn = 50;  // IP_ECN == IP_RECVECN
constexpr int kWinIpv6Pktinfo = 19;
constexpr int kWinIpv6Hoplimit = 21;
constexpr int kWinIpv6Recvif = 24;
constexpr int kWinIpv6Recvdstaddr = 25;
constexpr int kWinIpv6Tclass = 39;
constexpr int kWinIpv6Recvtclass = 40;
constexpr int kWinIpv6Ecn = 50;  // IPV6_ECN == IPV6_RECVECN
constexpr int kWinUdpSendMsgSize = 2;  // UDP_SEND_MSG_SIZE (ws2ipdef.h)

struct WinCmsgLayout {
    u32 header = 0;  // sizeof(WSACMSGHDR) rounded by WSA_CMSGDATA_ALIGN
    u32 align = 0;   // WSA_CMSGHDR_ALIGN / WSA_CMSGDATA_ALIGN
};
WinCmsgLayout cmsg_layout(bool is64) {
    // ws2def.h: WSACMSGHDR { SIZE_T cmsg_len; INT cmsg_level; INT cmsg_type; }
    // aligned to TYPE_ALIGNMENT(WSACMSGHDR) (ws2_msg_probe bits: cmsghdr=16,
    // cmsg_space_int=24 on x64; 12 / 16 on x86).
    return is64 ? WinCmsgLayout{16u, 8u} : WinCmsgLayout{12u, 4u};
}
u32 align_up(u32 v, u32 a) { return (v + a - 1u) & ~(a - 1u); }

void append_cmsg(std::vector<u8>& image, bool is64, int level, int type,
                 const void* data, u32 data_len) {
    const WinCmsgLayout l = cmsg_layout(is64);
    const u32 len = l.header + data_len;
    const u32 space = align_up(l.header + align_up(data_len, l.align), l.align);
    const size_t at = image.size();
    image.resize(at + space, 0);
    u8* p = image.data() + at;
    if (is64) {
        const u64 len64 = len;
        std::memcpy(p, &len64, 8);
        std::memcpy(p + 8, &level, 4);
        std::memcpy(p + 12, &type, 4);
    } else {
        std::memcpy(p, &len, 4);
        std::memcpy(p + 4, &level, 4);
        std::memcpy(p + 8, &type, 4);
    }
    if (data_len != 0) std::memcpy(p + l.header, data, data_len);
}

void append_int(std::vector<u8>& image, bool is64, int level, int type, i32 v) {
    append_cmsg(image, is64, level, type, &v, 4);
}

bool is_v4_mapped(const in6_addr& a) { return IN6_IS_ADDR_V4MAPPED(&a) != 0; }

} // namespace

u32 ws2_recv_ctl_bit(int level, int opt) {
    if (level == kWinIpprotoIp) {
        switch (opt) {
        case kWinIpPktinfo: return ws2ctl::kV4Pktinfo;
        case kWinIpHoplimit: return ws2ctl::kV4Hoplimit;
        case kWinIpRecvif: return ws2ctl::kV4Recvif;
        case kWinIpRecvdstaddr: return ws2ctl::kV4Dstaddr;
        case kWinIpRecvtos: return ws2ctl::kV4Tos;
        case kWinIpOriginalArrivalIf: return ws2ctl::kV4ArrivalIf;
        case kWinIpEcn: return ws2ctl::kV4Ecn;
        default: return 0;
        }
    }
    if (level == kWinIpprotoIpv6) {
        switch (opt) {
        case kWinIpv6Pktinfo: return ws2ctl::kV6Pktinfo;
        case kWinIpv6Hoplimit: return ws2ctl::kV6Hoplimit;
        case kWinIpv6Recvif: return ws2ctl::kV6Recvif;
        case kWinIpv6Recvdstaddr: return ws2ctl::kV6Dstaddr;
        case kWinIpv6Recvtclass: return ws2ctl::kV6Tclass;
        case kWinIpv6Ecn: return ws2ctl::kV6Ecn;
        default: return 0;
        }
    }
    return 0;
}

u32 ws2_apply_recv_ctl_host(int fd, int host_family, u32 mask) {
    auto set = [fd](int level, int opt, bool on) -> bool {
        const int v = on ? 1 : 0;
        return ::setsockopt(fd, level, opt, &v, sizeof(v)) == 0;
    };
    constexpr u32 kV4Where = ws2ctl::kV4Pktinfo | ws2ctl::kV4Recvif |
                             ws2ctl::kV4Dstaddr | ws2ctl::kV4ArrivalIf;
    constexpr u32 kV6Where = ws2ctl::kV6Pktinfo | ws2ctl::kV6Recvif |
                             ws2ctl::kV6Dstaddr;
    if (host_family == AF_INET) {
        // IP_RECVPKTINFO carries both the destination address and the arrival
        // interface, which is every "where" option Windows offers.
        if (!set(IPPROTO_IP, IP_RECVPKTINFO, (mask & kV4Where) != 0) ||
            !set(IPPROTO_IP, IP_RECVTTL, (mask & ws2ctl::kV4Hoplimit) != 0) ||
            !set(IPPROTO_IP, IP_RECVTOS,
                 (mask & (ws2ctl::kV4Tos | ws2ctl::kV4Ecn)) != 0)) {
            return Ws2Manager::errno_to_wsa(errno);
        }
        return 0;
    }
    if (host_family == AF_INET6) {
        // A dual-stack socket takes the IP-level options too (Windows applies
        // them to v4-mapped traffic: ws2_msg_probe ro6_IP_RECVTOS_from4). Darwin
        // rejects IP-level options on an AF_INET6 socket but reports v4-mapped
        // traffic through the IPv6 ones (the TOS byte arrives as IPV6_TCLASS),
        // so the IPv6 options serve both. PKTINFO is always on while any option
        // is, because it is what tells v4-mapped from native IPv6 traffic.
        const bool any = mask != 0;
        const bool hop = (mask & (ws2ctl::kV4Hoplimit | ws2ctl::kV6Hoplimit)) != 0;
        const bool tclass = (mask & (ws2ctl::kV4Tos | ws2ctl::kV4Ecn |
                                     ws2ctl::kV6Tclass | ws2ctl::kV6Ecn)) != 0;
        (void)kV6Where;
        if (!set(IPPROTO_IPV6, IPV6_RECVPKTINFO, any) ||
            !set(IPPROTO_IPV6, IPV6_RECVHOPLIMIT, hop) ||
            !set(IPPROTO_IPV6, IPV6_RECVTCLASS, tclass)) {
            return Ws2Manager::errno_to_wsa(errno);
        }
        return 0;
    }
    return WSAEINVAL;
}

std::vector<u8> ws2_build_recv_control(const struct msghdr& native, u32 mask,
                                       bool is64, int host_family) {
    std::vector<u8> image;
    if (mask == 0 || native.msg_control == nullptr) return image;

    bool have_dst4 = false, have_dst6 = false, have_ttl = false, have_tos = false;
    bool have_if = false;
    in_addr dst4{};
    in6_addr dst6{};
    u32 ifindex = 0;
    i32 ttl = 0;
    i32 tos = 0;
    for (const struct cmsghdr* c = CMSG_FIRSTHDR(&native); c != nullptr;
         c = CMSG_NXTHDR(const_cast<struct msghdr*>(&native),
                         const_cast<struct cmsghdr*>(c))) {
        const size_t dl = c->cmsg_len > CMSG_LEN(0) ? c->cmsg_len - CMSG_LEN(0) : 0;
        const u8* d = CMSG_DATA(c);
        if (c->cmsg_level == IPPROTO_IP) {
            if (c->cmsg_type == IP_PKTINFO && dl >= sizeof(in_pktinfo)) {
                in_pktinfo pi{};
                std::memcpy(&pi, d, sizeof(pi));
                dst4 = pi.ipi_addr;
                ifindex = pi.ipi_ifindex;
                have_dst4 = have_if = true;
            } else if (c->cmsg_type == IP_RECVTTL && dl >= 1) {
                ttl = d[0];
                have_ttl = true;
            } else if (c->cmsg_type == IP_RECVTOS && dl >= 1) {
                tos = d[0];
                have_tos = true;
            }
        } else if (c->cmsg_level == IPPROTO_IPV6) {
            if (c->cmsg_type == IPV6_PKTINFO && dl >= sizeof(in6_pktinfo)) {
                in6_pktinfo pi{};
                std::memcpy(&pi, d, sizeof(pi));
                dst6 = pi.ipi6_addr;
                ifindex = pi.ipi6_ifindex;
                have_dst6 = have_if = true;
            } else if (c->cmsg_type == IPV6_HOPLIMIT && dl >= sizeof(int)) {
                std::memcpy(&ttl, d, sizeof(int));
                have_ttl = true;
            } else if (c->cmsg_type == IPV6_TCLASS && dl >= sizeof(int)) {
                std::memcpy(&tos, d, sizeof(int));
                tos &= 0xFF;
                have_tos = true;
            }
        }
    }

    // Which family the datagram is, as Windows reports it: a v4-mapped
    // arrival on a dual-stack socket gets the IP-level set.
    bool v4 = host_family == AF_INET;
    if (!v4 && have_dst6 && is_v4_mapped(dst6)) {
        v4 = true;
        std::memcpy(&dst4, &dst6.s6_addr[12], 4);
        have_dst4 = true;
    }

    // Order measured by ws2_msg_probe all4_recv / all6_recv_from6:
    // PKTINFO, RECVDSTADDR, ORIGINAL_ARRIVAL_IF, RECVIF, ECN, TTL/HOPLIMIT, TOS/TCLASS.
    if (v4) {
        if ((mask & ws2ctl::kV4Pktinfo) != 0 && have_dst4) {
            u8 pk[8];  // IN_PKTINFO { IN_ADDR ipi_addr; ULONG ipi_ifindex; }
            std::memcpy(pk, &dst4, 4);
            std::memcpy(pk + 4, &ifindex, 4);
            append_cmsg(image, is64, kWinIpprotoIp, kWinIpPktinfo, pk, 8);
        }
        if ((mask & ws2ctl::kV4Dstaddr) != 0 && have_dst4) {
            append_cmsg(image, is64, kWinIpprotoIp, kWinIpRecvdstaddr, &dst4, 4);
        }
        if ((mask & ws2ctl::kV4ArrivalIf) != 0 && have_if) {
            append_cmsg(image, is64, kWinIpprotoIp, kWinIpOriginalArrivalIf, &ifindex, 4);
        }
        if ((mask & ws2ctl::kV4Recvif) != 0 && have_if) {
            append_cmsg(image, is64, kWinIpprotoIp, kWinIpRecvif, &ifindex, 4);
        }
        if ((mask & ws2ctl::kV4Ecn) != 0 && have_tos) {
            append_int(image, is64, kWinIpprotoIp, kWinIpEcn, tos & 3);
        }
        // IP_HOPLIMIT is reported under IP_TTL (ws2_msg_probe ro4_recv_IP_HOPLIMIT: 0/4).
        if ((mask & ws2ctl::kV4Hoplimit) != 0 && have_ttl) {
            append_int(image, is64, kWinIpprotoIp, kWinIpTtl, ttl);
        }
        // IP_RECVTOS is reported under IP_TOS, the whole byte as an INT.
        if ((mask & ws2ctl::kV4Tos) != 0 && have_tos) {
            append_int(image, is64, kWinIpprotoIp, kWinIpTos, tos);
        }
        return image;
    }
    if ((mask & ws2ctl::kV6Pktinfo) != 0 && have_dst6) {
        u8 pk[20];  // IN6_PKTINFO { IN6_ADDR ipi6_addr; ULONG ipi6_ifindex; }
        std::memcpy(pk, &dst6, 16);
        std::memcpy(pk + 16, &ifindex, 4);
        append_cmsg(image, is64, kWinIpprotoIpv6, kWinIpv6Pktinfo, pk, 20);
    }
    if ((mask & ws2ctl::kV6Dstaddr) != 0 && have_dst6) {
        append_cmsg(image, is64, kWinIpprotoIpv6, kWinIpv6Recvdstaddr, &dst6, 16);
    }
    if ((mask & ws2ctl::kV6Recvif) != 0 && have_if) {
        append_cmsg(image, is64, kWinIpprotoIpv6, kWinIpv6Recvif, &ifindex, 4);
    }
    if ((mask & ws2ctl::kV6Ecn) != 0 && have_tos) {
        append_int(image, is64, kWinIpprotoIpv6, kWinIpv6Ecn, tos & 3);
    }
    if ((mask & ws2ctl::kV6Hoplimit) != 0 && have_ttl) {
        append_int(image, is64, kWinIpprotoIpv6, kWinIpv6Hoplimit, ttl);
    }
    if ((mask & ws2ctl::kV6Tclass) != 0 && have_tos) {
        append_int(image, is64, kWinIpprotoIpv6, kWinIpv6Tclass, tos);
    }
    return image;
}

Ws2ControlPublish ws2_publish_recv_control(GuestMemory& mem, bool is64,
                                           GuestAddr buf, u32 cap,
                                           const std::vector<u8>& image) {
    Ws2ControlPublish out;
    out.control_len = static_cast<u32>(image.size());
    if (image.empty()) return out;
    if (buf != 0 && image.size() <= cap) {
        (void)mem.write_bytes(buf, image.data(), image.size());
        return out;
    }
    // Too small (ws2_msg_probe ctl2_cap_*): Control.len reports the full
    // size and MSG_CTRUNC is raised. x64 receives a byte prefix of the image;
    // x86 (the WOW64 thunk) receives each header and each payload only when
    // it fits whole.
    out.truncated = true;
    if (buf == 0 || cap == 0) return out;
    if (is64) {
        (void)mem.write_bytes(buf, image.data(), cap);
        return out;
    }
    const WinCmsgLayout l = cmsg_layout(false);
    u32 off = 0;
    while (off + l.header <= image.size()) {
        u32 len = 0;
        std::memcpy(&len, image.data() + off, 4);
        if (off + l.header > cap) break;
        (void)mem.write_bytes(buf + off, image.data() + off, l.header);
        const u32 data_len = len - l.header;
        if (off + l.header + data_len > cap) break;
        if (data_len != 0) {
            (void)mem.write_bytes(buf + off + l.header,
                                  image.data() + off + l.header, data_len);
        }
        off += align_up(len, l.align);
    }
    return out;
}

u32 ws2_translate_send_control(GuestMemory& mem, bool is64, GuestAddr buf,
                               u32 len, int fd, int host_family,
                               const struct sockaddr* dest,
                               Ws2SendControl* out) {
    out->native.clear();
    out->segment = 0;
    const WinCmsgLayout l = cmsg_layout(is64);
    // A control block shorter than one header: x64 ignores it, the x86
    // (WOW64) path rejects it (ws2_msg_probe snd4_ctl_len_4: rc=0 on x64,
    // err=10022 in the .x86.expected).
    if (len < l.header) return (is64 || len == 0) ? 0u : WSAEINVAL;
    if (buf == 0) return WSAEFAULT;  // snd4_ctl_null_buf
    std::vector<u8> raw(len);
    if (mem.read_bytes(buf, raw.data(), len).is_err()) return WSAEFAULT;

    // The destination family decides which level is legal: IP-level control
    // for IPv4 (including v4-mapped through a dual-stack socket), IPv6-level
    // for native IPv6 (ws2_msg_probe v4_send_ipv6ecn_on_v4, v6_send_ipecn_on_v6,
    // v6mapped_send_ipv6ecn / v6mapped_send_ipecn).
    bool v4 = host_family == AF_INET;
    sockaddr_storage peer{};
    if (dest == nullptr) {
        socklen_t pl = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0) {
            dest = reinterpret_cast<const sockaddr*>(&peer);
        }
    }
    if (!v4 && dest != nullptr && dest->sa_family == AF_INET6 &&
        is_v4_mapped(reinterpret_cast<const sockaddr_in6*>(dest)->sin6_addr)) {
        v4 = true;
    }

    bool have_tos = false, have_ecn = false, have_pkt4 = false, have_pkt6 = false;
    bool have_hop6 = false;
    i32 tos = 0, ecn = 0, hop6 = 0;
    u8 pkt4[8]{};
    u8 pkt6[20]{};
    u32 off = 0;
    while (off + l.header <= len) {
        u64 clen = 0;
        i32 level = 0, type = 0;
        if (is64) {
            std::memcpy(&clen, raw.data() + off, 8);
            std::memcpy(&level, raw.data() + off + 8, 4);
            std::memcpy(&type, raw.data() + off + 12, 4);
        } else {
            u32 c32 = 0;
            std::memcpy(&c32, raw.data() + off, 4);
            clen = c32;
            std::memcpy(&level, raw.data() + off + 4, 4);
            std::memcpy(&type, raw.data() + off + 8, 4);
        }
        // Malformed headers, including zero padding after the last message
        // (snd4_ctl_trailing_zeros / cmsg_len_short / cmsg_len_long).
        if (clen < l.header || off + clen > len) return WSAEINVAL;
        const u32 dl = static_cast<u32>(clen) - l.header;
        const u8* d = raw.data() + off + l.header;
        i32 iv = 0;
        const bool is_int = dl >= 4;
        if (is_int) std::memcpy(&iv, d, 4);
        if (level == kWinIpprotoIp && v4 && type == kWinIpEcn) {
            // ECN codepoints 0..2 only; CE (3) is refused (snd4_IP_ECN_3).
            if (!is_int || iv < 0 || iv > 2) return WSAEINVAL;
            ecn = iv;
            have_ecn = true;
        } else if (level == kWinIpprotoIp && v4 && type == kWinIpTos) {
            if (!is_int || iv < 0 || iv > 255) return WSAEINVAL;
            tos = iv;
            have_tos = true;
        } else if (level == kWinIpprotoIp && v4 && type == kWinIpPktinfo) {
            if (dl < 8) return WSAEINVAL;
            std::memcpy(pkt4, d, 8);
            have_pkt4 = true;
        } else if (level == kWinIpprotoIp && v4 && type == kWinIpHoplimit) {
            // Windows sets this datagram's TTL (hop4_recv: 9). Darwin has no
            // per-datagram IPv4 TTL, so it cannot be honoured.
            if (!is_int) return WSAEINVAL;
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED ws2_32!WSASendMsg: IP_HOPLIMIT "
                             "control message (per-datagram IPv4 TTL)\n");
            }
            return WSAEINVAL;
        } else if (level == kWinIpprotoIpv6 && !v4 && type == kWinIpv6Ecn) {
            if (!is_int || iv < 0 || iv > 2) return WSAEINVAL;
            ecn = iv;
            have_ecn = true;
        } else if (level == kWinIpprotoIpv6 && !v4 && type == kWinIpv6Tclass) {
            if (!is_int || iv < 0 || iv > 255) return WSAEINVAL;
            tos = iv;
            have_tos = true;
        } else if (level == kWinIpprotoIpv6 && !v4 && type == kWinIpv6Pktinfo) {
            if (dl < 20) return WSAEINVAL;
            std::memcpy(pkt6, d, 20);
            have_pkt6 = true;
        } else if (level == kWinIpprotoIpv6 && !v4 && type == kWinIpv6Hoplimit) {
            if (!is_int || iv < -1 || iv > 255) return WSAEINVAL;
            hop6 = iv;
            have_hop6 = true;
        } else if (level == kWinIpprotoUdp && type == kWinUdpSendMsgSize) {
            // UDP_SEND_MSG_SIZE: send the payload as datagrams of this size
            // (uso_recv: 10 bytes by 4 -> 4 4 2; by 0 -> one datagram).
            if (!is_int) return WSAEINVAL;
            out->segment = static_cast<u32>(iv);
        } else {
            return WSAEINVAL;  // v4_send_unknown_cmsg and wrong-family levels
        }
        const u32 next = static_cast<u32>(align_up(static_cast<u32>(clen), l.align));
        if (next == 0) break;
        off += next;
    }

    auto push = [out](int level, int type, const void* data, size_t n) {
        const size_t at = out->native.size();
        out->native.resize(at + CMSG_SPACE(n), 0);
        auto* c = reinterpret_cast<struct cmsghdr*>(out->native.data() + at);
        c->cmsg_len = static_cast<socklen_t>(CMSG_LEN(n));
        c->cmsg_level = level;
        c->cmsg_type = type;
        std::memcpy(CMSG_DATA(c), data, n);
    };

    if (have_ecn || have_tos) {
        // ECN replaces the two low bits of the TOS byte: the socket's IP_TOS
        // (so_recv_ecn1_after_IP_TOS: 8|1 = 9) or an IP_TOS control message
        // in the same send (snd4_tos_then_ecn_recv: 0x20|2 = 0x22).
        if (!have_tos) {
            int cur = 0;
            socklen_t cl = sizeof(cur);
            if (host_family == AF_INET) {
                (void)::getsockopt(fd, IPPROTO_IP, IP_TOS, &cur, &cl);
            } else {
                (void)::getsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &cur, &cl);
            }
            tos = cur < 0 ? 0 : (cur & 0xFF);
        }
        if (have_ecn) tos = (tos & ~3) | ecn;
        const int v = tos;
        if (v4) {
            // Darwin takes IP_TOS as a per-datagram int, on an AF_INET socket and
            // on a dual-stack socket sending to a v4-mapped address alike.
            push(IPPROTO_IP, IP_TOS, &v, sizeof(v));
        } else {
            push(IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v));
        }
    }
    if (have_pkt4) {
        in_addr src{};
        u32 ifx = 0;
        std::memcpy(&src, pkt4, 4);
        std::memcpy(&ifx, pkt4 + 4, 4);
        if (host_family == AF_INET) {
            // Darwin selects the source from ipi_spec_dst (Windows' ipi_addr).
            in_pktinfo pi{};
            pi.ipi_ifindex = ifx;
            pi.ipi_spec_dst = src;
            push(IPPROTO_IP, IP_PKTINFO, &pi, sizeof(pi));
        } else {
            in6_pktinfo pi{};
            pi.ipi6_addr.s6_addr[10] = 0xFF;
            pi.ipi6_addr.s6_addr[11] = 0xFF;
            std::memcpy(&pi.ipi6_addr.s6_addr[12], &src, 4);
            pi.ipi6_ifindex = ifx;
            push(IPPROTO_IPV6, IPV6_PKTINFO, &pi, sizeof(pi));
        }
    }
    if (have_pkt6) {
        in6_pktinfo pi{};
        std::memcpy(&pi.ipi6_addr, pkt6, 16);
        std::memcpy(&pi.ipi6_ifindex, pkt6 + 16, 4);
        push(IPPROTO_IPV6, IPV6_PKTINFO, &pi, sizeof(pi));
    }
    if (have_hop6) {
        const int v = hop6;
        push(IPPROTO_IPV6, IPV6_HOPLIMIT, &v, sizeof(v));
    }
    return 0;
}

u32 ws2_wsa_error_to_ntstatus(u32 wsa) {
    // The status an overlapped completion leaves in OVERLAPPED.Internal for a
    // Winsock error: the inverse of ws2_ntstatus_to_wsa_error, choosing the
    // status Windows itself produces where the probe observed one
    // (ws2_ovres_probe gor_reset / gor_cancelled / gor_refused / gor_trunc /
    // closed_under_recv).
    switch (wsa) {
    case 0: return STATUS_SUCCESS;
    case WSAEMSGSIZE: return 0x80000005u;        // STATUS_BUFFER_OVERFLOW
    case WSAEOPNOTSUPP: return 0xC00000BBu;      // STATUS_NOT_SUPPORTED
    case WSAEFAULT: return 0xC0000005u;          // STATUS_ACCESS_VIOLATION
    case WSAENOTSOCK: return 0xC0000008u;        // STATUS_INVALID_HANDLE
    case WSAEINVAL: return 0xC000000Du;          // STATUS_INVALID_PARAMETER
    case WSAENETDOWN: return 0xC00000C4u;        // STATUS_UNEXPECTED_NETWORK_ERROR
    case WSAENOBUFS: return 0xC000009Au;         // STATUS_INSUFFICIENT_RESOURCES
    case WSAEACCES: return 0xC0000022u;          // STATUS_ACCESS_DENIED
    case WSAESHUTDOWN: return 0xC00000B0u;       // STATUS_PIPE_DISCONNECTED
    case WSAETIMEDOUT: return 0xC00000B5u;       // STATUS_IO_TIMEOUT
    case WSA_OPERATION_ABORTED: return STATUS_CANCELLED;
    case WSAECONNABORTED: return 0xC0000241u;    // STATUS_CONNECTION_ABORTED
    case WSAECONNRESET: return 0xC000020Du;      // STATUS_CONNECTION_RESET
    case WSAENOTCONN: return 0xC0000140u;        // STATUS_INVALID_CONNECTION
    case WSAEADDRNOTAVAIL: return 0xC0000207u;   // STATUS_INVALID_ADDRESS_COMPONENT
    case WSAEADDRINUSE: return 0xC000020Au;      // STATUS_ADDRESS_ALREADY_EXISTS
    case WSAECONNREFUSED: return 0xC0000236u;    // STATUS_CONNECTION_REFUSED
    case 10101u: return 0xC0000237u;             // WSAEDISCON: STATUS_GRACEFUL_DISCONNECT
    case WSAEISCONN: return 0xC000023Bu;         // STATUS_CONNECTION_ACTIVE
    case WSAENETUNREACH: return 0xC000023Cu;     // STATUS_NETWORK_UNREACHABLE
    case WSAEHOSTUNREACH: return 0xC000023Du;    // STATUS_HOST_UNREACHABLE
    case WSAENETRESET: return 0xC000A012u;       // STATUS_HOPLIMIT_EXCEEDED
    default: break;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED ws2_32!overlapped completion: no NTSTATUS "
                     "for Winsock error %u (reported as WSAEINVAL)\n", wsa);
    }
    return 0xC000000Du;
}

u32 ws2_ntstatus_to_wsa_error(u32 status) {
    // WSAGetOverlappedResult's mapping, measured status by status
    // (ws2_ovres_probe map: lines). Unlisted failures map to WSAEINVAL, as
    // STATUS_UNSUCCESSFUL does.
    switch (status) {
    case STATUS_SUCCESS: return 0;
    case 0x00000102u: return 1460;               // STATUS_TIMEOUT -> ERROR_TIMEOUT
    case 0x40000000u: return 698;                // -> ERROR_OBJECT_NAME_EXISTS
    case 0x80000005u: return WSAEMSGSIZE;
    case 0xC0000002u: return WSAEOPNOTSUPP;
    case 0xC0000005u: return WSAEFAULT;
    case 0xC0000008u: return WSAENOTSOCK;
    case 0xC000000Eu: return WSAENETDOWN;
    case 0xC0000017u: return WSAENOBUFS;
    case 0xC0000022u: return WSAEACCES;
    case 0xC0000023u: return WSAEFAULT;
    case 0xC000009Au: return WSAENOBUFS;
    case 0xC00000B0u: return WSAESHUTDOWN;
    case 0xC00000B5u: return WSAETIMEDOUT;
    case 0xC00000BBu: return WSAEOPNOTSUPP;
    case 0xC00000BFu: return WSAENETDOWN;
    case 0xC00000C4u: return WSAENETDOWN;
    case 0xC0000120u: return WSA_OPERATION_ABORTED;
    case 0xC000013Bu: return WSAECONNABORTED;
    case 0xC000013Cu: return WSAECONNRESET;
    case 0xC0000140u: return WSAENOTCONN;
    case 0xC0000206u: return WSAEMSGSIZE;
    case 0xC0000207u: return WSAEADDRNOTAVAIL;
    case 0xC0000209u: return WSAENOBUFS;
    case 0xC000020Au: return WSAEADDRINUSE;
    case 0xC000020Cu: return WSAECONNRESET;
    case 0xC000020Du: return WSAECONNRESET;
    case 0xC0000236u: return WSAECONNREFUSED;
    case 0xC0000237u: return 10101u;             // WSAEDISCON
    case 0xC000023Bu: return WSAEISCONN;
    case 0xC000023Cu: return WSAENETUNREACH;
    case 0xC000023Du: return WSAEHOSTUNREACH;
    case 0xC000023Eu: return WSAENETUNREACH;
    case 0xC000023Fu: return WSAECONNRESET;
    case 0xC0000241u: return WSAECONNABORTED;
    case 0xC000A012u: return WSAENETRESET;
    default: break;
    }
    return static_cast<i32>(status) >= 0 ? 0u : WSAEINVAL;
}

} // namespace vitreo::winapi
