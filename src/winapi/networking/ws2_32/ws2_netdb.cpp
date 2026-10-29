// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_netdb.cpp
/// @brief getprotobyname / getprotobynumber / getservbyname / getservbyport and
///        the per-thread PROTOENT / SERVENT / HOSTENT builders.
///
/// Contract (Learn getprotobyname, getservbyname, protoent, servent, hostent):
///  - the structures are allocated by Winsock, one copy per thread, and are
///    overwritten by the next call of the same kind on that thread;
///  - the layout is pointer-width sensitive; winsock2.h swaps s_proto and
///    s_port on _WIN64;
///  - a miss returns NULL with WSANO_DATA (11004), measured in
///    tools/oracle_probe/ws2_resolve_probe.expected.
/// The lookups use the Windows protocol/services databases (Winsock reads
/// %SystemRoot%\System32\drivers\etc\{protocol,services}), not Darwin's
/// /etc/protocols, whose aliases differ ("IPV6" vs Windows "IPv6").

#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <optional>
#include <strings.h>
#include <vector>

#include <arpa/inet.h>

namespace vitreo::winapi {

namespace {

#include "winapi/networking/ws2_32/ws2_netdb_data.inc"

struct ProtoEntry {
    std::string name;
    int number = 0;
    std::vector<std::string> aliases;
};

struct ServEntry {
    std::string name;
    u16 port = 0;  // host order
    std::string proto;
    std::vector<std::string> aliases;
};

std::vector<std::string> split_ws(const char* line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = line;; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

const std::vector<ProtoEntry>& protocols() {
    static const std::vector<ProtoEntry> table = [] {
        std::vector<ProtoEntry> t;
        for (const char* line : kWinProtocolDb) {
            auto f = split_ws(line);
            if (f.size() < 2) continue;
            ProtoEntry e;
            e.name = f[0];
            e.number = std::atoi(f[1].c_str());
            e.aliases.assign(f.begin() + 2, f.end());
            t.push_back(std::move(e));
        }
        return t;
    }();
    return table;
}

const std::vector<ServEntry>& services() {
    static const std::vector<ServEntry> table = [] {
        std::vector<ServEntry> t;
        for (const char* line : kWinServicesDb) {
            auto f = split_ws(line);
            if (f.size() < 2) continue;
            const auto slash = f[1].find('/');
            if (slash == std::string::npos) continue;
            ServEntry e;
            e.name = f[0];
            e.port = static_cast<u16>(std::atoi(f[1].substr(0, slash).c_str()));
            e.proto = f[1].substr(slash + 1);
            e.aliases.assign(f.begin() + 2, f.end());
            t.push_back(std::move(e));
        }
        return t;
    }();
    return table;
}

bool name_matches(const std::string& want, const std::string& name,
                  const std::vector<std::string>& aliases) {
    if (::strcasecmp(want.c_str(), name.c_str()) == 0) return true;
    for (const auto& a : aliases) {
        if (::strcasecmp(want.c_str(), a.c_str()) == 0) return true;
    }
    return false;
}

/// Lays out a netdb structure plus its strings and pointer arrays in one
/// guest block: [struct][pointer arrays][strings].
class DbLayout {
public:
    DbLayout(bool is64, u32 struct_size) : is64_(is64), struct_size_(struct_size) {}
    [[nodiscard]] u32 ptr() const { return is64_ ? 8u : 4u; }
    void reserve_array(usize entries) { arrays_ += static_cast<u32>((entries + 1) * ptr()); }
    void reserve_string(const std::string& s) { strings_ += static_cast<u32>(s.size() + 1); }
    void reserve_bytes(u32 n) { strings_ += n; }
    [[nodiscard]] u32 total() const { return struct_size_ + arrays_ + strings_; }

    void begin(GuestAddr base) {
        base_ = base;
        array_cursor_ = base + struct_size_;
        string_cursor_ = array_cursor_ + arrays_;
    }
    [[nodiscard]] GuestAddr take_array(usize entries) {
        const GuestAddr a = array_cursor_;
        array_cursor_ += static_cast<u32>((entries + 1) * ptr());
        return a;
    }
    GuestAddr put_string(Marshaler& m, const std::string& s) {
        const GuestAddr a = string_cursor_;
        (void)m.memory().write_bytes(a, reinterpret_cast<const Byte*>(s.c_str()),
                                     s.size() + 1);
        string_cursor_ += static_cast<u32>(s.size() + 1);
        return a;
    }
    GuestAddr put_bytes(Marshaler& m, const u8* data, u32 n) {
        const GuestAddr a = string_cursor_;
        (void)m.memory().write_bytes(a, reinterpret_cast<const Byte*>(data), n);
        string_cursor_ += n;
        return a;
    }
    void put_ptr(Marshaler& m, GuestAddr at, GuestAddr value) {
        if (is64_) (void)m.write_u64(at, value);
        else (void)m.write_u32(at, static_cast<u32>(value));
    }
    GuestAddr put_string_array(Marshaler& m, const std::vector<std::string>& v) {
        const GuestAddr arr = take_array(v.size());
        for (usize i = 0; i < v.size(); ++i) {
            put_ptr(m, arr + static_cast<GuestAddr>(i * ptr()), put_string(m, v[i]));
        }
        put_ptr(m, arr + static_cast<GuestAddr>(v.size() * ptr()), 0);
        return arr;
    }

private:
    bool is64_;
    u32 struct_size_;
    u32 arrays_ = 0;
    u32 strings_ = 0;
    GuestAddr base_ = 0;
    GuestAddr array_cursor_ = 0;
    GuestAddr string_cursor_ = 0;
};

void fail_null(Marshaler& m, Ws2Manager& mgr, u32 error) {
    mgr.set_last_error(error);
    m.set_return(0);
}

/// PROTOENT: {char* p_name; char** p_aliases; short p_proto;}
/// x86 = 12 bytes (p_proto@8), x64 = 24 bytes (p_proto@16).
void return_protoent(Marshaler& m, Ws2Manager& mgr, const ProtoEntry& e) {
    const bool is64 = m.cpu_is_64bit();
    DbLayout lay(is64, is64 ? 24u : 12u);
    lay.reserve_string(e.name);
    lay.reserve_array(e.aliases.size());
    for (const auto& a : e.aliases) lay.reserve_string(a);
    const GuestAddr base = mgr.thread_db_block(Ws2Manager::DbBlock::Proto, lay.total());
    if (base == 0) {
        fail_null(m, mgr, WSAENOBUFS);
        return;
    }
    lay.begin(base);
    const GuestAddr aliases = lay.put_string_array(m, e.aliases);
    lay.put_ptr(m, base, lay.put_string(m, e.name));
    lay.put_ptr(m, base + lay.ptr(), aliases);
    (void)m.write_u16(base + 2 * lay.ptr(), static_cast<u16>(e.number));
    (void)m.write_u16(base + 2 * lay.ptr() + 2, 0);
    if (is64) (void)m.write_u32(base + 20, 0);
    m.set_return(base);
}

/// SERVENT: x86 {s_name@0, s_aliases@4, s_port@8, s_proto@12} = 16 bytes;
/// x64 {s_name@0, s_aliases@8, s_proto@16, s_port@24} = 32 bytes.
void return_servent(Marshaler& m, Ws2Manager& mgr, const ServEntry& e) {
    const bool is64 = m.cpu_is_64bit();
    DbLayout lay(is64, is64 ? 32u : 16u);
    lay.reserve_string(e.name);
    lay.reserve_string(e.proto);
    lay.reserve_array(e.aliases.size());
    for (const auto& a : e.aliases) lay.reserve_string(a);
    const GuestAddr base = mgr.thread_db_block(Ws2Manager::DbBlock::Serv, lay.total());
    if (base == 0) {
        fail_null(m, mgr, WSAENOBUFS);
        return;
    }
    lay.begin(base);
    const GuestAddr aliases = lay.put_string_array(m, e.aliases);
    const GuestAddr name = lay.put_string(m, e.name);
    const GuestAddr proto = lay.put_string(m, e.proto);
    const u16 port_net = htons(e.port);
    lay.put_ptr(m, base, name);
    lay.put_ptr(m, base + lay.ptr(), aliases);
    if (is64) {
        lay.put_ptr(m, base + 16, proto);
        (void)m.write_u64(base + 24, port_net);
    } else {
        (void)m.write_u32(base + 8, port_net);
        lay.put_ptr(m, base + 12, proto);
    }
    m.set_return(base);
}

/// Read an optional guest ANSI string. Returns false (WSAEFAULT) when the
/// pointer is non-NULL but unreadable.
bool read_opt_string(Marshaler& m, GuestAddr p, std::string& out) {
    out.clear();
    if (p == 0) return true;
    auto r = m.read_ansi_string(p);
    if (r.is_err()) return false;
    out = r.value();
    return true;
}

}  // namespace

/// HOSTENT: {char* h_name; char** h_aliases; short h_addrtype;
///           short h_length; char** h_addr_list;}
/// x86 = 16 bytes (h_addr_list@12); x64 = 32 bytes (h_addrtype@16,
/// h_length@18, h_addr_list@24) -- winsock2.h.
GuestAddr write_hostent_at(Marshaler& m, GuestAddr base, const std::string& name,
                           const std::vector<std::string>& aliases, u16 addrtype,
                           const std::vector<std::vector<u8>>& addrs) {
    const bool is64 = m.cpu_is_64bit();
    DbLayout lay(is64, is64 ? 32u : 16u);
    lay.reserve_string(name);
    lay.reserve_array(aliases.size());
    for (const auto& a : aliases) lay.reserve_string(a);
    lay.reserve_array(addrs.size());
    u16 length = 0;
    for (const auto& a : addrs) {
        length = static_cast<u16>(a.size());
        lay.reserve_bytes(static_cast<u32>(a.size()));
    }
    lay.begin(base);
    const GuestAddr alias_arr = lay.put_string_array(m, aliases);
    const GuestAddr addr_arr = lay.take_array(addrs.size());
    for (usize i = 0; i < addrs.size(); ++i) {
        lay.put_ptr(m, addr_arr + static_cast<GuestAddr>(i * lay.ptr()),
                    lay.put_bytes(m, addrs[i].data(),
                                  static_cast<u32>(addrs[i].size())));
    }
    lay.put_ptr(m, addr_arr + static_cast<GuestAddr>(addrs.size() * lay.ptr()), 0);
    lay.put_ptr(m, base, lay.put_string(m, name));
    lay.put_ptr(m, base + lay.ptr(), alias_arr);
    (void)m.write_u16(base + 2 * lay.ptr(), addrtype);
    (void)m.write_u16(base + 2 * lay.ptr() + 2, length);
    if (is64) (void)m.write_u32(base + 20, 0);
    lay.put_ptr(m, base + (is64 ? 24u : 12u), addr_arr);
    return base;
}

u32 hostent_packed_size(Marshaler& m, const std::string& name,
                        const std::vector<std::string>& aliases,
                        const std::vector<std::vector<u8>>& addrs) {
    const bool is64 = m.cpu_is_64bit();
    DbLayout lay(is64, is64 ? 32u : 16u);
    lay.reserve_string(name);
    lay.reserve_array(aliases.size());
    for (const auto& a : aliases) lay.reserve_string(a);
    lay.reserve_array(addrs.size());
    for (const auto& a : addrs) lay.reserve_bytes(static_cast<u32>(a.size()));
    return lay.total();
}

GuestAddr write_hostent(Marshaler& m, Ws2Manager& mgr, const std::string& name,
                        const std::vector<std::string>& aliases, u16 addrtype,
                        const std::vector<std::vector<u8>>& addrs) {
    const u32 need = hostent_packed_size(m, name, aliases, addrs);
    const GuestAddr base = mgr.thread_db_block(Ws2Manager::DbBlock::Host, need);
    if (base == 0) return 0;
    return write_hostent_at(m, base, name, aliases, addrtype, addrs);
}

bool write_hostent_into(Marshaler& m, GuestAddr base, u32 capacity,
                        const std::string& name,
                        const std::vector<std::string>& aliases, u16 addrtype,
                        const std::vector<std::vector<u8>>& addrs) {
    if (hostent_packed_size(m, name, aliases, addrs) > capacity) return false;
    return write_hostent_at(m, base, name, aliases, addrtype, addrs) != 0;
}

std::optional<u16> ws2_service_port(const std::string& name, const char* proto) {
    for (const auto& e : services()) {
        if (::strcasecmp(proto, e.proto.c_str()) != 0) continue;
        if (name_matches(name, e.name, e.aliases)) return e.port;
    }
    return std::nullopt;
}

std::string ws2_service_name(u16 port, bool udp) {
    const char* want = udp ? "udp" : "tcp";
    for (const auto& e : services()) {
        if (e.port == port && ::strcasecmp(e.proto.c_str(), want) == 0) return e.name;
    }
    return {};
}

void ws2::impl_getprotobyname(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::NullPointer)) return;

    std::string name;
    if (m.arg_ptr(0) == 0 || !read_opt_string(m, m.arg_ptr(0), name)) {
        fail_null(m, mgr, WSAEFAULT);
        return;
    }
    for (const auto& e : protocols()) {
        if (name_matches(name, e.name, e.aliases)) {
            return_protoent(m, mgr, e);
            return;
        }
    }
    fail_null(m, mgr, WSANO_DATA);
}

void ws2::impl_getprotobynumber(Marshaler& m, Ws2Manager& mgr) {
    const int number = static_cast<int>(static_cast<u32>(m.arg(0)));
    for (const auto& e : protocols()) {
        if (e.number == number) {
            return_protoent(m, mgr, e);
            return;
        }
    }
    fail_null(m, mgr, WSANO_DATA);
}

void impl_getservbyname(Marshaler& m, Ws2Manager& mgr) {
    std::string name, proto;
    if (m.arg_ptr(0) == 0 || !read_opt_string(m, m.arg_ptr(0), name) ||
        !read_opt_string(m, m.arg_ptr(1), proto)) {
        fail_null(m, mgr, WSAEFAULT);
        return;
    }
    for (const auto& e : services()) {
        if (!proto.empty() && ::strcasecmp(proto.c_str(), e.proto.c_str()) != 0) continue;
        if (name_matches(name, e.name, e.aliases)) {
            return_servent(m, mgr, e);
            return;
        }
    }
    fail_null(m, mgr, WSANO_DATA);
}

void ws2::impl_getservbyport(Marshaler& m, Ws2Manager& mgr) {
    // port is in network byte order (Learn getservbyport).
    const u16 port = ntohs(static_cast<u16>(m.arg(0)));
    std::string proto;
    if (!read_opt_string(m, m.arg_ptr(1), proto)) {
        fail_null(m, mgr, WSAEFAULT);
        return;
    }
    for (const auto& e : services()) {
        if (!proto.empty() && ::strcasecmp(proto.c_str(), e.proto.c_str()) != 0) continue;
        if (e.port == port) {
            return_servent(m, mgr, e);
            return;
        }
    }
    fail_null(m, mgr, WSANO_DATA);
}

}  // namespace vitreo::winapi
