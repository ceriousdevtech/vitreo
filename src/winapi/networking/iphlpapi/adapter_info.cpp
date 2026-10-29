/// @file adapter_info.cpp
/// @brief GetAdaptersInfo, GetNetworkParams, GetInterfaceInfo and the DHCP
///        lease release/renew entry points.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {

// FIXED_INFO fields Windows reports with no WINS/routing/proxy configuration
// (iphlp_adapters_probe gnp: NodeType=8 EnableRouting=0 EnableProxy=0 EnableDns=0).
constexpr u32 kFixedInfoNodeType = 8;

[[nodiscard]] std::string dotted_mask(u8 prefix) {
    const u32 mask = prefix == 0 ? 0u : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u));
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", mask >> 24, (mask >> 16) & 255u, (mask >> 8) & 255u, mask & 255u);
    return text;
}

/// IPv4 resolvers the host uses system-wide, in order (FIXED_INFO.DnsServerList).
[[nodiscard]] std::vector<std::string> resolver_ipv4_servers() {
    std::vector<std::string> servers;
    std::ifstream in("/etc/resolv.conf");
    std::string line;
    while (in && std::getline(in, line)) {
        if (line.rfind("nameserver", 0) != 0) continue;
        std::size_t i = 10;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '#') ++j;
        const std::string server = line.substr(i, j - i);
        in_addr probe{};
        if (::inet_pton(AF_INET, server.c_str(), &probe) == 1) servers.push_back(server);
    }
    return servers;
}

/// IP_ADDR_STRING at blob offset `off`: dotted address, mask and the address
/// as Context (iphlp_adapters_probe raw-gai ctx/gwctx/dhcpctx, gnp dns0ctx).
void put_addr_string(GuestBlob& blob, const AdapterAbi& L, usize off, const std::string& address,
                     const std::string& mask) {
    blob.put_ascii(off + L.IP_ADDR_STRING_IpAddress, address, 16);
    blob.put_ascii(off + L.IP_ADDR_STRING_IpMask, mask, 16);
    in_addr value{};
    if (!address.empty() && ::inet_pton(AF_INET, address.c_str(), &value) == 1)
        blob.put<u32>(off + L.IP_ADDR_STRING_Context, value.s_addr);
}

} // namespace

void IphlpapiManager::impl_GetAdaptersInfo(Marshaler& m) {
    // ULONG GetAdaptersInfo(PIP_ADAPTER_INFO AdapterInfo, PULONG SizePointer)
    // https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersinfo
    // Layout: adapter_contract.inc, from the x86/x64 iphlp_adapters_probe. The
    // x86 DLL writes 640-byte entries with 32-bit lease times.
    const GuestAddr buffer = m.arg_ptr(0);
    const GuestAddr size_ptr = m.arg_ptr(1);
    const auto supplied = read_size(m, size_ptr);
    if (!supplied || !m.memory().guest_can_write(size_ptr, 4)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    if (!snapshot.ok) { m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE); return; }
    // IPv4 adapters only, and never the loopback interface (Learn remarks;
    // iphlp_adapters_probe gai-relations loopback_listed=0).
    std::vector<const iphlp::HostInterface*> adapters;
    for (const auto* iface : iphlp::adapter_order(snapshot))
        if (iface->ipv4_enabled && !iface->loopback) adapters.push_back(iface);
    if (adapters.empty()) { m.set_return(ERROR_NO_DATA); return; }

    const bool wide = m.cpu_is_64bit();
    const AdapterAbi& L = adapter_abi(m);
    GuestBlob blob(wide);
    std::vector<usize> entries;
    for (usize i = 0; i < adapters.size(); ++i) entries.push_back(blob.reserve(L.IP_ADAPTER_INFO_size, 8));
    const std::string none = "0.0.0.0";
    for (usize i = 0; i < adapters.size(); ++i) {
        const auto& a = *adapters[i];
        const usize e = entries[i];
        if (i + 1 < adapters.size()) blob.pointer(e, entries[i + 1]);
        blob.put<u32>(e + L.IP_ADAPTER_INFO_ComboIndex, a.index);  // gai-relations combo_eq_index
        blob.put_ascii(e + L.IP_ADAPTER_INFO_AdapterName, iphlp::format_guid_braced(a.guid), kMAX_ADAPTER_NAME_LENGTH + 4);
        blob.put_ascii(e + L.IP_ADAPTER_INFO_Description, ansi_text(a.description), kMAX_ADAPTER_DESCRIPTION_LENGTH + 4);
        const usize mac_len = std::min<usize>(a.mac.size(), kMAX_ADAPTER_ADDRESS_LENGTH);
        blob.put<u32>(e + L.IP_ADAPTER_INFO_AddressLength, static_cast<u32>(mac_len));
        blob.put_bytes(e + L.IP_ADAPTER_INFO_Address, a.mac.data(), mac_len);
        blob.put<u32>(e + L.IP_ADAPTER_INFO_Index, a.index);
        blob.put<u32>(e + L.IP_ADAPTER_INFO_Type, a.if_type);
        blob.put<u32>(e + L.IP_ADAPTER_INFO_DhcpEnabled, a.dhcp_enabled ? 1u : 0u);
        // CurrentIpAddress stays NULL (gai-relations current_ip_null=1).

        // IpAddressList: every IPv4 address, first one embedded, the rest
        // chained after all entries (gai-addresses ip_chain_len_eq).
        std::vector<std::pair<std::string, std::string>> ips, gws;
        for (const auto& u : a.unicast)
            if (u.address.is_v4()) ips.push_back({u.address.to_dotted(), dotted_mask(u.prefix_length)});
        for (const auto& g : a.gateways)
            if (g.is_v4()) gws.push_back({g.to_dotted(), "255.255.255.255"});
        // An IPv4 adapter without an address or a gateway reports the
        // unspecified address (not measured on Windows: its one adapter has
        // both; see report).
        if (ips.empty()) ips.push_back({none, none});
        if (gws.empty()) gws.push_back({none, none});
        auto fill_list = [&](usize first, const std::vector<std::pair<std::string, std::string>>& items) {
            usize at = first;
            for (usize k = 0; k < items.size(); ++k) {
                put_addr_string(blob, L, at, items[k].first, items[k].second);
                if (k + 1 < items.size()) {
                    const usize next = blob.reserve(L.IP_ADDR_STRING_size, 8);
                    blob.pointer(at, next);
                    at = next;
                }
            }
        };
        fill_list(e + L.IP_ADAPTER_INFO_IpAddressList, ips);
        fill_list(e + L.IP_ADAPTER_INFO_GatewayList, gws);
        if (a.dhcp_enabled && a.dhcp_server) {
            put_addr_string(blob, L, e + L.IP_ADAPTER_INFO_DhcpServer, a.dhcp_server->to_dotted(), "255.255.255.255");
        }
        // HaveWins stays FALSE: Vitreo configures no WINS servers, and the
        // WINS IP_ADDR_STRINGs are empty (gai-empty-strings).
        if (a.dhcp_enabled && a.lease_obtained) {
            if (wide) {
                blob.put<i64>(e + L.IP_ADAPTER_INFO_LeaseObtained, a.lease_obtained);
                blob.put<i64>(e + L.IP_ADAPTER_INFO_LeaseExpires, a.lease_expires);
            } else {
                blob.put<u32>(e + L.IP_ADAPTER_INFO_LeaseObtained, static_cast<u32>(a.lease_obtained));
                blob.put<u32>(e + L.IP_ADAPTER_INFO_LeaseExpires, static_cast<u32>(a.lease_expires));
            }
        }
    }
    const u32 need = static_cast<u32>(blob.size());
    // Windows clears the last error on the overflow and success paths
    // (iphlp_adapters_probe gai-size-query / gai-exact le=0x0).
    if (buffer == 0 || *supplied < need) {
        (void)m.write_u32(size_ptr, need);
        winapi::set_last_error(NO_ERROR);
        m.set_return(ERROR_BUFFER_OVERFLOW);
        return;
    }
    const auto image = blob.finish(buffer);
    if (!guest_writable(m, buffer, image.size()) ||
        m.memory().write_bytes(buffer, image.data(), image.size()).is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    winapi::set_last_error(NO_ERROR);
    m.set_return(NO_ERROR);
}

void IphlpapiManager::impl_GetNetworkParams(Marshaler& m) {
    // DWORD GetNetworkParams(PFIXED_INFO pFixedInfo, PULONG pOutBufLen)
    // https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getnetworkparams
    // FIXED_INFO layout per bitness from adapter_contract.inc; values measured
    // by iphlp_adapters_probe (gnp*).
    const GuestAddr info = m.arg_ptr(0);
    const GuestAddr size_ptr = m.arg_ptr(1);
    const auto have = read_size(m, size_ptr);
    if (!have) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const AdapterAbi& L = adapter_abi(m);
    GuestBlob blob(m.cpu_is_64bit());
    const usize base = blob.reserve(L.FIXED_INFO_size, 8);
    const std::string host = m_host_name_provider ? m_host_name_provider() : std::string();
    blob.put_ascii(base, host, L.FIXED_INFO_DomainName);  // HostName[MAX_HOSTNAME_LEN + 4]
    // DomainName: the computer's primary DNS suffix; a capsule is not joined
    // to a DNS domain (empty, as on the workgroup oracle).
    // CurrentDnsServer stays NULL (gnp current_dns_null=1).
    const auto servers = resolver_ipv4_servers();
    usize at = base + L.FIXED_INFO_DnsServerList;
    for (usize i = 0; i < servers.size(); ++i) {
        put_addr_string(blob, L, at, servers[i], "255.255.255.255");
        if (i + 1 < servers.size()) {
            const usize next = blob.reserve(L.IP_ADDR_STRING_size, 8);
            blob.pointer(at, next);
            at = next;
        }
    }
    blob.put<u32>(base + L.FIXED_INFO_NodeType, kFixedInfoNodeType);
    // ScopeId empty; EnableRouting/EnableProxy/EnableDns FALSE (gnp).
    const u32 need = static_cast<u32>(blob.size());
    if (info == 0 || *have < need) {
        (void)m.write_u32(size_ptr, need);
        winapi::set_last_error(NO_ERROR);  // gnp-size-query le=0x0
        m.set_return(ERROR_BUFFER_OVERFLOW);
        return;
    }
    const auto image = blob.finish(info);
    if (!guest_writable(m, info, image.size()) ||
        m.memory().write_bytes(info, image.data(), image.size()).is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    winapi::set_last_error(NO_ERROR);
    m.set_return(NO_ERROR);
}

void IphlpapiManager::impl_GetInterfaceInfo(Marshaler& m) {
    // DWORD GetInterfaceInfo(PIP_INTERFACE_INFO pIfTable, PULONG dwOutBufLen)
    // https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getinterfaceinfo
    const GuestAddr table = m.arg_ptr(0);
    const GuestAddr size_ptr = m.arg_ptr(1);
    const auto supplied = read_size(m, size_ptr);
    if (!supplied || !m.memory().guest_can_write(size_ptr, 4)) {
        m.set_return(ERROR_INVALID_PARAMETER);  // gii-null-null
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    if (!snapshot.ok) { m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE); return; }
    // IPv4 adapters, loopback excluded (Learn remarks; gii-relations).
    std::vector<const iphlp::HostInterface*> adapters;
    for (const auto* iface : iphlp::adapter_order(snapshot))
        if (iface->ipv4_enabled && !iface->loopback) adapters.push_back(iface);
    if (adapters.empty()) { m.set_return(ERROR_NO_DATA); return; }
    const AdapterAbi& L = adapter_abi(m);
    const u32 need = L.IP_INTERFACE_INFO_Adapter + static_cast<u32>(adapters.size()) * L.IP_ADAPTER_INDEX_MAP_size;
    if (table == 0 || *supplied < need) {
        (void)m.write_u32(size_ptr, need);
        winapi::set_last_error(NO_ERROR);  // gii-size-query le=0x0
        m.set_return(ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    std::vector<u8> image(need, 0);
    const u32 count = static_cast<u32>(adapters.size());
    std::memcpy(image.data(), &count, 4);
    for (usize i = 0; i < adapters.size(); ++i) {
        u8* map = image.data() + L.IP_INTERFACE_INFO_Adapter + i * L.IP_ADAPTER_INDEX_MAP_size;
        std::memcpy(map, &adapters[i]->index, 4);
        const auto name = tcpip_device_name(*adapters[i]);
        const usize chars = std::min<usize>(name.size(), kMAX_ADAPTER_NAME - 1);
        std::memcpy(map + L.IP_ADAPTER_INDEX_MAP_Name, name.data(), chars * 2);
    }
    if (m.memory().write_bytes(table, image.data(), image.size()).is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    winapi::set_last_error(NO_ERROR);
    m.set_return(NO_ERROR);
}

void IphlpapiManager::release_or_renew(Marshaler& m, const char* api) {
    // DWORD IpReleaseAddress / IpRenewAddress(PIP_ADAPTER_INDEX_MAP AdapterInfo)
    // Only Name selects the adapter (Learn remarks). Measured by
    // iphlp_misc_probe release-* / renew-*: NULL or an empty name is
    // ERROR_INVALID_PARAMETER, a name that is not "\DEVICE\TCPIP_{GUID}" is
    // ERROR_INVALID_NAME, an unknown adapter is ERROR_INVALID_PARAMETER with
    // the last error cleared.
    const GuestAddr map = m.arg_ptr(0);
    const AdapterAbi& L = adapter_abi(m);
    if (map == 0) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    const std::string name = read_guest_wide(m, map + L.IP_ADAPTER_INDEX_MAP_Name, kMAX_ADAPTER_NAME);
    if (name.empty()) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    auto upper = [](std::string s) {
        for (auto& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        return s;
    };
    const std::string text = upper(name);
    const bool shaped = text.size() == kTcpipDevicePrefix.size() + 38 &&
                        text.compare(0, kTcpipDevicePrefix.size(), kTcpipDevicePrefix) == 0 &&
                        text[kTcpipDevicePrefix.size()] == '{' && text.back() == '}';
    if (!shaped) { m.set_return(ERROR_INVALID_NAME_CODE); return; }
    winapi::set_last_error(NO_ERROR);
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    const iphlp::HostInterface* target = nullptr;
    for (const auto& iface : snapshot.interfaces)
        if (upper(std::string(kTcpipDevicePrefix) + iphlp::format_guid_braced(iface.guid)) == text && iface.ipv4_enabled &&
            !iface.loopback)
            target = &iface;
    if (!target) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    // The DHCP lease belongs to the host's configd; a guest must not release
    // or renew the Mac's address, and there is no per-capsule DHCP client.
    static std::mutex mutex;
    static std::unordered_set<std::string> reported;
    {
        std::lock_guard lock(mutex);
        if (reported.insert(api).second)
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!%s: releasing or renewing the host's DHCP lease\n", api);
    }
    m.set_return(ERROR_NOT_SUPPORTED);
}

void IphlpapiManager::impl_IpReleaseAddress(Marshaler& m) { release_or_renew(m, "IpReleaseAddress"); }
void IphlpapiManager::impl_IpRenewAddress(Marshaler& m) { release_or_renew(m, "IpRenewAddress"); }

} // namespace vitreo::winapi
