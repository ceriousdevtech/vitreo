// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_32_address.cpp
/// @brief Addresses, byte order, and the name/number lookups beside them.
///
/// inet_addr / inet_ntoa, the htons family in both its BSD and WSA spellings,
/// WSAStringToAddress / WSAAddressToString, and the protocol and service
/// lookups. None of these touches a socket: they convert between the shapes a
/// guest writes an endpoint in.
///
/// Two things here are Windows-specific rather than BSD. AF_INET6 is 23 on
/// Windows and 30 on Darwin, so every address that crosses this file is
/// translated, not copied. And the WSA byte-order calls differ from the BSD
/// ones in returning an error code and writing their result through a pointer,
/// which is why they are not aliases.
///
/// Split out of ws2_32.cpp. Its 19 functions used nothing file-local from the
/// rest of the file, which is what made it a cut rather than a gather — checked
/// before moving anything.

#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
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

namespace vitreo::winapi {

// gethostname / GetHostNameW (Learn): the name is the one the rest of the
// system reports (ws2_resolve_probe gethostname_eq_dns_hostname: equal to
// GetComputerNameEx(ComputerNameDnsHostname), case exact) and it resolves via
// getaddrinfo/gethostbyname. namelen counts the terminating NUL: a buffer of
// exactly strlen(name) fails WSAEFAULT, strlen+1 succeeds; NULL, 0 and
// negative lengths fail WSAEFAULT (gethostname_len1 / _exact_len / _len0 /
// _null / _neg, GetHostNameW_len1 / _null).
void ws2::impl_GetHostNameW(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    const GuestAddr name_buf = m.arg_ptr(0);
    const int name_len = static_cast<int>(static_cast<u32>(m.arg(1)));
    const std::string host = mgr.host_name();
    if (name_buf == 0 || name_len <= static_cast<int>(host.size()) ||
        m.write_wide_string(name_buf, host).is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    m.set_return(0);
}

void ws2::impl_gethostname(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    const GuestAddr name_buf = m.arg_ptr(0);
    const int name_len = static_cast<int>(static_cast<u32>(m.arg(1)));
    const std::string host = mgr.host_name();
    if (name_buf == 0 || name_len <= static_cast<int>(host.size()) ||
        m.write_ansi_string(name_buf, host).is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    m.set_return(0);
}

namespace {

/// Windows inet_addr's grammar (Learn inet_addr, and ws2_misc_probe inet_addr
/// lines): one to four '.'-separated parts, each decimal, octal (leading 0) or
/// hex (0x); the last part fills the remaining bytes; the text must end, or
/// continue only after whitespace. Returns false for INADDR_NONE.
bool parse_inet_addr(const std::string& text, u32* host_order) {
    if (text == " ") {  // ws2_misc_probe inet_addr in=" " v=0x0
        *host_order = 0;
        return true;
    }
    u64 parts[4] = {};
    int count = 0;
    size_t i = 0;
    const size_t n = text.size();
    for (;;) {
        if (count == 4 || i >= n) return false;
        u64 base = 10;
        if (text[i] == '0') {
            if (i + 1 < n && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                base = 16;
                i += 2;
            } else {
                base = 8;
            }
        }
        u64 value = 0;
        size_t digits = 0;
        while (i < n) {
            const char c = text[i];
            u64 d;
            if (c >= '0' && c <= '9') d = static_cast<u64>(c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<u64>(c - 'a' + 10);
            else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<u64>(c - 'A' + 10);
            else break;
            if (d >= base) return false;  // "08", "09.1.1.1"
            value = value * base + d;
            if (value > 0xFFFFFFFFull) return false;
            ++digits, ++i;
        }
        // A bare "0" is octal zero; "0x" with no digits is invalid.
        if (digits == 0 && !(base == 8)) return false;
        parts[count++] = value;
        if (i < n && text[i] == '.') {
            ++i;
            continue;
        }
        break;
    }
    if (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' &&
        text[i] != '\r' && text[i] != '\v' && text[i] != '\f') {
        return false;  // "1.2.3.4x"
    }
    u64 v = 0;
    switch (count) {
    case 1:
        v = parts[0];
        break;
    case 2:
        if (parts[0] > 0xFF || parts[1] > 0xFFFFFF) return false;
        v = (parts[0] << 24) | parts[1];
        break;
    case 3:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFFFF) return false;
        v = (parts[0] << 24) | (parts[1] << 16) | parts[2];
        break;
    default:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFF || parts[3] > 0xFF) {
            return false;
        }
        v = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        break;
    }
    *host_order = static_cast<u32>(v);
    return true;
}

} // namespace

void ws2::impl_inet_addr(Marshaler& m, Ws2Manager& mgr) {
    // unsigned long inet_addr(const char* cp)
    const GuestAddr cp_ptr = m.arg_ptr(0);
    if (!cp_ptr) {
        // ws2_misc_probe inet_addr in="(NULL)": INADDR_NONE and WSAEFAULT.
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(0xFFFFFFFF));
        return;
    }
    auto str_res = m.read_ansi_string(cp_ptr, 256);
    if (!str_res) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(0xFFFFFFFF));
        return;
    }
    // Every other outcome leaves the last error alone (le=57005 throughout).
    u32 host_order = 0;
    if (!parse_inet_addr(str_res.value(), &host_order)) {
        m.set_return(static_cast<u64>(0xFFFFFFFF));
        return;
    }
    m.set_return(static_cast<u64>(htonl(host_order)));
}

void ws2::impl_inet_ntoa(Marshaler& m, Ws2Manager& mgr) {
    // char* inet_ntoa(struct in_addr in): the dotted string in a per-thread
    // Winsock buffer, reused by the next call on the thread, and the last
    // error cleared (ws2_misc_probe inet_ntoa le=0, inet_ntoa_same_buffer).
    const u32 raw = static_cast<u32>(m.arg(0));
    const u8* b = reinterpret_cast<const u8*>(&raw);
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    const GuestAddr buf = mgr.thread_db_block(Ws2Manager::DbBlock::Ntoa, 16);
    if (buf == 0 || m.write_ansi_string(buf, text).is_err()) {
        mgr.set_last_error(WSAENOBUFS);
        m.set_return(0);
        return;
    }
    mgr.set_last_error(0);
    m.set_return(static_cast<u64>(buf));
}

void ws2::impl_ntohl(Marshaler& m, Ws2Manager& /*mgr*/) {
    // ntohl(netlong) → byte-swap uint32_t
    u32 val = static_cast<u32>(m.arg(0));
    u32 swapped = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
                  ((val << 8) & 0xFF0000) | ((val << 24) & 0xFF000000);
    m.set_return(static_cast<u64>(swapped));
}

void ws2::impl_htonl(Marshaler& m, Ws2Manager& /*mgr*/) {
    // htonl(hostlong) → byte-swap uint32_t (same as ntohl)
    u32 val = static_cast<u32>(m.arg(0));
    u32 swapped = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
                  ((val << 8) & 0xFF0000) | ((val << 24) & 0xFF000000);
    m.set_return(static_cast<u64>(swapped));
}

void ws2::impl_ntohs(Marshaler& m, Ws2Manager& /*mgr*/) {
    // ntohs(netshort) → byte-swap uint16_t
    u16 val = static_cast<u16>(m.arg(0));
    u16 swapped = static_cast<u16>((val >> 8) | (val << 8));
    m.set_return(static_cast<u64>(swapped));
}

void ws2::impl_htons(Marshaler& m, Ws2Manager& /*mgr*/) {
    // htons(hostshort) → byte-swap uint16_t (same as ntohs)
    u16 val = static_cast<u16>(m.arg(0));
    u16 swapped = static_cast<u16>((val >> 8) | (val << 8));
    m.set_return(static_cast<u64>(swapped));
}

void ws2::impl_WSANtohs(Marshaler& m, Ws2Manager& mgr) {
    // int WSANtohs(SOCKET s, u_short netshort, u_short* lphostshort)
    const u32 sock = static_cast<u32>(m.arg(0));
    const u16 netshort = static_cast<u16>(m.arg(1));
    GuestAddr out = m.arg_ptr(2);
    if (mgr.get_native_fd(sock) < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (!out) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const u16 host = static_cast<u16>((netshort >> 8) | (netshort << 8));
    (void)m.write_u16(out, host);
    m.set_return(0);
}

void ws2::impl_WSAHtons(Marshaler& m, Ws2Manager& mgr) {
    // int WSAHtons(SOCKET s, u_short hostshort, u_short* lpnetshort)
    const u32 sock = static_cast<u32>(m.arg(0));
    const u16 hostshort = static_cast<u16>(m.arg(1));
    GuestAddr out = m.arg_ptr(2);
    if (mgr.get_native_fd(sock) < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (!out) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const u16 net = static_cast<u16>((hostshort >> 8) | (hostshort << 8));
    (void)m.write_u16(out, net);
    m.set_return(0);
}

void ws2::impl_WSANtohl(Marshaler& m, Ws2Manager& mgr) {
    const u32 sock = static_cast<u32>(m.arg(0));
    const u32 netlong = static_cast<u32>(m.arg(1));
    GuestAddr out = m.arg_ptr(2);
    if (mgr.get_native_fd(sock) < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (!out) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const u32 host = ((netlong >> 24) & 0xFF) | ((netlong >> 8) & 0xFF00) |
                     ((netlong << 8) & 0xFF0000) | ((netlong << 24) & 0xFF000000);
    (void)m.write_u32(out, host);
    m.set_return(0);
}

void ws2::impl_WSAHtonl(Marshaler& m, Ws2Manager& mgr) {
    const u32 sock = static_cast<u32>(m.arg(0));
    const u32 hostlong = static_cast<u32>(m.arg(1));
    GuestAddr out = m.arg_ptr(2);
    if (mgr.get_native_fd(sock) < 0) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (!out) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const u32 net = ((hostlong >> 24) & 0xFF) | ((hostlong >> 8) & 0xFF00) |
                    ((hostlong << 8) & 0xFF0000) | ((hostlong << 24) & 0xFF000000);
    (void)m.write_u32(out, net);
    m.set_return(0);
}

// -----------------------------------------------------------------------------
// WSAStringToAddressA — Convert address string to sockaddr
// -----------------------------------------------------------------------------
void ws2::impl_WSAStringToAddressA(Marshaler& m, Ws2Manager& mgr) {
    // int WSAStringToAddressA(LPSTR AddressString, INT AddressFamily,
    //                         LPWSAPROTOCOL_INFOA lpProtocolInfo,
    //                         LPSOCKADDR lpAddress, LPINT lpAddressLength)
    GuestAddr str_ptr = m.arg_ptr(0);
    int af = static_cast<int>(m.arg(1));
    // arg(2) is protocol info - unused
    GuestAddr addr_ptr = m.arg_ptr(3);
    GuestAddr len_ptr = m.arg_ptr(4);
    
    VITREO_TRACE("[ws2_32] WSAStringToAddressA(str=0x%llx, af=%d, addr=0x%llx)\n",
                 str_ptr, af, addr_ptr);
    
    if (!str_ptr || !addr_ptr || !len_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    
    auto str_res = m.read_ansi_string(str_ptr, 64);
    if (str_res.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    std::string addr_str = str_res.value();
    
    auto len_res = m.read_u32(len_ptr);
    if (len_res.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    u32 buf_len = len_res.value();
    
    if (af == AF_INET) {
        if (buf_len < 16) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        
        // Parse "address:port" or just "address"
        std::string ip_part = addr_str;
        u16 port = 0;
        
        size_t colon = addr_str.rfind(':');
        if (colon != std::string::npos) {
            ip_part = addr_str.substr(0, colon);
            const char* port_str = addr_str.c_str() + colon + 1;
            char* end = nullptr;
            long port_val = std::strtol(port_str, &end, 10);
            if (end == port_str || port_val < 0 || port_val > 65535) {
                mgr.set_last_error(WSAEINVAL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            port = static_cast<u16>(port_val);
        }
        
        struct in_addr in;
        if (::inet_pton(AF_INET, ip_part.c_str(), &in) != 1) {
            mgr.set_last_error(WSAEINVAL);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
        
        // Write Windows sockaddr_in (16 bytes)
        (void)m.write_u16(addr_ptr + 0, AF_INET);
        (void)m.write_u16(addr_ptr + 2, htons(port));
        (void)m.write_u32(addr_ptr + 4, in.s_addr);
        for (u32 i = 8; i < 16; ++i)
            (void)m.write_u8(addr_ptr + i, 0);
        
        (void)m.write_u32(len_ptr, 16);
        m.set_return(0);
    } else {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    }
}

// -----------------------------------------------------------------------------
// WSAAddressToStringA / WSAAddressToStringW — sockaddr to text
// -----------------------------------------------------------------------------
//
// int WSAAddressToStringX(LPSOCKADDR lpsaAddress, DWORD dwAddressLength,
//     LPWSAPROTOCOL_INFOX lpProtocolInfo, LPXSTR lpszAddressString,
//     LPDWORD lpdwAddressStringLength)
//
// Learn gives the parameters; tools/oracle_probe/ws2_address_to_string_probe
// measured the rest on Windows 11 (identical for x86 and x64, and for A and W
// in characters):
// - before WSAStartup: WSANOTINITIALISED, nothing written;
// - a NULL address or length pointer, or a zero address length: WSAEINVAL;
// - a family other than AF_INET / AF_INET6: WSAEINVAL; a length below the
//   family's sockaddr (16 / 28 bytes): WSAEFAULT;
// - IPv4 is "a.b.c.d", IPv6 the RtlIpv6AddressToString text with "%scope"
//   when the scope id is nonzero; a nonzero port appends ":port" (IPv6 in
//   brackets). Flow info and sin_zero are ignored;
// - *lpdwAddressStringLength below the text plus NUL: WSAEFAULT with the
//   required size written back; a NULL buffer otherwise: WSAEFAULT with the
//   length untouched; success writes the text, the length including the NUL,
//   and clears the last error.
namespace {

/// The IPv6 text RtlIpv6AddressToString produces (probe "v6" rows): lowercase
/// hex without leading zeros, the longest run of two or more zero groups
/// (the first of equal runs) as "::". The last 32 bits are dotted for an
/// IPv4-compatible (::w.x.y.z), mapped (::ffff:) or translated (::ffff:0:)
/// address whose 7th group is nonzero, and always for an ISATAP interface
/// identifier (0 or 200 then 5efe).
std::string ipv6_text(const u8 bytes[16]) {
    u16 w[8];
    for (int i = 0; i < 8; ++i) {
        w[i] = static_cast<u16>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    }
    auto dotted = [&] {
        char text[16];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[12], bytes[13],
                      bytes[14], bytes[15]);
        return std::string(text);
    };
    auto hex_groups = [&](int count) {
        int best = -1, best_len = 0;
        for (int i = 0; i < count;) {
            if (w[i] != 0) { ++i; continue; }
            int j = i;
            while (j < count && w[j] == 0) ++j;
            if (j - i >= 2 && j - i > best_len) { best = i; best_len = j - i; }
            i = j;
        }
        std::string out;
        char group[8];
        for (int i = 0; i < count;) {
            if (i == best) {
                out += "::";
                i += best_len;
                continue;
            }
            if (!out.empty() && out.back() != ':') out += ':';
            std::snprintf(group, sizeof(group), "%x", w[i]);
            out += group;
            ++i;
        }
        return out;
    };
    const bool first4_zero = w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0;
    if (first4_zero && w[6] != 0) {
        if (w[4] == 0 && w[5] == 0) return "::" + dotted();
        if (w[4] == 0 && w[5] == 0xffff) return "::ffff:" + dotted();
        if (w[4] == 0xffff && w[5] == 0) return "::ffff:0:" + dotted();
    }
    if ((w[4] & 0xfdff) == 0 && w[5] == 0x5efe) {
        std::string head = hex_groups(6);
        if (head.back() != ':') head += ':';
        return head + dotted();
    }
    return hex_groups(8);
}

void address_to_string(Marshaler& m, Ws2Manager& mgr, bool wide) {
    const GuestAddr addr_ptr = m.arg_ptr(0);
    const u32 addr_len = static_cast<u32>(m.arg(1));
    // arg(2), lpProtocolInfo, chooses among providers; every provider of
    // these two families formats the same way.
    const GuestAddr str_ptr = m.arg_ptr(3);
    const GuestAddr str_len_ptr = m.arg_ptr(4);
    VITREO_TRACE("[ws2_32] WSAAddressToString%c(addr=0x%llx, len=%u, str=0x%llx)\n",
                 wide ? 'W' : 'A', static_cast<unsigned long long>(addr_ptr), addr_len,
                 static_cast<unsigned long long>(str_ptr));
    auto fail = [&](u32 error) {
        mgr.set_last_error(error);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    };
    if (!mgr.winsock_ready()) return fail(WSANOTINITIALISED);
    if (addr_ptr == 0 || str_len_ptr == 0 || addr_len == 0) return fail(WSAEINVAL);

    u8 raw[28] = {};
    const auto family = m.read_u16(addr_ptr);
    if (family.is_err()) return fail(WSAEFAULT);
    std::string text;
    if (family.value() == kWinAfInet) {
        if (addr_len < 16) return fail(WSAEFAULT);
        if (m.memory().read_bytes(addr_ptr, reinterpret_cast<Byte*>(raw), 16).is_err()) {
            return fail(WSAEFAULT);
        }
        char buf[32];
        const u16 port = static_cast<u16>((raw[2] << 8) | raw[3]);
        if (port != 0) {
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", raw[4], raw[5], raw[6], raw[7], port);
        } else {
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", raw[4], raw[5], raw[6], raw[7]);
        }
        text = buf;
    } else if (family.value() == kWinAfInet6) {
        if (addr_len < 28) return fail(WSAEFAULT);
        if (m.memory().read_bytes(addr_ptr, reinterpret_cast<Byte*>(raw), 28).is_err()) {
            return fail(WSAEFAULT);
        }
        const u16 port = static_cast<u16>((raw[2] << 8) | raw[3]);
        const u32 scope = static_cast<u32>(raw[24]) | (static_cast<u32>(raw[25]) << 8) |
                          (static_cast<u32>(raw[26]) << 16) | (static_cast<u32>(raw[27]) << 24);
        text = ipv6_text(raw + 8);
        if (scope != 0) text += "%" + std::to_string(scope);
        if (port != 0) text = "[" + text + "]:" + std::to_string(port);
    } else {
        return fail(WSAEINVAL);
    }

    const u32 need = static_cast<u32>(text.size() + 1);
    const auto have = m.read_u32(str_len_ptr);
    if (have.is_err()) return fail(WSAEFAULT);
    if (have.value() < need) {
        (void)m.write_u32(str_len_ptr, need);
        return fail(WSAEFAULT);
    }
    if (str_ptr == 0) return fail(WSAEFAULT);
    const auto written = wide ? m.write_wide_string(str_ptr, text)
                              : m.write_ansi_string(str_ptr, text);
    if (written.is_err()) return fail(WSAEFAULT);
    (void)m.write_u32(str_len_ptr, need);
    mgr.set_last_error(0);
    m.set_return(0);
}

} // namespace

void ws2::impl_WSAAddressToStringA(Marshaler& m, Ws2Manager& mgr) {
    address_to_string(m, mgr, false);
}

// -----------------------------------------------------------------------------
// WSAStringToAddressW — Unicode address string to sockaddr
// -----------------------------------------------------------------------------
void ws2::impl_WSAStringToAddressW(Marshaler& m, Ws2Manager& mgr) {
    // int WSAStringToAddressW(LPWSTR AddressString, INT AddressFamily,
    //                         LPWSAPROTOCOL_INFOW lpProtocolInfo,
    //                         LPSOCKADDR lpAddress, LPINT lpAddressLength)
    GuestAddr str_ptr = m.arg_ptr(0);
    int af = static_cast<int>(m.arg(1));
    GuestAddr addr_ptr = m.arg_ptr(3);
    GuestAddr len_ptr = m.arg_ptr(4);

    VITREO_TRACE("[ws2_32] WSAStringToAddressW(str=0x%llx, af=%d, addr=0x%llx)\n",
                 str_ptr, af, addr_ptr);

    if (!str_ptr || !addr_ptr || !len_ptr) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    auto str_res = m.read_wide_string(str_ptr, 64);
    if (str_res.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    // Reuse the ANSI body by writing the UTF-8 form through the same parser.
    // Temporarily synthesize the A-path arguments on the stack is awkward;
    // duplicate the AF_INET parse against the UTF-8 conversion of the wide
    // string (read_wide_string already returns UTF-8).
    std::string addr_str = str_res.value();

    auto len_res = m.read_u32(len_ptr);
    if (len_res.is_err()) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    u32 buf_len = len_res.value();

    if (af == AF_INET) {
        if (buf_len < 16) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }

        std::string ip_part = addr_str;
        u16 port = 0;
        size_t colon = addr_str.rfind(':');
        if (colon != std::string::npos) {
            ip_part = addr_str.substr(0, colon);
            const char* port_str = addr_str.c_str() + colon + 1;
            char* end = nullptr;
            long port_val = std::strtol(port_str, &end, 10);
            if (end == port_str || port_val < 0 || port_val > 65535) {
                mgr.set_last_error(WSAEINVAL);
                m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
                return;
            }
            port = static_cast<u16>(port_val);
        }

        struct in_addr in;
        if (::inet_pton(AF_INET, ip_part.c_str(), &in) != 1) {
            mgr.set_last_error(WSAEINVAL);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }

        (void)m.write_u16(addr_ptr + 0, AF_INET);
        (void)m.write_u16(addr_ptr + 2, htons(port));
        (void)m.write_u32(addr_ptr + 4, in.s_addr);
        for (u32 i = 8; i < 16; ++i)
            (void)m.write_u8(addr_ptr + i, 0);

        (void)m.write_u32(len_ptr, 16);
        m.set_return(0);
    } else {
        mgr.set_last_error(WSAEAFNOSUPPORT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    }
}

void ws2::impl_WSAAddressToStringW(Marshaler& m, Ws2Manager& mgr) {
    address_to_string(m, mgr, true);
}

} // namespace vitreo::winapi
