// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_select_send.cpp
/// @brief select / send thunks
///
/// Closed method cuts from ws2_32.cpp.

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
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

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

void ws2::impl_select(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    // Untraced socket calls were a blind spot: connect/closesocket were
    // visible while everything the app did in between was not.
    VITREO_TRACE("[ws2_32] select(nfds=%u read=0x%llx write=0x%llx exc=0x%llx"
                 " tv=0x%llx)\n",
                 static_cast<unsigned>(m.arg(0)),
                 static_cast<unsigned long long>(m.arg_ptr(1)),
                 static_cast<unsigned long long>(m.arg_ptr(2)),
                 static_cast<unsigned long long>(m.arg_ptr(3)),
                 static_cast<unsigned long long>(m.arg_ptr(4)));
    // select(nfds, readfds, writefds, exceptfds, timeout) → count or SOCKET_ERROR
    // Windows fd_set: first u32 is count, then array of SOCKET handles
    [[maybe_unused]] int nfds = static_cast<int>(m.arg(0));

    GuestAddr read_ptr = m.arg_ptr(1);
    GuestAddr write_ptr = m.arg_ptr(2);
    GuestAddr except_ptr = m.arg_ptr(3);
    GuestAddr timeout_ptr = m.arg_ptr(4);
    
    // Read timeout (Windows timeval: two i32s - seconds and microseconds)
    struct timeval native_tv{};
    struct timeval* tv_ptr = nullptr;
    if (timeout_ptr) {
        auto sec_res = m.read_u32(timeout_ptr);
        auto usec_res = m.read_u32(timeout_ptr + 4);
        if (sec_res.is_ok() && usec_res.is_ok()) {
            // Darwin rejects timeval seconds above 100,000,000 with EINVAL,
            // while Winsock accepts LONG_MAX as an effectively-infinite wait.
            // Clamp only the host representation; readiness still returns
            // immediately and the practical timeout remains over three years.
            // timeval's fields are signed LONGs: a negative one is not a huge
            // wait -- Windows times out at once (ws2_core_probe
            // select_negative_sec / select_negative_usec: rc=0).
            constexpr i32 kDarwinMaxSelectSeconds = 100'000'000;
            const i32 guest_seconds = static_cast<i32>(sec_res.value());
            native_tv.tv_sec = static_cast<long>(
                std::min(guest_seconds, kDarwinMaxSelectSeconds));
            native_tv.tv_usec =
                static_cast<suseconds_t>(static_cast<i32>(usec_res.value()));
            if (guest_seconds > kDarwinMaxSelectSeconds) {
                VITREO_TRACE(
                    "[ws2_32] select: clamped timeout seconds %u -> %u for Darwin\n",
                    guest_seconds, kDarwinMaxSelectSeconds);
            }
            tv_ptr = &native_tv;
        }
    }

    // Windows fd_set layout:
    //   struct { u_int fd_count; SOCKET fd_array[FD_SETSIZE]; }
    // SOCKET is UINT_PTR — 4 bytes on x86, 8 bytes on x64. On x64 the
    // 8-byte alignment of SOCKET inserts 4 bytes of padding after fd_count,
    // so fd_array begins at offset 8 with an 8-byte stride. Reading with the
    // x86 layout (offset 4, stride 4) on a 64-bit guest yields the padding
    // word (0) instead of the real socket handle.
    const bool fdset_is64 = m.cpu_is_64bit();
    const u32 fd_array_off = fdset_is64 ? 8u : 4u;
    const u32 fd_stride    = fdset_is64 ? 8u : 4u;

    // poll-based wait: Darwin fd_set is indexed by HOST fd and aborts when
    // fd >= FD_SETSIZE (1024). Winsock has no host-fd limit (ws2_core_probe
    // select_index100; audit MEDIUM FD_SETSIZE).
    struct SelectInterest {
        u32 guest = 0;
        int fd = -1;
        bool want_r = false;
        bool want_w = false;
        bool want_x = false;
        bool ready_r = false;
        bool ready_w = false;
        bool ready_x = false;
    };
    std::vector<SelectInterest> interests;
    interests.reserve(64);
    auto interest_for = [&](u32 guest, int fd) -> SelectInterest& {
        for (auto& it : interests) {
            if (it.guest == guest) return it;
        }
        interests.push_back(SelectInterest{guest, fd, false, false, false,
                                           false, false, false});
        return interests.back();
    };

    bool had_invalid = false;
    auto ingest = [&](GuestAddr ptr, bool is_r, bool is_w, bool is_x) -> bool {
        if (!ptr) return true;
        auto count_res = m.read_u32(ptr);
        if (count_res.is_err()) return false;
        u32 count = count_res.value();
        // Guest FD_SETSIZE is app-defined; Mono uses 1024. Cap the walk so a
        // corrupt count cannot read unbounded guest memory.
        constexpr u32 kMaxGuestFdSet = 65536u;
        if (count > kMaxGuestFdSet) count = kMaxGuestFdSet;
        for (u32 i = 0; i < count; ++i) {
            auto sock_res = m.read_u32(ptr + fd_array_off + i * fd_stride);
            if (sock_res.is_err()) return false;
            u32 guest_sock = sock_res.value();
            int native_fd = mgr.get_native_fd(guest_sock);
            if (native_fd < 0) {
                had_invalid = true;
                return true;
            }
            auto& it = interest_for(guest_sock, native_fd);
            if (is_r) it.want_r = true;
            if (is_w) it.want_w = true;
            if (is_x) it.want_x = true;
        }
        return true;
    };

    if (!ingest(read_ptr, true, false, false) ||
        !ingest(write_ptr, false, true, false) ||
        !ingest(except_ptr, false, false, true)) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (had_invalid) {
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    if (interests.empty()) {
        mgr.set_last_error(WSAEINVAL);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    std::vector<pollfd> pfds;
    pfds.reserve(interests.size());
    for (const auto& sel : interests) {
        short ev = 0;
        if (sel.want_r) ev = static_cast<short>(ev | POLLIN);
        if (sel.want_w) ev = static_cast<short>(ev | POLLOUT);
        if (sel.want_x) ev = static_cast<short>(ev | POLLPRI);
        pfds.push_back(pollfd{sel.fd, ev, 0});
    }

    // A nonzero timeout waits floor(timeout) + 1 ms on Windows, so every
    // sub-millisecond timeval waits about a millisecond and none returns
    // early (ws2_select_timeout_probe: {0,1}..{0,500} -> ~1000 us,
    // {0,1000} -> ~1990 us, {0,5000} -> ~5970 us, at both timer periods).
    // Truncating to poll() milliseconds instead made every sub-ms timeout a
    // zero-length poll: a game's "wait up to 0.5 ms for a datagram" loop
    // became two threads spinning 46,000 select calls a second each.
    int timeout_ms = -1;
    if (tv_ptr) {
        const i64 us = static_cast<i64>(native_tv.tv_sec) * 1000000 +
                       static_cast<i64>(native_tv.tv_usec);
        if (us <= 0) timeout_ms = 0;
        else if (us / 1000 + 1 > 0x7fffffff) timeout_ms = 0x7fffffff;
        else timeout_ms = static_cast<int>(us / 1000 + 1);
    }

    const bool perf = ws2_perf_enabled();
    auto select_start = std::chrono::steady_clock::time_point{};
    if (perf) select_start = std::chrono::steady_clock::now();
    int prc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
    if (perf) {
        s_ws2_perf_stats.select_calls.fetch_add(1, std::memory_order_relaxed);
        s_ws2_perf_stats.select_wait_ns.fetch_add(
            ns_between(select_start, std::chrono::steady_clock::now()),
            std::memory_order_relaxed);
    }

    if (prc < 0) {
        mgr.set_last_error(Ws2Manager::errno_to_wsa(errno));
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }

    int rc = 0;
    for (usize i = 0; i < interests.size(); ++i) {
        auto& sel = interests[i];
        const short re = pfds[i].revents;
        if (sel.want_r && (re & (POLLIN | POLLHUP | POLLERR))) {
            sel.ready_r = true;
        }
        if (sel.want_w && (re & (POLLOUT | POLLERR | POLLHUP))) {
            sel.ready_w = true;
        }
        if (sel.want_x && (re & POLLPRI)) {
            sel.ready_x = true;
        }
    }

    // Winsock: failed nonblocking connect -> exceptfds only, clear writefds.
    for (auto& sel : interests) {
        if (!sel.want_w && !sel.want_x) continue;
        const auto st = mgr.socket_state(sel.guest);
        if (!st.connect_in_progress && st.connect_error == 0) continue;
        u32 wsa = st.connect_error;
        if (wsa == 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            if (::getsockopt(sel.fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                err != 0) {
                wsa = Ws2Manager::errno_to_wsa(err);
                mgr.update_socket_state(sel.guest, [&](Ws2Manager::SocketState& s) {
                    s.connect_error = wsa;
                    s.connect_in_progress = false;
                });
            } else if (st.connect_in_progress && (sel.ready_w || sel.ready_x)) {
                continue;
            } else {
                continue;
            }
        }
        if (wsa == 0) {
            mgr.update_socket_state(sel.guest, [&](Ws2Manager::SocketState& s) {
                s.connect_in_progress = false;
            });
            continue;
        }
        sel.ready_w = false;
        sel.ready_r = false;
        if (except_ptr && sel.want_x) {
            sel.ready_x = true;
        }
    }

    for (const auto& sel : interests) {
        if (sel.ready_r) ++rc;
        if (sel.ready_w) ++rc;
        if (sel.ready_x) ++rc;
    }

    auto write_fd_set_back = [&](GuestAddr ptr, bool want_r, bool want_w,
                                 bool want_x) -> bool {
        if (!ptr) return true;
        auto count_res = m.read_u32(ptr);
        if (count_res.is_err()) return false;
        u32 orig_count = count_res.value();
        constexpr u32 kMaxGuestFdSet = 65536u;
        if (orig_count > kMaxGuestFdSet) orig_count = kMaxGuestFdSet;

        std::vector<u32> ready_socks;
        for (u32 i = 0; i < orig_count; ++i) {
            auto sock_res = m.read_u32(ptr + fd_array_off + i * fd_stride);
            if (sock_res.is_err()) return false;
            u32 guest_sock = sock_res.value();
            for (const auto& sel : interests) {
                if (sel.guest != guest_sock) continue;
                const bool ready =
                    (want_r && sel.ready_r) || (want_w && sel.ready_w) ||
                    (want_x && sel.ready_x);
                if (ready) ready_socks.push_back(guest_sock);
                break;
            }
        }
        const u32 new_count = static_cast<u32>(ready_socks.size());
        (void)m.write_u32(ptr, new_count);
        for (u32 i = 0; i < new_count; ++i) {
            (void)m.write_u32(ptr + fd_array_off + i * fd_stride, ready_socks[i]);
            if (fdset_is64) {
                (void)m.write_u32(ptr + fd_array_off + i * fd_stride + 4, 0);
            }
        }
        return true;
    };

    (void)write_fd_set_back(read_ptr, true, false, false);
    (void)write_fd_set_back(write_ptr, false, true, false);
    (void)write_fd_set_back(except_ptr, false, false, true);

    m.set_return(static_cast<u64>(static_cast<u32>(rc)));
}

void ws2::impl_send(Marshaler& m, Ws2Manager& mgr) {
    if (!require_winsock(mgr, m, Ws2NotInitReturn::SocketError)) return;

    s_ws2_io_gen.fetch_add(1, std::memory_order_release);
    // send(socket, buf, len, flags) → bytes sent or SOCKET_ERROR
    u32 guest_socket = static_cast<u32>(m.arg(0));
    GuestAddr buf_ptr = m.arg_ptr(1);
    int len = static_cast<int>(m.arg(2));
    int flags = static_cast<int>(m.arg(3));
    u32 guest_tid = 0;

    if (ws2_trace_enabled()) {
        guest_tid = ThreadManager::get_current_thread_id_static();
        u32 caller = 0;
        u32 caller_frames[4] = {};
        if (auto* cpu = m.cpu()) {
            auto ret = m.memory().read_u32(cpu->sp());
            if (ret.is_ok()) {
                caller = ret.value();
            }

            u32 frame = cpu->gpr.get32(static_cast<u8>(cpu::RegIndex::RBP));
            for (u32 i = 0; i < 4 && frame != 0 && frame != 0xFFFFFFFF; ++i) {
                auto next_frame = m.memory().read_u32(frame);
                auto frame_ret = m.memory().read_u32(frame + 4);
                if (frame_ret.is_ok()) {
                    caller_frames[i] = frame_ret.value();
                }
                if (next_frame.is_err()) {
                    break;
                }
                frame = next_frame.value();
            }
        }

        VITREO_TRACE(
            "[ws2_32] send(tid=%u, socket=0x%x, len=%d, flags=%d, caller=0x%08x, frames=%08x,%08x,%08x,%08x)\n",
            guest_tid, guest_socket, len, flags, caller,
            caller_frames[0], caller_frames[1], caller_frames[2], caller_frames[3]);
    }
    
    int native_fd = mgr.get_native_fd(guest_socket);
    if (native_fd < 0) {
        VITREO_TRACE( "[ws2_32] send: invalid socket -> WSAENOTSOCK\n");
        mgr.set_last_error(WSAENOTSOCK);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    // ws2_corex_probe send_len_neg / send_null_buf / send_bad_ptr: a
    // negative length or an unreadable buffer is WSAEFAULT and the
    // connection survives (send_after_faults); a zero length sends nothing
    // and returns 0 (ws2_core_probe send_len0, send_null_len0).
    if (len < 0) {
        mgr.set_last_error(WSAEFAULT);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    {
        const auto st = mgr.socket_state(guest_socket);
        // A stream the local side aborted, or whose connect failed.
        const u32 gate = st.aborted ? WSAECONNABORTED
                         : (st.connect_error != 0 && !st.connect_in_progress)
                             ? WSAENOTCONN
                             : st.shut_wr ? WSAESHUTDOWN : 0u;
        if (gate != 0) {
            mgr.set_last_error(gate);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }
    
    // Reuse a thread-local host buffer to avoid per-call heap allocations.
    thread_local std::vector<char> host_buf;
    if (host_buf.size() < static_cast<size_t>(len) + 1) {
        host_buf.resize(static_cast<size_t>(len) + 1);
    }

    // Read data from guest memory
    if (len > 0) {
        auto res = m.memory().read_bytes(buf_ptr,
            reinterpret_cast<Byte*>(host_buf.data()), static_cast<size_t>(len));
        if (res.is_err()) {
            mgr.set_last_error(WSAEFAULT);
            m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
            return;
        }
    }
    
    // Build packet hex dumps only when tracing is enabled.
    if (ws2_trace_enabled()) {
        // 128 bytes covers a control packet whose payload extends past byte 32.
        size_t dump_len = static_cast<size_t>(len) < 128 ? static_cast<size_t>(len) : 128;
        char hex[512] = {};
        for (size_t i = 0; i < dump_len; ++i) {
            snprintf(hex + i * 3, 4, "%02x ", static_cast<unsigned char>(host_buf[i]));
        }
        VITREO_TRACE("[ws2_32] send data: %s\n", hex);
    }
    
    // Diagnostic: dump guest state for a plaintext TLS alert, or for the
    // TLS 1.3 terminal flight shape (CCS followed by one short encrypted
    // application-data record). The latter keeps the alert payload encrypted,
    // but still identifies the exact BoringSSL failure edge for CPU tracing.
    const bool tls13_terminal_flight =
        len >= 11 && static_cast<unsigned char>(host_buf[0]) == 0x14 &&
        static_cast<unsigned char>(host_buf[1]) == 0x03 &&
        static_cast<unsigned char>(host_buf[3]) == 0x00 &&
        static_cast<unsigned char>(host_buf[4]) == 0x01 &&
        static_cast<unsigned char>(host_buf[5]) == 0x01 &&
        static_cast<unsigned char>(host_buf[6]) == 0x17;
    if (ws2_trace_enabled() && len >= 7 &&
        (static_cast<unsigned char>(host_buf[0]) == 0x15 ||
         tls13_terminal_flight)) {
        unsigned alert_level = static_cast<unsigned char>(host_buf[5]);
        unsigned alert_desc = static_cast<unsigned char>(host_buf[6]);
        u32 tid = winapi::ThreadManager::get_current_thread_id_static();
        VITREO_TRACE("[ws2_32] TLS ALERT SENT on thread %u: level=%u desc=%u (0x%02x)\n",
                     tid, alert_level, alert_desc, alert_desc);
        // Walk the guest stack to find return addresses
        auto* cpu = m.cpu();
        if (cpu) {
            u32 esp_val = static_cast<u32>(cpu->gpr.rsp);
            u32 ebp_val = static_cast<u32>(cpu->gpr.rbp);
            u32 esi_val = static_cast<u32>(cpu->gpr.rsi);
            u32 edi_val = static_cast<u32>(cpu->gpr.rdi);
            u32 eax_val = static_cast<u32>(cpu->gpr.rax);
            u32 ecx_val = static_cast<u32>(cpu->gpr.rcx);
            VITREO_TRACE("[ws2_32] TLS ALERT regs: EAX=0x%08x ECX=0x%08x ESI=0x%08x EDI=0x%08x EBP=0x%08x ESP=0x%08x\n",
                         eax_val, ecx_val, esi_val, edi_val, ebp_val, esp_val);
            // Dump 64 bytes of stack around ESP
            for (int off = 0; off < 128; off += 4) {
                auto v = m.read_u32(esp_val + off);
                if (v.is_ok()) {
                    VITREO_TRACE("[ws2_32] TLS ALERT stack[ESP+0x%02x] = 0x%08x\n", off, v.value());
                }
            }
            // Dump memory at ESI (likely 'this' pointer) if non-zero
            if (esi_val > 0x10000) {
                VITREO_TRACE("[ws2_32] TLS ALERT ESI object dump:\n");
                for (int off = 0; off < 0x20; off += 4) {
                    auto v = m.read_u32(esi_val + off);
                    if (v.is_ok()) {
                        VITREO_TRACE("[ws2_32]   [ESI+0x%02x] = 0x%08x\n", off, v.value());
                    } else {
                        VITREO_TRACE("[ws2_32]   [ESI+0x%02x] = <read failed>\n", off);
                    }
                }
            }
            // Request trace ring dump from execution loop
            g_trace_ring_dump_requested.store(true, std::memory_order_release);

            // Dump per-thread trace ring (captures worker thread execution)
            if (t_thread_trace.active && t_thread_trace.idx > 0) {
                usize total = t_thread_trace.idx;
                usize count = total < THREAD_TRACE_SIZE ? total : THREAD_TRACE_SIZE;
                usize start = total > count ? total - count : 0;
                VITREO_TRACE("[ws2_32] TLS ALERT thread trace ring (%zu blocks, tid=%u):\n",
                             count, tid);
                for (usize i = start; i < total; ++i) {
                    VITREO_TRACE("  [%zu] 0x%08x\n",
                                 i, static_cast<unsigned>(t_thread_trace.rips[i % THREAD_TRACE_SIZE]));
                }
                VITREO_TRACE("[ws2_32] End thread trace ring\n");
            }
        }
    }
    
    // Optional TLS dump for deep debugging only.
    if (tls_dump_enabled() && len > 5 && (static_cast<unsigned char>(host_buf[0]) == 0x16 ||
                    static_cast<unsigned char>(host_buf[0]) == 0x14)) {
        FILE* fp = fopen("/tmp/vitreo_clientsend.bin", "wb");
        if (fp) {
            fwrite(host_buf.data(), 1, static_cast<size_t>(len), fp);
            fclose(fp);
            VITREO_TRACE("[ws2_32] send: dumped %d bytes to /tmp/vitreo_clientsend.bin\n", len);
        }
    }
    
    int native_send_flags = 0;
    if (const u32 ferr = check_send_flags(flags, &native_send_flags)) {
        mgr.set_last_error(ferr);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
        return;
    }
    const bool perf = ws2_perf_enabled();
    auto send_start = std::chrono::steady_clock::time_point{};
    if (perf) send_start = std::chrono::steady_clock::now();
    ssize_t rc = ::send(native_fd, host_buf.data(), static_cast<size_t>(len), native_send_flags);
    if (perf) {
        s_ws2_perf_stats.send_calls.fetch_add(1, std::memory_order_relaxed);
        s_ws2_perf_stats.send_wait_ns.fetch_add(
            ns_between(send_start, std::chrono::steady_clock::now()),
            std::memory_order_relaxed);
        if (rc > 0) {
            s_ws2_perf_stats.send_bytes.fetch_add(static_cast<u64>(rc), std::memory_order_relaxed);
        }
    }
    if (rc < 0) {
        const int send_errno = errno;
        u32 wsa_err = ws2_io_error(mgr, guest_socket, native_fd, send_errno, true);
        // An unconnected datagram socket has no destination: Winsock says
        // WSAENOTCONN, Darwin EDESTADDRREQ (ws2_core_probe
        // udp_send_unconnected / udp_send_after_zero).
        if (send_errno == EDESTADDRREQ) wsa_err = WSAENOTCONN;
        VITREO_TRACE("[ws2_32] send -> SOCKET_ERROR (wsa=%u)\n", wsa_err);
        if (wsa_err == WSAEWOULDBLOCK) {
            mgr.arm_fd_write(guest_socket);
        }
        ipc::ipc_log("send socket=0x%x len=%d -> SOCKET_ERROR wsa=%u",
                     guest_socket, len, wsa_err);
        mgr.set_last_error(wsa_err);
        m.set_return(static_cast<u64>(SOCKET_ERROR_VAL));
    } else {
        VITREO_TRACE("[ws2_32] send -> %zd bytes\n", rc);
        ipc::ipc_log("send socket=0x%x -> %zd bytes", guest_socket, rc);
        m.set_return(static_cast<u64>(static_cast<u32>(rc)));
        track_send(guest_socket, rc);
    }
}

} // namespace vitreo::winapi
