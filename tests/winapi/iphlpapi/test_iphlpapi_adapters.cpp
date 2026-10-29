/// @file test_iphlpapi_adapters.cpp
/// @brief IP Helper adapter views, identity conversions, TCP table and DHCP
///        client API through the exported thunks, on x86 and x64.
///
/// Layout offsets and Windows values are the ones measured by
/// tools/oracle_probe/iphlp_adapters_probe{32,64}.expected and
/// iphlp_misc_probe{32,64}.expected (typed here, not taken from the generated
/// adapter_contract.inc the implementation uses). Host facts (addresses,
/// masks, MACs, interface indices) come from getifaddrs in the test itself.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/networking/iphlpapi/iphlpapi.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/com/ole32/com_core.hpp>
#include <winapi/com/ole32/task_memory.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

struct HostIpv4 { std::string address; std::string mask; u8 prefix; };
struct HostLink { u32 index = 0; std::vector<u8> mac; std::vector<HostIpv4> v4; bool loopback = false; };

/// Ground truth straight from the host, independent of the implementation.
std::map<std::string, HostLink> host_links() {
    std::map<std::string, HostLink> out;
    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0) return out;
    for (auto* e = head; e; e = e->ifa_next) {
        if (!e->ifa_name || !e->ifa_addr) continue;
        auto& link = out[e->ifa_name];
        link.loopback = (e->ifa_flags & IFF_LOOPBACK) != 0;
        if (e->ifa_addr->sa_family == AF_LINK) {
            const auto* dl = reinterpret_cast<const sockaddr_dl*>(e->ifa_addr);
            link.index = dl->sdl_index;
            const auto* mac = reinterpret_cast<const u8*>(LLADDR(dl));
            link.mac.assign(mac, mac + dl->sdl_alen);
        } else if (e->ifa_addr->sa_family == AF_INET) {
            char a[INET_ADDRSTRLEN] = {}, m[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(e->ifa_addr)->sin_addr, a, sizeof(a));
            u8 prefix = 32;
            if (e->ifa_netmask) {
                const auto mask = reinterpret_cast<const sockaddr_in*>(e->ifa_netmask)->sin_addr;
                ::inet_ntop(AF_INET, &mask, m, sizeof(m));
                prefix = static_cast<u8>(__builtin_popcount(mask.s_addr));
            }
            link.v4.push_back({a, m, prefix});
        }
    }
    ::freeifaddrs(head);
    return out;
}

const HostLink* link_by_index(const std::map<std::string, HostLink>& links, u32 index) {
    for (const auto& [name, link] : links) if (link.index == index) return &link;
    return nullptr;
}

std::string braced_guid(const u8* g) {
    char text[40];
    std::snprintf(text, sizeof(text), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return text;
}

bool is_braced_guid(const std::string& s) {
    if (s.size() != 38 || s.front() != '{' || s.back() != '}') return false;
    for (usize i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (s[i] != '-') return false; continue; }
        if (!std::isxdigit(static_cast<unsigned char>(s[i])) || std::islower(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// Windows layouts measured by iphlp_adapters_probe (x86 / x64).
struct InfoLayout {
    u32 size, combo, name, desc, addr_len, addr, index, type, dhcp, current, ip_list, gw_list, dhcp_server, have_wins,
        lease_obtained, lease_expires, addr_string, mask_string, context, addr_string_size;
};
constexpr InfoLayout kInfo32{640, 4, 8, 268, 400, 404, 412, 416, 420, 424, 428, 468, 508, 548, 632, 636, 4, 20, 36, 40};
constexpr InfoLayout kInfo64{704, 8, 12, 272, 404, 408, 416, 420, 424, 432, 440, 488, 536, 584, 688, 696, 8, 24, 40, 48};

struct GaaLayout {
    u32 size, next, name, unicast, anycast, multicast, dns, suffix, desc, friendly, phys, phys_len, flags, mtu, if_type,
        oper, v6index, zones, prefix, tx, rx, wins, gateway, metric4, metric6, luid, dhcp4, compartment, net_guid,
        conn_type, tunnel;
    u32 uni_size, uni_addr, uni_prefix_origin, uni_suffix_origin, uni_dad, uni_valid, uni_preferred, uni_lease, uni_onlink;
    u32 list_size, list_addr, prefix_size, prefix_len, sockaddr_len_off, ptr;
};
constexpr GaaLayout kGaa32{376, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 56, 60, 64, 68, 72, 76, 140, 144, 152, 160,
                           164, 168, 172, 176, 184, 192, 196, 212, 216,
                           48, 12, 20, 24, 28, 32, 36, 40, 44, 24, 12, 24, 20, 4, 4};
constexpr GaaLayout kGaa64{448, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 92, 96, 100, 104, 108, 112, 176, 184, 192,
                           200, 208, 216, 220, 224, 232, 248, 252, 268, 272,
                           64, 16, 32, 36, 40, 44, 48, 52, 56, 32, 16, 40, 32, 8, 8};

class IphlpapiAdaptersTest : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA = 0x400000;
    static constexpr usize DATA_SIZE = 0x100000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, DATA_SIZE, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        mgr.set_heap_manager(&heap);
        mgr.set_host_name_provider([] { return std::string("VITREOHOST"); });
        exports = create_iphlpapi_thunks(mgr);
        dhcp = create_dhcpcsvc_thunks();
        marshaler.attach_cpu(cpu);
    }

    void mode(bool wide) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        wide_ = wide;
    }

    /// Place stdcall (x86) or Microsoft x64 arguments. On x86 every value is
    /// one 4-byte slot, so a by-value structure is passed as its fields.
    void args(std::initializer_list<u64> values) {
        usize i = 0;
        for (u64 v : values) {
            if (wide_) {
                if (i == 0) cpu.gpr.rcx = v;
                else if (i == 1) cpu.gpr.rdx = v;
                else if (i == 2) cpu.gpr.r8 = v;
                else if (i == 3) cpu.gpr.r9 = v;
                else ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 8 + i * 8, v).is_ok());
            } else {
                ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(v)).is_ok());
            }
            ++i;
        }
    }

    u64 call(DllExports& table, const char* name, std::initializer_list<u64> values) {
        args(values);
        for (auto& e : table.entries) {
            if (e.name == name) { e.handler(marshaler); return wide_ ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFu); }
        }
        ADD_FAILURE() << "missing export " << name;
        return ~u64{0};
    }
    u64 call(const char* name, std::initializer_list<u64> values) { return call(exports, name, values); }

    u32 u32at(GuestAddr a) { return memory.read_u32(a).value_or(0xDEADBEEF); }
    u64 u64at(GuestAddr a) { return memory.read_u64(a).value_or(0xDEADBEEFDEADBEEFull); }
    u64 ptrat(GuestAddr a) { return wide_ ? u64at(a) : u32at(a); }
    std::string ascii(GuestAddr a, usize max = 512) {
        std::string s;
        for (usize i = 0; i < max; ++i) { const u8 c = memory.read_u8(a + i).value_or(0); if (!c) break; s.push_back(static_cast<char>(c)); }
        return s;
    }
    std::string wide(GuestAddr a, usize max = 512) {
        std::string s;
        for (usize i = 0; i < max; ++i) { const u16 c = memory.read_u16(a + i * 2).value_or(0); if (!c) break; s.push_back(c < 0x80 ? static_cast<char>(c) : '?'); }
        return s;
    }

    /// GetAdaptersAddresses into DATA; returns the adapter addresses.
    std::vector<GuestAddr> gaa(u32 family, u32 flags, u32* rc_out = nullptr) {
        const GuestAddr size = DATA + 0x10;
        const GuestAddr buffer = DATA + 0x100;
        EXPECT_TRUE(memory.write_u32(size, 0).is_ok());
        u32 rc = static_cast<u32>(call("GetAdaptersAddresses", {family, flags, 0, 0, size}));
        EXPECT_EQ(rc, 111u);
        EXPECT_LT(u32at(size), DATA_SIZE - 0x200);
        rc = static_cast<u32>(call("GetAdaptersAddresses", {family, flags, 0, buffer, size}));
        if (rc_out) *rc_out = rc;
        std::vector<GuestAddr> out;
        if (rc != 0) return out;
        const GaaLayout& L = wide_ ? kGaa64 : kGaa32;
        for (GuestAddr a = buffer; a; a = ptrat(a + L.next)) out.push_back(a);
        return out;
    }
    GuestAddr find_loopback(const std::vector<GuestAddr>& list) {
        const GaaLayout& L = wide_ ? kGaa64 : kGaa32;
        for (GuestAddr a : list) if (u32at(a + L.if_type) == 24) return a;
        return 0;
    }
    std::vector<GuestAddr> chain(GuestAddr first) {
        std::vector<GuestAddr> out;
        for (GuestAddr n = first; n && out.size() < 4096; n = ptrat(n + 8)) out.push_back(n);
        return out;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    IphlpapiManager mgr;
    DllExports exports;
    DllExports dhcp;
    bool wide_ = false;
};

}  // namespace

// GetAdaptersInfo: x86 entries are 640 bytes with 32-bit lease times, x64 704
// (iphlp_adapters_probe raw-gai-size need=640/704); every field sits at the
// SDK offset and describes the host adapter it names.
TEST_F(IphlpapiAdaptersTest, GetAdaptersInfoLayoutAndHostFieldsBothBitnesses) {
    const auto links = host_links();
    const u32 loopback_index = ::if_nametoindex("lo0");
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const InfoLayout& L = w ? kInfo64 : kInfo32;
        const GuestAddr size = DATA + 0x10, buffer = DATA + 0x1000;
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetAdaptersInfo", {0, 0}), 87u);  // gai-null-null
        EXPECT_EQ(get_last_error(), 0xDEADu);
        ASSERT_TRUE(memory.write_u32(size, 0).is_ok());
        set_last_error(0xDEAD);
        ASSERT_EQ(call("GetAdaptersInfo", {0, size}), 111u);
        EXPECT_EQ(get_last_error(), 0u);  // gai-size-query le=0x0
        const u32 need = u32at(size);
        ASSERT_GE(need, L.size);
        ASSERT_TRUE(memory.write_u32(size, 0x100000).is_ok());
        EXPECT_EQ(call("GetAdaptersInfo", {0, size}), 111u);  // NULL buffer, any size
        EXPECT_EQ(u32at(size), need);
        ASSERT_TRUE(memory.write_u32(size, need - 1).is_ok());
        EXPECT_EQ(call("GetAdaptersInfo", {buffer, size}), 111u);
        EXPECT_EQ(u32at(size), need);
        set_last_error(0xDEAD);
        ASSERT_EQ(call("GetAdaptersInfo", {buffer, size}), 0u);
        EXPECT_EQ(u32at(size), need);
        EXPECT_EQ(get_last_error(), 0u);

        usize entries = 0, with_address = 0;
        for (GuestAddr e = buffer; e; e = ptrat(e)) {
            ++entries;
            ASSERT_LT(entries, 64u);
            const u32 index = u32at(e + L.index);
            EXPECT_NE(index, loopback_index) << "loopback is never listed (Learn)";
            EXPECT_EQ(u32at(e + L.combo), index);
            EXPECT_TRUE(is_braced_guid(ascii(e + L.name))) << ascii(e + L.name);
            EXPECT_FALSE(ascii(e + L.desc).empty());
            EXPECT_EQ(ptrat(e + L.current), 0u);
            EXPECT_EQ(u32at(e + L.have_wins), 0u);
            const HostLink* link = link_by_index(links, index);
            ASSERT_NE(link, nullptr) << "entry index " << index;
            ASSERT_EQ(u32at(e + L.addr_len), std::min<usize>(link->mac.size(), 8));
            for (usize i = 0; i < link->mac.size() && i < 8; ++i)
                EXPECT_EQ(memory.read_u8(e + L.addr + i).value_or(0), link->mac[i]);
            const std::string ip = ascii(e + L.ip_list + L.addr_string, 16);
            const std::string mask = ascii(e + L.ip_list + L.mask_string, 16);
            if (!link->v4.empty()) {
                ++with_address;
                EXPECT_EQ(ip, link->v4[0].address);
                EXPECT_EQ(mask, link->v4[0].mask);
                in_addr value{};
                ASSERT_EQ(::inet_pton(AF_INET, ip.c_str(), &value), 1);
                EXPECT_EQ(u32at(e + L.ip_list + L.context), value.s_addr);  // raw-gai ctx
                // Every further address is chained, in host order.
                GuestAddr more = ptrat(e + L.ip_list);
                for (usize k = 1; k < link->v4.size(); ++k) {
                    ASSERT_NE(more, 0u);
                    EXPECT_EQ(ascii(more + L.addr_string, 16), link->v4[k].address);
                    more = ptrat(more);
                }
                EXPECT_EQ(more, 0u);
                // A gateway lies on the adapter's subnet with a /32 mask (gai-addresses).
                const std::string gw = ascii(e + L.gw_list + L.addr_string, 16);
                if (gw != "0.0.0.0") {
                    in_addr g{}, a{}, m{};
                    ASSERT_EQ(::inet_pton(AF_INET, gw.c_str(), &g), 1);
                    ::inet_pton(AF_INET, ip.c_str(), &a);
                    ::inet_pton(AF_INET, mask.c_str(), &m);
                    EXPECT_EQ(g.s_addr & m.s_addr, a.s_addr & m.s_addr);
                    EXPECT_EQ(ascii(e + L.gw_list + L.mask_string, 16), "255.255.255.255");
                    EXPECT_EQ(u32at(e + L.gw_list + L.context), g.s_addr);
                }
            }
            if (u32at(e + L.dhcp) && !ascii(e + L.dhcp_server + L.addr_string, 16).empty()) {
                EXPECT_EQ(ascii(e + L.dhcp_server + L.mask_string, 16), "255.255.255.255");
                const u64 obtained = w ? u64at(e + L.lease_obtained) : u32at(e + L.lease_obtained);
                const u64 expires = w ? u64at(e + L.lease_expires) : u32at(e + L.lease_expires);
                EXPECT_GT(obtained, 0u);
                EXPECT_GT(expires, obtained);
            }
        }
        usize host_v4 = 0;
        for (const auto& [name, link] : links) if (!link.loopback && !link.v4.empty()) ++host_v4;
        EXPECT_EQ(with_address, host_v4);
        EXPECT_EQ(call("GetAdaptersInfo", {buffer, 0}), 87u);
    }
}

// Windows' software loopback adapter, every field (iphlp_adapters_probe gaa-loopback*).
TEST_F(IphlpapiAdaptersTest, GetAdaptersAddressesLoopbackMatchesWindowsBothBitnesses) {
    const u32 lo = ::if_nametoindex("lo0");
    ASSERT_NE(lo, 0u);
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        const auto list = gaa(0, 0x10 | 0x80);  // INCLUDE_PREFIX | INCLUDE_GATEWAYS
        ASSERT_FALSE(list.empty());
        const GuestAddr a = find_loopback(list);
        ASSERT_NE(a, 0u);
        EXPECT_EQ(a, list.back()) << "loopback is listed last (gaa-order)";
        EXPECT_EQ(u32at(a), L.size);  // Length: 376 on x86, 448 on x64
        EXPECT_EQ(u32at(a + 4), lo);
        EXPECT_EQ(u32at(a + L.v6index), lo);
        EXPECT_TRUE(is_braced_guid(ascii(ptrat(a + L.name))));
        EXPECT_EQ(wide(ptrat(a + L.friendly)), "Loopback Pseudo-Interface 1");
        EXPECT_EQ(wide(ptrat(a + L.desc)), "Software Loopback Interface 1");
        EXPECT_NE(ptrat(a + L.suffix), 0u);
        EXPECT_EQ(wide(ptrat(a + L.suffix)), "");
        EXPECT_EQ(u32at(a + L.phys_len), 0u);
        EXPECT_EQ(u32at(a + L.flags), 0x181u);
        EXPECT_EQ(u32at(a + L.mtu), 0xFFFFFFFFu);
        EXPECT_EQ(u32at(a + L.oper), 1u);
        EXPECT_EQ(u64at(a + L.tx), 1073741824u);
        EXPECT_EQ(u64at(a + L.rx), 1073741824u);
        EXPECT_EQ(u32at(a + L.metric4), 75u);
        EXPECT_EQ(u32at(a + L.metric6), 75u);
        EXPECT_EQ(u64at(a + L.luid), 0x0018000000000000ull);
        EXPECT_EQ(u32at(a + L.compartment), 1u);
        EXPECT_EQ(u32at(a + L.conn_type), 1u);
        EXPECT_EQ(u32at(a + L.tunnel), 0u);
        EXPECT_EQ(ptrat(a + L.dhcp4), 0u);
        EXPECT_EQ(ptrat(a + L.gateway), 0u);
        EXPECT_EQ(ptrat(a + L.wins), 0u);
        for (u32 z = 0; z < 16; ++z) EXPECT_EQ(u32at(a + L.zones + z * 4), z == 14 ? 0u : 1u) << z;

        // Unicast: ::1 then 127.0.0.1, well-known origins, preferred, infinite.
        const auto uni = chain(ptrat(a + L.unicast));
        ASSERT_EQ(uni.size(), 2u);
        for (usize i = 0; i < 2; ++i) {
            const GuestAddr u = uni[i];
            EXPECT_EQ(u32at(u), L.uni_size);  // 48 / 64
            EXPECT_EQ(u32at(u + 4), 0u);      // Flags
            EXPECT_EQ(u32at(u + L.uni_prefix_origin), 2u);
            EXPECT_EQ(u32at(u + L.uni_suffix_origin), 2u);
            EXPECT_EQ(u32at(u + L.uni_dad), 4u);
            EXPECT_EQ(u32at(u + L.uni_valid), 0xFFFFFFFFu);
            EXPECT_EQ(u32at(u + L.uni_preferred), 0xFFFFFFFFu);
            EXPECT_NE(u32at(u + L.uni_lease), 0u);
        }
        const GuestAddr v6 = ptrat(uni[0] + L.uni_addr);
        EXPECT_EQ(memory.read_u16(v6).value_or(0), 23u);
        EXPECT_EQ(u32at(uni[0] + L.uni_addr + L.sockaddr_len_off), 28u);
        EXPECT_EQ(memory.read_u8(v6 + 8 + 15).value_or(0), 1u);
        EXPECT_EQ(u32at(v6 + 24), 0u);  // ::1 has no zone
        EXPECT_EQ(memory.read_u8(uni[0] + L.uni_onlink).value_or(0), 128u);
        const GuestAddr v4 = ptrat(uni[1] + L.uni_addr);
        EXPECT_EQ(memory.read_u16(v4).value_or(0), 2u);
        EXPECT_EQ(u32at(uni[1] + L.uni_addr + L.sockaddr_len_off), 16u);
        EXPECT_EQ(u32at(v4 + 4), 0x0100007Fu);
        EXPECT_EQ(memory.read_u8(uni[1] + L.uni_onlink).value_or(0), 8u);

        // DNS: the IPv6 stack's site-local defaults, zone 1.
        const auto dns = chain(ptrat(a + L.dns));
        ASSERT_EQ(dns.size(), 3u);
        for (usize i = 0; i < 3; ++i) {
            EXPECT_EQ(u32at(dns[i]), L.list_size);
            const GuestAddr sa = ptrat(dns[i] + L.list_addr);
            const u8 expect[16] = {0xFE, 0xC0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, static_cast<u8>(i + 1)};
            for (usize b = 0; b < 16; ++b) EXPECT_EQ(memory.read_u8(sa + 8 + b).value_or(0), expect[b]);
            EXPECT_EQ(u32at(sa + 24), 1u);
        }
        // INCLUDE_PREFIX: on-link prefixes in the measured order.
        const auto prefixes = chain(ptrat(a + L.prefix));
        ASSERT_EQ(prefixes.size(), 7u);
        const std::vector<std::pair<std::string, u32>> expected = {
            {"::1", 128}, {"ff00::", 8}, {"127.0.0.0", 8}, {"127.0.0.1", 32},
            {"127.255.255.255", 32}, {"224.0.0.0", 4}, {"255.255.255.255", 32}};
        for (usize i = 0; i < 7; ++i) {
            EXPECT_EQ(u32at(prefixes[i]), L.prefix_size);
            EXPECT_EQ(u32at(prefixes[i] + L.prefix_len), expected[i].second);
            const GuestAddr sa = ptrat(prefixes[i] + L.list_addr);
            char text[64] = {};
            u8 raw[16] = {};
            for (usize b = 0; b < 16; ++b) raw[b] = memory.read_u8(sa + (memory.read_u16(sa).value_or(0) == 2 ? 4 : 8) + b).value_or(0);
            ::inet_ntop(memory.read_u16(sa).value_or(0) == 2 ? AF_INET : AF_INET6, raw, text, sizeof(text));
            EXPECT_EQ(std::string(text), expected[i].first);
        }
        EXPECT_EQ(u32at(ptrat(prefixes[1] + L.list_addr) + 24), lo);  // ff00::/8 zone = interface

        // AF_INET: IPv4 unicast only, no fec0 resolvers, IPv4 prefixes only.
        const auto v4list = gaa(2, 0x10);
        const GuestAddr a4 = find_loopback(v4list);
        ASSERT_NE(a4, 0u);
        EXPECT_EQ(u32at(a4 + L.flags), 0x181u);
        EXPECT_EQ(chain(ptrat(a4 + L.unicast)).size(), 1u);
        EXPECT_EQ(chain(ptrat(a4 + L.dns)).size(), 0u);
        EXPECT_EQ(chain(ptrat(a4 + L.prefix)).size(), 5u);
        // AF_INET6: ::1 only, 3 resolvers, 2 prefixes (gaa-family family=23).
        const auto v6list = gaa(23, 0x10);
        const GuestAddr a6 = find_loopback(v6list);
        ASSERT_NE(a6, 0u);
        EXPECT_EQ(chain(ptrat(a6 + L.unicast)).size(), 1u);
        EXPECT_EQ(chain(ptrat(a6 + L.dns)).size(), 3u);
        EXPECT_EQ(chain(ptrat(a6 + L.prefix)).size(), 2u);
        // Flag effects (gaa-flag): SKIP_UNICAST/SKIP_DNS_SERVER/SKIP_MULTICAST
        // empty their lists; SKIP_FRIENDLY_NAME is ignored.
        EXPECT_EQ(ptrat(find_loopback(gaa(0, 0x1)) + L.unicast), 0u);
        EXPECT_EQ(ptrat(find_loopback(gaa(0, 0x8)) + L.dns), 0u);
        EXPECT_EQ(ptrat(find_loopback(gaa(0, 0x800)) + L.dns), 0u);
        EXPECT_EQ(ptrat(find_loopback(gaa(0, 0x4)) + L.multicast), 0u);
        EXPECT_EQ(wide(ptrat(find_loopback(gaa(0, 0x20)) + L.friendly)), "Loopback Pseudo-Interface 1");
    }
}

// Errors and last-error behaviour (gaa-null-size, gaa-bad-family, gaa-size-query).
TEST_F(IphlpapiAdaptersTest, GetAdaptersAddressesValidatesFamilyAndSize) {
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GuestAddr size = DATA + 0x10;
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetAdaptersAddresses", {0, 0, 0, 0, 0}), 87u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        for (u32 family : {1u, 3u, 17u, 24u, 0xFFFFu}) {
            ASSERT_TRUE(memory.write_u32(size, 0x1234).is_ok());
            set_last_error(0xDEAD);
            EXPECT_EQ(call("GetAdaptersAddresses", {family, 0, 0, 0, size}), 87u) << family;
            EXPECT_EQ(u32at(size), 0x1234u);
            EXPECT_EQ(get_last_error(), 0xDEADu);
        }
        ASSERT_TRUE(memory.write_u32(size, 16).is_ok());
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetAdaptersAddresses", {0, 0, 0, DATA + 0x100, size}), 111u);
        EXPECT_EQ(get_last_error(), 0u);
        const u32 need = u32at(size);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetAdaptersAddresses", {0, 0, DATA + 0x100, DATA + 0x100, size}), 0u);  // Reserved ignored
        EXPECT_EQ(u32at(size), need);
        EXPECT_EQ(get_last_error(), 0u);
        // GAA_FLAG_INCLUDE_ALL_INTERFACES lists every interface, IP-bound or not.
        usize links = 0;
        for (const auto& [name, link] : host_links()) if (link.index) ++links;
        EXPECT_EQ(gaa(2, 0x100).size(), links);
        EXPECT_LE(gaa(2, 0).size(), links);
    }
}

// The unicast node is written inside the caller's buffer (the fields used to
// go to absolute low addresses, leaving DadState 0) and agrees with the host
// and with GetAdaptersInfo for the same adapter.
TEST_F(IphlpapiAdaptersTest, GetAdaptersAddressesUnicastFieldsAgreeWithHostAndAdaptersInfo) {
    const auto links = host_links();
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        const InfoLayout& I = w ? kInfo64 : kInfo32;
        const auto list = gaa(0, 0x80);
        std::map<u32, std::string> names;
        usize checked = 0;
        for (GuestAddr a : list) {
            ASSERT_GE(a, DATA);
            ASSERT_LT(a, DATA + DATA_SIZE);
            const u32 index = u32at(a + 4) ? u32at(a + 4) : u32at(a + L.v6index);
            names[index] = ascii(ptrat(a + L.name));
            const u64 luid = u64at(a + L.luid);
            EXPECT_EQ(luid >> 48, u32at(a + L.if_type)) << "gaa-invariants luid_iftype_matches";
            const HostLink* link = link_by_index(links, index);
            ASSERT_NE(link, nullptr);
            if (u32at(a + L.if_type) == 24 || link->v4.empty()) continue;
            EXPECT_EQ(u32at(a + L.phys_len), link->mac.size());
            for (GuestAddr u : chain(ptrat(a + L.unicast))) {
                ASSERT_GE(u, DATA);
                const GuestAddr sa = ptrat(u + L.uni_addr);
                if (memory.read_u16(sa).value_or(0) != 2) continue;
                in_addr value{};
                value.s_addr = u32at(sa + 4);
                char text[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &value, text, sizeof(text));
                const auto host = std::find_if(link->v4.begin(), link->v4.end(),
                                               [&](const HostIpv4& h) { return h.address == text; });
                ASSERT_NE(host, link->v4.end()) << text;
                EXPECT_EQ(u32at(u), L.uni_size);
                EXPECT_EQ(u32at(u + L.uni_dad), 4u);  // IpDadStatePreferred
                EXPECT_EQ(memory.read_u8(u + L.uni_onlink).value_or(0), host->prefix);
                if ((value.s_addr & 0xFFFFu) != 0xFEA9u) EXPECT_EQ(u32at(u + 4), 1u);  // DNS_ELIGIBLE (not APIPA)
                const u32 origin = u32at(u + L.uni_prefix_origin);
                EXPECT_TRUE(origin == 1 || origin == 3) << origin;  // Manual or Dhcp
                EXPECT_EQ(origin, u32at(u + L.uni_suffix_origin));
                ++checked;
            }
        }
        EXPECT_GT(checked, 0u);
        // Same AdapterName in GetAdaptersInfo for every IPv4 adapter.
        const GuestAddr size = DATA + 0x10, buffer = DATA + 0x80000;
        ASSERT_TRUE(memory.write_u32(size, 0x10000).is_ok());
        ASSERT_EQ(call("GetAdaptersInfo", {buffer, size}), 0u);
        for (GuestAddr e = buffer; e; e = ptrat(e))
            EXPECT_EQ(ascii(e + I.name), names[u32at(e + I.index)]);
    }
}

TEST_F(IphlpapiAdaptersTest, GetInterfaceInfoListsIpv4AdaptersByTcpipDeviceName) {
    const u32 lo = ::if_nametoindex("lo0");
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        std::map<u32, std::string> names;
        for (GuestAddr a : gaa(2, 0)) names[u32at(a + 4)] = ascii(ptrat(a + L.name));
        const GuestAddr size = DATA + 0x10, table = DATA + 0x80000;
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetInterfaceInfo", {0, 0}), 87u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        ASSERT_TRUE(memory.write_u32(size, 0).is_ok());
        set_last_error(0xDEAD);
        ASSERT_EQ(call("GetInterfaceInfo", {0, size}), 122u);
        EXPECT_EQ(get_last_error(), 0u);  // gii-size-query le=0x0
        const u32 need = u32at(size);
        EXPECT_GE(need, 264u);            // IP_INTERFACE_INFO holds at least one map
        ASSERT_TRUE(memory.write_u32(size, 8).is_ok());
        EXPECT_EQ(call("GetInterfaceInfo", {table, size}), 122u);
        EXPECT_EQ(u32at(size), need);
        ASSERT_EQ(call("GetInterfaceInfo", {table, size}), 0u);
        const u32 count = u32at(table);
        ASSERT_GT(count, 0u);
        EXPECT_EQ(need, 4 + count * 260);
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr map = table + 4 + i * 260;
            const u32 index = u32at(map);
            EXPECT_NE(index, lo);
            ASSERT_TRUE(names.count(index)) << index;
            EXPECT_EQ(wide(map + 4, 128), "\\DEVICE\\TCPIP_" + names[index]);
        }
    }
}

// GetIfTable2 writes a whole pointer (a u32 used to land in the 8-byte slot on
// x64) to a process-heap table FreeMibTable releases; rows agree with GAA.
TEST_F(IphlpapiAdaptersTest, GetIfTable2AllocatesRowsMatchingAdaptersAndFreeMibTableReleases) {
    usize host_links_count = 0;
    for (const auto& [name, link] : host_links()) if (link.index) ++host_links_count;
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        std::map<u32, std::pair<std::string, u64>> adapters;
        for (GuestAddr a : gaa(0, 0)) {
            const u32 index = u32at(a + 4) ? u32at(a + 4) : u32at(a + L.v6index);
            adapters[index] = {ascii(ptrat(a + L.name)), u64at(a + L.luid)};
        }
        const GuestAddr out = DATA + 0x20;
        ASSERT_TRUE(memory.write_u64(out, 0xAAAAAAAAAAAAAAAAull).is_ok());
        set_last_error(0xDEAD);
        ASSERT_EQ(call("GetIfTable2", {out}), 0u);
        EXPECT_EQ(get_last_error(), 0u);
        const u64 table = w ? u64at(out) : u32at(out);
        if (!w) EXPECT_EQ(u32at(out + 4), 0xAAAAAAAAu) << "x86 writes a 4-byte pointer only";
        ASSERT_NE(table, 0u);
        ASSERT_NE(table, 0xAAAAAAAAAAAAAAAAull);
        const u32 count = u32at(table);
        EXPECT_EQ(count, host_links_count);
        const GuestAddr rows = table + 8;  // offset.MIB_IF_TABLE2.Table
        usize loopbacks = 0;
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr row = rows + i * 1352;
            const u64 luid = u64at(row);
            const u32 index = u32at(row + 8);
            u8 guid[16];
            for (int b = 0; b < 16; ++b) guid[b] = memory.read_u8(row + 12 + b).value_or(0);
            if (adapters.count(index)) {
                EXPECT_EQ(braced_guid(guid), adapters[index].first);
                EXPECT_EQ(luid, adapters[index].second);
            }
            if (u32at(row + 1128) == 24) {  // Type: software loopback
                ++loopbacks;
                EXPECT_EQ(wide(row + 28, 257), "Loopback Pseudo-Interface 1");
                EXPECT_EQ(u32at(row + 1124), 1500u);  // ift2 loopback_row_mtu=1500
                EXPECT_EQ(luid, 0x0018000000000000ull);
            }
        }
        EXPECT_EQ(loopbacks, 1u);
        EXPECT_EQ(heap.heap_size(heap.get_process_heap(), 0, table), 8u + count * 1352u);
        call("FreeMibTable", {table});
        EXPECT_EQ(heap.heap_size(heap.get_process_heap(), 0, table), ~0ULL) << "FreeMibTable released the table";
        EXPECT_EQ(call("GetIfTable2", {0}), 87u);
    }
}

// NDIS names and identity round trips (iphlp_adapters_probe name-*, gaa-identity).
TEST_F(IphlpapiAdaptersTest, InterfaceNamesLuidsAndGuidsRoundTrip) {
    const u32 lo = ::if_nametoindex("lo0");
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        const GuestAddr luid = DATA + 0x20, name = DATA + 0x200, guid = DATA + 0x40, index = DATA + 0x60;
        ASSERT_TRUE(memory.write_u64(luid, 0x0018000000000000ull).is_ok());
        set_last_error(0xDEAD);
        ASSERT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 257}), 0u);
        EXPECT_EQ(wide(name), "loopback_0");
        EXPECT_EQ(get_last_error(), 0xDEADu);
        EXPECT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 3}), 8u);
        EXPECT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 0}), 8u);
        ASSERT_TRUE(memory.write_u64(luid, 0x0018000001000000ull).is_ok());  // no such interface
        EXPECT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 257}), 0u);
        EXPECT_EQ(wide(name), "loopback_1");
        ASSERT_TRUE(memory.write_u64(luid, (71ull << 48) | (5ull << 24)).is_ok());
        ASSERT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 257}), 0u);
        EXPECT_EQ(wide(name), "wireless_5");
        ASSERT_TRUE(memory.write_u64(luid, (209ull << 48) | (5ull << 24)).is_ok());
        ASSERT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 257}), 0u);
        EXPECT_EQ(wide(name), "iftype209_5");
        auto put_wide = [&](const char* text) {
            for (usize i = 0;; ++i) { ASSERT_TRUE(memory.write_u16(name + i * 2, static_cast<u8>(text[i])).is_ok()); if (!text[i]) break; }
        };
        put_wide("ethernet_99999");
        ASSERT_EQ(call("ConvertInterfaceNameToLuidW", {name, luid}), 0u);
        EXPECT_EQ(u64at(luid), 0x000601869F000000ull);
        put_wide("no_such_interface_7");
        ASSERT_TRUE(memory.write_u64(luid, 0x1234).is_ok());
        set_last_error(0xDEAD);
        EXPECT_EQ(call("ConvertInterfaceNameToLuidW", {name, luid}), 123u);
        EXPECT_EQ(u64at(luid), 0u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        ASSERT_TRUE(memory.write_bytes(DATA + 0x300, reinterpret_cast<const u8*>("loopback_0"), 11).is_ok());
        EXPECT_EQ(call("if_nametoindex", {DATA + 0x300}), lo);
        ASSERT_TRUE(memory.write_bytes(DATA + 0x300, reinterpret_cast<const u8*>("lo0"), 4).is_ok());
        EXPECT_EQ(call("if_nametoindex", {DATA + 0x300}), 0u) << "host names are not NDIS names";
        EXPECT_EQ(call("if_indextoname", {lo, DATA + 0x400}), DATA + 0x400);
        EXPECT_EQ(ascii(DATA + 0x400), "loopback_0");
        EXPECT_EQ(call("if_indextoname", {0x7ffffff0u, DATA + 0x400}), 0u);

        for (GuestAddr a : gaa(0, 0)) {
            const std::string adapter_name = ascii(ptrat(a + L.name));
            const u64 expect_luid = u64at(a + L.luid);
            const u32 expect_index = u32at(a + 4) ? u32at(a + 4) : u32at(a + L.v6index);
            ASSERT_TRUE(memory.write_u64(luid, expect_luid).is_ok());
            ASSERT_EQ(call("ConvertInterfaceLuidToGuid", {luid, guid}), 0u);
            u8 bytes[16];
            for (int b = 0; b < 16; ++b) bytes[b] = memory.read_u8(guid + b).value_or(0);
            EXPECT_EQ(braced_guid(bytes), adapter_name);
            ASSERT_TRUE(memory.write_u64(luid, 0).is_ok());
            ASSERT_EQ(call("ConvertInterfaceGuidToLuid", {guid, luid}), 0u);
            EXPECT_EQ(u64at(luid), expect_luid);
            ASSERT_EQ(call("ConvertInterfaceLuidToIndex", {luid, index}), 0u);
            EXPECT_EQ(u32at(index), expect_index);
            ASSERT_EQ(call("ConvertInterfaceIndexToLuid", {expect_index, luid}), 0u);
            EXPECT_EQ(u64at(luid), expect_luid);
            ASSERT_EQ(call("ConvertInterfaceLuidToNameW", {luid, name, 257}), 0u);
            std::string text = wide(name);
            ASSERT_TRUE(memory.write_bytes(DATA + 0x300, reinterpret_cast<const u8*>(text.c_str()), text.size() + 1).is_ok());
            EXPECT_EQ(call("if_nametoindex", {DATA + 0x300}), expect_index) << text;
        }
    }
}

// Argument validation measured by iphlp_misc_probe release-* / renew-*; a
// real adapter's lease is the host's and fails loudly.
TEST_F(IphlpapiAdaptersTest, IpReleaseAndRenewValidateBeforeFailingLoudly) {
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        std::string real;
        for (GuestAddr a : gaa(2, 0)) if (u32at(a + L.if_type) != 24) { real = ascii(ptrat(a + L.name)); break; }
        const GuestAddr map = DATA + 0x800;
        auto set_name = [&](const std::string& text) {
            ASSERT_TRUE(memory.write_u32(map, 0x7fffffff).is_ok());
            for (usize i = 0; i <= text.size(); ++i)
                ASSERT_TRUE(memory.write_u16(map + 4 + i * 2, i < text.size() ? static_cast<u8>(text[i]) : 0).is_ok());
        };
        for (const char* api : {"IpReleaseAddress", "IpRenewAddress"}) {
            set_last_error(0xDEAD);
            EXPECT_EQ(call(api, {0}), 87u);
            EXPECT_EQ(get_last_error(), 0xDEADu);
            set_name("");
            EXPECT_EQ(call(api, {map}), 87u);
            set_name("not-a-device-name");
            set_last_error(0xDEAD);
            EXPECT_EQ(call(api, {map}), 123u);
            EXPECT_EQ(get_last_error(), 0xDEADu);
            set_name("\\DEVICE\\TCPIP_{00000000-0000-0000-0000-000000000001}");
            set_last_error(0xDEAD);
            EXPECT_EQ(call(api, {map}), 87u);
            EXPECT_EQ(get_last_error(), 0u);
            if (!real.empty()) {
                set_name("\\DEVICE\\TCPIP_" + real);
                EXPECT_EQ(call(api, {map}), 50u) << "host DHCP lease is never released";
            }
        }
    }
}

// iphlp_misc_probe tcp-*: class/family validation, IPv6 OWNER_PID rows,
// bOrder sorting, OWNER_MODULE failing loudly.
TEST_F(IphlpapiAdaptersTest, GetExtendedTcpTableValidatesAndListsOwnSocketsBothFamilies) {
    int listeners[3];
    u16 ports[3];
    for (int i = 0; i < 3; ++i) {
        listeners[i] = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_len = sizeof(a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(listeners[i], reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        ASSERT_EQ(::listen(listeners[i], 1), 0);
        socklen_t l = sizeof(a);
        ::getsockname(listeners[i], reinterpret_cast<sockaddr*>(&a), &l);
        ports[i] = a.sin_port;
    }
    const int v6 = ::socket(AF_INET6, SOCK_STREAM, 0);
    sockaddr_in6 a6{};
    a6.sin6_len = sizeof(a6);
    a6.sin6_family = AF_INET6;
    a6.sin6_addr = in6addr_loopback;
    ASSERT_EQ(::bind(v6, reinterpret_cast<sockaddr*>(&a6), sizeof(a6)), 0);
    ASSERT_EQ(::listen(v6, 1), 0);
    socklen_t l6 = sizeof(a6);
    ::getsockname(v6, reinterpret_cast<sockaddr*>(&a6), &l6);
    const u32 pid = static_cast<u32>(::getpid());

    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GuestAddr size = DATA + 0x10, table = DATA + 0x1000;
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetExtendedTcpTable", {0, 0, 0, 2, 5, 0}), 87u);
        for (u64 cls : {9ull, 10ull, 0xFFFFull}) {
            ASSERT_TRUE(memory.write_u32(size, 0x1234).is_ok());
            EXPECT_EQ(call("GetExtendedTcpTable", {0, size, 0, 2, cls, 0}), 87u);
            EXPECT_EQ(u32at(size), 0x1234u);
        }
        for (u64 af : {0ull, 1ull, 3ull, 17ull, 24ull}) {
            EXPECT_EQ(call("GetExtendedTcpTable", {0, size, 0, af, 5, 0}), 87u) << af;
        }
        for (u64 cls : {0ull, 1ull, 2ull}) EXPECT_EQ(call("GetExtendedTcpTable", {0, size, 0, 23, cls, 0}), 87u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        EXPECT_EQ(call("GetExtendedTcpTable", {0, size, 0, 2, 8, 0}), 50u);  // OWNER_MODULE fails loudly

        // IPv4 OWNER_PID_LISTENER, ordered.
        ASSERT_TRUE(memory.write_u32(size, 0).is_ok());
        ASSERT_EQ(call("GetExtendedTcpTable", {0, size, 1, 2, 3, 0}), 122u);
        EXPECT_EQ(get_last_error(), 0u);
        ASSERT_TRUE(memory.write_u32(size, 0x40000).is_ok());
        ASSERT_EQ(call("GetExtendedTcpTable", {table, size, 1, 2, 3, 0}), 0u);
        const u32 n = u32at(table);
        EXPECT_EQ(u32at(size), 4 + n * 24);
        int mine = 0;
        for (u32 i = 0; i < n; ++i) {
            const GuestAddr row = table + 4 + i * 24;
            EXPECT_EQ(u32at(row), 2u);  // listeners only
            for (u16 p : ports)
                if (u32at(row + 8) == p && u32at(row + 4) == htonl(INADDR_LOOPBACK)) { ++mine; EXPECT_EQ(u32at(row + 20), pid); }
            if (i) {
                const u32 prev_a = ntohl(u32at(row - 24 + 4)), cur_a = ntohl(u32at(row + 4));
                EXPECT_LE(prev_a, cur_a);
                if (prev_a == cur_a) EXPECT_LE(ntohs(static_cast<u16>(u32at(row - 24 + 8))), ntohs(static_cast<u16>(u32at(row + 8))));
            }
        }
        EXPECT_EQ(mine, 3);
        // IPv6 OWNER_PID_ALL: 56-byte rows with scope ids.
        ASSERT_TRUE(memory.write_u32(size, 0x40000).is_ok());
        ASSERT_EQ(call("GetExtendedTcpTable", {table, size, 1, 23, 5, 0}), 0u);
        const u32 n6 = u32at(table);
        EXPECT_EQ(u32at(size), 4 + n6 * 56);
        int mine6 = 0;
        for (u32 i = 0; i < n6; ++i) {
            const GuestAddr row = table + 4 + i * 56;
            bool loop = memory.read_u8(row + 15).value_or(0) == 1;
            for (int b = 0; b < 15; ++b) loop = loop && memory.read_u8(row + b).value_or(1) == 0;
            if (loop && u32at(row + 20) == a6.sin6_port && u32at(row + 48) == 2) {
                ++mine6;
                EXPECT_EQ(u32at(row + 16), 0u);  // dwLocalScopeId
                EXPECT_EQ(u32at(row + 52), pid);
            }
        }
        EXPECT_EQ(mine6, 1);
    }
    for (int s : listeners) ::close(s);
    ::close(v6);
}

// DHCP client API: x86 frame is 44 bytes (the by-value DHCPCAPI_PARAMS_ARRAY
// pair is four slots), argument order and errors per iphlp_misc_probe.
TEST_F(IphlpapiAdaptersTest, DhcpClientApiFrameArgumentsAndCachedOptions) {
    for (auto& e : dhcp.entries) {
        if (e.name == "DhcpRequestParams") EXPECT_EQ(e.stdcall_bytes, 44u);
        if (e.name == "DhcpCApiInitialize") EXPECT_EQ(e.stdcall_bytes, 4u);
        if (e.name == "DhcpCApiCleanup") EXPECT_EQ(e.stdcall_bytes, 0u);
    }
    const auto links = host_links();
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const GaaLayout& L = w ? kGaa64 : kGaa32;
        const GuestAddr version = DATA + 0x10;
        ASSERT_TRUE(memory.write_u32(version, 0xcccccccc).is_ok());
        EXPECT_EQ(call(dhcp, "DhcpCApiInitialize", {version}), 0u);
        EXPECT_EQ(u32at(version), 2u);  // DHCPCAPI_VERSION_V2
        EXPECT_EQ(call(dhcp, "DhcpCApiInitialize", {0}), 0u);

        // Away from DATA + 0x100, where gaa() lays out the adapter list.
        const GuestAddr name = DATA + 0xF0100, params = DATA + 0xF0300, recd = DATA + 0xF0380,
                        buffer = DATA + 0xF0400, size = DATA + 0xF0020;
        auto request = [&](u64 flags, GuestAddr adapter, GuestAddr size_ptr) {
            if (w) {
                // x64: the two 16-byte arrays go by hidden reference.
                EXPECT_TRUE(memory.write_u32(recd, 1).is_ok());
                EXPECT_TRUE(memory.write_u64(recd + 8, params).is_ok());
                return call(dhcp, "DhcpRequestParams", {flags, 0, adapter, 0, DATA + 0xF03C0, recd, buffer, size_ptr, 0});
            }
            return call(dhcp, "DhcpRequestParams", {flags, 0, adapter, 0, 0, 0, 1, params, buffer, size_ptr, 0});
        };
        auto set_name = [&](const std::string& text) {
            for (usize i = 0; i <= text.size(); ++i)
                ASSERT_TRUE(memory.write_u16(name + i * 2, i < text.size() ? static_cast<u8>(text[i]) : 0).is_ok());
        };
        const u32 param_size = w ? 32 : 20, data_off = w ? 16 : 12, bytes_off = w ? 24 : 16;
        ASSERT_TRUE(memory.write_bytes(params, std::vector<u8>(param_size, 0).data(), param_size).is_ok());
        ASSERT_TRUE(memory.write_u32(params + 4, 1).is_ok());  // OPTION_SUBNET_MASK
        ASSERT_TRUE(memory.write_u32(size, 256).is_ok());
        set_name("{00000000-0000-0000-0000-000000000001}");
        set_last_error(0xDEAD);
        EXPECT_EQ(request(2, 0, size), 87u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        EXPECT_EQ(request(2, name, 0), 87u);
        EXPECT_EQ(request(0, name, size), 87u);
        EXPECT_EQ(request(0x100, name, size), 87u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        EXPECT_EQ(request(2, name, size), 87u);
        EXPECT_EQ(get_last_error(), 0u);  // dhcp-request-missing-adapter le=0x0
        EXPECT_EQ(u32at(size), 256u);

        // A host adapter holding a DHCP lease answers from the lease cache.
        std::string adapter;
        std::string mask;
        for (GuestAddr a : gaa(2, 0)) {
            if (!(u32at(a + L.flags) & 0x4) || !ptrat(a + L.dhcp4)) continue;
            adapter = ascii(ptrat(a + L.name));
            const HostLink* link = link_by_index(links, u32at(a + 4));
            if (link && !link->v4.empty()) mask = link->v4[0].mask;
            break;
        }
        if (adapter.empty() || mask.empty()) continue;
        set_name(adapter);
        ASSERT_TRUE(memory.write_u32(size, 2).is_ok());
        EXPECT_EQ(request(2, name, size), 234u);  // ERROR_MORE_DATA
        EXPECT_EQ(u32at(size), 4u);
        ASSERT_TRUE(memory.write_u32(size, 64).is_ok());
        ASSERT_EQ(request(2, name, size), 0u);
        EXPECT_EQ(u32at(size), 4u);
        EXPECT_EQ(u32at(params + bytes_off), 4u);
        const u64 data = w ? u64at(params + data_off) : u32at(params + data_off);
        EXPECT_EQ(data, buffer);
        in_addr expect{};
        ASSERT_EQ(::inet_pton(AF_INET, mask.c_str(), &expect), 1);
        EXPECT_EQ(u32at(buffer), expect.s_addr);
    }
}

// FIXED_INFO per bitness (iphlp_adapters_probe offset.FIXED_INFO.* and gnp):
// x64 grew by the 8-byte CurrentDnsServer pointer, so every later field moved.
TEST_F(IphlpapiAdaptersTest, GetNetworkParamsLayoutAndValuesBothBitnesses) {
    std::vector<std::string> resolvers;
    {
        std::ifstream in("/etc/resolv.conf");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("nameserver", 0) != 0) continue;
            std::string server = line.substr(line.find_first_not_of(" \t", 10));
            server = server.substr(0, server.find_first_of(" \t#"));
            in_addr probe{};
            if (::inet_pton(AF_INET, server.c_str(), &probe) == 1) resolvers.push_back(server);
        }
    }
    for (bool w : {false, true}) {
        SCOPED_TRACE(w ? "x64" : "x86");
        mode(w);
        const u32 base_size = w ? 600 : 584, dns = w ? 272 : 268, node = w ? 320 : 308, routing = w ? 584 : 572;
        const u32 addr_string = w ? 48 : 40, ip = w ? 8 : 4, mask = w ? 24 : 20, ctx = w ? 40 : 36;
        const GuestAddr size = DATA + 0x10, info = DATA + 0x1000;
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetNetworkParams", {0, 0}), 87u);
        EXPECT_EQ(get_last_error(), 0xDEADu);
        ASSERT_TRUE(memory.write_u32(size, 0).is_ok());
        ASSERT_EQ(call("GetNetworkParams", {0, size}), 111u);
        EXPECT_EQ(get_last_error(), 0u);
        const u32 need = u32at(size);
        EXPECT_EQ(need, base_size + (resolvers.empty() ? 0 : resolvers.size() - 1) * addr_string);
        ASSERT_TRUE(memory.write_u32(size, base_size - 1).is_ok());
        EXPECT_EQ(call("GetNetworkParams", {info, size}), 111u);
        EXPECT_EQ(u32at(size), need);
        ASSERT_EQ(call("GetNetworkParams", {info, size}), 0u);
        EXPECT_EQ(ascii(info), "VITREOHOST");
        EXPECT_EQ(ascii(info + 132), "");
        EXPECT_EQ(ptrat(info + 264), 0u);  // CurrentDnsServer
        EXPECT_EQ(u32at(info + node), 8u);
        EXPECT_EQ(u32at(info + routing), 0u);
        EXPECT_EQ(u32at(info + routing + 4), 0u);
        EXPECT_EQ(u32at(info + routing + 8), 0u);  // EnableDns (gnp EnableDns=0)
        GuestAddr entry = info + dns;
        for (usize i = 0; i < resolvers.size(); ++i) {
            ASSERT_NE(entry, 0u);
            EXPECT_EQ(ascii(entry + ip, 16), resolvers[i]);
            EXPECT_EQ(ascii(entry + mask, 16), "255.255.255.255");
            in_addr value{};
            ::inet_pton(AF_INET, resolvers[i].c_str(), &value);
            EXPECT_EQ(u32at(entry + ctx), value.s_addr);
            entry = ptrat(entry);
        }
        EXPECT_EQ(entry, 0u);
    }
}

// INetworkConnection::GetAdapterId names the adapter carrying the default
// route with the GUID IP Helper reports as its AdapterName (Windows exposes
// one GUID per adapter across both APIs).
TEST_F(IphlpapiAdaptersTest, NetworkListAdapterIdIsTheDefaultRouteAdapterName) {
    mode(true);
    const GaaLayout& L = kGaa64;
    std::string expected;
    for (GuestAddr a : gaa(2, 0x80)) {
        for (GuestAddr g : chain(ptrat(a + L.gateway))) {
            if (memory.read_u16(ptrat(g + L.list_addr)).value_or(0) == 2) { expected = ascii(ptrat(a + L.name)); break; }
        }
        if (!expected.empty()) break;
    }
    if (expected.empty()) GTEST_SKIP() << "host has no IPv4 default route";

    constexpr GuestAddr kCom = ComCoreManager::COM_SCRATCH_BASE;
    constexpr GuestAddr kThunkBase = 0x7FFB00000000ull;
    ASSERT_TRUE(memory.map_region(kCom, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ComTaskMemoryManager task_memory{heap};
    ComCoreManager com_mgr{task_memory};
    ThunkRegistry registry{kThunkBase};
    ASSERT_TRUE(registry.register_dll("ole32.dll", create_com_core_thunks(com_mgr)).is_ok());
    com_mgr.set_thunk_registry(&registry);
    const u8 clsid[16] = {0x01, 0x0C, 0xB0, 0xDC, 0x0F, 0x57, 0x9B, 0x4A, 0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B};
    const u8 iid[16] = {0x00, 0x00, 0xB0, 0xDC, 0x0F, 0x57, 0x9B, 0x4A, 0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B};
    const GuestAddr clsid_at = DATA + 0xF8000, iid_at = DATA + 0xF8020, out = DATA + 0xF8040;
    ASSERT_TRUE(memory.write_bytes(clsid_at, clsid, 16).is_ok());
    ASSERT_TRUE(memory.write_bytes(iid_at, iid, 16).is_ok());
    // CoCreateInstance on a thread that never called CoInitialize(Ex) is
    // CO_E_NOTINITIALIZED (Learn; enforced since the S7 COM pass).
    ASSERT_EQ(com_mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_OK);
    args({clsid_at, 0, 1, iid_at, out});
    ASSERT_EQ(com_mgr.co_create_instance(marshaler), com::S_OK);
    const GuestAddr manager = u64at(out);
    args({manager, out + 8});
    ASSERT_EQ(com_mgr.network_list_manager_get_network_connections(marshaler), com::S_OK);
    const GuestAddr enumerator = u64at(out + 8);
    args({enumerator, 1, out + 16, out + 24});
    ASSERT_EQ(com_mgr.network_list_manager_enum_next(marshaler), com::S_OK);
    const GuestAddr connection = u64at(out + 16);
    args({connection, out + 32});
    ASSERT_EQ(com_mgr.network_connection_get_adapter_id(marshaler), com::S_OK);
    u8 guid[16];
    for (int b = 0; b < 16; ++b) guid[b] = memory.read_u8(out + 32 + b).value_or(0);
    EXPECT_EQ(braced_guid(guid), expected);
    com_mgr.co_uninitialize();
}

// NotifyIpInterfaceChange / CancelMibChangeNotify2, from
// tools/oracle_probe/netio_notify_route_probe{32,64}.expected (identical):
// the initial notification runs synchronously on the registering thread,
// once per family (twice for AF_UNSPEC), with Row NULL and
// MibInitialNotification (3); a bad family is 87 with the handle NULLed; a
// NULL callback is accepted; CancelMibChangeNotify2(NULL) is 6.
TEST_F(IphlpapiAdaptersTest, NotifyIpInterfaceChangeMatchesWindowsBothBitnesses) {
    SyncManager sync{handles};
    ThreadManager threads{handles};
    mgr.set_notification_managers(handles, sync);
    struct GuestCallRecord { GuestAddr function; std::vector<u64> args; };
    std::vector<GuestCallRecord> calls;
    mgr.set_guest_callbacks(
        [&](GuestAddr function, const u64* a, usize n, bool) -> u64 {
            calls.push_back({function, std::vector<u64>(a, a + n)});
            return 0;
        },
        &threads);
    constexpr GuestAddr kCallback = 0x500000;
    constexpr u64 kContext = 0x1234;
    const GuestAddr out = DATA + 0x10;
    for (bool is_wide : {true, false}) {
        SCOPED_TRACE(is_wide ? "x64" : "x86");
        mode(is_wide);
        const struct { const char* name; u64 family; bool initial; usize expected_calls; } kCases[] = {
            {"unspec initial", 0, true, 2},
            {"inet initial", 2, true, 1},
            {"inet6 initial", 23, true, 1},
            {"unspec no-initial", 0, false, 0},
        };
        for (const auto& c : kCases) {
            SCOPED_TRACE(c.name);
            calls.clear();
            ASSERT_TRUE(memory.write_u64(out, 0xBADBADBADull).is_ok());
            EXPECT_EQ(call("NotifyIpInterfaceChange",
                           {c.family, kCallback, kContext, c.initial ? 1u : 0u, out}), 0u);
            const u64 handle = ptrat(out);
            EXPECT_NE(handle, 0u);
            EXPECT_NE(handle, is_wide ? 0xBADBADBADull : 0xADBADBADull);
            ASSERT_EQ(calls.size(), c.expected_calls);
            for (const auto& record : calls) {
                EXPECT_EQ(record.function, kCallback);
                EXPECT_EQ(record.args, (std::vector<u64>{kContext, 0, 3}));
            }
            EXPECT_EQ(call("CancelMibChangeNotify2", {handle}), 0u);
        }
        for (u64 family : {u64{99}, u64{1}}) {
            ASSERT_TRUE(memory.write_u64(out, ~u64{0}).is_ok());
            EXPECT_EQ(call("NotifyIpInterfaceChange", {family, kCallback, kContext, 1, out}), 87u);
            EXPECT_EQ(ptrat(out), 0u);
        }
        calls.clear();
        EXPECT_EQ(call("NotifyIpInterfaceChange", {0, 0, kContext, 1, out}), 0u);
        EXPECT_TRUE(calls.empty());
        EXPECT_EQ(call("CancelMibChangeNotify2", {ptrat(out)}), 0u);
        EXPECT_EQ(call("CancelMibChangeNotify2", {0}), 6u);
    }
}

// GetBestRoute2 (netio_notify_route_probe): parameter errors are 87 with the
// outputs untouched; an unknown interface is 2 with the outputs written; the
// loopback host route is MIB_IPPROTO_LOCAL, Loopback 1, Immortal 0, a /32
// (/128) prefix and an on-link next hop. MIB_IPFORWARD_ROW2 offsets are the
// SDK's: index 8, prefix 12 (length at +28), next hop 44, protocol 88,
// loopback 92, autoconf 93, immortal 95.
TEST_F(IphlpapiAdaptersTest, GetBestRoute2MatchesWindowsBothBitnesses) {
    const u32 lo0 = ::if_nametoindex("lo0");
    ASSERT_NE(lo0, 0u);
    const GuestAddr dest = DATA + 0x100, route = DATA + 0x200, src = DATA + 0x300, luid = DATA + 0x400;
    const auto put_dest = [&](u16 family, const char* text) {
        std::array<u8, 28> image{};
        image[0] = static_cast<u8>(family);
        image[1] = static_cast<u8>(family >> 8);
        if (family == 2) ::inet_pton(AF_INET, text, image.data() + 4);
        if (family == 23) ::inet_pton(AF_INET6, text, image.data() + 8);
        ASSERT_TRUE(memory.write_bytes(dest, image.data(), image.size()).is_ok());
    };
    const auto poison = [&] {
        std::vector<u8> fill(0x100, 0xCC);
        ASSERT_TRUE(memory.write_bytes(route, fill.data(), 104).is_ok());
        ASSERT_TRUE(memory.write_bytes(src, fill.data(), 28).is_ok());
    };
    for (bool is_wide : {true, false}) {
        SCOPED_TRACE(is_wide ? "x64" : "x86");
        mode(is_wide);
        put_dest(2, "127.0.0.1");
        poison();
        EXPECT_EQ(call("GetBestRoute2", {0, 0, 0, 0, 0, route, src}), 87u);
        EXPECT_EQ(call("GetBestRoute2", {0, 0, 0, dest, 0, 0, src}), 87u);
        EXPECT_EQ(call("GetBestRoute2", {0, 0, 0, dest, 0, route, 0}), 87u);
        put_dest(99, nullptr);
        EXPECT_EQ(call("GetBestRoute2", {0, 0, 0, dest, 0, route, src}), 87u);
        put_dest(0, nullptr);
        EXPECT_EQ(call("GetBestRoute2", {0, 0, 0, dest, 0, route, src}), 87u);
        EXPECT_EQ(memory.read_u8(route).value_or(0), 0xCCu) << "untouched on 87";
        EXPECT_EQ(memory.read_u8(src).value_or(0), 0xCCu);

        put_dest(2, "127.0.0.1");
        EXPECT_EQ(call("GetBestRoute2", {0, 9999, 0, dest, 0, route, src}), 2u);
        EXPECT_NE(memory.read_u8(route).value_or(0xCC), 0xCCu) << "written on 2";

        // LUID 0 and index 0 mean any interface.
        ASSERT_TRUE(memory.write_u64(luid, 0).is_ok());
        poison();
        EXPECT_EQ(call("GetBestRoute2", {luid, 0, 0, dest, 0, route, src}), 0u);
        EXPECT_EQ(u32at(route + 8), lo0);
        EXPECT_NE(u64at(route + 0), 0u);
        EXPECT_EQ(memory.read_u16(route + 12).value_or(0), 2u);
        EXPECT_EQ(memory.read_u8(route + 40).value_or(0), 32u);
        EXPECT_EQ(memory.read_u16(route + 44).value_or(0), 2u);
        EXPECT_EQ(u32at(route + 48), 0u) << "on-link next hop";
        EXPECT_EQ(u32at(route + 88), 2u) << "MIB_IPPROTO_LOCAL";
        EXPECT_EQ(memory.read_u8(route + 92).value_or(0), 1u);
        EXPECT_EQ(memory.read_u8(route + 93).value_or(0), 1u);
        EXPECT_EQ(memory.read_u8(route + 95).value_or(1), 0u);
        EXPECT_NE(u32at(route + 84), 0u) << "non-zero metric";
        EXPECT_NE(u32at(route + 96), 0u) << "age";
        EXPECT_EQ(memory.read_u16(src).value_or(0), 2u);
        EXPECT_EQ(memory.read_u32(src + 4).value_or(0), htonl(INADDR_LOOPBACK));

        put_dest(23, "::1");
        EXPECT_EQ(call("GetBestRoute2", {0, 0, 0, dest, 0, route, src}), 0u);
        EXPECT_EQ(u32at(route + 8), lo0);
        EXPECT_EQ(memory.read_u16(route + 12).value_or(0), 23u);
        EXPECT_EQ(memory.read_u8(route + 40).value_or(0), 128u);
        EXPECT_EQ(memory.read_u8(route + 92).value_or(0), 1u);
        EXPECT_EQ(memory.read_u16(src).value_or(0), 23u);
    }
}
