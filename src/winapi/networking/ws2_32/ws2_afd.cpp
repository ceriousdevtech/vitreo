// SPDX-License-Identifier: LGPL-2.1-only
/// @file ws2_afd.cpp
/// @brief NtDeviceIoControlFile on a socket handle (the AFD driver requests).
///
/// A socket handle is a handle to the AFD driver, and ws2_32's own calls are
/// device requests on it; some programs issue those requests directly with
/// NtDeviceIoControlFile. The driver interface is not documented, so every
/// rule here is measured: tools/oracle_probe/afd_recv_probe.c on Windows 11
/// (.expected for x64, .x86.expected for x86).

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"

#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 STATUS_SUCCESS_ = 0x00000000u;
constexpr u32 STATUS_PENDING_ = 0x00000103u;
constexpr u32 STATUS_INVALID_PARAMETER_ = 0xC000000Du;
constexpr u32 STATUS_INVALID_DEVICE_REQUEST_ = 0xC0000010u;
constexpr u32 STATUS_DEVICE_NOT_READY_ = 0xC00000A3u;
constexpr u32 STATUS_NOT_SUPPORTED_ = 0xC00000BBu;

constexpr u32 kAfdDeviceType = 0x12;            // FILE_DEVICE_NETWORK
constexpr u32 IOCTL_AFD_RECEIVE = 0x00012017u;  // function 5, METHOD_NEITHER

// AFD_RECV_INFO.AfdFlags / TdiFlags.
constexpr u32 AFD_OVERLAPPED = 0x2;
constexpr u32 TDI_RECEIVE_NORMAL = 0x20;
constexpr u32 TDI_RECEIVE_EXPEDITED = 0x40;
constexpr u32 TDI_RECEIVE_PEEK = 0x80;

void report_unimplemented(u32 control_code) {
    static std::atomic<u32> reported{0};
    if (reported.fetch_add(1) < 8) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!NtDeviceIoControlFile: AFD request 0x%08x on a socket\n",
                     control_code);
    }
}

} // namespace

u32 Ws2Manager::afd_device_control(Marshaler& m, u32 guest_socket, u32 control_code, GuestAddr input,
                                   u32 input_length, GuestAddr output, u32 output_length,
                                   GuestAddr io_status_block, GuestAddr apc_context, Handle event,
                                   GuestAddr apc_routine, bool& handled) {
    (void)output;
    (void)output_length;
    const int native_fd = get_native_fd(guest_socket);
    handled = native_fd >= 0;
    if (!handled) return STATUS_INVALID_DEVICE_REQUEST_;

    if (control_code != IOCTL_AFD_RECEIVE) {
        // Other AFD functions exist on Windows; Vitreo serves only the
        // receive. Anything else is not a request this device accepts
        // (afd_recv_probe unknown_afd_code).
        if ((control_code >> 16) == kAfdDeviceType) report_unimplemented(control_code);
        return STATUS_INVALID_DEVICE_REQUEST_;
    }

    // AFD_RECV_INFO { LPWSABUF BufferArray; ULONG BufferCount; ULONG AfdFlags;
    // ULONG TdiFlags; }: 24 bytes on x64, 16 on x86 (afd_recv_probe
    // sizeof_info). A shorter input is rejected before anything is read.
    const bool is_64bit = m.cpu_is_64bit();
    const u32 info_size = is_64bit ? 24u : 16u;
    const u32 ptr = is_64bit ? 8u : 4u;
    if (input == 0 || input_length < info_size) return STATUS_INVALID_PARAMETER_;
    const GuestAddr buffers = is_64bit ? m.memory().read_u64(input).value_or(0)
                                       : m.memory().read_u32(input).value_or(0);
    const u32 buffer_count = m.memory().read_u32(input + ptr).value_or(0);
    const u32 afd_flags = m.memory().read_u32(input + ptr + 4).value_or(0);
    const u32 tdi_flags = m.memory().read_u32(input + ptr + 8).value_or(0);

    // TdiFlags must name a normal receive, optionally a peek; an expedited
    // (out-of-band) receive is not supported on a datagram socket, and any
    // other combination is invalid (afd_recv_probe tdi_*).
    if ((tdi_flags & TDI_RECEIVE_NORMAL) == 0) {
        if (tdi_flags == TDI_RECEIVE_EXPEDITED) {
            int type = 0;
            socklen_t len = sizeof(type);
            (void)::getsockopt(native_fd, SOL_SOCKET, SO_TYPE, &type, &len);
            if (type != SOCK_STREAM) return STATUS_NOT_SUPPORTED_;
            report_unimplemented(control_code);
            return STATUS_NOT_SUPPORTED_;
        }
        return STATUS_INVALID_PARAMETER_;
    }
    if ((tdi_flags & ~(TDI_RECEIVE_NORMAL | TDI_RECEIVE_PEEK)) != 0) return STATUS_INVALID_PARAMETER_;
    const int recv_flags = (tdi_flags & TDI_RECEIVE_PEEK) != 0 ? MSG_PEEK : 0;

    // WSABUF { ULONG len; CHAR* buf; }: 16 bytes on x64, 8 on x86.
    std::vector<std::pair<GuestAddr, u32>> guest_bufs;
    const u32 wsabuf_size = is_64bit ? 16u : 8u;
    for (u32 i = 0; i < buffer_count && buffer_count <= 1024; ++i) {
        const GuestAddr entry = buffers + static_cast<GuestAddr>(i) * wsabuf_size;
        const u32 len = m.memory().read_u32(entry).value_or(0);
        const GuestAddr buf = is_64bit ? m.memory().read_u64(entry + 8).value_or(0)
                                       : m.memory().read_u32(entry + 4).value_or(0);
        guest_bufs.emplace_back(buf, len);
    }

    // A request without AFD_OVERLAPPED on a nonblocking socket fails instead
    // of pending when nothing is queued (afd_recv_probe nonblocking_empty).
    // Any other request with nothing queued pends -- a socket handle is an
    // asynchronous file object, so even a blocking socket's request returns
    // STATUS_PENDING (blocking_waits).
    const bool nonblocking = (::fcntl(native_fd, F_GETFL, 0) & O_NONBLOCK) != 0;
    if ((afd_flags & AFD_OVERLAPPED) == 0 && nonblocking) {
        char probe = 0;
        const ssize_t queued = ::recv(native_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (queued < 0) return STATUS_DEVICE_NOT_READY_;
    }

    const u32 issuing_tid = ThreadManager::get_current_thread_id_static();
    {
        std::lock_guard<std::mutex> lock(m_nt_route_mutex);
        m_nt_routes[io_status_block] = NtRequestRoute{apc_context, event, apc_routine, issuing_tid};
    }
    bool completed_immediately = false;
    const bool accepted = begin_overlapped_recv(
        guest_socket, native_fd, std::move(guest_bufs), recv_flags, io_status_block, is_64bit,
        /*bytes_recvd_ptr=*/0, m, 0, 0, 0, 0, 0, 0, /*completion_routine=*/0, issuing_tid,
        &completed_immediately, /*write_pending_status=*/false);

    std::lock_guard<std::mutex> lock(m_nt_route_mutex);
    auto route = m_nt_routes.find(io_status_block);
    if (!accepted) {
        if (route != m_nt_routes.end()) m_nt_routes.erase(route);
        return ws2_wsa_error_to_ntstatus(get_last_error());
    }
    if (route == m_nt_routes.end()) return STATUS_PENDING_;
    if (route->second.completed) {
        // It completed (successfully, or with STATUS_BUFFER_OVERFLOW for a
        // datagram longer than the buffers) before the call returned; the
        // call reports that status (afd_recv_probe udp_sync_data,
        // udp_truncated, iocp_immediate).
        const u32 status = route->second.status;
        m_nt_routes.erase(route);
        return status;
    }
    route->second.issuing = false;
    (void)STATUS_SUCCESS_;
    return STATUS_PENDING_;
}

} // namespace vitreo::winapi
