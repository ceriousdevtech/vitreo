// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file file_pipe_internal.hpp
/// @brief Shared pipe endpoint types and helpers for FileManager TUs.

#include "winapi/kernel32/filesystem/file.hpp"
#include "ipc/unix_pipe.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

[[nodiscard]] bool pipe_diag_enabled();
void pipe_diag_lock();
void pipe_diag_unlock();
void pipe_diag(const char* operation, Handle handle, u32 requested,
               isize result, GuestAddr overlapped = 0);
void pipe_diag_payload(const char* operation, Handle handle,
                       const u8* data, usize len);

struct PipeState {
    std::mutex mutex;
    std::vector<u8> buffer;
    usize read_pos{0};
    bool read_open{true};
    bool write_open{true};
};

/// Which end of a Windows pipe an endpoint represents. GetNamedPipeInfo
/// reports this, and it is not derivable from the transport alone: a connected
/// AF_UNIX stream looks identical whether it was accepted by a server or opened
/// by a client.
enum class PipeKind : u8 {
    Anonymous,
    NamedServer,
    NamedClient,
};

/// Windows byte-type vs message-type pipe semantics.
enum class PipeDataMode : u8 {
    Byte,
    Message,
};

/// An overlapped WriteFile that returned ERROR_IO_PENDING. @p wire is the
/// snapshot of the caller's buffer (with its message frame header), of which
/// the first @p offset bytes are already in the transport.
struct QueuedPipeWrite {
    Handle handle{0};
    GuestAddr overlapped{0};
    u32 bytes_to_write{0};
    bool is_64bit{false};
    std::vector<u8> wire;
    usize offset{0};
};

struct PipeEndpoint {
    std::mutex mutex;
    std::mutex read_io_mutex;
    /// Guards queued_writes. Pended writes reach the transport in issue
    /// order; a new overlapped write is sent inline only when none is queued.
    std::mutex write_io_mutex;
    std::deque<QueuedPipeWrite> queued_writes;
    std::shared_ptr<PipeState> state;
    bool readable{false};
    bool writable{false};

    // Reported by GetNamedPipeInfo and modified by SetNamedPipeHandleState.
    // AF_UNIX SOCK_STREAM is the transport for both kinds. Message-type named
    // pipes add an internal length frame around each WriteFile operation.
    PipeKind kind{PipeKind::Anonymous};
    PipeDataMode pipe_type{PipeDataMode::Byte};
    PipeDataMode read_mode{PipeDataMode::Byte};
    bool blocking{true};
    u32 file_mode{0x20}; // FILE_SYNCHRONOUS_IO_NONALERT; zero for overlapped.
    u32 input_buffer_size{4096};
    u32 output_buffer_size{4096};
    u32 max_instances{1};
    u32 default_timeout_ms{0};
    /// Remote-client read batching, settable via SetNamedPipeHandleState.
    u32 max_collection_count{0};
    u32 collect_data_timeout_ms{0};
    /// Cross-process named pipe (Stage 4).
    std::shared_ptr<ipc::UnixPipe> sock;
    std::shared_ptr<ipc::UnixPipe> conn;
    std::string win_name;
    bool connect_pending{false};
    /// Overlapped ConnectNamedPipe in flight: its OVERLAPPED, the issuing
    /// thread (CancelIo only cancels the caller's own requests), whether a
    /// cancel arrived, and a self-pipe that wakes the waiting accept.
    GuestAddr connect_overlapped{0};
    Handle connect_handle{0};
    u32 connect_issuer_tid{0};
    bool connect_cancelled{false};
    bool connect_closed{false};
    /// DisconnectNamedPipe ran; cleared by the next ConnectNamedPipe.
    bool disconnected{false};
    /// Number of the latest armed ConnectNamedPipe, and of the one a
    /// DisconnectNamedPipe ended while it was pending. A disconnected
    /// request's waiter may still be running when the next one is armed.
    u64 connect_generation{0};
    u64 connect_disconnected_generation{0};
    int connect_wake[2]{-1, -1};
    bool closed{false};
    u32 server_process_id{0};

    std::vector<u8> pending_message;
    usize pending_message_offset{0};
    bool pending_message_loaded{false};
};

using PipeEndpointHolder = std::shared_ptr<PipeEndpoint>;

[[nodiscard]] Handle allocate_pipe_endpoint(
    HandleTable& handles, PipeEndpointHolder endpoint, u32 flags = 0,
    u32 access = HandleAccess::All);

[[nodiscard]] std::shared_ptr<ipc::UnixPipe> pipe_io(
    const PipeEndpointHolder& ep);
/// Win32 error for I/O on a pipe endpoint pipe_io() found no transport for.
[[nodiscard]] u32 unconnected_pipe_error(const PipeEndpointHolder& ep);

constexpr u32 PIPE_METADATA_MAGIC = 0x5654504Du;  // "VTPM"
constexpr u32 PIPE_METADATA_VERSION = 1;
constexpr u32 PIPE_MESSAGE_FRAME_LIMIT = 64u * 1024u * 1024u;

struct PipeMetadata {
    u32 magic{PIPE_METADATA_MAGIC};
    u32 version{PIPE_METADATA_VERSION};
    u32 message_type{0};
    u32 input_buffer_size{4096};
    u32 output_buffer_size{4096};
    u32 max_instances{1};
    u32 default_timeout_ms{0};
};

[[nodiscard]] std::string pipe_metadata_path(std::string_view win_name);
[[nodiscard]] bool write_all_fd(int fd, const void* data, usize size);
[[nodiscard]] bool read_all_fd(int fd, void* data, usize size);
[[nodiscard]] bool publish_pipe_metadata(
    std::string_view win_name, const NamedPipeConfig& config);
[[nodiscard]] std::optional<NamedPipeConfig> read_pipe_metadata(
    std::string_view win_name);
[[nodiscard]] bool read_exact_pipe(
    const std::shared_ptr<ipc::UnixPipe>& io, void* data, usize size);
[[nodiscard]] bool write_exact_pipe(
    const std::shared_ptr<ipc::UnixPipe>& io,
    const void* data, usize size);
[[nodiscard]] bool write_message_frame(
    const std::shared_ptr<ipc::UnixPipe>& io,
    const u8* data, u32 size);
[[nodiscard]] bool receive_message_frame(
    PipeEndpoint& endpoint,
    const std::shared_ptr<ipc::UnixPipe>& io);
[[nodiscard]] usize pipe_available_locked(const PipeState& st);


[[nodiscard]] std::string encode_pipe_state(const PipeEndpoint& ep);
[[nodiscard]] std::string_view decode_pipe_state(std::string_view field, PipeEndpoint& ep);

constexpr u32 PIPE_NOWAIT_FLAG = 0x00000001;
constexpr u32 PIPE_READMODE_MESSAGE_FLAG = 0x00000002;
constexpr u32 PIPE_MODE_SUPPORTED_MASK =
    PIPE_READMODE_MESSAGE_FLAG | PIPE_NOWAIT_FLAG;

} // namespace vitreo::winapi
