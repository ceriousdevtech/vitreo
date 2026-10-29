// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32_resolve.cpp
/// @brief Winsock name resolution and address-string conversion.
///
/// getaddrinfo, gethostbyname, gethostbyaddr, getservbyname, and the
/// inet_ntop/inet_pton pair. These share a shape that separates them from the
/// rest of Winsock: each takes a name or a textual address and produces guest
/// memory describing an address, rather than operating on a socket.
///
/// The awkward part is lifetime. getaddrinfo hands the guest a linked list it
/// will later pass to freeaddrinfo, so the addresses are allocated from a
/// guest-visible arena the manager owns rather than on the host heap.
///
/// Split out of ws2_32.cpp, which was 6,842 lines.

#include "ws2_32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <CommonCrypto/CommonDigest.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <strings.h>
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"

namespace vitreo::winapi {

namespace {

/// Host-side resolver results, possibly several host getaddrinfo chains.
struct HostAiList {
    std::vector<struct addrinfo*> roots;
    std::vector<const struct addrinfo*> entries;
    HostAiList() = default;
    HostAiList(const HostAiList&) = delete;
    HostAiList& operator=(const HostAiList&) = delete;
    ~HostAiList() {
        for (auto* r : roots) ::freeaddrinfo(r);
    }
};

/// The machine's registered addresses: IPv6 first, then IPv4, excluding
/// loopback (ws2_resolve_probe gai_empty_node: has_loopback4=0). Falls back to
/// loopback when the host has no other address up.
std::vector<std::string> local_address_strings() {
    std::vector<std::string> v6, v4;
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) == 0) {
        for (const struct ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr == nullptr || (i->ifa_flags & IFF_UP) == 0 ||
                (i->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }
            char buf[INET6_ADDRSTRLEN + IFNAMSIZ + 2] = {};
            if (i->ifa_addr->sa_family == AF_INET6) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(i->ifa_addr);
                if (::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf)) == nullptr) continue;
                std::string text = buf;
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
                    text += "%";
                    text += i->ifa_name;
                }
                v6.push_back(text);
            } else if (i->ifa_addr->sa_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(i->ifa_addr);
                if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == nullptr) continue;
                v4.push_back(buf);
            }
        }
        ::freeifaddrs(ifs);
    }
    if (v6.empty() && v4.empty()) {
        v6.push_back("::1");
        v4.push_back("127.0.0.1");
    }
    v6.insert(v6.end(), v4.begin(), v4.end());
    return v6;
}

/// ::getaddrinfo, except that the empty string and the machine's own host
/// name resolve to the local addresses. Learn getaddrinfo: "If the pNodeName
/// parameter contains an empty string, all registered addresses on the local
/// computer are returned"; the name gethostname reports resolves the same way.
int host_getaddrinfo(Ws2Manager& mgr, bool node_given, const std::string& node,
                     const char* serv, const struct addrinfo* hints,
                     HostAiList& out) {
    if (node_given &&
        (node.empty() || ::strcasecmp(node.c_str(), mgr.host_name().c_str()) == 0)) {
        struct addrinfo h{};
        if (hints != nullptr) h = *hints;
        h.ai_flags = (h.ai_flags | AI_NUMERICHOST) & ~AI_CANONNAME;
        int last = EAI_NONAME;
        for (const auto& a : local_address_strings()) {
            struct addrinfo* r = nullptr;
            const int rc = ::getaddrinfo(a.c_str(), serv, &h, &r);
            if (rc == 0 && r != nullptr) out.roots.push_back(r);
            else last = rc != 0 ? rc : EAI_NONAME;
        }
        if (out.roots.empty()) return last;
    } else {
        struct addrinfo* r = nullptr;
        const int rc = ::getaddrinfo(node_given ? node.c_str() : nullptr, serv, hints, &r);
        if (rc != 0) return rc;
        if (r != nullptr) out.roots.push_back(r);
    }
    for (auto* root : out.roots) {
        for (const struct addrinfo* ai = root; ai != nullptr; ai = ai->ai_next) {
            out.entries.push_back(ai);
        }
    }
    return 0;
}

/// EAI_* -> Winsock (Learn getaddrinfo error table).
u32 eai_to_wsa(int rc) {
    switch (rc) {
    case EAI_AGAIN:    return WSATRY_AGAIN;
    case EAI_FAIL:     return WSANO_RECOVERY;
    case EAI_NONAME:   return WSAHOST_NOT_FOUND;
#ifdef EAI_NODATA
#if EAI_NODATA != EAI_NONAME
    case EAI_NODATA:   return WSANO_DATA;
#endif
#endif
    case EAI_MEMORY:   return WSA_NOT_ENOUGH_MEMORY;
    case EAI_FAMILY:   return WSAEAFNOSUPPORT;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT;
    case EAI_SERVICE:  return WSATYPE_NOT_FOUND;
    case EAI_BADFLAGS: return WSAEINVAL;
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW: return WSAENOBUFS;
#endif
    default:           return WSAHOST_NOT_FOUND;
    }
}

/// One getaddrinfo result before it is laid out in guest memory.
struct WinAi {
    sockaddr_storage ss{};  // host layout, port filled in
    u32 flags = 0;          // ai_flags Windows reports for this entry
};

struct WinAiRequest {
    bool node_given = false;
    std::string node;
    bool serv_given = false;
    std::string serv;
    u32 flags = 0;
    int family = 0;    // Windows AF_* (0, 2, 23)
    int socktype = 0;
    int protocol = 0;
};

struct WinAiResult {
    std::vector<WinAi> entries;
    std::string canon;      // ai_canonname of the first entry, when requested
    bool has_canon = false;
    int socktype = 0;       // ai_socktype of every entry
};

constexpr u32 kAiPassive = 0x1, kAiCanonName = 0x2, kAiNumericHost = 0x4,
              kAiNumericServ = 0x8, kAiAll = 0x100, kAiV4Mapped = 0x800,
              kAiFqdn = 0x20000;

/// Strict dotted-decimal IPv4: four parts of one to three digits, each at
/// most 255. Windows resolves nothing looser as a numeric host
/// (ws2_corex_probe gx_v4_short_form / one_number / hex / trailing_dot).
bool parse_strict_v4(const std::string& text, in_addr* out) {
    u32 parts[4] = {};
    usize part = 0, digits = 0;
    for (usize i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '.';
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return false;
            parts[part] = parts[part] * 10 + static_cast<u32>(c - '0');
            if (parts[part] > 255) return false;
        } else if (c == '.') {
            if (digits == 0 || part >= 4) return false;
            ++part;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part != 4) return false;
    out->s_addr = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    return true;
}

/// Numeric IPv6, optionally bracketed and with a numeric %scope
/// (ws2_corex_probe gx_v6_scope / gx_v6_brackets).
bool parse_numeric_v6(std::string text, in6_addr* out, u32* scope) {
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = text.substr(1, text.size() - 2);
    }
    *scope = 0;
    const auto pct = text.find('%');
    if (pct != std::string::npos) {
        const std::string sc = text.substr(pct + 1);
        if (sc.empty() || sc.size() > 10) return false;
        u64 v = 0;
        for (char c : sc) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<u64>(c - '0');
        }
        if (v > 0xFFFFFFFFull) return false;
        *scope = static_cast<u32>(v);
        text = text.substr(0, pct);
    }
    return ::inet_pton(AF_INET6, text.c_str(), out) == 1;
}

sockaddr_storage make_v4(in_addr a) {
    sockaddr_storage ss{};
    auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
    sin->sin_len = sizeof(sockaddr_in);
    sin->sin_family = AF_INET;
    sin->sin_addr = a;
    return ss;
}

sockaddr_storage make_v6(const in6_addr& a, u32 scope) {
    sockaddr_storage ss{};
    auto* sin6 = reinterpret_cast<sockaddr_in6*>(&ss);
    sin6->sin6_len = sizeof(sockaddr_in6);
    sin6->sin6_family = AF_INET6;
    sin6->sin6_addr = a;
    sin6->sin6_scope_id = scope;
    return ss;
}

sockaddr_storage v4_mapped(in_addr a) {
    in6_addr m{};
    m.s6_addr[10] = 0xFF;
    m.s6_addr[11] = 0xFF;
    std::memcpy(&m.s6_addr[12], &a, 4);
    return make_v6(m, 0);
}

/// Windows strtoul over the service string: leading white space, an
/// optional sign (a minus negates), decimal digits, ULONG_MAX on overflow.
/// Numeric when nothing follows the digits (an empty string is port 0).
/// ws2_corex_probe gx_serv_*: "-1" and "4294967376" are 65535, " 80" is 80,
/// "80x" and "0x50" are names; ws2_resolve_probe gai_service_num_big: 70000
/// is 4464 -- the port is the low 16 bits.
bool parse_service_number(const std::string& s, u16* port) {
    usize i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    u64 v = 0;
    bool overflow = false;
    const usize digits_start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + static_cast<u64>(s[i] - '0');
        if (v > 0xFFFFFFFFull) overflow = true;
        ++i;
    }
    if (i != s.size()) return false;
    if (i == digits_start && digits_start != 0) {
        // Only white space / a sign: strtoul consumed nothing -> end points
        // at the start, which is not the terminator.
        return false;
    }
    u32 ul = overflow ? 0xFFFFFFFFu : static_cast<u32>(v);
    if (neg && !overflow) ul = static_cast<u32>(0u - ul);
    *port = static_cast<u16>(ul & 0xFFFFu);
    return true;
}

/// Windows getaddrinfo, measured end to end by ws2_resolve_probe gai_* and
/// ws2_corex_probe gx_*: one entry per address (never one per socket type),
/// ai_socktype / ai_protocol as the hints give them, AI_NUMERICHOST reported
/// in ai_flags of a numeric node's entries, IPv6 before IPv4 for AF_UNSPEC.
/// Returns 0 or the WSA error.
u32 resolve_windows(Ws2Manager& mgr, const WinAiRequest& rq, WinAiResult& out) {
    if (rq.family != kWinAfUnspec && rq.family != kWinAfInet && rq.family != kWinAfInet6) {
        return WSAEAFNOSUPPORT;                              // gai_bad_family, gx_family_1
    }
    if (rq.socktype < 0 || rq.socktype > SOCK_RAW) return WSAESOCKTNOSUPPORT;  // gx_socktype*
    if ((rq.flags & kAiCanonName) != 0 &&
        ((rq.flags & kAiFqdn) != 0 || !rq.node_given)) {
        return WSAEINVAL;                                    // gai_canon_fqdn / _null_node
    }
    if (!rq.node_given && !rq.serv_given) return WSAHOST_NOT_FOUND;  // gai_null_null
    const bool want4 = rq.family != kWinAfInet6;
    const bool want6 = rq.family != kWinAfInet;

    // Service.
    u16 port = 0;
    out.socktype = rq.socktype;
    if (rq.serv_given && !parse_service_number(rq.serv, &port)) {
        if ((rq.flags & kAiNumericServ) != 0) return WSAHOST_NOT_FOUND;  // gai_numericserv_name
        const auto tcp = ws2_service_port(rq.serv, "tcp");
        const auto udp = ws2_service_port(rq.serv, "udp");
        if (rq.socktype == SOCK_STREAM) {
            if (!tcp) return WSATYPE_NOT_FOUND;
            port = *tcp;
        } else if (rq.socktype == SOCK_DGRAM) {
            if (!udp) return WSATYPE_NOT_FOUND;              // gai_dgram_service_http
            port = *udp;
        } else if (!tcp && !udp) {
            return WSATYPE_NOT_FOUND;                        // gai_bad_service
        } else if (rq.socktype == SOCK_RAW) {
            port = tcp ? *tcp : *udp;                        // gx_serv_http_raw
        } else {
            // Unspecified socket type: a service known for one protocol
            // settles the type (gx_serv_http_socktype0 -> SOCK_STREAM);
            // one known for both leaves it open (gx_serv_domain_socktype0).
            port = tcp ? *tcp : *udp;
            if (tcp && !udp) out.socktype = SOCK_STREAM;
            else if (udp && !tcp) out.socktype = SOCK_DGRAM;
        }
    }

    auto add = [&](const sockaddr_storage& ss, u32 flags) {
        WinAi e;
        e.ss = ss;
        e.flags = flags;
        if (e.ss.ss_family == AF_INET) {
            reinterpret_cast<sockaddr_in*>(&e.ss)->sin_port = htons(port);
        } else {
            reinterpret_cast<sockaddr_in6*>(&e.ss)->sin6_port = htons(port);
        }
        for (const auto& have : out.entries) {
            if (have.ss.ss_family == e.ss.ss_family &&
                std::memcmp(&have.ss, &e.ss, sizeof(e.ss)) == 0) {
                return;
            }
        }
        out.entries.push_back(e);
    };

    if (!rq.node_given) {
        // gai_passive_null / gai_active_null / gx_null_*: the wildcard or the
        // loopback address of each wanted family, IPv6 first.
        const bool passive = (rq.flags & kAiPassive) != 0;
        if (want6) add(make_v6(passive ? in6addr_any : in6addr_loopback, 0), 0);
        if (want4) {
            in_addr a{};
            a.s_addr = htonl(passive ? INADDR_ANY : INADDR_LOOPBACK);
            add(make_v4(a), 0);
        }
        return 0;
    }

    // Numeric host.
    in_addr a4{};
    in6_addr a6{};
    u32 scope = 0;
    if (parse_strict_v4(rq.node, &a4)) {
        if (want4) {
            add(make_v4(a4), kAiNumericHost);
        } else if ((rq.flags & kAiV4Mapped) != 0) {
            add(v4_mapped(a4), 0);                           // gx_v4_with_v6_hint_mapped
        } else {
            return WSAHOST_NOT_FOUND;                        // gx_v4_with_v6_hint
        }
        if ((rq.flags & kAiCanonName) != 0) {
            out.canon = rq.node;                             // gai_canon_numeric
            out.has_canon = true;
        }
        return 0;
    }
    if (parse_numeric_v6(rq.node, &a6, &scope)) {
        if (!want6) return WSAHOST_NOT_FOUND;                // gx_v6_with_v4_hint
        add(make_v6(a6, scope), kAiNumericHost);
        if ((rq.flags & kAiCanonName) != 0) {
            out.canon = rq.node;
            out.has_canon = true;
        }
        return 0;
    }
    if ((rq.flags & kAiNumericHost) != 0) return WSAHOST_NOT_FOUND;  // gai_numerichost_name
    // Anything the host's inet_aton would still read as an address ("127.1",
    // "0x7f.0.0.1", "2130706433") is a DNS name to Windows, which does not
    // resolve (ws2_corex_probe gx_v4_*: 11001).
    {
        in_addr loose{};
        if (::inet_aton(rq.node.c_str(), &loose) != 0) return WSAHOST_NOT_FOUND;
    }

    if (::strcasecmp(rq.node.c_str(), "localhost") == 0) {
        // Resolved by Windows itself: ::1 then 127.0.0.1
        // (ws2_corex_probe gx_localhost_unspec / gx_localhost_v6).
        if (want6) add(make_v6(in6addr_loopback, 0), 0);
        if (want4) {
            in_addr lo{};
            lo.s_addr = htonl(INADDR_LOOPBACK);
            add(make_v4(lo), 0);
        }
        if ((rq.flags & kAiCanonName) != 0) {
            out.canon = rq.node;
            out.has_canon = true;
        }
        return 0;
    }

    // A name: the host resolver, or the machine's own addresses for the empty
    // string and the host name (host_getaddrinfo).
    struct addrinfo h{};
    const bool mapped_v4 = rq.family == kWinAfInet6 && (rq.flags & kAiV4Mapped) != 0;
    h.ai_family = mapped_v4 ? AF_UNSPEC : rq.family == kWinAfInet6 ? AF_INET6
                                        : rq.family == kWinAfInet ? AF_INET : AF_UNSPEC;
    h.ai_socktype = SOCK_STREAM;  // one host entry per address
    if ((rq.flags & kAiCanonName) != 0) h.ai_flags |= AI_CANONNAME;
    HostAiList list;
    const int rc = host_getaddrinfo(mgr, true, rq.node, nullptr, &h, list);
    if (rc != 0) return eai_to_wsa(rc);
    bool any_v6 = false;
    for (const struct addrinfo* ai : list.entries) {
        if (ai->ai_family == AF_INET6) any_v6 = true;
    }
    // IPv6 first, as Windows orders its answers.
    for (int pass = 0; pass < 2; ++pass) {
        for (const struct addrinfo* ai : list.entries) {
            if (ai->ai_addr == nullptr) continue;
            if (pass == 0 && ai->ai_family == AF_INET6 && want6) {
                sockaddr_storage ss{};
                std::memcpy(&ss, ai->ai_addr, sizeof(sockaddr_in6));
                add(ss, 0);
            } else if (pass == 1 && ai->ai_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
                if (want4) {
                    add(make_v4(sin->sin_addr), 0);
                } else if (mapped_v4 && (!any_v6 || (rq.flags & kAiAll) != 0)) {
                    add(v4_mapped(sin->sin_addr), 0);
                }
            }
        }
    }
    if (out.entries.empty()) return WSAHOST_NOT_FOUND;
    if ((rq.flags & kAiCanonName) != 0) {
        for (const struct addrinfo* ai : list.entries) {
            if (ai->ai_canonname != nullptr) {
                out.canon = ai->ai_canonname;
                out.has_canon = true;
                break;
            }
        }
        if (!out.has_canon) {
            out.canon = rq.node;
            out.has_canon = true;
        }
    }
    return 0;
}

}  // namespace

void impl_freeaddrinfo(Marshaler& m, Ws2Manager& mgr) {
    // freeaddrinfo(info) → void
    GuestAddr info = m.arg_ptr(0);
    ipc::ipc_log("freeaddrinfo root=0x%llx caller=0x%llx tid=%u",
                 (unsigned long long)info,
                 (unsigned long long)guest_caller_ret(m),
                 ThreadManager::get_current_thread_id_static());
    mgr.addrinfo_free(info);
    m.set_return(0);
}

/// gethostbyname(const char* name) -> hostent*
/// 
/// Guest hostent layout (32-bit):
///   offset 0:  char*   h_name       (4B ptr to hostname string)
///   offset 4:  char**  h_aliases    (4B ptr to NULL-terminated alias array)
///   offset 8:  short   h_addrtype   (2B = AF_INET = 2)
///   offset 10: short   h_length     (2B = 4 for IPv4)
///   offset 12: char**  h_addr_list  (4B ptr to NULL-terminated addr array)
/// Total: 16 bytes
void impl_gethostbyname(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::NullPointer)) return;

    GuestAddr name_ptr = m.arg_ptr(0);
    
    std::string hostname;
    if (!name_ptr) {
        // NULL name means resolve the local hostname (Windows behavior)
        hostname = mgr.host_name();
        VITREO_TRACE("[ws2_32] gethostbyname(NULL) -> resolving local '%s'\n", hostname.c_str());
    } else {
        // Read hostname from guest
        auto name_result = m.read_ansi_string(name_ptr, 256);
        if (name_result.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(0);
            return;
        }
        hostname = name_result.value();
    }

    // Learn gethostbyname: "If the name parameter points to an empty string
    // or name is NULL, the returned string is the same as the string returned
    // by a successful gethostname" -- the local host (ws2_misc_probe
    // ghbn_empty: addrtype=2 has_addr=1).
    if (hostname.empty()) hostname = mgr.host_name();

    // Use native getaddrinfo to resolve
    struct addrinfo hints{};
    hints.ai_family = AF_INET;  // IPv4 only for gethostbyname compatibility
    hints.ai_socktype = SOCK_STREAM;

    HostAiList result;
    const int rc = host_getaddrinfo(mgr, true, hostname, nullptr, &hints, result);
    if (rc != 0 || result.entries.empty()) {
        // Learn's error table: a transient server failure is WSATRY_AGAIN and a
        // non-recoverable one WSANO_RECOVERY; an unknown name or an address
        // literal it cannot use is WSAHOST_NOT_FOUND (ghbn_v6_literal,
        // ghbn_bad_literal: 11001).
        mgr.set_last_error(rc == EAI_AGAIN ? WSATRY_AGAIN
                           : rc == EAI_FAIL ? WSANO_RECOVERY
                                            : WSAHOST_NOT_FOUND);
        m.set_return(0);
        return;
    }
    std::vector<std::vector<u8>> addrs;
    for (const struct addrinfo* ai : result.entries) {
        if (ai->ai_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
        const auto* b = reinterpret_cast<const u8*>(&sin->sin_addr.s_addr);
        addrs.emplace_back(b, b + 4);
    }
    if (addrs.empty()) {
        mgr.set_last_error(WSANO_DATA);
        m.set_return(0);
        return;
    }
    // Windows names the local machine for "localhost" (ws2_misc_probe
    // ghbn_localhost: name=<computer name>, a0=127.0.0.1); a literal or any
    // other name is reported as queried (ghbn_v4_literal).
    const std::string h_name =
        ::strcasecmp(hostname.c_str(), "localhost") == 0 ? mgr.host_name() : hostname;
    // Per-thread HOSTENT in the guest's pointer width (ws2_netdb.cpp).
    const GuestAddr hostent_ptr =
        write_hostent(m, mgr, h_name, {}, kWinAfInet, addrs);
    if (hostent_ptr == 0) {
        mgr.set_last_error(WSAENOBUFS);
        m.set_return(0);
        return;
    }
    m.set_return(static_cast<u64>(hostent_ptr));
}

/// HANDLE WSAAsyncGetHostByName(HWND, u_int wMsg, const char* name, char* buf,
///                              int buflen)
/// Learn: returns a nonzero async task handle on successful initiation; the
/// window later receives wMsg with wParam=handle and
/// HIWORD(lParam)=error / LOWORD=buflen (or required size on WSAENOBUFS).
/// The buffer receives a packed HOSTENT (MAXGETHOSTSTRUCT=1024 recommended).
void ws2::impl_WSAAsyncGetHostByName(Marshaler& m, Ws2Manager& mgr) {
    const u64 hwnd = m.arg(0);
    const u32 wmsg = static_cast<u32>(m.arg(1));
    const GuestAddr name_ptr = m.arg_ptr(2);
    const GuestAddr buf = m.arg_ptr(3);
    const int buflen = static_cast<int>(m.arg32(4));

    if (hwnd == 0 || name_ptr == 0 || buf == 0 || buflen <= 0) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(0);
        return;
    }
    auto name_result = m.read_ansi_string(name_ptr, 256);
    if (name_result.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(0);
        return;
    }
    const std::string hostname = name_result.value();
    if (hostname.empty()) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(0);
        return;
    }

    const u32 handle = mgr.alloc_async_task();

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    HostAiList result;
    const int rc = host_getaddrinfo(mgr, true, hostname, nullptr, &hints, result);

    u16 async_err = 0;
    u16 async_len = static_cast<u16>(buflen > 0xFFFF ? 0xFFFF : buflen);
    if (rc != 0 || result.entries.empty()) {
        async_err = static_cast<u16>(WSAHOST_NOT_FOUND);
    } else {
        std::vector<std::vector<u8>> addrs;
        for (const struct addrinfo* ai : result.entries) {
            if (ai->ai_family != AF_INET) continue;
            const auto* sin =
                reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
            const auto* b = reinterpret_cast<const u8*>(&sin->sin_addr.s_addr);
            addrs.emplace_back(b, b + 4);
        }
        if (addrs.empty()) {
            async_err = static_cast<u16>(WSANO_DATA);
        } else {
            const u32 need = hostent_packed_size(m, hostname, {}, addrs);
            if (need > static_cast<u32>(buflen) ||
                !write_hostent_into(m, buf, static_cast<u32>(buflen), hostname, {},
                                    kWinAfInet, addrs)) {
                async_err = static_cast<u16>(WSAENOBUFS);
                async_len = static_cast<u16>(need > 0xFFFF ? 0xFFFF : need);
            }
        }
    }

    if (!mgr.complete_async_task(handle)) {
        // Cancelled before delivery — no message (Learn WSACancelAsyncRequest).
        m.set_return(static_cast<u64>(handle));
        return;
    }
    const u64 lparam =
        (static_cast<u64>(async_err) << 16) | static_cast<u64>(async_len);
    (void)mgr.post_window_message(hwnd, wmsg, handle, lparam);
    m.set_return(static_cast<u64>(handle));
}

void ws2::impl_WSACancelAsyncRequest(Marshaler& m, Ws2Manager& mgr) {
    // int WSACancelAsyncRequest(HANDLE hAsyncTaskHandle)
    const u32 handle = static_cast<u32>(m.arg(0));
    const u32 err = mgr.cancel_async_task(handle);
    if (err != 0) {
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    m.set_return(0);
}

/// getaddrinfo/GetAddrInfoW(nodename, servname, hints, res) -> int
///
/// Guest addrinfo layout (32-bit):
///   offset 0:  int     ai_flags     (4B)
///   offset 4:  int     ai_family    (4B)
///   offset 8:  int     ai_socktype  (4B)
///   offset 12: int     ai_protocol  (4B)
///   offset 16: size_t  ai_addrlen   (4B)
///   offset 20: char/wchar_t* ai_canonname (4B ptr)
///   offset 24: sockaddr* ai_addr    (4B ptr)
///   offset 28: addrinfo* ai_next    (4B ptr)
/// Total: 32 bytes
///
/// Guest sockaddr_in layout:
///   offset 0:  short   sin_family   (2B)
///   offset 2:  u_short sin_port     (2B)
///   offset 4:  in_addr sin_addr     (4B)
///   offset 8:  char[8] sin_zero     (8B)
/// Total: 16 bytes
static void impl_getaddrinfo_common(Marshaler& m, Ws2Manager& mgr, bool wide) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::ErrorAsReturn)) return;

    GuestAddr nodename_ptr = m.arg_ptr(0);
    GuestAddr servname_ptr = m.arg_ptr(1);
    GuestAddr hints_ptr = m.arg_ptr(2);
    GuestAddr result_ptr = m.arg_ptr(3);

    // LPADDRINFO* / pointer fields are pointer-width: 8 bytes in x64, 4 in x32.
    const bool is64 = m.cpu_is_64bit();
    auto write_uptr = [&](GuestAddr addr, u64 value) {
        if (is64) (void)m.write_u64(addr, value);
        else      (void)m.write_u32(addr, static_cast<u32>(value));
    };
    // The return value is the error and WSAGetLastError reports it too --
    // 0 on success (ws2_resolve_probe gai_*: wsaerr=0).
    auto finish = [&](u32 err) {
        if (err != 0 && result_ptr != 0) write_uptr(result_ptr, 0);
        mgr.set_last_error(err);
        m.set_return(static_cast<u64>(err));
    };

    if (!result_ptr) return finish(WSAEINVAL);

    WinAiRequest rq;
    // Read nodename if provided (NI_MAXHOST-class; unreadable -> WSAEFAULT).
    if (nodename_ptr) {
        auto nr = wide ? m.read_wide_string(nodename_ptr, 1025)
                       : m.read_ansi_string(nodename_ptr, 1025);
        if (nr.is_err()) return finish(WSAEFAULT);
        rq.node_given = true;
        rq.node = nr.value();
    }
    // Read servname if provided (NI_MAXSERV-class).
    if (servname_ptr) {
        auto sr = wide ? m.read_wide_string(servname_ptr, 32)
                       : m.read_ansi_string(servname_ptr, 32);
        if (sr.is_err()) return finish(WSAEFAULT);
        rq.serv_given = true;
        rq.serv = sr.value();
    }

    if (hints_ptr) {
        auto flags_r = m.read_u32(hints_ptr + 0);
        auto family_r = m.read_u32(hints_ptr + 4);
        auto socktype_r = m.read_u32(hints_ptr + 8);
        auto protocol_r = m.read_u32(hints_ptr + 12);
        // Learn: ai_addrlen and ai_next must be 0/NULL (ws2_resolve_probe
        // gai_hints_addrlen_nonzero / gai_hints_next_nonnull -> WSANO_RECOVERY).
        const u32 addrlen_off = 16;
        const u32 next_off = is64 ? 40u : 28u;
        auto addrlen_r = is64 ? m.read_u64(hints_ptr + addrlen_off)
                              : m.read_u32(hints_ptr + addrlen_off).map(
                                    [](u32 v) { return static_cast<u64>(v); });
        auto next_r = is64 ? m.read_u64(hints_ptr + next_off)
                           : m.read_u32(hints_ptr + next_off).map(
                                 [](u32 v) { return static_cast<u64>(v); });
        if (flags_r.is_err() || family_r.is_err() || socktype_r.is_err() ||
            protocol_r.is_err() || addrlen_r.is_err() || next_r.is_err()) {
            return finish(WSAEFAULT);
        }
        if (addrlen_r.value() != 0 || next_r.value() != 0) return finish(WSANO_RECOVERY);
        rq.flags = flags_r.value();
        rq.family = static_cast<int>(family_r.value());
        rq.socktype = static_cast<int>(socktype_r.value());
        rq.protocol = static_cast<int>(protocol_r.value());
    }

    ipc::ipc_log("getaddrinfo node=\"%s\" serv=\"%s\" family=%d caller=0x%llx tid=%u",
                 rq.node_given ? rq.node.c_str() : "(null)",
                 rq.serv_given ? rq.serv.c_str() : "(null)", rq.family,
                 (unsigned long long)guest_caller_ret(m),
                 ThreadManager::get_current_thread_id_static());

    WinAiResult res;
    if (const u32 err = resolve_windows(mgr, rq, res)) {
        ipc::ipc_log("getaddrinfo node=\"%s\" -> %u", rq.node.c_str(), err);
        return finish(err);
    }

    // Durable arena — must survive until freeaddrinfo (do not scratch_reset).
    // ADDRINFOA/ADDRINFOW layout is pointer-width sensitive:
    //   x86 (32 bytes): flags@0 family@4 socktype@8 protocol@12
    //                   ai_addrlen@16(4) ai_canonname@20(4) ai_addr@24(4) ai_next@28(4)
    //   x64 (48 bytes): flags@0 family@4 socktype@8 protocol@12
    //                   ai_addrlen@16(8) ai_canonname@24(8) ai_addr@32(8) ai_next@40(8)
    const u32 ai_struct_size = is64 ? 48u : 32u;
    const u32 off_addrlen   = 16;
    const u32 off_canonname = is64 ? 24u : 20u;
    const u32 off_addr      = is64 ? 32u : 24u;
    const u32 off_next      = is64 ? 40u : 28u;

    const size_t count = res.entries.size();
    const size_t sockaddr_bytes = count * static_cast<size_t>(kWinSockaddrIn6Bytes);
    const size_t canon_bytes = res.has_canon ? (res.canon.size() + 1) * (wide ? 2u : 1u) : 0;
    const size_t total_size = count * ai_struct_size + sockaddr_bytes + canon_bytes + 16;
    GuestAddr base = mgr.addrinfo_alloc(static_cast<u32>(total_size));
    if (!base) return finish(WSAENOBUFS);

    GuestAddr current_sockaddr = base + static_cast<u32>(count * ai_struct_size);
    GuestAddr canon_ptr = 0;
    if (res.has_canon) {
        canon_ptr = current_sockaddr + static_cast<u32>(sockaddr_bytes);
        if (wide) (void)m.write_wide_string(canon_ptr, res.canon);
        else (void)m.write_ansi_string(canon_ptr, res.canon);
    }
    for (size_t i = 0; i < count; ++i) {
        const WinAi& e = res.entries[i];
        const GuestAddr ai = base + static_cast<u32>(i * ai_struct_size);
        const u32 win_family = e.ss.ss_family == AF_INET6 ? kWinAfInet6 : kWinAfInet;
        (void)m.write_u32(ai + 0, e.flags);
        (void)m.write_u32(ai + 4, win_family);
        (void)m.write_u32(ai + 8, static_cast<u32>(res.socktype));
        (void)m.write_u32(ai + 12, static_cast<u32>(rq.protocol));
        const u32 addrlen = write_win_sockaddr(m, current_sockaddr,
                                               static_cast<u32>(kWinSockaddrIn6Bytes), e.ss);
        write_uptr(ai + off_addrlen, addrlen);
        write_uptr(ai + off_canonname, i == 0 ? canon_ptr : 0);
        write_uptr(ai + off_addr, addrlen != 0 ? current_sockaddr : 0);
        write_uptr(ai + off_next,
                   i + 1 < count ? base + static_cast<u32>((i + 1) * ai_struct_size) : 0);
        current_sockaddr += addrlen;
    }
    write_uptr(result_ptr, base);
    finish(0);
}

void impl_getaddrinfo(Marshaler& m, Ws2Manager& mgr) {
    impl_getaddrinfo_common(m, mgr, false);
}

void impl_GetAddrInfoW(Marshaler& m, Ws2Manager& mgr) {
    impl_getaddrinfo_common(m, mgr, true);
}

void impl_FreeAddrInfoW(Marshaler& m, Ws2Manager& mgr) {
    impl_freeaddrinfo(m, mgr);
}


// -----------------------------------------------------------------------------
// GetAddrInfoExW / FreeAddrInfoExW / GetAddrInfoExCancel
// -----------------------------------------------------------------------------

namespace {

// ADDRINFOEXW sizes (MSVC packing).
constexpr u32 kAddrInfoExW32 = 44;
constexpr u32 kAddrInfoExW64 = 72;

[[nodiscard]] u32 addrinfoexw_size(bool is64) {
    return is64 ? kAddrInfoExW64 : kAddrInfoExW32;
}

// Write a NUL-terminated UTF-16LE string from ASCII/UTF-8 bytes that are in
// the ASCII subset (DNS labels). Non-ASCII bytes are replaced with '?'.
GuestAddr write_guest_utf16_ascii(Marshaler& m, GuestAddr dst, const char* s) {
    if (s == nullptr) {
        return dst;
    }
    GuestAddr cur = dst;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
         *p != 0; ++p) {
        const u16 unit = (*p < 0x80) ? static_cast<u16>(*p) : static_cast<u16>('?');
        (void)m.write_u16(cur, unit);
        cur += 2;
    }
    (void)m.write_u16(cur, 0);
    return cur + 2;
}

void signal_addrinfoex_overlapped(Marshaler& m, Ws2Manager& mgr,
                                  GuestAddr overlapped, bool is64) {
    if (overlapped == 0) {
        return;
    }
    const u32 ov_event_off = is64 ? 24u : 16u;
    const u32 ov_high_off = is64 ? 8u : 4u;
    GuestMemory& mem = m.memory();
    if (is64) {
        (void)mem.write_u64(overlapped, STATUS_SUCCESS);
        (void)mem.write_u64(overlapped + ov_high_off, 0);
    } else {
        (void)mem.write_u32(overlapped, STATUS_SUCCESS);
        (void)mem.write_u32(overlapped + ov_high_off, 0);
    }
    Handle hevent = 0;
    if (is64) {
        hevent = static_cast<Handle>(
            mem.read_u64(overlapped + ov_event_off).value_or(0));
    } else {
        hevent = static_cast<Handle>(
            mem.read_u32(overlapped + ov_event_off).value_or(0));
    }
    if (hevent != 0 && mgr.sync_manager()) {
        (void)mgr.sync_manager()->set_event(hevent & ~Handle{1});
    }
}

}  // namespace

void impl_FreeAddrInfoExW(Marshaler& m, Ws2Manager& mgr) {
    // FreeAddrInfoEx[W](pAddrInfoEx) → void
    GuestAddr info = m.arg_ptr(0);
    ipc::ipc_log("FreeAddrInfoExW root=0x%llx caller=0x%llx tid=%u",
                 (unsigned long long)info,
                 (unsigned long long)guest_caller_ret(m),
                 ThreadManager::get_current_thread_id_static());
    mgr.addrinfo_free(info);
    m.set_return(0);
}

void impl_GetAddrInfoExCancel(Marshaler& m, Ws2Manager& /*mgr*/) {
    // BOOL GetAddrInfoExCancel(LPHANDLE lpHandle)
    // Completions are synchronous today, so there is nothing to cancel.
    (void)m.arg(0);
    m.set_return(1);
}

void impl_GetAddrInfoExW(Marshaler& m, Ws2Manager& mgr) {
    // INT GetAddrInfoExW(PCWSTR pName, PCWSTR pServiceName, DWORD dwNameSpace,
    //   LPGUID lpNspId, const ADDRINFOEXW* hints, PADDRINFOEXW* ppResult,
    //   timeval* timeout, LPOVERLAPPED lpOverlapped,
    //   LPLOOKUPSERVICE_COMPLETION_ROUTINE lpCompletionRoutine,
    //   LPHANDLE lpNameHandle)
    GuestAddr name_ptr = m.arg_ptr(0);
    GuestAddr service_ptr = m.arg_ptr(1);
    const u32 name_space = static_cast<u32>(m.arg(2));
    (void)m.arg(3);  // lpNspId
    GuestAddr hints_ptr = m.arg_ptr(4);
    GuestAddr result_ptr = m.arg_ptr(5);
    (void)m.arg(6);  // timeout
    GuestAddr overlapped = m.arg_ptr(7);
    GuestAddr completion = m.arg_ptr(8);
    GuestAddr handle_ptr = m.arg_ptr(9);
    (void)name_space;

    const bool is64 = m.cpu_is_64bit();
    auto write_uptr = [&](GuestAddr addr, u64 value) {
        if (is64) (void)m.write_u64(addr, value);
        else (void)m.write_u32(addr, static_cast<u32>(value));
    };

    if (result_ptr == 0) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(WSAEINVAL));
        return;
    }
    // No cancel handle for synchronous completion.
    if (handle_ptr != 0) {
        write_uptr(handle_ptr, 0);
    }

    // Completion routines require a guest APC path we do not wire here yet.
    // Event-based OVERLAPPED still works via the signal below.
    if (completion != 0 && overlapped == 0) {
        mgr.set_last_error(WSAEOPNOTSUPP);
        write_uptr(result_ptr, 0);
        m.set_return(static_cast<u64>(WSAEOPNOTSUPP));
        return;
    }

    // Same resolution as getaddrinfo (resolve_windows); ADDRINFOEXW adds
    // ai_blob / ai_bloblen / ai_provider, which the DNS namespace leaves 0.
    WinAiRequest rq;
    if (name_ptr != 0) {
        auto nr = m.read_wide_string(name_ptr, 1025);
        if (nr.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            write_uptr(result_ptr, 0);
            m.set_return(static_cast<u64>(WSAEFAULT));
            return;
        }
        rq.node_given = true;
        rq.node = nr.value();
    }
    if (service_ptr != 0) {
        auto sr = m.read_wide_string(service_ptr, 64);
        if (sr.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            write_uptr(result_ptr, 0);
            m.set_return(static_cast<u64>(WSAEFAULT));
            return;
        }
        rq.serv_given = true;
        rq.serv = sr.value();
    }
    if (hints_ptr != 0) {
        auto flags_r = m.read_u32(hints_ptr + 0);
        auto family_r = m.read_u32(hints_ptr + 4);
        auto socktype_r = m.read_u32(hints_ptr + 8);
        auto protocol_r = m.read_u32(hints_ptr + 12);
        if (flags_r.is_err() || family_r.is_err() || socktype_r.is_err() ||
            protocol_r.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            write_uptr(result_ptr, 0);
            m.set_return(static_cast<u64>(WSAEFAULT));
            return;
        }
        rq.flags = flags_r.value();
        rq.family = static_cast<int>(family_r.value());
        rq.socktype = static_cast<int>(socktype_r.value());
        rq.protocol = static_cast<int>(protocol_r.value());
    }

    ipc::ipc_log(
        "GetAddrInfoExW node=\"%s\" serv=\"%s\" family=%d ov=0x%llx tid=%u",
        rq.node_given ? rq.node.c_str() : "(null)",
        rq.serv_given ? rq.serv.c_str() : "(null)", rq.family,
        static_cast<unsigned long long>(overlapped),
        ThreadManager::get_current_thread_id_static());

    WinAiResult res;
    if (const u32 wsa_err = resolve_windows(mgr, rq, res)) {
        mgr.set_last_error(wsa_err);
        write_uptr(result_ptr, 0);
        signal_addrinfoex_overlapped(m, mgr, overlapped, is64);
        m.set_return(static_cast<u64>(wsa_err));
        return;
    }

    const u32 ai_struct_size = addrinfoexw_size(is64);
    const u32 off_addrlen = 16;
    const u32 off_canonname = is64 ? 24u : 20u;
    const u32 off_addr = is64 ? 32u : 24u;
    const u32 off_blob = is64 ? 40u : 28u;
    const u32 off_bloblen = is64 ? 48u : 32u;
    const u32 off_provider = is64 ? 56u : 36u;
    const u32 off_next = is64 ? 64u : 40u;

    const size_t count = res.entries.size();
    const size_t sockaddr_bytes = count * static_cast<size_t>(kWinSockaddrIn6Bytes);
    const size_t canon_bytes = res.has_canon ? (res.canon.size() + 1) * 2 : 0;
    const size_t total_size = count * ai_struct_size + sockaddr_bytes + canon_bytes + 16;
    GuestAddr base = mgr.addrinfo_alloc(static_cast<u32>(total_size));
    if (!base) {
        mgr.set_last_error(WSAENOBUFS);
        write_uptr(result_ptr, 0);
        signal_addrinfoex_overlapped(m, mgr, overlapped, is64);
        m.set_return(static_cast<u64>(WSAENOBUFS));
        return;
    }
    GuestAddr current_sockaddr = base + static_cast<u32>(count * ai_struct_size);
    GuestAddr canon_ptr = 0;
    if (res.has_canon) {
        canon_ptr = current_sockaddr + static_cast<u32>(sockaddr_bytes);
        (void)write_guest_utf16_ascii(m, canon_ptr, res.canon.c_str());
    }
    for (size_t i = 0; i < count; ++i) {
        const WinAi& e = res.entries[i];
        const GuestAddr ai = base + static_cast<u32>(i * ai_struct_size);
        (void)m.write_u32(ai + 0, e.flags);
        (void)m.write_u32(ai + 4, e.ss.ss_family == AF_INET6 ? kWinAfInet6 : kWinAfInet);
        (void)m.write_u32(ai + 8, static_cast<u32>(res.socktype));
        (void)m.write_u32(ai + 12, static_cast<u32>(rq.protocol));
        const u32 addrlen = write_win_sockaddr(m, current_sockaddr,
                                               static_cast<u32>(kWinSockaddrIn6Bytes), e.ss);
        write_uptr(ai + off_addrlen, addrlen);
        write_uptr(ai + off_canonname, i == 0 ? canon_ptr : 0);
        write_uptr(ai + off_addr, addrlen != 0 ? current_sockaddr : 0);
        write_uptr(ai + off_blob, 0);
        write_uptr(ai + off_bloblen, 0);
        write_uptr(ai + off_provider, 0);
        write_uptr(ai + off_next,
                   i + 1 < count ? base + static_cast<u32>((i + 1) * ai_struct_size) : 0);
        current_sockaddr += addrlen;
    }
    write_uptr(result_ptr, base);
    mgr.set_last_error(0);
    signal_addrinfoex_overlapped(m, mgr, overlapped, is64);
    ipc::ipc_log("GetAddrInfoExW node=\"%s\" -> 0 first=0x%llx count=%zu",
                 rq.node.c_str(), static_cast<unsigned long long>(base), count);
    m.set_return(0);
}


// -----------------------------------------------------------------------------
// getnameinfo
// -----------------------------------------------------------------------------
// Learn getnameinfo plus ws2_resolve_probe.expected (gni_*):
//  - the return value is the error and WSAGetLastError is set to it -- also to
//    0 on success (gni_numeric_both: rc=0 wsaerr=0);
//  - pSockaddr NULL or SockaddrLength shorter than the family's sockaddr ->
//    WSAEFAULT; a longer length is accepted; an unknown family ->
//    WSAEAFNOSUPPORT;
//  - an output buffer too small for its string gets the first NodeBufferSize
//    (ServiceBufferSize) bytes, unterminated, and the call returns
//    ERROR_INSUFFICIENT_BUFFER (122); the host is produced before the service;
//  - numeric IPv6 hosts carry "%<scope id>" when the scope id is nonzero;
//  - service names come from the Windows services database (NI_DGRAM picks the
//    udp entry), a port without one is printed numerically; unknown flag bits
//    are ignored (gni_bad_flags).
//  - GetNameInfoW is the same call with WCHAR buffers whose sizes count
//    WCHARs (ws2_resolve_probe GetNameInfoW_numeric).
static void impl_getnameinfo_common(Marshaler& m, Ws2Manager& mgr, bool wide) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::ErrorAsReturn)) return;

    const GuestAddr sa_ptr = m.arg_ptr(0);
    const int salen = static_cast<int>(static_cast<u32>(m.arg(1)));
    const GuestAddr host_ptr = m.arg_ptr(2);
    const u32 hostlen = static_cast<u32>(m.arg(3));
    const GuestAddr serv_ptr = m.arg_ptr(4);
    const u32 servlen = static_cast<u32>(m.arg32(5));
    const u32 flags = static_cast<u32>(m.arg32(6));
    constexpr u32 kNiNoFqdn = 0x01, kNiNumericHost = 0x02, kNiNameReqd = 0x04,
                  kNiNumericServ = 0x08, kNiDgram = 0x10;
    constexpr u32 kErrorInsufficientBuffer = 122;
    constexpr u32 kWsaNotEnoughMemory = 8;
    auto finish = [&](u32 err) {
        mgr.set_last_error(err);
        m.set_return(err);
    };

    if (sa_ptr == 0) {
        finish(WSAEFAULT);
        return;
    }
    const auto family = m.read_u16(sa_ptr);
    if (family.is_err()) {
        finish(WSAEFAULT);
        return;
    }
    if (family.value() != kWinAfInet && family.value() != kWinAfInet6) {
        finish(WSAEAFNOSUPPORT);
        return;
    }
    MappedSockaddr mapped{};
    if (!read_win_sockaddr_mapped(m, sa_ptr, salen, mapped)) {
        finish(WSAEFAULT);
        return;
    }

    // Copy a result string: all of it plus NUL when it fits, otherwise the
    // first `cap` bytes unterminated and ERROR_INSUFFICIENT_BUFFER.
    auto emit = [&](GuestAddr dst, u32 cap, const std::string& text) -> bool {
        const u32 need = static_cast<u32>(text.size()) + 1;
        const u32 n = std::min(need, cap);
        bool wrote = true;
        if (wide) {
            // Host names and service names here are ASCII.
            std::vector<Byte> units(static_cast<usize>(n) * 2, 0);
            for (u32 i = 0; i < n && i < text.size(); ++i) {
                units[i * 2] = static_cast<Byte>(text[i]);
            }
            wrote = n == 0 || m.memory().write_bytes(dst, units.data(), units.size()).is_ok();
        } else if (n != 0) {
            wrote = m.memory()
                        .write_bytes(dst, reinterpret_cast<const Byte*>(text.c_str()), n)
                        .is_ok();
        }
        if (!wrote) {
            finish(WSAEFAULT);
            return false;
        }
        if (need > cap) {
            finish(kErrorInsufficientBuffer);
            return false;
        }
        return true;
    };

    const bool is_v6 = mapped.ss.ss_family == AF_INET6;
    if (host_ptr != 0 && hostlen != 0) {
        std::string host;
        if ((flags & kNiNumericHost) == 0) {
            char name[NI_MAXHOST] = {};
            const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&mapped.ss),
                                         mapped.len, name, sizeof(name), nullptr, 0,
                                         NI_NAMEREQD | ((flags & kNiNoFqdn) ? NI_NOFQDN : 0));
            if (rc == 0) {
                host = name;
            } else if ((flags & kNiNameReqd) != 0) {
                finish(rc == EAI_AGAIN   ? WSATRY_AGAIN
                       : rc == EAI_MEMORY ? kWsaNotEnoughMemory
                       : rc == EAI_FAIL   ? WSANO_RECOVERY
                                          : WSAHOST_NOT_FOUND);
                return;
            }
        }
        if (host.empty()) {
            char text[INET6_ADDRSTRLEN] = {};
            if (is_v6) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&mapped.ss);
                (void)::inet_ntop(AF_INET6, &sin6->sin6_addr, text, sizeof(text));
                host = text;
                if (sin6->sin6_scope_id != 0) {
                    host += "%" + std::to_string(sin6->sin6_scope_id);
                }
            } else {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(&mapped.ss);
                (void)::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
                host = text;
            }
        }
        if (!emit(host_ptr, hostlen, host)) return;
    }
    if (serv_ptr != 0 && servlen != 0) {
        const u16 port = ntohs(is_v6
            ? reinterpret_cast<const sockaddr_in6*>(&mapped.ss)->sin6_port
            : reinterpret_cast<const sockaddr_in*>(&mapped.ss)->sin_port);
        std::string serv;
        if ((flags & kNiNumericServ) == 0) {
            serv = ws2_service_name(port, (flags & kNiDgram) != 0);
        }
        if (serv.empty()) serv = std::to_string(port);
        if (!emit(serv_ptr, servlen, serv)) return;
    }
    finish(0);
}

void impl_getnameinfo(Marshaler& m, Ws2Manager& mgr) {
    impl_getnameinfo_common(m, mgr, false);
}

void impl_GetNameInfoW(Marshaler& m, Ws2Manager& mgr) {
    impl_getnameinfo_common(m, mgr, true);
}

// -----------------------------------------------------------------------------
// inet_ntop — Convert network address to string (IPv4/IPv6)
// -----------------------------------------------------------------------------
void impl_inet_ntop(Marshaler& m, Ws2Manager& mgr) {
    // const char* inet_ntop(int af, const void* src, char* dst, socklen_t size)
    int af = static_cast<int>(m.arg(0));
    GuestAddr src_ptr = m.arg_ptr(1);
    GuestAddr dst_ptr = m.arg_ptr(2);
    u32 size = static_cast<u32>(m.arg(3));
    
    VITREO_TRACE("[ws2_32] inet_ntop(af=%d, src=0x%llx, dst=0x%llx, size=%u)\n",
                 af, static_cast<unsigned long long>(src_ptr),
                 static_cast<unsigned long long>(dst_ptr), size);
    
    // A NULL or zero-size output buffer is WSAEINVAL (ws2_misc_probe
    // ntop_nullbuf / ntop_zero); the family is Windows' numbering, so Darwin's
    // AF_INET6 (30) is an unsupported family (ntop_af30: 10047).
    if (!dst_ptr || size == 0) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(0);
        return;
    }
    if (af != static_cast<int>(kWinAfInet) && af != static_cast<int>(kWinAfInet6)) {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(0);
        return;
    }
    if (!src_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(0);
        return;
    }

    char result[INET6_ADDRSTRLEN] = {};
    af = af == static_cast<int>(kWinAfInet6) ? AF_INET6 : AF_INET;

    if (af == AF_INET) {
        // Read 4 bytes for IPv4
        u32 addr;
        auto r = m.read_u32(src_ptr);
        if (r.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(0);
            return;
        }
        addr = r.value();
        
        struct in_addr in;
        in.s_addr = addr;
        if (::inet_ntop(AF_INET, &in, result, sizeof(result)) == nullptr) {
            mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
            m.set_return(0);
            return;
        }
    } else if (af == AF_INET6) {
        // Read 16 bytes for IPv6
        u8 addr6[16];
        if (m.memory().read_bytes(src_ptr, addr6, 16).is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(0);
            return;
        }

        struct in6_addr in6;
        memcpy(&in6, addr6, 16);
        if (::inet_ntop(AF_INET6, &in6, result, sizeof(result)) == nullptr) {
            mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
            m.set_return(0);
            return;
        }
    }

    // ws2_misc_probe ntop_exact8 / ntop_7: the terminator must fit.
    size_t len = strlen(result) + 1;
    if (len > size) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(0);
        return;
    }
    
    (void)m.write_ansi_string(dst_ptr, result);
    
    VITREO_TRACE("[ws2_32] inet_ntop -> %s\n", result);
    m.set_return(dst_ptr);
}

// -----------------------------------------------------------------------------
// inet_pton — Convert string to network address (IPv4/IPv6)
// -----------------------------------------------------------------------------
void impl_inet_pton(Marshaler& m, Ws2Manager& mgr) {
    // int inet_pton(int af, const char* src, void* dst)
    // Returns: 1 on success, 0 if invalid format, -1 on error
    int af = static_cast<int>(m.arg(0));
    GuestAddr src_ptr = m.arg_ptr(1);
    GuestAddr dst_ptr = m.arg_ptr(2);
    
    VITREO_TRACE("[ws2_32] inet_pton(af=%d, src=0x%llx, dst=0x%llx)\n", af,
                 static_cast<unsigned long long>(src_ptr),
                 static_cast<unsigned long long>(dst_ptr));
    
    if (!src_ptr || !dst_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(-1));
        return;
    }
    
    auto src_res = m.read_ansi_string(src_ptr, 64);
    if (src_res.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(-1));
        return;
    }
    std::string src = src_res.value();
    // Learn inet_pton: only AF_INET / AF_INET6. Windows AF_INET6 is 23;
    // af=30 is AF_ATM on Windows (Darwin AF_INET6) and must fail.
    if (af == static_cast<int>(kWinAfInet)) {
        af = AF_INET;
    } else if (af == static_cast<int>(kWinAfInet6)) {
        af = AF_INET6;
    } else {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(static_cast<u64>(-1));
        return;
    }

    if (af == AF_INET) {
        struct in_addr in;
        int rc = ::inet_pton(AF_INET, src.c_str(), &in);
        if (rc == 1) {
            (void)m.write_u32(dst_ptr, in.s_addr);
            m.set_return(1);
        } else if (rc == 0) {
            m.set_return(0);
        } else {
            mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
            m.set_return(static_cast<u64>(-1));
        }
    } else if (af == AF_INET6) {
        struct in6_addr in6;
        int rc = ::inet_pton(AF_INET6, src.c_str(), &in6);
        if (rc == 1) {
            (void)m.memory().write_bytes(dst_ptr, reinterpret_cast<const Byte*>(&in6), 16);
            m.set_return(1);
        } else if (rc == 0) {
            m.set_return(0);
        } else {
            mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
            m.set_return(static_cast<u64>(-1));
        }
    } else {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(static_cast<u64>(-1));
    }
}

// -----------------------------------------------------------------------------
// gethostbyaddr — Reverse DNS lookup
// -----------------------------------------------------------------------------
void impl_gethostbyaddr(Marshaler& m, Ws2Manager& mgr) {
    // struct hostent* gethostbyaddr(const char* addr, int len, int type)
    GuestAddr addr_ptr = m.arg_ptr(0);
    int len = static_cast<int>(m.arg(1));
    int type = static_cast<int>(m.arg(2));
    
    VITREO_TRACE("[ws2_32] gethostbyaddr(addr=0x%llx, len=%d, type=%d)\n",
                 static_cast<unsigned long long>(addr_ptr), len, type);
    
    if (!addr_ptr || len <= 0) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(0);
        return;
    }
    
    // Only support IPv4 for now
    if (type != AF_INET || len != 4) {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(0);
        return;
    }
    
    u32 addr_val;
    auto r = m.read_u32(addr_ptr);
    if (r.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(0);
        return;
    }
    addr_val = r.value();
    
    struct in_addr in;
    in.s_addr = addr_val;
    
    // Use getaddrinfo with AI_NUMERICHOST and then getnameinfo
    char hostname[NI_MAXHOST] = {};
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr = in;
    
    int rc = getnameinfo(reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa),
                         hostname, sizeof(hostname), nullptr, 0, 0);
    if (rc != 0) {
        mgr.set_last_error(WSAHOST_NOT_FOUND);
        m.set_return(0);
        return;
    }
    
    std::vector<std::vector<u8>> addrs;
    {
        const auto* bytes = reinterpret_cast<const u8*>(&addr_val);
        addrs.emplace_back(bytes, bytes + 4);
    }
    const GuestAddr hostent_ptr =
        write_hostent(m, mgr, hostname, {}, kWinAfInet, addrs);
    if (hostent_ptr == 0) {
        mgr.set_last_error(WSAENOBUFS);
        m.set_return(0);
        return;
    }
    VITREO_TRACE("[ws2_32] gethostbyaddr -> %s\n", hostname);
    m.set_return(hostent_ptr);
}

} // namespace vitreo::winapi
