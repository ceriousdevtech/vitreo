#pragma once

/// @file ws2_32.hpp
/// @brief ws2_32.dll API stubs (Winsock 2)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/common/reaped_workers.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class Marshaler;
class HeapManager;
class FileManager;

// =============================================================================
// Ws2Manager
// =============================================================================

/// Thread-local WSA error code — matches Windows per-thread semantics.
/// Shares the kernel32 LastErrorValue slot (ws2_core_probe
/// SetLastError_then_WSAGetLastError / WSASetLastError_then_GetLastError).
inline thread_local u32 g_wsa_last_error = 0;

/// Scratch buffer constants for DNS resolution results.
/// gethostbyname uses a bump+reset arena (Windows invalidates hostent* on the
/// next hostent call). getaddrinfo uses a durable sub-arena that survives until
/// freeaddrinfo — resetting shared scratch between calls was wiping live
/// ADDRINFOA chains and blocking a later lookup after the first name
/// resolved.
static constexpr GuestAddr WS2_SCRATCH_BASE = 0x5E000000;
static constexpr u32 WS2_SCRATCH_SIZE = 0x100000; // 1 MiB
static constexpr u32 WS2_HOSTENT_SCRATCH_SIZE = 0x10000; // first 64 KiB
static constexpr GuestAddr WS2_ADDRINFO_BASE =
    WS2_SCRATCH_BASE + WS2_HOSTENT_SCRATCH_SIZE;
static constexpr u32 WS2_ADDRINFO_SIZE =
    WS2_SCRATCH_SIZE - WS2_HOSTENT_SCRATCH_SIZE;

class Ws2Manager {
public:
    /// (handle, bytes, overlapped, is_64bit, nt_status, win_error,
    ///  completed_synchronously). Socket overlapped completions pass false.
    using OverlappedCompletionCallback =
        std::function<void(Handle, u32, GuestAddr, bool, u32, u32, bool)>;

    /// Queue a WSA LPWSAOVERLAPPED_COMPLETION_ROUTINE on the issuing guest
    /// thread (Learn: hEvent is ignored when the routine is non-NULL).
    using WsaCompletionRoutineQueue =
        std::function<void(u32 thread_id, GuestAddr pfn, u32 error_code,
                           u32 bytes, GuestAddr overlapped, u32 flags)>;

    /// PostMessage for WSAAsyncGet* completion (hwnd, msg, wParam, lParam).
    using PostMessageFn =
        std::function<bool(u64 hwnd, u32 msg, u64 wparam, u64 lparam)>;

    Ws2Manager();
    ~Ws2Manager();

    /// No copy/move
    Ws2Manager(const Ws2Manager&) = delete;
    Ws2Manager& operator=(const Ws2Manager&) = delete;
    Ws2Manager(Ws2Manager&&) = delete;
    Ws2Manager& operator=(Ws2Manager&&) = delete;

    /// Set thunk registry for extension function lookups
    void set_thunk_registry(ThunkRegistry* registry) { m_thunk_registry = registry; }

    /// Guest memory for overlapped WSARecv buffer / OVERLAPPED writes.
    void set_memory(GuestMemory* memory) { m_memory = memory; }
    [[nodiscard]] GuestMemory* guest_memory() const noexcept { return m_memory; }

    /// Deliver overlapped socket I/O to kernel32 IOCP / event machinery.
    void set_overlapped_completion_callback(OverlappedCompletionCallback callback) {
        m_overlapped_completion = std::move(callback);
    }
    void clear_overlapped_completion_callback() { m_overlapped_completion = {}; }
    void set_wsa_completion_routine_queue(WsaCompletionRoutineQueue callback) {
        m_wsa_completion_queue = std::move(callback);
    }
    void clear_wsa_completion_routine_queue() { m_wsa_completion_queue = {}; }
    void set_post_message(PostMessageFn callback) {
        m_post_message = std::move(callback);
    }
    void clear_post_message() { m_post_message = {}; }
    [[nodiscard]] bool post_window_message(u64 hwnd, u32 msg, u64 wparam,
                                           u64 lparam) const {
        if (!m_post_message) return false;
        return m_post_message(hwnd, msg, wparam, lparam);
    }

    /// Async DNS task handles for WSAAsyncGetHostByName / CancelAsyncRequest.
    [[nodiscard]] u32 alloc_async_task();
    /// Mark completed; returns false if already cancelled or unknown.
    [[nodiscard]] bool complete_async_task(u32 handle);
    /// Cancel a pending task. Returns 0 on success, or a WSA error.
    [[nodiscard]] u32 cancel_async_task(u32 handle);
    [[nodiscard]] bool async_task_active(u32 handle) const;

    /// When completion_routine is non-zero, OVERLAPPED is updated and the
    /// routine is queued on issuing_tid (hEvent / IOCP are not used).
    void notify_overlapped_completion(
        Handle handle, u32 bytes_transferred, GuestAddr overlapped,
        bool is_64bit, u32 nt_status = 0, u32 win_error = 0,
        bool completed_synchronously = false,
        GuestAddr completion_routine = 0, u32 issuing_tid = 0,
        u32 completion_flags = 0) const;

    /// Finish an overlapped WSAIoctl / socket request: either queue a
    /// completion routine APC or deliver via IOCP/hEvent.
    void complete_socket_request(u32 guest_socket, GuestAddr overlapped,
                                bool is_64bit, GuestAddr completion_routine,
                                u32 bytes_transferred, u32 nt_status,
                                u32 win_error, u32 flags,
                                bool completed_synchronously);

    /// Guest process heap. Winsock's getaddrinfo results and its per-thread
    /// hostent/protoent/servent storage live in guest memory the guest can
    /// read; they are allocated here rather than from a fixed arena.
    void set_heap_manager(HeapManager* heap) { m_heap = heap; }
    /// Zero-filled guest allocation (0 when no heap is attached or it fails).
    [[nodiscard]] GuestAddr guest_alloc(u32 size);
    void guest_free(GuestAddr addr);

    /// Learn (gethostbyname / getprotobyname / getservbyname): "only one copy
    /// of this structure is allocated per thread". One block per guest thread
    /// and structure kind, grown on demand; the previous result of the same
    /// kind on the same thread is invalidated, exactly as on Windows.
    enum class DbBlock : u8 { Host, Proto, Serv, Ntoa };
    [[nodiscard]] GuestAddr thread_db_block(DbBlock kind, u32 size);

    /// The machine's DNS host name as the rest of the guest sees it
    /// (GetComputerNameEx(ComputerNameDnsHostname)). gethostname returns it
    /// and name resolution maps it to the local addresses, as Windows does
    /// (ws2_resolve_probe gethostname_eq_dns_hostname / gethostname_resolves).
    void set_host_name_provider(std::function<std::string()> provider) {
        m_host_name_provider = std::move(provider);
    }
    [[nodiscard]] std::string host_name() const {
        return m_host_name_provider ? m_host_name_provider() : std::string("VITREO");
    }

    /// Set sync manager for WSA event functions
    void set_sync_manager(SyncManager* sync) { m_sync_manager = sync; }
    SyncManager* sync_manager() const { return m_sync_manager; }

    /// FileManager for TransmitFile / TransmitPackets file-backed sends.
    void set_file_manager(FileManager* files) { m_file_manager = files; }
    [[nodiscard]] FileManager* file_manager() const noexcept {
        return m_file_manager;
    }

    /// Cancel and join overlapped socket I/O workers (process teardown).
    void shutdown_overlapped_socket_io();

    /// Overlapped WSARecv: probe/wait, fill guest buffers, post IOCP via callback.
    /// Returns true when the call was accepted as pending (caller returns
    /// SOCKET_ERROR + WSA_IO_PENDING). Returns false when the call failed
    /// immediately (WSA last error already set).
    /// Overlapped WSASend that could not complete immediately: keep the
    /// remaining bytes, return WSA_IO_PENDING, and finish them on a worker.
    /// Returns true when accepted as pending (caller returns SOCKET_ERROR +
    /// WSA_IO_PENDING); false when the send failed outright (last error set).
    [[nodiscard]] bool begin_overlapped_send(
        u32 guest_socket, int native_fd, std::vector<char> remaining,
        u32 already_sent, int send_flags, GuestAddr overlapped, bool is_64bit,
        GuestAddr bytes_sent_ptr, Marshaler& m,
        GuestAddr completion_routine = 0, u32 issuing_tid = 0);

    [[nodiscard]] bool begin_overlapped_recv(
        u32 guest_socket, int native_fd,
        std::vector<std::pair<GuestAddr, u32>> guest_bufs,
        int recv_flags, GuestAddr overlapped, bool is_64bit,
        GuestAddr bytes_recvd_ptr, Marshaler& m,
        GuestAddr from_ptr = 0, GuestAddr fromlen_ptr = 0,
        GuestAddr control_ptr = 0, u32 control_capacity = 0,
        GuestAddr control_len_ptr = 0, GuestAddr msg_flags_ptr = 0,
        GuestAddr completion_routine = 0, u32 issuing_tid = 0,
        bool* completed_immediately = nullptr, bool write_pending_status = true);

    /// NT delivery for a socket request issued through NtDeviceIoControlFile:
    /// (socket, ApcContext, Event, ApcRoutine, IoStatusBlock, issuing thread,
    /// bytes, NTSTATUS, Win32 error, completed while being issued). The
    /// IO_STATUS_BLOCK is already written when it is called.
    using NtCompletionCallback = std::function<void(Handle, GuestAddr, Handle, GuestAddr, GuestAddr, u32, u32,
                                                    u32, u32, bool)>;
    void set_nt_completion_callback(NtCompletionCallback callback) { m_nt_completion = std::move(callback); }

    /// NtDeviceIoControlFile on a socket handle: the AFD driver requests the
    /// handle accepts. Returns the NTSTATUS the call reports; @p handled is
    /// false when @p guest_socket is not a socket.
    /// Measured contract: tools/oracle_probe/afd_recv_probe.c (.expected,
    /// .x86.expected).
    [[nodiscard]] u32 afd_device_control(Marshaler& m, u32 guest_socket, u32 control_code, GuestAddr input,
                                         u32 input_length, GuestAddr output, u32 output_length,
                                         GuestAddr io_status_block, GuestAddr apc_context, Handle event,
                                         GuestAddr apc_routine, bool& handled);

    // -------------------------------------------------------------------------
    // WSAEventSelect Tracking
    // -------------------------------------------------------------------------

    /// One WSAEventSelect association and its network event record (Learn
    /// WSAEventSelect / WSAEnumNetworkEvents): the FD_* events that occurred
    /// since the last WSAEnumNetworkEvents, with their error codes. An event
    /// is recorded -- and the event object signaled -- when it occurs while
    /// selected and enabled; recording disables it until WSAEnumNetworkEvents
    /// or its re-enabling function (recv, a blocked send, accept).
    struct EventAssoc {
        u32 event_handle = 0;
        u32 event_mask = 0;
        u32 recorded = 0;
        u32 disabled = 0;              // recorded since last re-enabled
        std::array<u32, 10> errors{};  // iErrorCode[FD_*_BIT] of recorded events
    };

    /// Learn WSAEventSelect: replace the socket's association (clearing the
    /// record) and record every selected event whose condition already holds.
    void select_network_events(u32 guest_socket, u32 event_handle,
                               u32 event_mask);
    /// WSAEventSelect with lNetworkEvents 0: no association, no record.
    void cancel_network_events(u32 guest_socket);
    /// WSAEnumNetworkEvents: record what has occurred, then return the record
    /// (bits and error codes) and clear it; reset @p reset_event when nonzero.
    /// Done under the provider lock, so an event recorded afterwards signals
    /// after the reset.
    void take_network_events(u32 guest_socket, Handle reset_event,
                             u32* events, std::array<u32, 10>* errors);

    /// Learn WSAEventSelect re-enabling functions. recv, recvfrom, WSARecv,
    /// WSARecvEx and WSARecvFrom re-enable FD_READ and FD_OOB whatever they
    /// return: data still buffered is recorded again (level-triggered).
    void reenable_fd_read(u32 guest_socket, int native_fd);
    /// A send-family call failed WSAEWOULDBLOCK: FD_WRITE is recorded once
    /// buffer space is available again.
    void arm_fd_write(u32 guest_socket);
    /// accept / WSAAccept / AcceptEx re-enable FD_ACCEPT; a connection still
    /// pending is recorded again.
    void reenable_fd_accept(u32 listener);
    /// A stream socket connected (connect, ConnectEx): FD_WRITE is owed until
    /// it is recorded, and the circuit can now close (FD_CLOSE).
    void note_stream_connected(u32 guest_socket);

    /// True when the peer has shut down its send side and no bytes remain.
    [[nodiscard]] static bool peer_shut_down(int native_fd);
    /// WSA error of a nonblocking connect. Darwin clears SO_ERROR when it is
    /// read, so a failure is latched in SocketState::connect_error for every
    /// later reader (FD_CONNECT, getsockopt). @p still_pending is set while
    /// Darwin reports EINPROGRESS.
    [[nodiscard]] u32 latch_connect_error(u32 guest_socket, int native_fd,
                                          bool* still_pending);

    /// Remove event association for a socket or event
    void remove_event_assoc_by_socket(u32 guest_socket);
    void remove_event_assoc_by_event(u32 event_handle);

    /// Get a stable copy of the event association for a socket.
    std::optional<EventAssoc> get_event_assoc(u32 guest_socket) const;

    /// listen() succeeded. macOS SO_ACCEPTCONN is not readable via getsockopt.
    void mark_listening(u32 guest_socket);
    [[nodiscard]] bool is_listening(u32 guest_socket) const;

    /// A nonblocking connect is in progress: its completion records
    /// FD_CONNECT (with the connect's error) and, on success, FD_WRITE.
    void mark_pending_connect(u32 guest_socket);
    /// True if connect completion is still outstanding.
    [[nodiscard]] bool has_pending_connect(u32 guest_socket) const;


    /// Implement WSAWaitForMultipleEvents over WSA event handles. Socket
    /// associations are refreshed while waiting so WSAEventSelect readiness
    /// can signal the same manual-reset events observed by the wait.
    [[nodiscard]] u32 wait_for_multiple_events(
        std::span<const Handle> handles, bool wait_all, u32 milliseconds,
        bool alertable);
    
    /// Get thunk address for extension function (returns 0 if not found)
    GuestAddr get_extension_func(const std::string& name) const;

    /// WSAStartup — version negotiation; returns 0 or a Winsock error code
    /// (returned directly, not via WSAGetLastError).
    [[nodiscard]] int wsa_startup(u16 version_requested);
    /// WSACleanup — decrements the process startup count.
    [[nodiscard]] int wsa_cleanup();
    [[nodiscard]] bool winsock_ready() const noexcept {
        return m_startup_count.load(std::memory_order_acquire) > 0;
    }

    /// GetHostNameW — calls POSIX gethostname(), returns 0 on success, -1 on error
    [[nodiscard]] int get_hostname_w(char* hostname, size_t hostname_len);

    /// Thread-local WSA last error (same TEB LastErrorValue as GetLastError).
    u32 last_error() const noexcept {
        g_wsa_last_error = vitreo::winapi::get_last_error();
        return g_wsa_last_error;
    }
    void set_last_error(u32 err) noexcept {
        g_wsa_last_error = err;
        vitreo::winapi::set_last_error(err);
    }

    /// Guest address of static "0.0.0.0" string for inet_ntoa
    GuestAddr inet_ntoa_buf() const noexcept { return m_inet_ntoa_buf; }
    void set_inet_ntoa_buf(GuestAddr addr) noexcept { m_inet_ntoa_buf = addr; }

    // -------------------------------------------------------------------------
    // Socket Handle Management
    // -------------------------------------------------------------------------

    /// Allocate a guest socket handle for a native file descriptor
    /// @param native_fd Native BSD socket file descriptor
    /// @return Guest SOCKET handle (starting at 0x10000)
    u32 alloc_socket(int native_fd);

    /// Get the native fd for a guest socket handle
    /// @param guest_socket Guest SOCKET handle
    /// @return Native fd or -1 if not found
    int get_native_fd(u32 guest_socket) const;

    /// Close and remove a socket from the map
    /// @param guest_socket Guest SOCKET handle to close
    /// @return true if found and closed, false if not found
    /// @param reset WSACleanup: the connection is reset (Learn WSACleanup),
    ///        so pending I/O on a connected stream completes with
    ///        STATUS_CONNECTION_RESET instead of STATUS_CONNECTION_ABORTED
    ///        and the peer receives a reset.
    bool close_socket(u32 guest_socket, bool reset = false);

    /// Shutdown all open sockets (wakes threads blocked in recv/select)
    void shutdown_all_sockets();

    /// Every live guest SOCKET (WSACleanup deallocates them all).
    [[nodiscard]] std::vector<u32> socket_handles() const;


    /// Replace the native fd for a guest socket handle (overlapped I/O and
    /// AcceptEx)
    void replace_native_fd(u32 guest_socket, int new_fd);

    /// Guest-visible per-socket state Winsock keeps that the host socket does
    /// not (or keeps with different numbers/semantics): option values measured
    /// in tools/oracle_probe/ws2_opts_probe.expected, the sticky connect error
    /// SO_ERROR reports, shutdown directions, and connect time.
    struct SocketState {
        bool reuseaddr = false;
        bool exclusiveaddruse = false;
        u32 rcvtimeo_ms = 0;
        u32 sndtimeo_ms = 0;
        u16 linger_onoff = 0;       // stored raw, as Windows does (0x12c0007)
        u16 linger_seconds = 0;
        u32 sndbuf = 0x10000;        // Windows AFD default (tcp/udp_SO_SNDBUF)
        u32 rcvbuf = 0x10000;
        u32 connect_error = 0;       // WSA code of a failed nonblocking connect
        bool connect_in_progress = false;
        bool shut_rd = false;
        bool shut_wr = false;
        i64 connect_time_ms = -1;    // steady-clock ms when connected
        u32 recv_ctl = 0;            // ws2ctl:: WSARecvMsg control options
        std::unordered_map<u32, u32> emulated;  // (level<<16|optname) -> value
        // A stream socket that has been connected (connect/accept succeeded):
        // shutdown stays a no-op success after the host drops the connection
        // (ws2_corex_probe tcp_shut_after_peer_closed_again).
        bool was_connected = false;
        // The local side aborted the connection (Winsock resets a stream when
        // received data cannot be copied to the caller's buffer, or when data
        // arrives after SD_RECEIVE): recv/send then fail WSAECONNABORTED
        // (ws2_corex_probe data_null_*; ws2_core_probe tcp_recv_*).
        bool aborted = false;
        // Host sockaddr of the peer when the connection was aborted:
        // getpeername keeps reporting it (ws2_corex_probe
        // data_getpeername_after_abort: rc=0).
        std::vector<u8> aborted_peer;
        // connect() bound the socket implicitly: SO_BSP_STATE reports the
        // wildcard address it was bound to (ws2_corex_probe bsp_tcp_connected).
        bool bound_by_connect = false;
    };
    [[nodiscard]] SocketState socket_state(u32 guest_socket) const;
    /// Learn accept / SO_UPDATE_ACCEPT_CONTEXT: an accepted socket has the
    /// listener's properties. Copies the guest-visible option state.
    void inherit_listener_state(u32 listener, u32 accepted);
    void update_socket_state(u32 guest_socket,
                             const std::function<void(SocketState&)>& fn);
    /// WSADuplicateSocket to this process: keep a host duplicate of the
    /// socket under a nonzero token (WSAPROTOCOL_INFO.dwProviderReserved).
    [[nodiscard]] u32 register_duplicated_socket(u32 guest_socket, int dup_fd);
    /// WSASocket(FROM_PROTOCOL_INFO) with a token: a fresh host duplicate of
    /// the registered socket and the source's state, or -1 for an unknown
    /// token. The registration stays usable (ws2_corex_probe dup_second_use).
    [[nodiscard]] int take_duplicated_socket(u32 token, SocketState* state);
    /// Final WSACleanup: drop every registration.
    void clear_duplicated_sockets();

    /// Convert errno to WSA error code
    static u32 errno_to_wsa(int err);

    // -------------------------------------------------------------------------
    // Scratch Buffer for DNS Resolution
    // -------------------------------------------------------------------------

    /// Scratch buffer bump allocator - allocates from WS2_SCRATCH_BASE region
    /// @param size Number of bytes to allocate (will be 4-byte aligned)
    /// @return Guest address of allocated memory, or 0 if out of space
    GuestAddr scratch_alloc(u32 size) noexcept;

    /// Reset scratch allocator (call between major operations if needed)
    void scratch_reset() noexcept { m_scratch_offset = 0; }

    /// Durable getaddrinfo arena — results remain valid until freeaddrinfo.
    GuestAddr addrinfo_alloc(u32 size) noexcept;
    void addrinfo_free(GuestAddr root) noexcept;

    // Overlapped WSARecv/WSASend completion is queued from free-function
    // thunks in ws2_32.cpp; keep the tracker API visible to those thunks.
    struct PendingSocketIo {
        u32 guest_socket = 0;
        GuestAddr overlapped = 0;
        u32 issuing_tid = 0;
        GuestAddr completion_routine = 0;
        bool is_64bit = false;
        bool cancel = false;
    };
    std::mutex m_overlapped_socket_io_mutex;
    ReapedWorkers m_overlapped_socket_io_workers;
    std::unordered_map<GuestAddr, PendingSocketIo> m_pending_socket_io;
    /// Issue-order queue of pending stream WSARecv OVERLAPPEDs per SOCKET so
    /// data is delivered FIFO (Learn stream contract). Datagrams are unordered.
    std::unordered_map<u32, std::deque<GuestAddr>> m_stream_recv_fifo;
    void track_pending_socket_io(u32 guest_socket, GuestAddr overlapped,
                                 u32 issuing_tid = 0,
                                 GuestAddr completion_routine = 0,
                                 bool is_64bit = false);
    void enqueue_stream_recv(u32 guest_socket, GuestAddr overlapped);
    void dequeue_stream_recv(u32 guest_socket, GuestAddr overlapped);
    [[nodiscard]] bool is_stream_recv_head(u32 guest_socket,
                                           GuestAddr overlapped) const;
    [[nodiscard]] std::optional<bool> consume_pending_socket_io(
        GuestAddr overlapped);
    /// consume_pending_socket_io with m_overlapped_socket_io_mutex already held.
    [[nodiscard]] std::optional<bool> consume_pending_socket_io_locked(
        GuestAddr overlapped);
    void cancel_pending_socket_io_for_socket(u32 guest_socket);
    /// CancelIo / CancelIoEx for a SOCKET: mark matching pending ops.
    /// @param overlapped 0 = all on the socket (CancelIoEx NULL / CancelIo)
    /// @param only_calling_thread CancelIo: only ops issued by calling_tid
    /// @return number of ops marked cancelled
    [[nodiscard]] usize cancel_overlapped_socket_io(
        u32 guest_socket, GuestAddr overlapped, bool only_calling_thread,
        u32 calling_tid);

private:
    GuestAddr m_inet_ntoa_buf = 0;

    // Scratch allocator state (gethostbyname / gethostbyaddr)
    u32 m_scratch_offset = 0;

    // Durable getaddrinfo allocator (separate from hostent scratch)
    mutable std::mutex m_addrinfo_mutex;
    u32 m_addrinfo_offset = 0;
    std::unordered_map<GuestAddr, u32> m_addrinfo_live; // root -> size

    // Socket handle management
    mutable std::mutex m_socket_mutex;
    std::unordered_map<u32, int> m_socket_map;  // guest handle -> native fd
    std::unordered_map<u32, SocketState> m_socket_state;
    u32 m_next_socket = 0x10000;                 // first guest socket handle
    struct DuplicatedSocket { int fd = -1; SocketState state; };
    std::unordered_map<u32, DuplicatedSocket> m_duplicated;  // token -> dup
    u32 m_next_dup_token = 0x5D000001;
    
    // Thunk registry for extension function lookups
    ThunkRegistry* m_thunk_registry = nullptr;

    HeapManager* m_heap = nullptr;
    std::function<std::string()> m_host_name_provider;
    struct DbBlockSlot { GuestAddr addr = 0; u32 size = 0; };
    std::mutex m_db_block_mutex;
    std::unordered_map<u64, DbBlockSlot> m_db_blocks;  // (tid<<8)|kind

    // Sync manager for WSA event functions
    SyncManager* m_sync_manager = nullptr;
    FileManager* m_file_manager = nullptr;

    GuestMemory* m_memory = nullptr;
    OverlappedCompletionCallback m_overlapped_completion;
    NtCompletionCallback m_nt_completion;
    /// Requests issued through NtDeviceIoControlFile, keyed by their
    /// IO_STATUS_BLOCK (which is also the socket I/O's tracking key).
    struct NtRequestRoute {
        GuestAddr apc_context = 0;
        Handle event = 0;
        GuestAddr apc_routine = 0;
        u32 issuing_tid = 0;
        bool issuing = true;    // the NtDeviceIoControlFile call has not returned yet
        bool completed = false; // it completed while being issued
        u32 status = 0;
    };
    mutable std::mutex m_nt_route_mutex;
    mutable std::unordered_map<GuestAddr, NtRequestRoute> m_nt_routes;
    WsaCompletionRoutineQueue m_wsa_completion_queue;
    PostMessageFn m_post_message;
    std::atomic<u32> m_startup_count{0};

    // WSAAsyncGet* task handles (nonzero HANDLE values).
    mutable std::mutex m_async_mutex;
    std::atomic<u32> m_next_async_handle{1};
    struct AsyncTask {
        bool cancelled = false;
        bool completed = false;
    };
    std::unordered_map<u32, AsyncTask> m_async_tasks;

    // WSAEventSelect provider. m_event_mutex is taken before m_socket_mutex.
    mutable std::mutex m_event_mutex;
    std::unordered_map<u32, EventAssoc> m_event_assocs;  // guest socket -> event assoc
    /// What the provider has observed of one socket, independent of any
    /// association: the triggers a network event is recorded on.
    struct EventSource {
        bool stream = false;
        bool connected = false;     // a stream circuit exists (FD_CLOSE can occur)
        bool write_owed = false;    // FD_WRITE due: connected/accepted, or space after WSAEWOULDBLOCK
        bool send_blocked = false;  // a send failed WSAEWOULDBLOCK
        bool read_reenabled = false;
        bool accept_reenabled = false;
        bool oob_seen = false;
        bool close_seen = false;    // the peer's close was observed (FD_CLOSE once)
        u32 close_error = 0;
        u64 seen_readable = 0;      // bytes buffered at the last observation
        u64 seen_backlog = 0;       // pending connections at the last observation
    };
    std::unordered_map<u32, EventSource> m_event_sources;
    // Guest stream sockets whose nonblocking connect is in progress.
    std::unordered_set<u32> m_pending_connect;
    std::unordered_set<u32> m_listening;

    // Background pump: Windows signals WSAEVENT asynchronously. Callers use
    // RegisterWaitForSingleObject and never enter WSAWaitForMultipleEvents,
    // so the pump must SetEvent without a wait caller.
    std::mutex m_event_pump_mutex;
    std::condition_variable m_event_pump_cv;
    std::thread m_event_pump_thread;
    std::atomic<bool> m_event_pump_stop{false};
    bool m_event_pump_started = false;

    void ensure_event_pump_locked();  // caller holds m_event_mutex
    void stop_event_pump();
    void event_pump_loop();

    /// The socket's provider state, created on first use: a stream socket
    /// already connected owes FD_WRITE (its connect or accept was not seen).
    EventSource& event_source_locked(u32 guest_socket, int native_fd);
    /// Record the events of @p assoc whose trigger occurred since the last
    /// observation (@p selecting: the WSAEventSelect call itself, when every
    /// condition that holds is recorded). Returns the newly recorded bits.
    u32 observe_socket_locked(u32 guest_socket, EventAssoc& assoc,
                              bool selecting);
    /// Observe and signal: the event of every association whose record gained
    /// an event. Caller holds m_event_mutex.
    void observe_and_signal_locked(u32 guest_socket, EventAssoc& assoc,
                                   bool selecting);

    /// Record the network events of every association whose event is in
    /// @p handles. Returns true when at least one has a socket association.
    bool refresh_socket_events(std::span<const Handle> handles);
    /// Refresh every WSAEventSelect association (event-pump / async path).
    void refresh_all_socket_events();
};

/// Create ws2_32.dll export thunks
DllExports create_ws2_thunks(Ws2Manager& mgr);

/// AcceptEx / GetAcceptExSockaddrs — also exported from mswsock.dll.
/// Callers delay-load these by name from mswsock; WSAIoctl retrieves the
/// same implementations from ws2_32. Keep one body, two export tables.
void ws2_AcceptEx(Marshaler& m, Ws2Manager& mgr);
void ws2_GetAcceptExSockaddrs(Marshaler& m, Ws2Manager& mgr);

/// Create wsock32.dll (Winsock 1.1) export thunks.
///
/// wsock32 shares ws2_32's BSD-socket implementation but exports it under a
/// DIFFERENT ordinal numbering: in wsock32 ordinal 10=inet_addr, 11=inet_ntoa,
/// 12=ioctlsocket, whereas ws2_32 uses 10=ioctlsocket, 11=inet_addr,
/// 12=inet_ntoa. Registering the ws2_32 table for wsock32 mis-resolves any
/// import-by-ordinal of those three. This factory registers the correct
/// wsock32 ordinals against the shared Ws2Manager.
DllExports create_wsock32_thunks(Ws2Manager& mgr);

} // namespace vitreo::winapi
