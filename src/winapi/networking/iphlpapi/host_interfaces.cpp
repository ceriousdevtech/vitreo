/// @file host_interfaces.cpp
/// @brief Host network interface snapshot in Windows IP Helper terms.

#include "host_interfaces.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <unordered_map>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/route.h>
#include <netinet6/in6_var.h>
#include <sys/sysctl.h>
#endif

namespace vitreo::winapi::iphlp {

namespace {

#include "adapter_contract.inc"
#include "if_row2_contract.inc"

// InterfaceAndOperStatusFlags bits (ifdef.h NET_IF_...: HardwareInterface is
// bit 0, ConnectorPresent bit 2, NotMediaConnected bit 4).
constexpr u8 kStatusHardwareInterface = 0x01;
constexpr u8 kStatusConnectorPresent = 0x04;
constexpr u8 kStatusNotMediaConnected = 0x10;

// Windows' software loopback adapter (iphlp_adapters_probe gaa-loopback:
// FriendlyName, Description, Mtu, metrics; ift2 loopback_row_mtu).
constexpr const char* kLoopbackFriendlyName = "Loopback Pseudo-Interface 1";
constexpr const char* kLoopbackDescription = "Software Loopback Interface 1";
constexpr u32 kLoopbackIpMtu = 0xFFFFFFFFu;
constexpr u32 kLoopbackLinkMtu = 1500u;
constexpr u32 kLoopbackMetric = 75u;

[[nodiscard]] NetAddress v4_address(const in_addr& a) {
    NetAddress out;
    out.family = kGuestAfInet;
    std::memcpy(out.bytes.data(), &a, 4);
    return out;
}

[[nodiscard]] NetAddress v6_address(const in6_addr& a, u32 scope) {
    NetAddress out;
    out.family = kGuestAfInet6;
    std::memcpy(out.bytes.data(), &a, 16);
    out.scope_id = scope;
    return out;
}

[[nodiscard]] bool v6_link_local(const std::array<u8, 16>& b) noexcept {
    return b[0] == 0xFE && (b[1] & 0xC0) == 0x80;
}
[[nodiscard]] bool v6_site_local(const std::array<u8, 16>& b) noexcept {
    return b[0] == 0xFE && (b[1] & 0xC0) == 0xC0;
}
[[nodiscard]] bool v6_loopback(const std::array<u8, 16>& b) noexcept {
    for (int i = 0; i < 15; ++i) if (b[static_cast<usize>(i)]) return false;
    return b[15] == 1;
}

/// Windows scope id of an IPv6 address on interface `index`: the ZoneIndices
/// entry of the address's scope level (link-local unicast and multicast by
/// their scope nibble; ::1 and global addresses have no zone).
[[nodiscard]] u32 v6_scope_id(const std::array<u8, 16>& b, const std::array<u32, 16>& zones) {
    if (b[0] == 0xFF) return zones[b[1] & 0x0Fu];
    if (v6_link_local(b)) return zones[kScopeLevelLink];
    if (v6_site_local(b)) return zones[kScopeLevelSite];
    return 0;
}

[[nodiscard]] std::array<u32, 16> default_zone_indices(u32 index) noexcept {
    std::array<u32, 16> zones{};
    for (u32 level = 0; level < 16; ++level) {
        zones[level] = level <= kScopeLevelSubnet ? index : (level == kScopeLevelGlobal ? 0u : 1u);
    }
    return zones;
}

[[nodiscard]] u8 prefix_from_mask(const u8* mask, usize bytes) {
    u8 n = 0;
    for (usize i = 0; i < bytes; ++i) {
        u8 v = mask[i];
        while (v & 0x80u) { ++n; v = static_cast<u8>(v << 1); }
        if (mask[i] != 0xFF) break;
    }
    return n;
}

/// Windows 10 automatic interface metric (Learn: "The Automatic Metric
/// feature for IPv4 routes", Windows 10 tables). The software loopback
/// interface's metric is 75 on Windows (iphlp_adapters_probe gaa-loopback).
[[nodiscard]] u32 automatic_metric(const HostInterface& iface) noexcept {
    if (iface.loopback) return kLoopbackMetric;
    const u64 s = iface.link_speed;
    constexpr u64 K = 1000, M = 1000 * K, G = 1000 * M;
    if (iface.physical_medium == kNdisPhysicalMediumNative802_11) {
        if (s >= 2 * G) return 25;
        if (s >= 500 * M) return 30;
        if (s >= 200 * M) return 35;
        if (s >= 150 * M) return 40;
        if (s >= 80 * M) return 45;
        if (s >= 50 * M) return 50;
        if (s >= 20 * M) return 55;
        if (s >= 10 * M) return 60;
        if (s >= 4 * M) return 65;
        if (s >= 2 * M) return 70;
        if (s >= 500 * K) return 75;
        if (s >= 200 * K) return 80;
        return 85;
    }
    if (s >= 100 * G) return 5;
    if (s >= 40 * G) return 10;
    if (s >= 10 * G) return 15;
    if (s >= 2 * G) return 20;
    if (s >= 200 * M) return 25;
    if (s >= 80 * M) return 35;
    if (s >= 20 * M) return 45;
    if (s >= 4 * M) return 55;
    if (s >= 500 * K) return 65;
    return 75;
}

#ifdef __APPLE__
// ---------------------------------------------------------------------------
// SystemConfiguration
// ---------------------------------------------------------------------------
[[nodiscard]] std::string cf_string(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    const auto str = static_cast<CFStringRef>(value);
    const CFIndex length = CFStringGetLength(str);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<usize>(max), '\0');
    if (!CFStringGetCString(str, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

[[nodiscard]] CFTypeRef dict_get(CFDictionaryRef dict, const char* key) {
    if (!dict || CFGetTypeID(dict) != CFDictionaryGetTypeID()) return nullptr;
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(dict, k);
    CFRelease(k);
    return v;
}

struct ScInterface {
    std::string display_name;
    std::string type;  // kSCNetworkInterfaceType* value
};

/// Interface kinds and display names change only with hardware, so the list
/// (an IOKit walk) is cached briefly for the identity-only conversions.
[[nodiscard]] std::unordered_map<std::string, ScInterface> sc_interfaces() {
    static std::mutex mutex;
    static std::unordered_map<std::string, ScInterface> cache;
    static std::chrono::steady_clock::time_point stamp{};
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (stamp != std::chrono::steady_clock::time_point{} && now - stamp < std::chrono::seconds(5)) return cache;
    std::unordered_map<std::string, ScInterface> result;
    if (CFArrayRef all = SCNetworkInterfaceCopyAll()) {
        for (CFIndex i = 0; i < CFArrayGetCount(all); ++i) {
            auto iface = static_cast<SCNetworkInterfaceRef>(const_cast<void*>(CFArrayGetValueAtIndex(all, i)));
            const std::string bsd = cf_string(SCNetworkInterfaceGetBSDName(iface));
            if (bsd.empty()) continue;
            result[bsd] = {cf_string(SCNetworkInterfaceGetLocalizedDisplayName(iface)),
                           cf_string(SCNetworkInterfaceGetInterfaceType(iface))};
        }
        CFRelease(all);
    }
    cache = std::move(result);
    stamp = now;
    return cache;
}

struct ScService {
    std::string interface_name;
    std::string ipv4_method;
    std::string ipv6_method;
    std::vector<std::string> dns_servers;
    std::string domain;
    bool has_lease = false;
    i64 lease_start = 0;
    i64 lease_expires = 0;
    std::optional<std::array<u8, 4>> dhcp_server;
    std::map<u8, std::vector<u8>> dhcp_options;
    std::vector<std::string> ipv4_addresses;
};

[[nodiscard]] i64 cf_date_unix(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFDateGetTypeID()) return 0;
    return static_cast<i64>(CFDateGetAbsoluteTime(static_cast<CFDateRef>(value)) + kCFAbsoluteTimeIntervalSince1970);
}

[[nodiscard]] std::vector<std::string> cf_string_array(CFTypeRef value) {
    std::vector<std::string> out;
    if (!value || CFGetTypeID(value) != CFArrayGetTypeID()) return out;
    const auto array = static_cast<CFArrayRef>(value);
    for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) out.push_back(cf_string(CFArrayGetValueAtIndex(array, i)));
    return out;
}

/// Per-service configuration and state, keyed by BSD interface name. One
/// round trip to configd (SCDynamicStoreCopyMultiple).
[[nodiscard]] std::unordered_map<std::string, ScService> sc_services() {
    std::unordered_map<std::string, ScService> by_interface;
    SCDynamicStoreRef store = SCDynamicStoreCreate(nullptr, CFSTR("vitreo.iphlpapi"), nullptr, nullptr);
    if (!store) return by_interface;
    CFStringRef patterns_raw[] = {
        CFSTR("State:/Network/Service/[^/]+/(IPv4|IPv6|DNS|DHCP)"),
        CFSTR("Setup:/Network/Service/[^/]+/(Interface|IPv4|IPv6)"),
        CFSTR("Setup:/Network/Service/[^/]+$"),
    };
    CFArrayRef patterns = CFArrayCreate(nullptr, reinterpret_cast<const void**>(patterns_raw), 3, &kCFTypeArrayCallBacks);
    CFDictionaryRef values = SCDynamicStoreCopyMultiple(store, nullptr, patterns);
    CFRelease(patterns);
    CFRelease(store);
    if (!values) return by_interface;

    std::map<std::string, ScService> services;  // by service ID
    std::map<std::string, bool> inactive;
    const CFIndex count = CFDictionaryGetCount(values);
    std::vector<const void*> keys(static_cast<usize>(count)), vals(static_cast<usize>(count));
    CFDictionaryGetKeysAndValues(values, keys.data(), vals.data());
    for (CFIndex i = 0; i < count; ++i) {
        const std::string key = cf_string(keys[static_cast<usize>(i)]);
        const auto dict = static_cast<CFDictionaryRef>(vals[static_cast<usize>(i)]);
        if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) continue;
        const bool state = key.rfind("State:/Network/Service/", 0) == 0;
        const usize id_start = key.find("/Service/") + 9;
        const usize id_end = key.find('/', id_start);
        const std::string id = key.substr(id_start, id_end == std::string::npos ? std::string::npos : id_end - id_start);
        const std::string entity = id_end == std::string::npos ? std::string() : key.substr(id_end + 1);
        ScService& svc = services[id];
        if (!state && entity.empty()) {
            if (dict_get(dict, "__INACTIVE__")) inactive[id] = true;
            continue;
        }
        if (state && (entity == "IPv4" || entity == "IPv6")) {
            const std::string name = cf_string(dict_get(dict, "InterfaceName"));
            if (!name.empty()) svc.interface_name = name;
            if (entity == "IPv4") svc.ipv4_addresses = cf_string_array(dict_get(dict, "Addresses"));
        } else if (state && entity == "DNS") {
            svc.dns_servers = cf_string_array(dict_get(dict, "ServerAddresses"));
            svc.domain = cf_string(dict_get(dict, "DomainName"));
        } else if (state && entity == "DHCP") {
            svc.lease_start = cf_date_unix(dict_get(dict, "LeaseStartTime"));
            svc.lease_expires = cf_date_unix(dict_get(dict, "LeaseExpirationTime"));
            if (!svc.lease_expires && svc.lease_start) {
                // RFC 2132 option 51: IP address lease time, seconds.
                if (CFTypeRef t = dict_get(dict, "Option_51"); t && CFGetTypeID(t) == CFDataGetTypeID() &&
                    CFDataGetLength(static_cast<CFDataRef>(t)) == 4) {
                    const u8* p = CFDataGetBytePtr(static_cast<CFDataRef>(t));
                    svc.lease_expires = svc.lease_start + ((i64{p[0]} << 24) | (i64{p[1]} << 16) | (i64{p[2]} << 8) | p[3]);
                }
            }
            svc.has_lease = svc.lease_start != 0;
            // Every cached option is published as "Option_<code>" CFData.
            const CFIndex n = CFDictionaryGetCount(dict);
            std::vector<const void*> option_keys(static_cast<usize>(n)), option_values(static_cast<usize>(n));
            CFDictionaryGetKeysAndValues(dict, option_keys.data(), option_values.data());
            for (CFIndex k = 0; k < n; ++k) {
                const std::string name = cf_string(option_keys[static_cast<usize>(k)]);
                CFTypeRef value = option_values[static_cast<usize>(k)];
                if (name.rfind("Option_", 0) != 0 || CFGetTypeID(value) != CFDataGetTypeID()) continue;
                const unsigned long code = std::strtoul(name.c_str() + 7, nullptr, 10);
                if (code == 0 || code > 254) continue;
                const auto data = static_cast<CFDataRef>(value);
                const u8* bytes = CFDataGetBytePtr(data);
                svc.dhcp_options[static_cast<u8>(code)].assign(bytes, bytes + CFDataGetLength(data));
            }
            // RFC 2132 option 54: server identifier.
            if (CFTypeRef server = dict_get(dict, "Option_54"); server && CFGetTypeID(server) == CFDataGetTypeID() &&
                CFDataGetLength(static_cast<CFDataRef>(server)) == 4) {
                std::array<u8, 4> bytes{};
                std::memcpy(bytes.data(), CFDataGetBytePtr(static_cast<CFDataRef>(server)), 4);
                svc.dhcp_server = bytes;
            }
        } else if (!state && entity == "Interface") {
            const std::string device = cf_string(dict_get(dict, "DeviceName"));
            if (svc.interface_name.empty() && !device.empty()) svc.interface_name = device;
        } else if (!state && entity == "IPv4") {
            svc.ipv4_method = cf_string(dict_get(dict, "ConfigMethod"));
        } else if (!state && entity == "IPv6") {
            svc.ipv6_method = cf_string(dict_get(dict, "ConfigMethod"));
        }
    }
    CFRelease(values);
    for (auto& [id, svc] : services) {
        if (svc.interface_name.empty() || inactive.count(id)) continue;
        auto [it, inserted] = by_interface.try_emplace(svc.interface_name, svc);
        // An interface can carry several services (e.g. a VPN on top); the one
        // holding live IPv4 state describes the adapter.
        if (!inserted && it->second.ipv4_addresses.empty() && !svc.ipv4_addresses.empty()) it->second = svc;
    }
    return by_interface;
}

// ---------------------------------------------------------------------------
// Routing socket sysctls
// ---------------------------------------------------------------------------
[[nodiscard]] usize sa_round(const sockaddr* sa) noexcept {
    if (!sa || sa->sa_len == 0) return sizeof(u32);
    return 1 + ((static_cast<usize>(sa->sa_len) - 1) | (sizeof(u32) - 1));
}

[[nodiscard]] std::vector<u8> route_sysctl(int family, int what) {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, family, what, 0};
    for (int attempt = 0; attempt < 4; ++attempt) {
        size_t size = 0;
        if (::sysctl(mib, 6, nullptr, &size, nullptr, 0) != 0) return {};
        std::vector<u8> buffer(size + size / 4 + 512);
        size = buffer.size();
        if (::sysctl(mib, 6, buffer.data(), &size, nullptr, 0) == 0) {
            buffer.resize(size);
            return buffer;
        }
        if (errno != ENOMEM) return {};
    }
    return {};
}

/// Collect the sockaddrs that follow a routing message header, by RTAX slot.
void split_addrs(const u8* p, const u8* end, int addrs, std::array<const sockaddr*, RTAX_MAX>& out) {
    out.fill(nullptr);
    for (int i = 0; i < RTAX_MAX && p < end; ++i) {
        if (!(addrs & (1 << i))) continue;
        const auto* sa = reinterpret_cast<const sockaddr*>(p);
        out[static_cast<usize>(i)] = sa;
        p += sa_round(sa);
    }
}

/// KAME stores a link-local zone inside bytes 2..3 of kernel sockaddr_in6.
[[nodiscard]] NetAddress kernel_v6(const sockaddr_in6* sin6, u32 fallback_scope) {
    in6_addr a = sin6->sin6_addr;
    u32 scope = sin6->sin6_scope_id;
    const bool scoped = (a.s6_addr[0] == 0xFE && (a.s6_addr[1] & 0xC0) == 0x80) ||
                        (a.s6_addr[0] == 0xFF && ((a.s6_addr[1] & 0x0F) == 1 || (a.s6_addr[1] & 0x0F) == 2));
    if (scoped) {
        const u32 embedded = (static_cast<u32>(a.s6_addr[2]) << 8) | a.s6_addr[3];
        if (!scope) scope = embedded ? embedded : fallback_scope;
        a.s6_addr[2] = 0;
        a.s6_addr[3] = 0;
    }
    return v6_address(a, scope);
}
#endif

[[nodiscard]] std::string ascii_upper_hex(u32 v, int digits) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out(static_cast<usize>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) { out[static_cast<usize>(i)] = kHex[v & 15u]; v >>= 4; }
    return out;
}

struct TypeName { u32 type; const char* name; };
// NDIS interface-name prefixes (iphlp_adapters_probe name-type): any other
// type is spelled "iftype<N>".
constexpr TypeName kTypeNames[] = {
    {1, "other"}, {6, "ethernet"}, {9, "tokenring"}, {23, "ppp"}, {24, "loopback"},
    {37, "atm"}, {71, "wireless"}, {131, "tunnel"}, {144, "ieee1394"},
};

}  // namespace

u32 NetAddress::v4_network_order() const noexcept {
    u32 v = 0;
    std::memcpy(&v, bytes.data(), 4);
    return v;
}

std::string NetAddress::to_dotted() const {
    char text[INET_ADDRSTRLEN] = {};
    in_addr a{};
    std::memcpy(&a, bytes.data(), 4);
    ::inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}

std::array<u32, 16> HostInterface::zone_indices() const noexcept {
    return default_zone_indices(index);
}

std::vector<Prefix> HostInterface::prefixes(u16 family) const {
    // Windows reports the on-link prefixes the stack derives for each address:
    // the subnet, the host route and (IPv4) the subnet broadcast, plus the
    // multicast range and the IPv4 limited broadcast, ascending by address
    // (iphlp_adapters_probe gaa-loopback.prefix / gaa-primary-v4).
    std::vector<Prefix> out;
    const bool v4 = family == kGuestAfInet;
    if (v4 ? !ipv4_enabled : !ipv6_enabled) return out;
    const auto zones = zone_indices();
    const usize width = v4 ? 4 : 16;
    const u32 max_len = v4 ? 32 : 128;
    auto add = [&](NetAddress a, u32 length) {
        if (!v4) a.scope_id = v6_loopback(a.bytes) ? 0 : v6_scope_id(a.bytes, zones);
        for (const auto& p : out) if (p.length == length && p.address == a) return;
        out.push_back({a, length});
    };
    for (const auto& u : unicast) {
        if (u.address.family != family) continue;
        NetAddress subnet = u.address;
        subnet.scope_id = 0;
        for (usize i = 0; i < width; ++i) {
            const u32 bit = static_cast<u32>(i) * 8;
            const u8 keep = u.prefix_length >= bit + 8 ? 0xFF
                          : u.prefix_length <= bit ? 0x00
                          : static_cast<u8>(0xFF << (8 - (u.prefix_length - bit)));
            subnet.bytes[i] = static_cast<u8>(subnet.bytes[i] & keep);
        }
        if (u.prefix_length < max_len) add(subnet, u.prefix_length);
        NetAddress host = u.address;
        host.scope_id = 0;
        add(host, max_len);
        if (v4 && u.prefix_length <= 30) {
            NetAddress broadcast = subnet;
            for (usize i = 0; i < 4; ++i) {
                const u32 bit = static_cast<u32>(i) * 8;
                const u8 keep = u.prefix_length >= bit + 8 ? 0xFF
                              : u.prefix_length <= bit ? 0x00
                              : static_cast<u8>(0xFF << (8 - (u.prefix_length - bit)));
                broadcast.bytes[i] = static_cast<u8>(broadcast.bytes[i] | static_cast<u8>(~keep));
            }
            add(broadcast, 32);
        }
    }
    NetAddress multicast_range;
    multicast_range.family = family;
    multicast_range.bytes[0] = v4 ? 224 : 0xFF;
    add(multicast_range, v4 ? 4 : 8);
    if (v4) {
        NetAddress limited;
        limited.family = family;
        std::fill(limited.bytes.begin(), limited.bytes.begin() + 4, u8{0xFF});
        add(limited, 32);
    }
    std::sort(out.begin(), out.end(), [](const Prefix& a, const Prefix& b) {
        if (a.address.bytes != b.address.bytes) return a.address.bytes < b.address.bytes;
        return a.length < b.length;
    });
    return out;
}

std::vector<NetAddress> HostInterface::reported_dns_servers() const {
    if (!dns_servers.empty() || !ipv6_enabled) return dns_servers;
    // An IPv6 interface with no configured DNS server lists the well-known
    // site-local resolvers fec0:0:0:ffff::1..3 (iphlp_adapters_probe
    // gaa-loopback.dns; the primary adapter, which has IPv4 DNS, lists none:
    // gaa-v6-primary dns=0).
    std::vector<NetAddress> defaults;
    const u32 site_zone = zone_indices()[kScopeLevelSite];
    for (u8 n = 1; n <= 3; ++n) {
        NetAddress a;
        a.family = kGuestAfInet6;
        a.bytes = {0xFE, 0xC0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, n};
        a.scope_id = site_zone;
        defaults.push_back(a);
    }
    return defaults;
}

const HostInterface* Snapshot::by_index(u32 index) const noexcept {
    for (const auto& i : interfaces) if (i.index == index) return &i;
    return nullptr;
}
const HostInterface* Snapshot::by_luid(u64 luid) const noexcept {
    for (const auto& i : interfaces) if (i.luid == luid) return &i;
    return nullptr;
}
const HostInterface* Snapshot::by_guid(const std::array<u8, 16>& guid) const noexcept {
    for (const auto& i : interfaces) if (i.guid == guid) return &i;
    return nullptr;
}

std::string format_guid_braced(const std::array<u8, 16>& g) {
    const u32 d1 = static_cast<u32>(g[0]) | (static_cast<u32>(g[1]) << 8) | (static_cast<u32>(g[2]) << 16) |
                   (static_cast<u32>(g[3]) << 24);
    const u32 d2 = static_cast<u32>(g[4]) | (static_cast<u32>(g[5]) << 8);
    const u32 d3 = static_cast<u32>(g[6]) | (static_cast<u32>(g[7]) << 8);
    std::string out = "{" + ascii_upper_hex(d1, 8) + "-" + ascii_upper_hex(d2, 4) + "-" + ascii_upper_hex(d3, 4) + "-";
    for (usize i = 8; i < 16; ++i) {
        if (i == 10) out += "-";
        out += ascii_upper_hex(g[i], 2);
    }
    return out + "}";
}

std::string ndis_interface_name(u64 luid) {
    const u32 type = luid_if_type(luid);
    std::string prefix;
    for (const auto& t : kTypeNames) if (t.type == type) prefix = t.name;
    if (prefix.empty()) prefix = "iftype" + std::to_string(type);
    return prefix + "_" + std::to_string(luid_net_index(luid));
}

std::optional<u64> parse_ndis_interface_name(std::string_view name) {
    const usize sep = name.rfind('_');
    if (sep == std::string_view::npos || sep == 0 || sep + 1 >= name.size()) return std::nullopt;
    const std::string_view prefix = name.substr(0, sep), digits = name.substr(sep + 1);
    u64 index = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        index = index * 10 + static_cast<u64>(c - '0');
        if (index > 0xFFFFFFu) return std::nullopt;
    }
    std::optional<u32> type;
    for (const auto& t : kTypeNames) if (prefix == t.name) type = t.type;
    if (!type && prefix.size() > 6 && prefix.substr(0, 6) == "iftype") {
        u32 value = 0;
        for (char c : prefix.substr(6)) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<u32>(c - '0');
            if (value > 0xFFFFu) return std::nullopt;
        }
        type = value;
    }
    if (!type) return std::nullopt;
    return make_luid(*type, static_cast<u32>(index));
}

std::vector<const HostInterface*> adapter_order(const Snapshot& snapshot, bool include_all) {
    std::vector<const HostInterface*> out;
    for (const auto& i : snapshot.interfaces) if (include_all || i.ipv4_enabled || i.ipv6_enabled) out.push_back(&i);
    auto metric = [](const HostInterface* i) {
        u32 m = 0xFFFFFFFFu;
        if (i->ipv4_enabled) m = std::min(m, i->ipv4_metric);
        if (i->ipv6_enabled) m = std::min(m, i->ipv6_metric);
        return m;
    };
    // Learn (GetAdaptersAddresses remarks): "Starting with Windows 10, the order
    // ... is determined by the IPv4 or IPv6 route metric"; the loopback adapter
    // is listed last (iphlp_adapters_probe gaa-order).
    std::stable_sort(out.begin(), out.end(), [&](const HostInterface* a, const HostInterface* b) {
        if (a->loopback != b->loopback) return !a->loopback;
        if (metric(a) != metric(b)) return metric(a) < metric(b);
        return a->index < b->index;
    });
    return out;
}

Snapshot snapshot_host_interfaces(SnapshotDetail detail) {
    Snapshot snap;
#ifdef __APPLE__
    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0) return snap;
    snap.ok = true;
    const auto sc_ifaces = sc_interfaces();
    std::map<std::string, HostInterface> by_name;
    const int media_socket = ::socket(AF_INET, SOCK_DGRAM, 0);

    // Link layer first: every interface has exactly one AF_LINK entry.
    for (auto* e = head; e; e = e->ifa_next) {
        if (!e->ifa_name || !e->ifa_addr || e->ifa_addr->sa_family != AF_LINK) continue;
        const auto* link = reinterpret_cast<const sockaddr_dl*>(e->ifa_addr);
        const auto* data = static_cast<const if_data*>(e->ifa_data);
        HostInterface iface;
        iface.bsd_name = e->ifa_name;
        iface.index = link->sdl_index;
        iface.host_flags = e->ifa_flags;
        iface.loopback = (e->ifa_flags & IFF_LOOPBACK) != 0;
        const u32 host_type = data ? data->ifi_type : 0;
        iface.link_mtu = data ? data->ifi_mtu : 0;
        iface.ip_mtu = iface.link_mtu;
        iface.link_speed = data ? data->ifi_baudrate : 0;
        if (link->sdl_alen > 0 && link->sdl_alen <= kIF_MAX_PHYS_ADDRESS_LENGTH) {
            const auto* mac = reinterpret_cast<const u8*>(LLADDR(link));
            iface.mac.assign(mac, mac + link->sdl_alen);
        }
        const auto sc = sc_ifaces.find(iface.bsd_name);
        const std::string sc_type = sc == sc_ifaces.end() ? std::string() : sc->second.type;
        const bool wireless = sc_type == "IEEE80211";
        iface.hardware = sc != sc_ifaces.end() && (sc_type == "Ethernet" || wireless || sc_type == "FireWire");
        const bool p2p = (e->ifa_flags & IFF_POINTOPOINT) != 0;

        // ipifcons.h IFTYPE is the IANA ifType the host already reports, except
        // where BSD uses private numbers (gif, stf) or reports Wi-Fi as Ethernet.
        if (iface.loopback) {
            iface.if_type = kIF_TYPE_SOFTWARE_LOOPBACK;
            iface.media_type = static_cast<u32>(kLoopbackMediaType);
            iface.physical_medium = static_cast<u32>(kLoopbackPhysicalMediumType);
            iface.access_type = static_cast<u32>(kLoopbackAccessType);
            iface.tunnel_type = static_cast<u32>(kLoopbackTunnelType);
            iface.link_speed = kLoopbackTransmitLinkSpeed;
            iface.ip_mtu = kLoopbackIpMtu;
            iface.link_mtu = kLoopbackLinkMtu;
        } else if (wireless) {
            iface.if_type = kIF_TYPE_IEEE80211;
            iface.media_type = kNdisMediumNative802_11;
            iface.physical_medium = kNdisPhysicalMediumNative802_11;
            iface.access_type = kNET_IF_ACCESS_BROADCAST;
        } else if (host_type == IFT_GIF || host_type == IFT_STF) {
            iface.if_type = kIF_TYPE_TUNNEL;
            iface.tunnel_type = host_type == IFT_STF ? kTUNNEL_TYPE_6TO4 : kTUNNEL_TYPE_DIRECT;
            iface.media_type = kNdisMediumTunnel;
            iface.physical_medium = kNdisPhysicalMediumUnspecified;
            iface.access_type = kNET_IF_ACCESS_POINT_TO_POINT;
        } else if (host_type == IFT_PPP) {
            iface.if_type = kIF_TYPE_PPP;
            iface.media_type = kNdisMediumWan;
            iface.physical_medium = kNdisPhysicalMediumUnspecified;
            iface.access_type = kNET_IF_ACCESS_POINT_TO_POINT;
        } else if (host_type == IFT_ETHER || host_type == IFT_BRIDGE || host_type == IFT_L2VLAN ||
                   host_type == IFT_IEEE8023ADLAG) {
            iface.if_type = host_type;
            iface.media_type = kNdisMedium802_3;
            iface.physical_medium = iface.hardware ? kNdisPhysicalMedium802_3 : kNdisPhysicalMediumUnspecified;
            iface.access_type = kNET_IF_ACCESS_BROADCAST;
        } else {
            // Layer-3 interfaces without a link header (utun, ipsec, cellular).
            iface.if_type = host_type ? host_type : kIF_TYPE_OTHER;
            iface.media_type = kNdisMediumIP;
            iface.physical_medium = kNdisPhysicalMediumUnspecified;
            iface.access_type = p2p ? kNET_IF_ACCESS_POINT_TO_POINT : kNET_IF_ACCESS_POINT_TO_MULTI_POINT;
        }
        iface.connection_type = kNET_IF_CONNECTION_DEDICATED;
        const bool up = (e->ifa_flags & IFF_UP) != 0;
        bool running = (e->ifa_flags & IFF_RUNNING) != 0;
        // Carrier: Darwin keeps IFF_RUNNING on Ethernet ports with no link;
        // the media status is what ifconfig reports as active/inactive.
        if (media_socket >= 0) {
            ifmediareq media{};
            std::strncpy(media.ifm_name, e->ifa_name, sizeof(media.ifm_name) - 1);
            if (::ioctl(media_socket, SIOCGIFMEDIA, &media) == 0 && (media.ifm_status & IFM_AVALID))
                running = running && (media.ifm_status & IFM_ACTIVE) != 0;
        }
        iface.admin_status = up ? kNET_IF_ADMIN_STATUS_UP : kNET_IF_ADMIN_STATUS_DOWN;
        iface.oper_status = up && running ? kIfOperStatusUp : kIfOperStatusDown;
        iface.media_connect_state = !up ? kMediaConnectStateUnknown
                                  : running ? kMediaConnectStateConnected : kMediaConnectStateDisconnected;
        if (!iface.loopback) {
            if (iface.hardware) iface.status_flags |= kStatusHardwareInterface | kStatusConnectorPresent;
            if (up && !running) iface.status_flags |= kStatusNotMediaConnected;
        }
        iface.guid = interface_guid_for_name(iface.bsd_name);
        // A distinct, stable identity for the network the interface is on.
        iface.network_guid = interface_guid_for_name(iface.bsd_name + "/network");
        // NET_LUID: the software loopback adapter is NetLuidIndex 0 on Windows
        // (iphlp_adapters_probe gaa-loopback luid_index_is_nonzero=0).
        iface.luid = make_luid(iface.if_type, iface.loopback ? 0 : iface.index);
        if (iface.loopback) {
            iface.friendly_name = kLoopbackFriendlyName;
            iface.description = kLoopbackDescription;
        } else if (sc != sc_ifaces.end() && !sc->second.display_name.empty()) {
            iface.friendly_name = sc->second.display_name;
            iface.description = sc->second.display_name;
        } else {
            iface.friendly_name = iface.bsd_name;
            iface.description = iface.bsd_name;
        }
        by_name.emplace(iface.bsd_name, std::move(iface));
    }

    if (media_socket >= 0) ::close(media_socket);

    // IP addresses.
    const int v6_socket = detail == SnapshotDetail::Full ? ::socket(AF_INET6, SOCK_DGRAM, 0) : -1;
    const i64 now = static_cast<i64>(std::time(nullptr));
    for (auto* e = head; e; e = e->ifa_next) {
        if (!e->ifa_name || !e->ifa_addr) continue;
        auto it = by_name.find(e->ifa_name);
        if (it == by_name.end()) continue;
        HostInterface& iface = it->second;
        if (e->ifa_addr->sa_family == AF_INET) {
            UnicastAddress u;
            u.address = v4_address(reinterpret_cast<const sockaddr_in*>(e->ifa_addr)->sin_addr);
            if (e->ifa_netmask) {
                const auto& mask = reinterpret_cast<const sockaddr_in*>(e->ifa_netmask)->sin_addr;
                u.prefix_length = prefix_from_mask(reinterpret_cast<const u8*>(&mask), 4);
            } else {
                u.prefix_length = 32;
            }
            iface.ipv4_enabled = true;
            iface.unicast.push_back(u);
        } else if (e->ifa_addr->sa_family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(e->ifa_addr);
            UnicastAddress u;
            u.address = kernel_v6(sin6, iface.index);
            // Windows' software loopback adapter has ::1 only; lo0's fe80::1 is
            // a Darwin artefact with no Windows counterpart.
            if (iface.loopback && v6_link_local(u.address.bytes)) continue;
            if (e->ifa_netmask) {
                const auto& mask = reinterpret_cast<const sockaddr_in6*>(e->ifa_netmask)->sin6_addr;
                u.prefix_length = prefix_from_mask(mask.s6_addr, 16);
            } else {
                u.prefix_length = 128;
            }
            iface.ipv6_enabled = true;
            // Address flags and lifetimes (netinet6/in6_var.h); both are only
            // needed for the full adapter view.
            u32 flags6 = 0;
            if (v6_socket >= 0) {
                in6_ifreq request{};
                std::strncpy(request.ifr_name, e->ifa_name, sizeof(request.ifr_name) - 1);
                request.ifr_ifru.ifru_addr = *sin6;
                if (::ioctl(v6_socket, SIOCGIFAFLAG_IN6, &request) == 0) flags6 = static_cast<u32>(request.ifr_ifru.ifru_flags6);
                in6_ifreq life{};
                std::strncpy(life.ifr_name, e->ifa_name, sizeof(life.ifr_name) - 1);
                life.ifr_ifru.ifru_addr = *sin6;
                if (::ioctl(v6_socket, SIOCGIFALIFETIME_IN6, &life) == 0) {
                    const auto& lt = life.ifr_ifru.ifru_lifetime;
                    auto remaining = [&](time_t at) -> u32 {
                        if (at == 0) return 0xFFFFFFFFu;
                        return at <= now ? 0u : static_cast<u32>(std::min<i64>(at - now, 0xFFFFFFFEll));
                    };
                    u.valid_lifetime = remaining(lt.ia6t_expire);
                    u.preferred_lifetime = remaining(lt.ia6t_preferred);
                    u.lease_lifetime = u.valid_lifetime;
                }
            }
            if (flags6 & IN6_IFF_ANYCAST) {
                iface.anycast.push_back(u.address);
                continue;
            }
            if (flags6 & IN6_IFF_DUPLICATED) u.dad_state = kIpDadStateDuplicate;
            else if (flags6 & (IN6_IFF_TENTATIVE | IN6_IFF_OPTIMISTIC)) u.dad_state = kIpDadStateTentative;
            else if (flags6 & (IN6_IFF_DEPRECATED | IN6_IFF_DETACHED)) u.dad_state = kIpDadStateDeprecated;
            else u.dad_state = kIpDadStatePreferred;
            const bool link_local = v6_link_local(u.address.bytes);
            if (v6_loopback(u.address.bytes)) {
                u.prefix_origin = kIpPrefixOriginWellKnown;
                u.suffix_origin = kIpSuffixOriginWellKnown;
            } else if (link_local) {
                // Windows reports LinkLayerAddress for link-local suffixes even
                // when they are randomized (iphlp_adapters_probe raw-gaa).
                u.prefix_origin = kIpPrefixOriginWellKnown;
                u.suffix_origin = kIpSuffixOriginLinkLayerAddress;
            } else if (flags6 & IN6_IFF_DYNAMIC) {
                u.prefix_origin = kIpPrefixOriginDhcp;
                u.suffix_origin = kIpSuffixOriginDhcp;
            } else if (flags6 & IN6_IFF_AUTOCONF) {
                u.prefix_origin = kIpPrefixOriginRouterAdvertisement;
                u.suffix_origin = (flags6 & (IN6_IFF_TEMPORARY | IN6_IFF_SECURED))
                    ? kIpSuffixOriginRandom : kIpSuffixOriginLinkLayerAddress;
            } else {
                u.prefix_origin = kIpPrefixOriginManual;
                u.suffix_origin = kIpSuffixOriginManual;
            }
            // Only global, stable addresses are registered in DNS.
            if (!link_local && !v6_loopback(u.address.bytes) && !(flags6 & IN6_IFF_TEMPORARY))
                u.flags |= kIP_ADAPTER_ADDRESS_DNS_ELIGIBLE;
            iface.unicast.push_back(u);
        }
    }
    if (v6_socket >= 0) ::close(v6_socket);
    ::freeifaddrs(head);

    if (detail == SnapshotDetail::Full) {
        // Counters and 64-bit link speed (RTM_IFINFO2) and multicast groups
        // (RTM_NEWMADDR2), one sysctl.
        const auto list = route_sysctl(0, NET_RT_IFLIST2);
        std::unordered_map<u32, HostInterface*> by_index;
        for (auto& [name, iface] : by_name) by_index[iface.index] = &iface;
        for (usize off = 0; off + sizeof(u16) + 2 <= list.size();) {
            const auto* hdr = reinterpret_cast<const rt_msghdr*>(list.data() + off);
            if (hdr->rtm_msglen == 0 || off + hdr->rtm_msglen > list.size()) break;
            const u8* msg = list.data() + off;
            off += hdr->rtm_msglen;
            if (hdr->rtm_type == RTM_IFINFO2) {
                const auto* ifm = reinterpret_cast<const if_msghdr2*>(msg);
                auto found = by_index.find(ifm->ifm_index);
                if (found == by_index.end()) continue;
                HostInterface& iface = *found->second;
                const auto& d = ifm->ifm_data;
                if (!iface.loopback) {
                    iface.link_speed = d.ifi_baudrate;
                    iface.counters.in_octets = d.ifi_ibytes;
                    iface.counters.in_ucast = d.ifi_ipackets >= d.ifi_imcasts ? d.ifi_ipackets - d.ifi_imcasts : 0;
                    iface.counters.in_nucast = d.ifi_imcasts;
                    iface.counters.in_discards = d.ifi_iqdrops;
                    iface.counters.in_errors = d.ifi_ierrors;
                    iface.counters.in_unknown = d.ifi_noproto;
                    iface.counters.out_octets = d.ifi_obytes;
                    iface.counters.out_ucast = d.ifi_opackets >= d.ifi_omcasts ? d.ifi_opackets - d.ifi_omcasts : 0;
                    iface.counters.out_nucast = d.ifi_omcasts;
                    iface.counters.out_errors = d.ifi_oerrors;
                }
            } else if (hdr->rtm_type == RTM_NEWMADDR2) {
                const auto* ifmam = reinterpret_cast<const ifma_msghdr2*>(msg);
                auto found = by_index.find(ifmam->ifmam_index);
                if (found == by_index.end()) continue;
                std::array<const sockaddr*, RTAX_MAX> addrs{};
                split_addrs(msg + sizeof(ifma_msghdr2), msg + ifmam->ifmam_msglen, ifmam->ifmam_addrs, addrs);
                const sockaddr* group = addrs[RTAX_IFA];
                if (!group) continue;
                HostInterface& iface = *found->second;
                if (group->sa_family == AF_INET) {
                    iface.multicast.push_back(v4_address(reinterpret_cast<const sockaddr_in*>(group)->sin_addr));
                } else if (group->sa_family == AF_INET6) {
                    NetAddress a = kernel_v6(reinterpret_cast<const sockaddr_in6*>(group), iface.index);
                    a.scope_id = v6_scope_id(a.bytes, iface.zone_indices());
                    iface.multicast.push_back(a);
                }
            }
        }

        // Default gateways from the routing table (both families, scoped or not).
        for (int family : {AF_INET6, AF_INET}) {
            const auto dump = route_sysctl(family, NET_RT_DUMP);
            for (usize off = 0; off + sizeof(rt_msghdr) <= dump.size();) {
                const auto* rtm = reinterpret_cast<const rt_msghdr*>(dump.data() + off);
                if (rtm->rtm_msglen == 0 || off + rtm->rtm_msglen > dump.size()) break;
                const u8* msg = dump.data() + off;
                off += rtm->rtm_msglen;
                if ((rtm->rtm_flags & (RTF_UP | RTF_GATEWAY)) != (RTF_UP | RTF_GATEWAY) || (rtm->rtm_flags & RTF_HOST)) continue;
                std::array<const sockaddr*, RTAX_MAX> addrs{};
                split_addrs(msg + sizeof(rt_msghdr), msg + rtm->rtm_msglen, rtm->rtm_addrs, addrs);
                const sockaddr* dst = addrs[RTAX_DST];
                const sockaddr* gw = addrs[RTAX_GATEWAY];
                const sockaddr* mask = addrs[RTAX_NETMASK];
                if (!dst || !gw || dst->sa_family != family || gw->sa_family != family) continue;
                // Default route: destination and netmask all zero.
                bool zero_dst = true, zero_mask = true;
                const u8* dbytes = family == AF_INET
                    ? reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in*>(dst)->sin_addr)
                    : reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in6*>(dst)->sin6_addr);
                for (usize i = 0; i < (family == AF_INET ? 4u : 16u); ++i) if (dbytes[i]) zero_dst = false;
                if (mask && mask->sa_len > 0) {
                    const usize start = family == AF_INET ? offsetof(sockaddr_in, sin_addr) : offsetof(sockaddr_in6, sin6_addr);
                    for (usize i = start; i < mask->sa_len; ++i) if (reinterpret_cast<const u8*>(mask)[i]) zero_mask = false;
                }
                if (!zero_dst || !zero_mask) continue;
                auto found = by_index.find(rtm->rtm_index);
                if (found == by_index.end()) continue;
                HostInterface& iface = *found->second;
                NetAddress a = family == AF_INET
                    ? v4_address(reinterpret_cast<const sockaddr_in*>(gw)->sin_addr)
                    : kernel_v6(reinterpret_cast<const sockaddr_in6*>(gw), iface.index);
                if (family == AF_INET6) a.scope_id = v6_scope_id(a.bytes, iface.zone_indices());
                if (std::find(iface.gateways.begin(), iface.gateways.end(), a) == iface.gateways.end())
                    iface.gateways.push_back(a);
            }
        }

        // Configuration from SystemConfiguration: IP enablement, DHCP lease,
        // per-service DNS servers and suffix.
        const auto services = sc_services();
        for (auto& [name, iface] : by_name) {
            const auto svc = services.find(name);
            if (svc == services.end()) continue;
            const ScService& s = svc->second;
            if (!s.ipv4_method.empty()) iface.ipv4_enabled = true;
            if (!s.ipv6_method.empty()) iface.ipv6_enabled = true;
            iface.dhcp_enabled = s.ipv4_method == "DHCP" || s.ipv4_method == "BOOTP";
            iface.dns_suffix = s.domain;
            for (const auto& text : s.dns_servers) {
                in_addr a4{};
                in6_addr a6{};
                if (::inet_pton(AF_INET, text.c_str(), &a4) == 1) {
                    iface.dns_servers.push_back(v4_address(a4));
                } else {
                    const std::string bare = text.substr(0, text.find('%'));
                    if (::inet_pton(AF_INET6, bare.c_str(), &a6) == 1) {
                        NetAddress a = v6_address(a6, 0);
                        a.scope_id = v6_scope_id(a.bytes, iface.zone_indices());
                        iface.dns_servers.push_back(a);
                    }
                }
            }
            if (iface.dhcp_enabled && s.has_lease) {
                iface.lease_obtained = s.lease_start;
                iface.lease_expires = s.lease_expires;
                iface.dhcp_options = s.dhcp_options;
                if (s.dhcp_server) {
                    NetAddress server;
                    server.family = kGuestAfInet;
                    std::memcpy(server.bytes.data(), s.dhcp_server->data(), 4);
                    iface.dhcp_server = server;
                }
            }
        }
    }

    // IPv4 address origins: well-known loopback, APIPA link-local, the DHCP
    // lease, or manual (iphlp_adapters_probe gaa-primary-v4 origin_matches_dhcp).
    for (auto& [name, iface] : by_name) {
        for (auto& u : iface.unicast) {
            if (!u.address.is_v4()) continue;
            u.dad_state = kIpDadStatePreferred;
            const u8 first = u.address.bytes[0], second = u.address.bytes[1];
            if (first == 127) {
                u.prefix_origin = kIpPrefixOriginWellKnown;
                u.suffix_origin = kIpSuffixOriginWellKnown;
            } else if (first == 169 && second == 254) {
                u.prefix_origin = kIpPrefixOriginWellKnown;
                u.suffix_origin = kIpSuffixOriginLinkLayerAddress;
            } else if (iface.dhcp_enabled && iface.lease_obtained) {
                u.prefix_origin = kIpPrefixOriginDhcp;
                u.suffix_origin = kIpSuffixOriginDhcp;
                const i64 left = iface.lease_expires > now ? iface.lease_expires - now : 0;
                u.valid_lifetime = static_cast<u32>(std::min<i64>(left, 0xFFFFFFFEll));
                u.preferred_lifetime = u.valid_lifetime;
                u.lease_lifetime = u.valid_lifetime;
            } else {
                u.prefix_origin = kIpPrefixOriginManual;
                u.suffix_origin = kIpSuffixOriginManual;
            }
            if (first != 127 && !(first == 169 && second == 254)) u.flags |= kIP_ADAPTER_ADDRESS_DNS_ELIGIBLE;
        }
        // Windows lists IPv6 unicast addresses before IPv4 ones (gaa-loopback).
        std::stable_partition(iface.unicast.begin(), iface.unicast.end(),
                              [](const UnicastAddress& u) { return !u.address.is_v4(); });
        auto multicast_less = [](const NetAddress& a, const NetAddress& b) {
            if (a.family != b.family) return a.family == kGuestAfInet6;
            return a.bytes < b.bytes;
        };
        std::sort(iface.multicast.begin(), iface.multicast.end(), multicast_less);
        iface.multicast.erase(std::unique(iface.multicast.begin(), iface.multicast.end()), iface.multicast.end());
        std::stable_partition(iface.gateways.begin(), iface.gateways.end(),
                              [](const NetAddress& a) { return !a.is_v4(); });
        iface.ipv4_metric = iface.ipv4_enabled ? automatic_metric(iface) : 0;
        iface.ipv6_metric = iface.ipv6_enabled ? automatic_metric(iface) : 0;
        snap.interfaces.push_back(std::move(iface));
    }
    std::sort(snap.interfaces.begin(), snap.interfaces.end(),
              [](const HostInterface& a, const HostInterface& b) { return a.index < b.index; });
#else
    (void)detail;
#endif
    return snap;
}

}  // namespace vitreo::winapi::iphlp
