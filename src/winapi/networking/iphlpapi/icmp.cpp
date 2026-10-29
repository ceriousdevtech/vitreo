/// @file icmp.cpp
/// @brief IcmpCreateFile / IcmpCloseHandle / IcmpSendEcho over host ICMP datagram sockets.

#include "iphlpapi_internal.hpp"
#include "iphlpapi_state.hpp"

namespace vitreo::winapi {

namespace {
u32 icmp_host_error(int error) {
    switch (error) {
    case EAFNOSUPPORT: case EPROTONOSUPPORT: return ERROR_NOT_SUPPORTED;
    case ENOMEM: case ENOBUFS: case EMFILE: case ENFILE: return error_codes::ERROR_NOT_ENOUGH_MEMORY;
    case ENETUNREACH: return kIP_DEST_NET_UNREACHABLE;
    case EHOSTUNREACH: return kIP_DEST_HOST_UNREACHABLE;
    case EMSGSIZE: return kIP_PACKET_TOO_BIG;
    default: return kIP_GENERAL_FAILURE;
    }
}
u16 icmp_checksum(const u8* bytes, usize size) {
    u32 sum = 0;
    for (usize i = 0; i < size; i += 2) {
        sum += static_cast<u32>(bytes[i]) << 8;
        if (i + 1 < size) sum += bytes[i + 1];
    }
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return static_cast<u16>(~sum);
}
void icmp_unsupported(const char* reason) {
    static std::mutex mutex;
    static std::unordered_set<std::string> reported;
    std::lock_guard lock(mutex);
    if (reported.insert(reason).second)
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!IcmpSendEcho: %s\n", reason);
}
}

void IphlpapiManager::impl_IcmpCreateFile(Marshaler& m) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        winapi::set_last_error(icmp_host_error(errno));
        m.set_return(m.cpu_is_64bit() ? ~u64{0} : u64{0xffffffffu});
        return;
    }
    auto open = std::make_shared<IcmpState::Open>();
    open->socket = fd;
    std::lock_guard lock(m_icmp->mutex);
    const u64 handle = m_icmp->next++;
    m_icmp->handles.emplace(handle, std::move(open));
    winapi::set_last_error(NO_ERROR);
    m.set_return(handle);
}
void IphlpapiManager::impl_IcmpCloseHandle(Marshaler& m) {
    std::shared_ptr<IcmpState::Open> open;
    {
        std::lock_guard lock(m_icmp->mutex);
        const auto found = m_icmp->handles.find(m.arg(0));
        if (found == m_icmp->handles.end()) {
            winapi::set_last_error(ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        open = found->second;
        m_icmp->handles.erase(found);
    }
    // Windows waits for synchronous calls already issued on this handle;
    // closing does not manufacture aborted echo results (oracle evidence).
    std::unique_lock lock(open->mutex);
    open->idle.wait(lock, [&] { return open->active == 0; });
    lock.unlock();
    open.reset();
    m.set_return(1); // Successful close preserves last error.
}
void IphlpapiManager::impl_IcmpSendEcho(Marshaler& m) {
    auto fail = [&](u32 error) { winapi::set_last_error(error); m.set_return(0); };
    std::shared_ptr<IcmpState::Open> open;
    {
        std::lock_guard lock(m_icmp->mutex);
        const auto found = m_icmp->handles.find(m.arg(0));
        if (found == m_icmp->handles.end()) { fail(ERROR_INVALID_PARAMETER); return; }
        open = found->second;
        std::lock_guard operation_lock(open->mutex);
        ++open->active;
    }
    struct Operation {
        std::shared_ptr<IcmpState::Open> open;
        ~Operation() { std::lock_guard lock(open->mutex); --open->active; open->idle.notify_all(); }
    } operation{open};
    const bool wide = m.cpu_is_64bit();
    const u32 destination = static_cast<u32>(m.arg(1));
    const GuestAddr request_data = m.arg_ptr(2);
    const u16 request_size = static_cast<u16>(m.arg(3));
    const GuestAddr options = m.arg_ptr(4), reply = m.arg_ptr(5);
    const u32 reply_size = static_cast<u32>(m.arg(6)), timeout = static_cast<u32>(m.arg(7));
    const u32 header_size = wide ? kIcmpReplySize64 : kIcmpReplySize32;
    const u32 options_offset = wide ? kIcmpOptionsOffset64 : kIcmpOptionsOffset32;
    if (!reply || reply_size < header_size || !m.memory().guest_can_write(reply, reply_size)) {
        fail(ERROR_INVALID_PARAMETER); return;
    }
    std::vector<u8> packet(8u + request_size, 0);
    if (request_size && (!request_data || !m.memory().read_bytes(
            request_data, packet.data() + 8, request_size).is_ok())) {
        fail(error_codes::ERROR_NOACCESS); return;
    }
    std::array<u8, 16> option_header{};
    std::vector<u8> option_data;
    if (options) {
        const usize bytes = wide ? kIcmpOptionSize64 : kIcmpOptionSize32;
        if (!m.memory().read_bytes(options, option_header.data(), bytes).is_ok()) {
            fail(error_codes::ERROR_NOACCESS); return;
        }
        if (option_header[3]) {
            // Oracle confirms native pointer width, including +8 on x64,
            // despite Learn's confusing reference to INFORMATION32.
            const GuestAddr data = wide ? m.memory().read_u64(options + 8).value_or(0)
                                         : m.memory().read_u32(options + 4).value_or(0);
            option_data.resize(option_header[3]);
            if (!data || !m.memory().read_bytes(data, option_data.data(), option_data.size()).is_ok()) {
                fail(error_codes::ERROR_NOACCESS); return;
            }
        }
    }
    auto reply_failure = [&](u32 status) {
        std::array<u8, kIcmpReplySize64> zeros{};
        (void)m.memory().write_bytes(reply, zeros.data(), header_size);
        (void)m.write_u32(reply + 4, status);
        fail(status);
    };
    if (reply_size < header_size + request_size || option_data.size() > 40) {
        reply_failure(kIP_GENERAL_FAILURE); return;
    }
    if (options && option_header[0] == 0) {
        // Windows emits TTL zero (oracle observes a local TTL-expired reply).
        // Darwin's unprivileged IP_TTL socket option rejects zero; substituting
        // the default TTL would silently send a materially different packet.
        icmp_unsupported("zero TTL requires a raw IPv4 header transport");
        fail(ERROR_NOT_SUPPORTED); return;
    }
    if (options && (option_header[2] & kIP_FLAG_REVERSE)) {
        icmp_unsupported("IP_FLAG_REVERSE source route construction");
        fail(ERROR_NOT_SUPPORTED); return;
    }
    // Source routing requires Windows' first-hop rewrite, not blindly passing
    // bytes to a different host IP stack. Reject it until that contract exists.
    for (usize i = 0; i < option_data.size();) {
        const u8 type = option_data[i];
        if (type == IPOPT_EOL) break;
        if (type == IPOPT_NOP) { ++i; continue; }
        if (type == IPOPT_LSRR || type == IPOPT_SSRR) {
            icmp_unsupported("IPv4 loose/strict source-route option rewrite");
            fail(ERROR_NOT_SUPPORTED); return;
        }
        if (i + 1 >= option_data.size() || option_data[i + 1] < 2 ||
            option_data[i + 1] > option_data.size() - i) {
            reply_failure(kIP_GENERAL_FAILURE); return;
        }
        i += option_data[i + 1];
    }
    bool multiple_replies = (ntohl(destination) & 0xf0000000u) == 0xe0000000u ||
                            destination == 0xffffffffu;
    ifaddrs* interfaces = nullptr;
    if (!multiple_replies && ::getifaddrs(&interfaces) == 0) {
        for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
            if (!(interface->ifa_flags & IFF_BROADCAST) || !interface->ifa_broadaddr ||
                interface->ifa_broadaddr->sa_family != AF_INET) continue;
            const auto* broadcast = reinterpret_cast<const sockaddr_in*>(interface->ifa_broadaddr);
            if (broadcast->sin_addr.s_addr == destination) { multiple_replies = true; break; }
        }
        ::freeifaddrs(interfaces);
    }
    if (multiple_replies) {
        icmp_unsupported("multicast/broadcast multiple-reply collection");
        fail(ERROR_NOT_SUPPORTED); return;
    }
    // RFC 791 pads the options area to a 32-bit boundary with end-of-list
    // zeros. Windows accepts a one-byte EOL option; Darwin requires padding.
    if (!option_data.empty()) option_data.resize((option_data.size() + 3u) & ~usize{3}, 0);
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) { reply_failure(icmp_host_error(errno)); return; }
    struct Socket { int fd; ~Socket() { ::close(fd); } } socket_owner{fd};
    // Each synchronous operation owns its socket so concurrent sends on a
    // handle cannot steal one another's replies or mutate each other's options.
    const int ttl = options ? option_header[0] : 128;
    const int df = options && (option_header[2] & kIP_FLAG_DF) ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0 ||
        ::setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &df, sizeof(df)) != 0 ||
        (!option_data.empty() && ::setsockopt(fd, IPPROTO_IP, IP_OPTIONS,
          option_data.data(), static_cast<socklen_t>(option_data.size())) != 0)) {
        reply_failure(icmp_host_error(errno)); return;
    }
    // TOS is explicitly ignored by Windows (IP_OPTION_INFORMATION Learn).
    static std::atomic<u32> next_sequence{1};
    const u32 sequence = next_sequence.fetch_add(1);
    packet[0] = ICMP_ECHO;
    packet[4] = static_cast<u8>(sequence >> 24); packet[5] = static_cast<u8>(sequence >> 16);
    packet[6] = static_cast<u8>(sequence >> 8); packet[7] = static_cast<u8>(sequence);
    const u16 checksum = icmp_checksum(packet.data(), packet.size());
    packet[2] = static_cast<u8>(checksum >> 8); packet[3] = static_cast<u8>(checksum);
    sockaddr_in address{};
#ifdef __APPLE__
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET; address.sin_addr.s_addr = destination;
    const auto started = std::chrono::steady_clock::now();
    if (::sendto(fd, packet.data(), packet.size(), 0,
                 reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        reply_failure(icmp_host_error(errno)); return;
    }
    const auto deadline = started + std::chrono::milliseconds(timeout);
    bool first_poll = true;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (!first_poll && now >= deadline) { reply_failure(kIP_REQ_TIMED_OUT); return; }
        first_poll = false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd descriptor{fd, POLLIN, 0};
        const int wait_ms = static_cast<int>(std::clamp<i64>(remaining, 0, 0x7fffffff));
        const int ready = ::poll(&descriptor, 1, wait_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { reply_failure(icmp_host_error(errno)); return; }
        if (ready == 0) { reply_failure(kIP_REQ_TIMED_OUT); return; }
        std::array<u8, 65536> received{};
        const ssize_t received_size = ::recv(fd, received.data(), received.size(), 0);
        if (received_size < 0) { reply_failure(icmp_host_error(errno)); return; }
        if (received_size < 28 || (received[0] >> 4) != 4) continue;
        const usize ip_size = static_cast<usize>(received[0] & 15u) * 4;
        if (ip_size < 20 || ip_size + 8 > static_cast<usize>(received_size)) continue;
        const u8* icmp = received.data() + ip_size;
        u32 status = 0;
        const u8* echoed = icmp;
        usize data_size = static_cast<usize>(received_size) - ip_size - 8;
        if (icmp[0] != ICMP_ECHOREPLY) {
            // RFC 792 errors quote the triggering IPv4 header and ICMP header.
            if (ip_size + 8 + 20 + 8 > static_cast<usize>(received_size)) continue;
            const usize quoted_ip = static_cast<usize>(icmp[8] & 15u) * 4;
            if (quoted_ip < 20 || ip_size + 8 + quoted_ip + 8 > static_cast<usize>(received_size)) continue;
            echoed = icmp + 8 + quoted_ip;
            data_size = 0;
            if (icmp[0] == ICMP_UNREACH) {
                switch (icmp[1]) {
                case ICMP_UNREACH_NET: status = kIP_DEST_NET_UNREACHABLE; break;
                case ICMP_UNREACH_HOST: status = kIP_DEST_HOST_UNREACHABLE; break;
                case ICMP_UNREACH_PROTOCOL: status = kIP_DEST_PROT_UNREACHABLE; break;
                case ICMP_UNREACH_PORT: status = kIP_DEST_PORT_UNREACHABLE; break;
                case ICMP_UNREACH_NEEDFRAG: status = kIP_PACKET_TOO_BIG; break;
                case ICMP_UNREACH_SRCFAIL: status = kIP_BAD_ROUTE; break;
                default:
                    icmp_unsupported("extended ICMP unreachable code translation");
                    fail(ERROR_NOT_SUPPORTED); return;
                }
            } else if (icmp[0] == ICMP_TIMXCEED) {
                status = icmp[1] == ICMP_TIMXCEED_INTRANS ? kIP_TTL_EXPIRED_TRANSIT : kIP_TTL_EXPIRED_REASSEM;
            } else if (icmp[0] == ICMP_PARAMPROB) status = kIP_PARAM_PROBLEM;
            else if (icmp[0] == ICMP_SOURCEQUENCH) status = kIP_SOURCE_QUENCH;
            else continue;
        }
        if (!std::equal(packet.begin() + 4, packet.begin() + 8, echoed + 4)) continue;
        const usize option_size = ip_size - 20;
        if (header_size + data_size + option_size > reply_size) {
            reply_failure(kIP_BUF_TOO_SMALL); return;
        }
        std::vector<u8> result(header_size + data_size + option_size, 0);
        std::memcpy(result.data(), received.data() + 12, 4);
        auto put32 = [&](usize offset, u32 value) { std::memcpy(result.data() + offset, &value, 4); };
        auto pointer = [&](usize offset, u64 value) { std::memcpy(result.data() + offset, &value, wide ? 8 : 4); };
        put32(4, status);
        put32(8, static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started).count()));
        const u16 size16 = static_cast<u16>(data_size);
        std::memcpy(result.data() + 12, &size16, 2);
        pointer(wide ? kIcmpDataOffset64 : kIcmpDataOffset32, reply + header_size);
        result[options_offset] = received[8]; result[options_offset + 1] = received[1];
        // Darwin hands raw ICMP sockets ip_off in host byte order (ip(4)).
        u16 fragment = 0;
        std::memcpy(&fragment, received.data() + 6, sizeof(fragment));
#ifndef __APPLE__
        fragment = ntohs(fragment);
#endif
        result[options_offset + 2] = static_cast<u8>((fragment & IP_DF) ? kIP_FLAG_DF : 0);
        result[options_offset + 3] = static_cast<u8>(option_size);
        if (option_size) {
            pointer(options_offset + (wide ? 8 : 4), reply + header_size + data_size);
            std::memcpy(result.data() + header_size + data_size, received.data() + 20, option_size);
        }
        if (data_size) std::memcpy(result.data() + header_size, icmp + 8, data_size);
        if (!m.memory().write_bytes(reply, result.data(), result.size()).is_ok()) {
            fail(error_codes::ERROR_NOACCESS); return;
        }
        winapi::set_last_error(NO_ERROR);
        m.set_return(1);
        return;
    }
}

} // namespace vitreo::winapi
