/// @file tcp_table.cpp
/// @brief GetExtendedTcpTable: the host's TCP endpoints in MIB_TCPTABLE_* form.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {

// TCP_TABLE_CLASS (iprtrmib.h).
constexpr u32 TCP_TABLE_BASIC_LISTENER = 0;
constexpr u32 TCP_TABLE_BASIC_CONNECTIONS = 1;
constexpr u32 TCP_TABLE_BASIC_ALL = 2;
constexpr u32 TCP_TABLE_OWNER_PID_LISTENER = 3;
constexpr u32 TCP_TABLE_OWNER_PID_CONNECTIONS = 4;
constexpr u32 TCP_TABLE_OWNER_MODULE_LISTENER = 6;
constexpr u32 TCP_TABLE_OWNER_MODULE_ALL = 8;
constexpr u32 MIB_TCP_STATE_LISTEN = 2;

struct TcpEndpoint {
    u16 family = 0;                // iphlp::kGuestAfInet / kGuestAfInet6
    std::array<u8, 16> local{};    // network order (IPv4 in bytes 0..3)
    std::array<u8, 16> remote{};
    u32 local_scope = 0, remote_scope = 0;
    u32 local_port = 0;            // network byte order in the low 16 bits
    u32 remote_port = 0;
    u32 state = 0;                 // MIB_TCP_STATE
    u32 owning_pid = 0;
};

[[nodiscard]] u32 map_bsd_tcp_state_to_windows(int bsd_state) noexcept {
    // BSD TCPS_* → MIB_TCP_STATE_*
    switch (bsd_state) {
    case 0:  return 1;  // CLOSED
    case 1:  return 2;  // LISTEN
    case 2:  return 3;  // SYN_SENT
    case 3:  return 4;  // SYN_RCVD
    case 4:  return 5;  // ESTAB
    case 5:  return 8;  // CLOSE_WAIT
    case 6:  return 6;  // FIN_WAIT1
    case 7:  return 9;  // CLOSING
    case 8:  return 10; // LAST_ACK
    case 9:  return 7;  // FIN_WAIT2
    case 10: return 11; // TIME_WAIT
    default: return 1;
    }
}

#ifdef __APPLE__
/// Every TCP endpoint on the host with its owning process. IPv4 sockets fill
/// the IPv4 table; AF_INET6 sockets (including dual-stack ones carrying an
/// IPv4 peer, shown v4-mapped as on Windows) fill the IPv6 table.
[[nodiscard]] std::vector<TcpEndpoint> collect_tcp_endpoints() {
    std::vector<TcpEndpoint> rows;
    int hint = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (hint <= 0) return rows;
    std::vector<pid_t> pids(static_cast<usize>(hint) / sizeof(pid_t) + 16);
    int nbytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                               static_cast<int>(pids.size() * sizeof(pid_t)));
    if (nbytes <= 0) return rows;
    const int npids = nbytes / static_cast<int>(sizeof(pid_t));

    for (int i = 0; i < npids; ++i) {
        const pid_t pid = pids[static_cast<usize>(i)];
        if (pid <= 0) continue;
        int fd_bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (fd_bytes <= 0) continue;
        std::vector<proc_fdinfo> fds(
            static_cast<usize>(fd_bytes) / sizeof(proc_fdinfo) + 4);
        fd_bytes = proc_pidinfo(
            pid, PROC_PIDLISTFDS, 0, fds.data(),
            static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
        if (fd_bytes <= 0) continue;
        const int nfd = fd_bytes / static_cast<int>(sizeof(proc_fdinfo));
        for (int j = 0; j < nfd; ++j) {
            if (fds[static_cast<usize>(j)].proc_fdtype != PROX_FDTYPE_SOCKET) {
                continue;
            }
            socket_fdinfo si{};
            const int got = proc_pidfdinfo(
                pid, fds[static_cast<usize>(j)].proc_fd,
                PROC_PIDFDSOCKETINFO, &si, sizeof(si));
            if (got <= 0) continue;
            if ((si.psi.soi_family != AF_INET && si.psi.soi_family != AF_INET6) ||
                si.psi.soi_kind != SOCKINFO_TCP) {
                continue;
            }
            const auto& tcp = si.psi.soi_proto.pri_tcp;
            const auto& in = tcp.tcpsi_ini;
            TcpEndpoint row{};
            row.state = map_bsd_tcp_state_to_windows(tcp.tcpsi_state);
            row.local_port = static_cast<u32>(in.insi_lport) & 0xFFFFu;
            row.remote_port = static_cast<u32>(in.insi_fport) & 0xFFFFu;
            // Vitreo GetCurrentProcessId returns host getpid(), so host PIDs
            // are the correct guest-visible owning process identifiers.
            row.owning_pid = static_cast<u32>(pid);
            if (si.psi.soi_family == AF_INET) {
                row.family = iphlp::kGuestAfInet;
                std::memcpy(row.local.data(), &in.insi_laddr.ina_46.i46a_addr4, 4);
                std::memcpy(row.remote.data(), &in.insi_faddr.ina_46.i46a_addr4, 4);
            } else {
                row.family = iphlp::kGuestAfInet6;
                if ((in.insi_vflag & INI_IPV6) == 0) {
                    // IPv4 peer on an AF_INET6 socket: ::ffff:a.b.c.d.
                    row.local[10] = row.local[11] = row.remote[10] = row.remote[11] = 0xFF;
                    std::memcpy(row.local.data() + 12, &in.insi_laddr.ina_46.i46a_addr4, 4);
                    std::memcpy(row.remote.data() + 12, &in.insi_faddr.ina_46.i46a_addr4, 4);
                } else {
                    std::memcpy(row.local.data(), &in.insi_laddr.ina_6, 16);
                    std::memcpy(row.remote.data(), &in.insi_faddr.ina_6, 16);
                }
                // Link-local endpoints carry their zone (KAME embeds it in
                // bytes 2..3 inside the kernel; Windows reports a scope id).
                auto zone = [&](std::array<u8, 16>& a) -> u32 {
                    if (a[0] != 0xFE || (a[1] & 0xC0) != 0x80) return 0;
                    const u32 embedded = (static_cast<u32>(a[2]) << 8) | a[3];
                    a[2] = a[3] = 0;
                    return embedded ? embedded : in.insi_v6.in6_ifindex;
                };
                row.local_scope = zone(row.local);
                row.remote_scope = zone(row.remote);
            }
            rows.push_back(row);
        }
    }
    return rows;
}
#else
[[nodiscard]] std::vector<TcpEndpoint> collect_tcp_endpoints() {
    return {};
}
#endif

} // namespace

void IphlpapiManager::impl_GetExtendedTcpTable(Marshaler& m) {
    // DWORD GetExtendedTcpTable(PVOID pTcpTable, PDWORD pdwSize, BOOL bOrder,
    //                           ULONG ulAf, TCP_TABLE_CLASS TableClass, ULONG Reserved)
    // https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable
    // Error cases measured by tools/oracle_probe/iphlp_misc_probe.
    const GuestAddr table = m.arg_ptr(0);
    const GuestAddr size_ptr = m.arg_ptr(1);
    const bool order = m.arg32(2) != 0;
    const u32 af = m.arg32(3);
    const u32 table_class = m.arg32(4);
    (void)m.arg32(5);  // Reserved: ignored by Windows (tcp-reserved-nonzero)

    const auto capacity = read_size(m, size_ptr);
    if (!capacity) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    // Unknown families and classes, and AF_INET6 with a BASIC class, are all
    // ERROR_INVALID_PARAMETER with *pdwSize untouched (tcp-bad-af,
    // tcp-bad-class, tcp6-basic; Learn claims ERROR_NOT_SUPPORTED for the
    // last, Windows returns 87).
    if ((af != iphlp::kGuestAfInet && af != iphlp::kGuestAfInet6) || table_class > TCP_TABLE_OWNER_MODULE_ALL ||
        (af == iphlp::kGuestAfInet6 && table_class <= TCP_TABLE_BASIC_ALL)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (table_class >= TCP_TABLE_OWNER_MODULE_LISTENER) {
        // MIB_TCPROW_OWNER_MODULE needs the socket creation time and the
        // owning module's tag, which the host does not record per socket.
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!GetExtendedTcpTable: TCP_TABLE_OWNER_MODULE_* (per-socket creation time and owning module)\n");
        });
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }
    // Diagnostic escape hatch: restore pre-fix stub behavior.
    if (const char* disable = std::getenv("VITREO_TCPTABLE");
        disable != nullptr && disable[0] == '0' && disable[1] == '\0') {
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }

    auto rows = collect_tcp_endpoints();
    std::erase_if(rows, [&](const TcpEndpoint& r) { return r.family != af; });
    // Restrict owning PIDs to capsule-visible Vitreo guests when a provider is
    // wired. Always keep the current host PID (GetCurrentProcessId).
    if (m_pid_allowlist_provider) {
        auto allow = m_pid_allowlist_provider();
        allow.insert(static_cast<u32>(::getpid()));
        std::erase_if(rows, [&](const TcpEndpoint& r) { return allow.count(r.owning_pid) == 0; });
    }
    const bool listeners_only = table_class == TCP_TABLE_BASIC_LISTENER || table_class == TCP_TABLE_OWNER_PID_LISTENER;
    const bool connections_only = table_class == TCP_TABLE_BASIC_CONNECTIONS || table_class == TCP_TABLE_OWNER_PID_CONNECTIONS;
    std::erase_if(rows, [&](const TcpEndpoint& r) {
        const bool listen = r.state == MIB_TCP_STATE_LISTEN;
        return (listeners_only && !listen) || (connections_only && listen);
    });
    if (order) {
        // Learn bOrder: local address, local scope, local port, remote
        // address, remote scope, remote port, ascending (tcp4 sorted=1).
        auto port = [](u32 p) { return static_cast<u16>(((p & 0xFFu) << 8) | ((p >> 8) & 0xFFu)); };
        std::stable_sort(rows.begin(), rows.end(), [&](const TcpEndpoint& a, const TcpEndpoint& b) {
            if (a.local != b.local) return a.local < b.local;
            if (a.local_scope != b.local_scope) return a.local_scope < b.local_scope;
            if (port(a.local_port) != port(b.local_port)) return port(a.local_port) < port(b.local_port);
            if (a.remote != b.remote) return a.remote < b.remote;
            if (a.remote_scope != b.remote_scope) return a.remote_scope < b.remote_scope;
            return port(a.remote_port) < port(b.remote_port);
        });
    }
    const AdapterAbi& L = adapter_abi(m);
    const bool v6 = af == iphlp::kGuestAfInet6;
    const bool with_pid = table_class >= TCP_TABLE_OWNER_PID_LISTENER;
    const u32 header = v6 ? L.MIB_TCP6TABLE_OWNER_PID_table : (with_pid ? L.MIB_TCPTABLE_OWNER_PID_table : L.MIB_TCPTABLE_table);
    const u32 row_size = v6 ? L.MIB_TCP6ROW_OWNER_PID_size : (with_pid ? L.MIB_TCPROW_OWNER_PID_size : L.MIB_TCPROW_size);
    const u32 need = header + static_cast<u32>(rows.size()) * row_size;
    (void)m.write_u32(size_ptr, need);
    // Size query and success both clear the last error (tcp4 size_le/le).
    winapi::set_last_error(NO_ERROR);
    if (table == 0 || *capacity < need) {
        m.set_return(ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    std::vector<u8> image(need, 0);
    const u32 count = static_cast<u32>(rows.size());
    std::memcpy(image.data(), &count, 4);
    for (usize i = 0; i < rows.size(); ++i) {
        u8* row = image.data() + header + i * row_size;
        const TcpEndpoint& r = rows[i];
        auto put = [&](u32 off, u32 value) { std::memcpy(row + off, &value, 4); };
        if (v6) {
            std::memcpy(row, r.local.data(), 16);
            put(L.MIB_TCP6ROW_OWNER_PID_dwLocalScopeId, r.local_scope);
            put(L.MIB_TCP6ROW_OWNER_PID_dwLocalPort, r.local_port);
            std::memcpy(row + L.MIB_TCP6ROW_OWNER_PID_ucRemoteAddr, r.remote.data(), 16);
            put(L.MIB_TCP6ROW_OWNER_PID_dwRemoteScopeId, r.remote_scope);
            put(L.MIB_TCP6ROW_OWNER_PID_dwRemotePort, r.remote_port);
            put(L.MIB_TCP6ROW_OWNER_PID_dwState, r.state);
            put(L.MIB_TCP6ROW_OWNER_PID_dwOwningPid, r.owning_pid);
        } else {
            // MIB_TCPROW[_OWNER_PID]: state, local addr/port, remote addr/port, pid.
            put(0, r.state);
            std::memcpy(row + 4, r.local.data(), 4);
            put(8, r.local_port);
            std::memcpy(row + 12, r.remote.data(), 4);
            put(16, r.remote_port);
            if (with_pid) put(20, r.owning_pid);
        }
    }
    if (m.memory().write_bytes(table, image.data(), image.size()).is_err()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    ipc::ipc_log("GetExtendedTcpTable af=%u class=%u rows=%zu -> NO_ERROR", af, table_class, rows.size());
    m.set_return(NO_ERROR);
}

} // namespace vitreo::winapi
