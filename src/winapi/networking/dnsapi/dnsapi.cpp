#include "dnsapi.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "core/memory/guest_memory.hpp"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netdb.h>
#include <poll.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 ERROR_INVALID_PARAMETER = 87;
constexpr u32 ERROR_OUTOFMEMORY = 14;
constexpr u32 DNS_INFO_NO_RECORDS = 9501;
constexpr u32 DNS_ERROR_RCODE_NAME_ERROR = 9003;
constexpr u16 DNS_TYPE_A = 0x0001;

// DNS_RECORDW (32-bit) + DNS_A_DATA:
// pNext(4) pName(4) wType(2) wDataLength(2) Flags(4) dwTtl(4) dwReserved(4)
// Data.A.IpAddress(4) → 28 bytes payload; allocate with trailing name.
constexpr u32 DNS_RECORD_A_SIZE = 32;

[[nodiscard]] std::string read_guest_wstring(Marshaler& m, GuestAddr ptr) {
    std::string out;
    if (!ptr) return out;
    for (u32 i = 0; i < 512; ++i) {
        const u16 ch = m.read_u16(ptr + i * 2).value_or(0);
        if (ch == 0) break;
        if (ch < 128) out.push_back(static_cast<char>(ch));
    }
    return out;
}

void write_guest_wstring(Marshaler& m, GuestAddr ptr, const std::string& s) {
    for (u32 i = 0; i < s.size(); ++i) {
        (void)m.write_u16(ptr + i * 2,
                          static_cast<u16>(static_cast<u8>(s[i])));
    }
    (void)m.write_u16(ptr + static_cast<u32>(s.size()) * 2, 0);
}

} // namespace

GuestAddr DnsapiManager::alloc_guest(Marshaler& m, u32 bytes) {
    if (bytes == 0) return 0;
    const u32 rounded = (bytes + 0xFFFu) & ~0xFFFu;
    std::lock_guard<std::mutex> lock(m_mu);
    for (int attempt = 0; attempt < 64; ++attempt) {
        const GuestAddr base = m_next_base;
        m_next_base += rounded;
        if (m_next_base < base) m_next_base = 0x6F900000u;
        auto mapped =
            m.memory().map_region(base, rounded, MemoryProtection::ReadWrite);
        if (mapped.is_ok()) {
            m_allocs[base] = rounded;
            return base;
        }
    }
    return 0;
}

void DnsapiManager::free_guest(Marshaler& m, GuestAddr addr) {
    if (!addr) return;
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_allocs.find(addr);
    if (it == m_allocs.end()) return;
    (void)m.memory().unmap_region(addr, it->second);
    m_allocs.erase(it);
}

void DnsapiManager::impl_DnsQuery_W(Marshaler& m) {
    // DNS_STATUS DnsQuery_W(PCWSTR, WORD, DWORD, PVOID, PDNS_RECORD*, PVOID*)
    GuestAddr name_ptr = m.arg_ptr(0);
    const u16 wtype = static_cast<u16>(m.arg(1));
    (void)m.arg(2);
    (void)m.arg_ptr(3);
    GuestAddr results_ptr = m.arg_ptr(4);
    (void)m.arg_ptr(5);

    if (!name_ptr || !results_ptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    (void)m.write_u32(results_ptr, 0);

    if (wtype != DNS_TYPE_A) {
        ipc::ipc_log("DnsQuery_W unsupported type=%u", static_cast<unsigned>(wtype));
        m.set_return(DNS_INFO_NO_RECORDS);
        return;
    }

    const std::string host = read_guest_wstring(m, name_ptr);
    if (host.empty()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        ipc::ipc_log("DnsQuery_W host='%s' getaddrinfo=%d", host.c_str(), rc);
        m.set_return(DNS_ERROR_RCODE_NAME_ERROR);
        return;
    }

    std::vector<u32> addrs;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET || !ai->ai_addr) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
        addrs.push_back(static_cast<u32>(sin->sin_addr.s_addr));
    }
    ::freeaddrinfo(res);

    if (addrs.empty()) {
        m.set_return(DNS_INFO_NO_RECORDS);
        return;
    }

    const u32 name_bytes = static_cast<u32>((host.size() + 1) * 2);
    const u32 per = DNS_RECORD_A_SIZE + name_bytes;
    const u32 total = per * static_cast<u32>(addrs.size());
    GuestAddr block = alloc_guest(m, total);
    if (!block) {
        m.set_return(ERROR_OUTOFMEMORY);
        return;
    }

    GuestAddr first = 0;
    GuestAddr prev = 0;
    for (u32 i = 0; i < addrs.size(); ++i) {
        const GuestAddr rec = block + static_cast<GuestAddr>(i * per);
        const GuestAddr name = rec + DNS_RECORD_A_SIZE;
        std::vector<u8> zeros(DNS_RECORD_A_SIZE, 0);
        (void)m.memory().write_bytes(rec, zeros.data(), DNS_RECORD_A_SIZE);
        write_guest_wstring(m, name, host);

        (void)m.write_u32(rec + 0, 0); // pNext filled below
        (void)m.write_u32(rec + 4, static_cast<u32>(name));
        (void)m.write_u16(rec + 8, DNS_TYPE_A);
        (void)m.write_u16(rec + 10, 4); // wDataLength
        (void)m.write_u32(rec + 12, 0); // Flags
        (void)m.write_u32(rec + 16, 60); // dwTtl
        (void)m.write_u32(rec + 20, 0);
        (void)m.write_u32(rec + 24, addrs[i]);

        if (!first) first = rec;
        if (prev) (void)m.write_u32(prev + 0, static_cast<u32>(rec));
        prev = rec;
    }

    (void)m.write_u32(results_ptr, static_cast<u32>(first));
    ipc::ipc_log("DnsQuery_W host='%s' addrs=%zu -> 0", host.c_str(),
                 addrs.size());
    m.set_return(0);
}

void DnsapiManager::impl_DnsRecordListFree(Marshaler& m) {
    // VOID DnsRecordListFree(PDNS_RECORD pRecordList, DNS_FREE_TYPE FreeType)
    GuestAddr head = m.arg_ptr(0);
    (void)m.arg(1);
    if (!head) {
        m.set_return(0);
        return;
    }
    GuestAddr free_base = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_allocs.count(head)) {
            free_base = head;
        } else {
            for (const auto& kv : m_allocs) {
                if (head >= kv.first && head < kv.first + kv.second) {
                    free_base = kv.first;
                    break;
                }
            }
        }
    }
    if (free_base) free_guest(m, free_base);
    m.set_return(0);
}

void DnsapiManager::impl_DnsFree(Marshaler& m) {
    // VOID DnsFree(PVOID pData, DNS_FREE_TYPE FreeType)
    GuestAddr data = m.arg_ptr(0);
    (void)m.arg(1);
    if (!data) {
        m.set_return(0);
        return;
    }
    GuestAddr free_base = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_allocs.count(data)) {
            free_base = data;
        } else {
            for (const auto& kv : m_allocs) {
                if (data >= kv.first && data < kv.first + kv.second) {
                    free_base = kv.first;
                    break;
                }
            }
        }
    }
    if (free_base) free_guest(m, free_base);
    m.set_return(0);
}

namespace {

// windns.h via cl /d1reportSingleClassLayout on SDK 10.0.26100.0.
// x64 DNS_SERVICE_INSTANCE is 72; x86 is 40.
struct ServiceInstanceLayout {
    u32 size;
    u32 name;
    u32 host;
    u32 ip4;
    u32 ip6;
    u32 port;
    u32 priority;
    u32 weight;
    u32 count;
    u32 keys;
    u32 values;
    u32 ifindex;
    u32 ptr;
};

[[nodiscard]] ServiceInstanceLayout instance_layout(bool is64) {
    if (is64) {
        return {72, 0, 8, 16, 24, 32, 34, 36, 40, 48, 56, 64, 8};
    }
    return {40, 0, 4, 8, 12, 16, 18, 20, 24, 28, 32, 36, 4};
}

[[nodiscard]] bool read_wide(Marshaler& m, GuestAddr ptr, std::vector<u16>& out) {
    out.clear();
    if (ptr == 0) return false;
    for (u32 i = 0; i < 2048; ++i) {
        const auto ch = m.read_u16(ptr + i * 2);
        if (ch.is_err()) return false;
        out.push_back(ch.value());
        if (ch.value() == 0) return true;
    }
    return false;
}

void write_wide_units(std::vector<u8>& blob, u32 at, const std::vector<u16>& units) {
    for (u32 i = 0; i < units.size(); ++i) {
        blob[at + i * 2] = static_cast<u8>(units[i] & 0xFF);
        blob[at + i * 2 + 1] = static_cast<u8>((units[i] >> 8) & 0xFF);
    }
}

void write_u32_at(std::vector<u8>& blob, u32 at, u32 value) {
    blob[at] = static_cast<u8>(value);
    blob[at + 1] = static_cast<u8>(value >> 8);
    blob[at + 2] = static_cast<u8>(value >> 16);
    blob[at + 3] = static_cast<u8>(value >> 24);
}

void write_ptr_at(std::vector<u8>& blob, u32 at, u32 ptr_size, u64 value) {
    for (u32 i = 0; i < ptr_size; ++i) {
        blob[at + i] = static_cast<u8>(value >> (8 * i));
    }
}

[[nodiscard]] u32 align_up(u32 value, u32 align) {
    return (value + align - 1u) & ~(align - 1u);
}

} // namespace

GuestAddr DnsapiManager::build_service_instance(
    Marshaler& m, const std::vector<u16>& name, const std::vector<u16>& host,
    const u8* ip4, const u8* ip6, u16 port, u16 priority, u16 weight,
    const std::vector<std::vector<u16>>& keys,
    const std::vector<std::vector<u16>>& values, u32 interface_index) {
    const bool is64 = m.cpu_is_64bit();
    const ServiceInstanceLayout lay = instance_layout(is64);
    u32 cursor = lay.size;
    const u32 name_at = cursor;
    cursor += static_cast<u32>(name.size() * 2);
    cursor = align_up(cursor, lay.ptr);
    const u32 host_at = cursor;
    cursor += static_cast<u32>(host.size() * 2);
    cursor = align_up(cursor, lay.ptr);
    u32 ip4_at = 0;
    u32 ip6_at = 0;
    if (ip4 != nullptr) {
        ip4_at = cursor;
        cursor += 4;
        cursor = align_up(cursor, lay.ptr);
    }
    if (ip6 != nullptr) {
        ip6_at = cursor;
        cursor += 16;
        cursor = align_up(cursor, lay.ptr);
    }
    const u32 count = static_cast<u32>(keys.size());
    u32 keys_at = 0;
    u32 vals_at = 0;
    if (count != 0) {
        keys_at = cursor;
        cursor += count * lay.ptr;
        vals_at = cursor;
        cursor += count * lay.ptr;
    }
    std::vector<u32> key_at(count);
    std::vector<u32> val_at(count);
    for (u32 i = 0; i < count; ++i) {
        key_at[i] = cursor;
        cursor += static_cast<u32>(keys[i].size() * 2);
        cursor = align_up(cursor, 2);
        val_at[i] = cursor;
        cursor += static_cast<u32>(values[i].size() * 2);
        cursor = align_up(cursor, 2);
    }

    std::vector<u8> blob(cursor, 0);
    write_wide_units(blob, name_at, name);
    write_wide_units(blob, host_at, host);
    if (ip4 != nullptr) {
        for (u32 i = 0; i < 4; ++i) blob[ip4_at + i] = ip4[i];
    }
    if (ip6 != nullptr) {
        for (u32 i = 0; i < 16; ++i) blob[ip6_at + i] = ip6[i];
    }
    const GuestAddr base = alloc_guest(m, cursor);
    if (base == 0) return 0;
    write_ptr_at(blob, lay.name, lay.ptr, base + name_at);
    write_ptr_at(blob, lay.host, lay.ptr, base + host_at);
    if (ip4 != nullptr) write_ptr_at(blob, lay.ip4, lay.ptr, base + ip4_at);
    if (ip6 != nullptr) write_ptr_at(blob, lay.ip6, lay.ptr, base + ip6_at);
    write_u32_at(blob, lay.port, static_cast<u32>(port) |
                                     (static_cast<u32>(priority) << 16));
    write_u32_at(blob, lay.weight, weight);
    write_u32_at(blob, lay.count, count);
    write_u32_at(blob, lay.ifindex, interface_index);
    if (count != 0) {
        write_ptr_at(blob, lay.keys, lay.ptr, base + keys_at);
        write_ptr_at(blob, lay.values, lay.ptr, base + vals_at);
        for (u32 i = 0; i < count; ++i) {
            write_wide_units(blob, key_at[i], keys[i]);
            write_wide_units(blob, val_at[i], values[i]);
            write_ptr_at(blob, keys_at + i * lay.ptr, lay.ptr, base + key_at[i]);
            write_ptr_at(blob, vals_at + i * lay.ptr, lay.ptr, base + val_at[i]);
        }
    }
    if (m.memory().write_bytes(base, blob.data(), blob.size()).is_err()) {
        free_guest(m, base);
        return 0;
    }
    return base;
}

void DnsapiManager::impl_DnsServiceConstructInstance(Marshaler& m) {
    // PDNS_SERVICE_INSTANCE DnsServiceConstructInstance(
    //     PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS,
    //     WORD, WORD, WORD, DWORD, PCWSTR*, PCWSTR*)
    // WinDNS.h. NULL service or host name fails. NULL address pointers omit
    // that address. A property count with a NULL key or value array fails.
    std::vector<u16> name;
    std::vector<u16> host;
    if (!read_wide(m, m.arg_ptr(0), name) || !read_wide(m, m.arg_ptr(1), host)) {
        m.set_return(0);
        return;
    }
    u8 ip4[4]{};
    u8 ip6[16]{};
    const u8* ip4_ptr = nullptr;
    const u8* ip6_ptr = nullptr;
    if (const GuestAddr p4 = m.arg_ptr(2); p4 != 0) {
        if (m.memory().read_bytes(p4, ip4, 4).is_err()) {
            m.set_return(0);
            return;
        }
        ip4_ptr = ip4;
    }
    if (const GuestAddr p6 = m.arg_ptr(3); p6 != 0) {
        if (m.memory().read_bytes(p6, ip6, 16).is_err()) {
            m.set_return(0);
            return;
        }
        ip6_ptr = ip6;
    }
    const u16 port = static_cast<u16>(m.arg(4));
    const u16 priority = static_cast<u16>(m.arg(5));
    const u16 weight = static_cast<u16>(m.arg(6));
    const u32 count = static_cast<u32>(m.arg(7));
    const GuestAddr keys_ptr = m.arg_ptr(8);
    const GuestAddr vals_ptr = m.arg_ptr(9);
    if (count > 64 || (count != 0 && (keys_ptr == 0 || vals_ptr == 0))) {
        m.set_return(0);
        return;
    }
    const u32 ptr = m.cpu_is_64bit() ? 8u : 4u;
    std::vector<std::vector<u16>> keys(count);
    std::vector<std::vector<u16>> values(count);
    for (u32 i = 0; i < count; ++i) {
        const auto key = m.read_ptr(keys_ptr + i * ptr);
        const auto val = m.read_ptr(vals_ptr + i * ptr);
        if (key.is_err() || val.is_err() ||
            !read_wide(m, static_cast<GuestAddr>(key.value()), keys[i]) ||
            !read_wide(m, static_cast<GuestAddr>(val.value()), values[i])) {
            m.set_return(0);
            return;
        }
    }
    m.set_return(build_service_instance(
        m, name, host, ip4_ptr, ip6_ptr, port, priority, weight, keys, values,
        0));
}

void DnsapiManager::impl_DnsServiceCopyInstance(Marshaler& m) {
    const GuestAddr src = m.arg_ptr(0);
    if (src == 0) {
        m.set_return(0);
        return;
    }
    const ServiceInstanceLayout lay = instance_layout(m.cpu_is_64bit());
    const auto name_p = m.read_ptr(src + lay.name);
    const auto host_p = m.read_ptr(src + lay.host);
    const auto ip4_p = m.read_ptr(src + lay.ip4);
    const auto ip6_p = m.read_ptr(src + lay.ip6);
    const auto port = m.read_u16(src + lay.port);
    const auto priority = m.read_u16(src + lay.priority);
    const auto weight = m.read_u16(src + lay.weight);
    const auto count_r = m.read_u32(src + lay.count);
    const auto ifx = m.read_u32(src + lay.ifindex);
    if (name_p.is_err() || host_p.is_err() || ip4_p.is_err() || ip6_p.is_err() ||
        port.is_err() || priority.is_err() || weight.is_err() ||
        count_r.is_err() || ifx.is_err()) {
        m.set_return(0);
        return;
    }
    std::vector<u16> name;
    std::vector<u16> host;
    if (!read_wide(m, static_cast<GuestAddr>(name_p.value()), name) ||
        !read_wide(m, static_cast<GuestAddr>(host_p.value()), host)) {
        m.set_return(0);
        return;
    }
    u8 ip4[4]{};
    u8 ip6[16]{};
    const u8* ip4_ptr = nullptr;
    const u8* ip6_ptr = nullptr;
    if (ip4_p.value() != 0) {
        if (m.memory().read_bytes(static_cast<GuestAddr>(ip4_p.value()), ip4, 4)
                .is_err()) {
            m.set_return(0);
            return;
        }
        ip4_ptr = ip4;
    }
    if (ip6_p.value() != 0) {
        if (m.memory().read_bytes(static_cast<GuestAddr>(ip6_p.value()), ip6, 16)
                .is_err()) {
            m.set_return(0);
            return;
        }
        ip6_ptr = ip6;
    }
    const u32 count = count_r.value();
    if (count > 64) {
        m.set_return(0);
        return;
    }
    std::vector<std::vector<u16>> keys(count);
    std::vector<std::vector<u16>> values(count);
    if (count != 0) {
        const auto keys_p = m.read_ptr(src + lay.keys);
        const auto vals_p = m.read_ptr(src + lay.values);
        if (keys_p.is_err() || vals_p.is_err() || keys_p.value() == 0 ||
            vals_p.value() == 0) {
            m.set_return(0);
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            const auto key = m.read_ptr(static_cast<GuestAddr>(keys_p.value()) +
                                        i * lay.ptr);
            const auto val = m.read_ptr(static_cast<GuestAddr>(vals_p.value()) +
                                        i * lay.ptr);
            if (key.is_err() || val.is_err() ||
                !read_wide(m, static_cast<GuestAddr>(key.value()), keys[i]) ||
                !read_wide(m, static_cast<GuestAddr>(val.value()), values[i])) {
                m.set_return(0);
                return;
            }
        }
    }
    m.set_return(build_service_instance(
        m, name, host, ip4_ptr, ip6_ptr, port.value(), priority.value(),
        weight.value(), keys, values, ifx.value()));
}

void DnsapiManager::impl_DnsServiceFreeInstance(Marshaler& m) {
    // VOID DnsServiceFreeInstance. NULL is a no-op. Only an instance this
    // manager allocated is released.
    free_guest(m, m.arg_ptr(0));
}

namespace {

constexpr u32 DNS_ERROR_NOT_SUPPORTED = 9555;
constexpr u32 DNS_QUERY_REQUEST_VERSION1 = 1;
constexpr u32 DNS_QUERY_REQUEST_VERSION2 = 2;

void log_dns_unimplemented(const char* api) {
    static std::atomic_flag browse = ATOMIC_FLAG_INIT;
    static std::atomic_flag resolve = ATOMIC_FLAG_INIT;
    static std::atomic_flag reg = ATOMIC_FLAG_INIT;
    static std::atomic_flag dereg = ATOMIC_FLAG_INIT;
    std::atomic_flag* flag = &browse;
    if (std::strcmp(api, "DnsServiceResolve") == 0) flag = &resolve;
    else if (std::strcmp(api, "DnsServiceRegister") == 0) flag = &reg;
    else if (std::strcmp(api, "DnsServiceDeRegister") == 0) flag = &dereg;
    if (!flag->test_and_set()) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dnsapi!%s: "
                             "DNS_ERROR_NOT_SUPPORTED\n", api);
    }
}

[[nodiscard]] bool browse_request_ok(Marshaler& m, GuestAddr request) {
    if (request == 0) return false;
    const auto version = m.read_u32(request);
    const bool is64 = m.cpu_is_64bit();
    // DNS_SERVICE_BROWSE_REQUEST: x64 size 32, x86 size 20.
    const u32 query_at = is64 ? 8u : 8u;
    const u32 callback_at = is64 ? 16u : 12u;
    if (version.is_err()) return false;
    if (version.value() != DNS_QUERY_REQUEST_VERSION1 &&
        version.value() != DNS_QUERY_REQUEST_VERSION2) {
        return false;
    }
    const auto query = m.read_ptr(request + query_at);
    const auto callback = m.read_ptr(request + callback_at);
    return query.is_ok() && callback.is_ok() && query.value() != 0 &&
           callback.value() != 0;
}

} // namespace

namespace {

constexpr u32 DNS_REQUEST_PENDING = 9506;
constexpr u32 ERROR_SUCCESS_DNS = 0;

[[nodiscard]] std::string wide_to_utf8(const std::vector<u16>& units) {
    std::string out;
    for (u16 ch : units) {
        if (ch == 0) break;
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

[[nodiscard]] std::vector<u16> utf8_to_wide(std::string_view text) {
    std::vector<u16> out;
    for (unsigned char ch : text) {
        out.push_back(ch < 0x80 ? ch : static_cast<u16>('?'));
    }
    out.push_back(0);
    return out;
}

// "Instance._service._tcp.local" -> name, "_service._tcp", "local".
[[nodiscard]] bool split_dns_sd_name(const std::string& full, std::string& name,
                                     std::string& regtype, std::string& domain) {
    std::string trimmed = full;
    if (!trimmed.empty() && trimmed.back() == '.') trimmed.pop_back();
    const auto proto = trimmed.rfind("._tcp");
    const auto proto_udp = trimmed.rfind("._udp");
    const auto at = proto == std::string::npos ? proto_udp : proto;
    if (at == std::string::npos || at == 0) return false;
    const bool tcp = proto != std::string::npos &&
                     (proto_udp == std::string::npos || proto > proto_udp);
    const std::string suffix = tcp ? "._tcp" : "._udp";
    if (trimmed.compare(at, suffix.size(), suffix) != 0) return false;
    name = trimmed.substr(0, at);
    const auto svc = name.rfind('.');
    if (svc == std::string::npos || svc == 0 || name[svc + 1] != '_') return false;
    regtype = name.substr(svc + 1) + suffix;
    name.resize(svc);
    std::string rest = trimmed.substr(at + suffix.size());
    if (!rest.empty() && rest.front() == '.') rest.erase(rest.begin());
    domain = rest.empty() ? "local" : rest;
    return !name.empty();
}

struct ResolveReply {
    bool got = false;
    std::string host;
    u16 port = 0;
    std::vector<std::pair<std::string, std::string>> txt;
    u32 interface_index = 0;
};

void dnssd_resolve_reply(DNSServiceRef, DNSServiceFlags, uint32_t interface_index,
                         DNSServiceErrorType error, const char*, const char* host,
                         uint16_t port, uint16_t txt_len, const unsigned char* txt,
                         void* context) {
    auto* reply = static_cast<ResolveReply*>(context);
    if (error != kDNSServiceErr_NoError || host == nullptr) return;
    reply->got = true;
    reply->host = host;
    reply->port = ntohs(port);
    reply->interface_index = interface_index;
    u32 i = 0;
    while (i < txt_len) {
        const u32 len = txt[i++];
        if (i + len > txt_len) break;
        std::string pair(reinterpret_cast<const char*>(txt + i), len);
        i += len;
        const auto eq = pair.find('=');
        if (eq == std::string::npos) reply->txt.emplace_back(pair, "");
        else reply->txt.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
    }
}

[[nodiscard]] bool split_browse_type(const std::string& full, std::string& regtype,
                                    std::string& domain) {
    std::string trimmed = full;
    if (!trimmed.empty() && trimmed.back() == '.') trimmed.pop_back();
    const auto tcp = trimmed.rfind("._tcp");
    const auto udp = trimmed.rfind("._udp");
    const auto at = tcp == std::string::npos ? udp : tcp;
    if (at == std::string::npos || trimmed.empty() || trimmed[0] != '_') return false;
    const bool is_tcp = tcp != std::string::npos &&
                        (udp == std::string::npos || tcp > udp);
    const std::string suffix = is_tcp ? "._tcp" : "._udp";
    if (trimmed.compare(at, suffix.size(), suffix) != 0) return false;
    regtype = trimmed.substr(0, at + suffix.size());
    std::string rest = trimmed.substr(at + suffix.size());
    if (!rest.empty() && rest.front() == '.') rest.erase(rest.begin());
    domain = rest.empty() ? "local" : rest;
    return true;
}

struct BrowseCollect {
    std::vector<std::string> added;
};

void dnssd_browse_reply(DNSServiceRef, DNSServiceFlags flags, uint32_t,
                        DNSServiceErrorType error, const char* name,
                        const char* regtype, const char* domain, void* context) {
    if (error != kDNSServiceErr_NoError || name == nullptr || regtype == nullptr)
        return;
    if ((flags & kDNSServiceFlagsAdd) == 0) return;
    auto* collect = static_cast<BrowseCollect*>(context);
    std::string full = name;
    full.push_back('.');
    full += regtype;
    if (domain != nullptr && domain[0] != '\0') {
        full.push_back('.');
        full += domain;
    }
    collect->added.push_back(std::move(full));
}

struct RegisterCollect {
    bool done = false;
    DNSServiceErrorType error = kDNSServiceErr_NoError;
};

void dnssd_register_reply(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType error,
                          const char*, const char*, const char*, void* context) {
    auto* collect = static_cast<RegisterCollect*>(context);
    collect->done = true;
    collect->error = error;
}

void pump_mdns(DNSServiceRef ref, int timeout_ms) {
    if (ref == nullptr) return;
    pollfd pfd{};
    pfd.fd = DNSServiceRefSockFD(ref);
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) > 0) (void)DNSServiceProcessResult(ref);
}

} // namespace

DnsapiManager::~DnsapiManager() {
    for (auto& entry : m_live) {
        if (entry.second.ref != nullptr) {
            DNSServiceRefDeallocate(static_cast<DNSServiceRef>(entry.second.ref));
        }
    }
}

void DnsapiManager::impl_DnsServiceBrowse(Marshaler& m) {
    // DNS_STATUS DnsServiceBrowse. Success is DNS_REQUEST_PENDING. The browse
    // callback receives ERROR_SUCCESS and a PTR DNS_RECORD chain, or
    // DNS_INFO_NO_RECORDS. Version 2 uses a different completion routine.
    const GuestAddr request = m.arg_ptr(0);
    const GuestAddr cancel = m.arg_ptr(1);
    if (request == 0 || cancel == 0 || !m_guest_call) {
        if (request != 0 && cancel != 0 && !m_guest_call) {
            log_dns_unimplemented("DnsServiceBrowse");
            m.set_return(DNS_ERROR_NOT_SUPPORTED);
            return;
        }
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto version = m.read_u32(request);
    if (version.is_err() || version.value() != DNS_QUERY_REQUEST_VERSION1) {
        if (version.is_ok() && version.value() == DNS_QUERY_REQUEST_VERSION2) {
            log_dns_unimplemented("DnsServiceBrowse");
            m.set_return(DNS_ERROR_NOT_SUPPORTED);
            return;
        }
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const bool is64 = m.cpu_is_64bit();
    const u32 callback_at = is64 ? 16u : 12u;
    const u32 context_at = is64 ? 24u : 16u;
    const auto query_p = m.read_ptr(request + 8);
    const auto callback_p = m.read_ptr(request + callback_at);
    const auto context_p = m.read_ptr(request + context_at);
    const auto ifindex = m.read_u32(request + 4);
    if (query_p.is_err() || callback_p.is_err() || context_p.is_err() ||
        ifindex.is_err() || query_p.value() == 0 || callback_p.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::vector<u16> query_units;
    if (!read_wide(m, static_cast<GuestAddr>(query_p.value()), query_units)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::string regtype;
    std::string domain;
    if (!split_browse_type(wide_to_utf8(query_units), regtype, domain)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    BrowseCollect found;
    DNSServiceRef ref = nullptr;
    const DNSServiceErrorType started = DNSServiceBrowse(
        &ref, 0, ifindex.value(), regtype.c_str(), domain.c_str(),
        dnssd_browse_reply, &found);
    if (started != kDNSServiceErr_NoError || ref == nullptr) {
        m.set_return(DNS_ERROR_NOT_SUPPORTED);
        return;
    }
    pump_mdns(ref, 300);
    u64 handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        handle = m_next_cancel++;
        if (m_next_cancel == 0) m_next_cancel = 1;
        m_live[handle] = LiveQuery{ref, 0};
    }
    if (m.write_ptr(cancel, handle).is_err()) {
        DNSServiceRefDeallocate(ref);
        std::lock_guard<std::mutex> lock(m_mu);
        m_live.erase(handle);
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    GuestAddr head = 0;
    u32 status = DNS_INFO_NO_RECORDS;
    if (!found.added.empty()) {
        const u32 rec_size = is64 ? 40u : 28u;
        const u32 data_at = is64 ? 32u : 24u;
        const u32 ptr = is64 ? 8u : 4u;
        const auto owner = utf8_to_wide(wide_to_utf8(query_units));
        std::vector<std::vector<u16>> targets;
        u32 bytes = rec_size * static_cast<u32>(found.added.size());
        bytes += static_cast<u32>(owner.size() * 2);
        for (const auto& name : found.added) {
            targets.push_back(utf8_to_wide(name));
            bytes += static_cast<u32>(targets.back().size() * 2);
        }
        head = alloc_guest(m, bytes);
        if (head == 0) {
            status = ERROR_OUTOFMEMORY;
        } else {
            std::vector<u8> blob(bytes, 0);
            u32 cursor = rec_size * static_cast<u32>(found.added.size());
            const u32 owner_at = cursor;
            for (u32 i = 0; i < owner.size(); ++i) {
                blob[cursor++] = static_cast<u8>(owner[i]);
                blob[cursor++] = static_cast<u8>(owner[i] >> 8);
            }
            std::vector<u32> target_at;
            for (const auto& target : targets) {
                target_at.push_back(cursor);
                for (u16 ch : target) {
                    blob[cursor++] = static_cast<u8>(ch);
                    blob[cursor++] = static_cast<u8>(ch >> 8);
                }
            }
            for (u32 i = 0; i < targets.size(); ++i) {
                const u32 at = i * rec_size;
                if (i + 1 < targets.size()) {
                    for (u32 b = 0; b < ptr; ++b)
                        blob[at + b] = static_cast<u8>((head + (i + 1) * rec_size) >> (8 * b));
                }
                for (u32 b = 0; b < ptr; ++b)
                    blob[at + ptr + b] = static_cast<u8>((head + owner_at) >> (8 * b));
                blob[at + ptr * 2] = 12; // DNS_TYPE_PTR
                const u32 name_bytes = static_cast<u32>(targets[i].size() * 2);
                blob[at + ptr * 2 + 2] = static_cast<u8>(name_bytes);
                blob[at + ptr * 2 + 3] = static_cast<u8>(name_bytes >> 8);
                for (u32 b = 0; b < ptr; ++b)
                    blob[at + data_at + b] =
                        static_cast<u8>((head + target_at[i]) >> (8 * b));
            }
            if (m.memory().write_bytes(head, blob.data(), blob.size()).is_err()) {
                free_guest(m, head);
                head = 0;
                status = ERROR_OUTOFMEMORY;
            } else {
                status = ERROR_SUCCESS_DNS;
            }
        }
    }
    const u64 cb_args[3] = {status, context_p.value(), head};
    (void)m_guest_call(static_cast<GuestAddr>(callback_p.value()), cb_args, 3, is64);
    m.set_return(DNS_REQUEST_PENDING);
}

void DnsapiManager::impl_DnsServiceBrowseCancel(Marshaler& m) {
    const GuestAddr cancel = m.arg_ptr(0);
    if (cancel == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto handle = m.read_ptr(cancel);
    if (handle.is_err() || handle.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    DNSServiceRef ref = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        const auto it = m_live.find(handle.value());
        if (it == m_live.end()) {
            m.set_return(ERROR_INVALID_PARAMETER);
            return;
        }
        ref = static_cast<DNSServiceRef>(it->second.ref);
        m_live.erase(it);
    }
    if (ref != nullptr) DNSServiceRefDeallocate(ref);
    m.set_return(ERROR_SUCCESS_DNS);
}

void DnsapiManager::impl_DnsServiceResolve(Marshaler& m) {
    // DNS_STATUS DnsServiceResolve(PDNS_SERVICE_RESOLVE_REQUEST, PDNS_SERVICE_CANCEL)
    // Learn: asynchronous. Success is DNS_REQUEST_PENDING; the completion
    // callback then receives ERROR_SUCCESS and a DNS_SERVICE_INSTANCE the
    // caller frees with DnsServiceFreeInstance. pCancel is required.
    // DNS_SERVICE_RESOLVE_REQUEST matches the browse request layout measured
    // on SDK 10.0.26100.0 (x64 32 bytes, x86 20).
    const GuestAddr request = m.arg_ptr(0);
    const GuestAddr cancel = m.arg_ptr(1);
    if (request == 0 || cancel == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto version = m.read_u32(request);
    const bool is64 = m.cpu_is_64bit();
    const u32 query_at = 8u;
    const u32 callback_at = is64 ? 16u : 12u;
    const u32 context_at = is64 ? 24u : 16u;
    if (version.is_err() || version.value() != DNS_QUERY_REQUEST_VERSION1) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto query_p = m.read_ptr(request + query_at);
    const auto callback_p = m.read_ptr(request + callback_at);
    const auto context_p = m.read_ptr(request + context_at);
    const auto ifindex = m.read_u32(request + 4);
    if (query_p.is_err() || callback_p.is_err() || context_p.is_err() ||
        ifindex.is_err() || query_p.value() == 0 || callback_p.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (!m_guest_call) {
        log_dns_unimplemented("DnsServiceResolve");
        m.set_return(DNS_ERROR_NOT_SUPPORTED);
        return;
    }
    std::vector<u16> query_units;
    if (!read_wide(m, static_cast<GuestAddr>(query_p.value()), query_units)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const std::string query = wide_to_utf8(query_units);
    u64 handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        handle = m_next_cancel++;
        if (m_next_cancel == 0) m_next_cancel = 1;
        m_resolve_cancels.insert(handle);
    }
    if (m.write_ptr(cancel, handle).is_err()) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_resolve_cancels.erase(handle);
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }

    std::string svc_name;
    std::string regtype;
    std::string domain;
    std::string host = query;
    u16 port = 0;
    u32 iface = ifindex.value();
    std::vector<std::pair<std::string, std::string>> txt;
    bool resolved = false;
    if (split_dns_sd_name(query, svc_name, regtype, domain)) {
        ResolveReply reply;
        DNSServiceRef ref = nullptr;
        const DNSServiceErrorType started = DNSServiceResolve(
            &ref, 0, ifindex.value(), svc_name.c_str(), regtype.c_str(),
            domain.c_str(), dnssd_resolve_reply, &reply);
        if (started == kDNSServiceErr_NoError && ref != nullptr) {
            pollfd pfd{};
            pfd.fd = DNSServiceRefSockFD(ref);
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 1500) > 0) (void)DNSServiceProcessResult(ref);
            DNSServiceRefDeallocate(ref);
            if (reply.got) {
                host = reply.host;
                port = reply.port;
                iface = reply.interface_index;
                txt = std::move(reply.txt);
                resolved = true;
            }
        }
    }

    u8 ip4[4]{};
    const u8* ip4_ptr = nullptr;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
        for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_family != AF_INET || ai->ai_addr == nullptr) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
            std::memcpy(ip4, &sin->sin_addr, 4);
            ip4_ptr = ip4;
            resolved = true;
            break;
        }
        ::freeaddrinfo(res);
    }

    GuestAddr instance = 0;
    u32 status = DNS_INFO_NO_RECORDS;
    if (resolved) {
        std::vector<std::vector<u16>> keys;
        std::vector<std::vector<u16>> values;
        keys.reserve(txt.size());
        values.reserve(txt.size());
        for (const auto& pair : txt) {
            keys.push_back(utf8_to_wide(pair.first));
            values.push_back(utf8_to_wide(pair.second));
        }
        instance = build_service_instance(
            m, query_units, utf8_to_wide(host), ip4_ptr, nullptr, port, 0, 0,
            keys, values, iface);
        status = instance != 0 ? ERROR_SUCCESS_DNS : ERROR_OUTOFMEMORY;
    }
    const u64 cb_args[3] = {status, context_p.value(), instance};
    (void)m_guest_call(static_cast<GuestAddr>(callback_p.value()), cb_args, 3, is64);
    m.set_return(DNS_REQUEST_PENDING);
}

void DnsapiManager::impl_DnsServiceResolveCancel(Marshaler& m) {
    const GuestAddr cancel = m.arg_ptr(0);
    if (cancel == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto handle = m.read_ptr(cancel);
    if (handle.is_err() || handle.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_resolve_cancels.erase(handle.value()) == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(ERROR_SUCCESS_DNS);
}

void DnsapiManager::impl_DnsServiceRegister(Marshaler& m) {
    // DWORD DnsServiceRegister(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL)
    // x64 request is 48 bytes; x86 is 28. Version 1, instance and callback required.
    const GuestAddr request = m.arg_ptr(0);
    if (request == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto version = m.read_u32(request);
    const bool is64 = m.cpu_is_64bit();
    const u32 inst_at = is64 ? 8u : 8u;
    const u32 cb_at = is64 ? 16u : 12u;
    if (version.is_err() || version.value() != DNS_QUERY_REQUEST_VERSION1) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto inst = m.read_ptr(request + inst_at);
    const auto cb = m.read_ptr(request + cb_at);
    if (inst.is_err() || cb.is_err() || inst.value() == 0 || cb.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (!m_guest_call) {
        log_dns_unimplemented("DnsServiceRegister");
        m.set_return(DNS_ERROR_NOT_SUPPORTED);
        return;
    }
    const ServiceInstanceLayout lay = instance_layout(is64);
    const GuestAddr instance = static_cast<GuestAddr>(inst.value());
    const auto name_p = m.read_ptr(instance + lay.name);
    const auto host_p = m.read_ptr(instance + lay.host);
    const auto port_r = m.read_u16(instance + lay.port);
    const auto count_r = m.read_u32(instance + lay.count);
    const auto if_r = m.read_u32(instance + lay.ifindex);
    if (name_p.is_err() || host_p.is_err() || port_r.is_err() || count_r.is_err() ||
        if_r.is_err() || name_p.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::vector<u16> name_units;
    std::vector<u16> host_units;
    if (!read_wide(m, static_cast<GuestAddr>(name_p.value()), name_units) ||
        (host_p.value() != 0 &&
         !read_wide(m, static_cast<GuestAddr>(host_p.value()), host_units))) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::string svc;
    std::string regtype;
    std::string domain;
    if (!split_dns_sd_name(wide_to_utf8(name_units), svc, regtype, domain)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::string txt;
    const u32 count = count_r.value();
    if (count > 64) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (count != 0) {
        const auto keys_p = m.read_ptr(instance + lay.keys);
        const auto vals_p = m.read_ptr(instance + lay.values);
        if (keys_p.is_err() || vals_p.is_err() || keys_p.value() == 0 ||
            vals_p.value() == 0) {
            m.set_return(ERROR_INVALID_PARAMETER);
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            const auto key = m.read_ptr(static_cast<GuestAddr>(keys_p.value()) + i * lay.ptr);
            const auto val = m.read_ptr(static_cast<GuestAddr>(vals_p.value()) + i * lay.ptr);
            std::vector<u16> key_u;
            std::vector<u16> val_u;
            if (key.is_err() || val.is_err() ||
                !read_wide(m, static_cast<GuestAddr>(key.value()), key_u) ||
                !read_wide(m, static_cast<GuestAddr>(val.value()), val_u)) {
                m.set_return(ERROR_INVALID_PARAMETER);
                return;
            }
            std::string item = wide_to_utf8(key_u);
            item.push_back('=');
            item += wide_to_utf8(val_u);
            if (item.size() > 255) {
                m.set_return(ERROR_INVALID_PARAMETER);
                return;
            }
            txt.push_back(static_cast<char>(item.size()));
            txt += item;
        }
    }
    const GuestAddr cancel = m.arg_ptr(1);
    const auto context_p = m.read_ptr(request + (is64 ? 24u : 16u));
    if (context_p.is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    RegisterCollect reply;
    DNSServiceRef ref = nullptr;
    const std::string host = host_units.empty() ? std::string() : wide_to_utf8(host_units);
    const DNSServiceErrorType started = DNSServiceRegister(
        &ref, 0, if_r.value(), svc.c_str(), regtype.c_str(), domain.c_str(),
        host.empty() ? nullptr : host.c_str(), htons(port_r.value()),
        static_cast<uint16_t>(txt.size()), txt.empty() ? nullptr : txt.data(),
        dnssd_register_reply, &reply);
    if (started != kDNSServiceErr_NoError || ref == nullptr) {
        m.set_return(DNS_ERROR_NOT_SUPPORTED);
        return;
    }
    pump_mdns(ref, 300);
    u64 handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        handle = m_next_cancel++;
        if (m_next_cancel == 0) m_next_cancel = 1;
        m_live[handle] = LiveQuery{ref, instance};
    }
    if (cancel != 0 && m.write_ptr(cancel, handle).is_err()) {
        DNSServiceRefDeallocate(ref);
        std::lock_guard<std::mutex> lock(m_mu);
        m_live.erase(handle);
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const u32 status = (!reply.done || reply.error == kDNSServiceErr_NoError)
                           ? ERROR_SUCCESS_DNS
                           : DNS_ERROR_NOT_SUPPORTED;
    const u64 cb_args[3] = {status, context_p.value(), instance};
    (void)m_guest_call(static_cast<GuestAddr>(cb.value()), cb_args, 3, is64);
    m.set_return(DNS_REQUEST_PENDING);
}

void DnsapiManager::impl_DnsServiceDeRegister(Marshaler& m) {
    const GuestAddr request = m.arg_ptr(0);
    if (request == 0 || !m_guest_call) {
        if (request != 0 && !m_guest_call) {
            log_dns_unimplemented("DnsServiceDeRegister");
            m.set_return(DNS_ERROR_NOT_SUPPORTED);
            return;
        }
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const bool is64 = m.cpu_is_64bit();
    const auto version = m.read_u32(request);
    const auto inst = m.read_ptr(request + 8);
    const auto cb = m.read_ptr(request + (is64 ? 16u : 12u));
    const auto context_p = m.read_ptr(request + (is64 ? 24u : 16u));
    if (version.is_err() || version.value() != DNS_QUERY_REQUEST_VERSION1 ||
        inst.is_err() || cb.is_err() || context_p.is_err() || inst.value() == 0 ||
        cb.value() == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    DNSServiceRef ref = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        for (auto it = m_live.begin(); it != m_live.end(); ++it) {
            if (it->second.instance == static_cast<GuestAddr>(inst.value())) {
                ref = static_cast<DNSServiceRef>(it->second.ref);
                m_live.erase(it);
                break;
            }
        }
    }
    if (ref == nullptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    DNSServiceRefDeallocate(ref);
    const u64 cb_args[3] = {ERROR_SUCCESS_DNS, context_p.value(), inst.value()};
    (void)m_guest_call(static_cast<GuestAddr>(cb.value()), cb_args, 3, is64);
    m.set_return(DNS_REQUEST_PENDING);
}

void DnsapiManager::impl_DnsServiceRegisterCancel(Marshaler& m) {
    impl_DnsServiceBrowseCancel(m);
}

DllExports create_dnsapi_thunks(DnsapiManager& mgr) {
    DllExports exports;
    exports.entries.reserve(4);
    exports.entries.push_back(ExportEntry{
        "DnsQuery_W", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsQuery_W(m); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "DnsRecordListFree", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsRecordListFree(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DnsFree", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsFree(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceConstructInstance", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceConstructInstance(m); },
        40
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceCopyInstance", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceCopyInstance(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceFreeInstance", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceFreeInstance(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceBrowse", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceBrowse(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceBrowseCancel", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceBrowseCancel(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceResolve", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceResolve(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceResolveCancel", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceResolveCancel(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceRegister", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceRegister(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceDeRegister", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceDeRegister(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DnsServiceRegisterCancel", 0,
        [&mgr](Marshaler& m) { mgr.impl_DnsServiceRegisterCancel(m); },
        4
    });
    return exports;
}

} // namespace vitreo::winapi
