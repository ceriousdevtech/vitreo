// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_pipe_helpers.cpp
/// @brief Shared pipe helpers for FileManager translation units.

#include "file_pipe_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "ipc/ipc_name.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace vitreo::winapi {

namespace {
// An optional process-specific output prevents inherited stderr streams from
// interleaving payload bytes across guest processes. FILE's lock protects the
// entire record within one process, including multiple guest worker threads.
FILE* pipe_diag_output() {
    static FILE* output = [] {
        const char* configured = std::getenv("VITREO_PIPE_DIAG_FILE");
        if (!configured || !*configured) return stderr;
        std::string path(configured);
        if (path.find("%p") == std::string::npos) {
            std::fprintf(stderr, "[vitreo] VITREO_PIPE_DIAG_FILE requires %%p\n");
            std::abort();
        }
        const auto pid = std::to_string(static_cast<long long>(::getpid()));
        for (usize offset = 0; (offset = path.find("%p", offset)) != std::string::npos; offset += pid.size())
            path.replace(offset, 2, pid);
        FILE* file = std::fopen(path.c_str(), "w");
        if (!file) {
            std::fprintf(stderr, "[vitreo] cannot open pipe diagnostic output: %s\n", path.c_str());
            std::abort();
        }
        return file; // Process-lifetime diagnostic output, like stderr.
    }();
    return output;
}
} // namespace

bool pipe_diag_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_PIPE_DIAG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void pipe_diag_lock() {
    // Serialize each record within the process. The advisory lock also helps
    // independent output descriptions; inherited stderr descriptions need the
    // optional per-process output above to prevent cross-process interleaving.
    ::flockfile(pipe_diag_output());
    (void)::flock(::fileno(pipe_diag_output()), LOCK_EX);
}

void pipe_diag_unlock() {
    (void)::flock(::fileno(pipe_diag_output()), LOCK_UN);
    ::funlockfile(pipe_diag_output());
}

void pipe_diag(const char* operation, Handle handle, u32 requested,
               isize result, GuestAddr overlapped) {
    if (!pipe_diag_enabled()) return;
    const u32 gle = get_last_error();
    pipe_diag_lock();
    std::fprintf(
        pipe_diag_output(),
        "[pipe-diag pid=%d] %s h=0x%x requested=%u result=%lld "
        "overlapped=0x%llx gle=%u%s\n",
        static_cast<int>(::getpid()), operation,
        static_cast<unsigned>(handle), requested,
        static_cast<long long>(result),
        static_cast<unsigned long long>(overlapped), gle,
        gle == error_codes::ERROR_BROKEN_PIPE ? " ERROR_BROKEN_PIPE" : "");
    std::fflush(pipe_diag_output());
    pipe_diag_unlock();
}

void pipe_diag_payload(const char* operation, Handle handle,
                       const u8* data, usize len) {
    if (!pipe_diag_enabled() || data == nullptr || len == 0) return;
    static const bool metadata_only = [] {
        const char* value = std::getenv("VITREO_PIPE_DIAG");
        return value != nullptr && std::strcmp(value, "metadata") == 0;
    }();
    if (metadata_only) return;
    // Dump full channel coalesces (a ping frame is 1832 bytes) so every
    // accept frame's header remains visible; larger payloads still
    // get a leading trunc preview.
    static const usize kMax = [] {
        const char* value = std::getenv("VITREO_PIPE_DIAG_BYTES");
        if (!value || !*value) return usize{2048};
        char* end = nullptr;
        errno = 0;
        const auto parsed = std::strtoull(value, &end, 0);
        if (*value == '-' || end == value || *end || errno == ERANGE) {
            std::fprintf(pipe_diag_output(), "[vitreo] invalid VITREO_PIPE_DIAG_BYTES\n");
            std::abort();
        }
        return static_cast<usize>(std::min<unsigned long long>(parsed, 65536));
    }();
    const usize n = len < kMax ? len : kMax;
    // Ping frame name hash.
    constexpr u8 kPingHash[4] = {0xc3, 0xfe, 0xb3, 0x39};
    bool has_ping = false;
    if (len >= sizeof(kPingHash)) {
        for (usize i = 0; i + sizeof(kPingHash) <= len; ++i) {
            if (std::memcmp(data + i, kPingHash, sizeof(kPingHash)) == 0) {
                has_ping = true;
                break;
            }
        }
    }
    pipe_diag_lock();
    if (has_ping) {
        std::fprintf(pipe_diag_output(),
                     "[pipe-diag pid=%d] %s-payload h=0x%x len=%zu "
                     "contains=ChildProcessHost::Ping\n",
                     static_cast<int>(::getpid()), operation,
                     static_cast<unsigned>(handle), len);
    }
    std::fprintf(pipe_diag_output(),
                 "[pipe-diag pid=%d] %s-payload h=0x%x len=%zu%s:",
                 static_cast<int>(::getpid()), operation,
                 static_cast<unsigned>(handle), len,
                 len > kMax ? " (trunc)" : "");
    for (usize i = 0; i < n; ++i) {
        if ((i % 32) == 0) std::fprintf(pipe_diag_output(), "\n  ");
        std::fprintf(pipe_diag_output(), "%02x%s", data[i], (i + 1 < n) ? " " : "");
    }
    std::fputc('\n', pipe_diag_output());
    std::fflush(pipe_diag_output());
    pipe_diag_unlock();
}

[[nodiscard]] Handle allocate_pipe_endpoint(
    HandleTable& handles, PipeEndpointHolder endpoint, u32 flags,
    u32 access) {
    auto* holder = new PipeEndpointHolder(std::move(endpoint));
    return handles.allocate(HandleType::Pipe, holder, flags, access);
}

[[nodiscard]] std::shared_ptr<ipc::UnixPipe> pipe_io(
    const PipeEndpointHolder& ep) {
    if (!ep) return nullptr;
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (ep->conn) return ep->conn;
    if (ep->sock && !ep->sock->is_server()) return ep->sock;
    // A new server instance is listening from CreateNamedPipe on: a client
    // may connect before (or without) ConnectNamedPipe, and the server can
    // then read and write at once (ConnectNamedPipe would report
    // ERROR_PIPE_CONNECTED). Adopt such a queued client here. Not after
    // DisconnectNamedPipe: that instance takes no client until the next
    // ConnectNamedPipe (Learn, DisconnectNamedPipe / ConnectNamedPipe).
    if (ep->sock && ep->sock->is_server() && !ep->closed &&
        !ep->connect_pending && !ep->disconnected) {
        if (auto queued = ep->sock->accept(/*timeout_ms=*/0)) {
            ep->conn = std::move(queued);
            return ep->conn;
        }
    }
    return nullptr;
}

[[nodiscard]] u32 unconnected_pipe_error(const PipeEndpointHolder& ep) {
    // Server instance without a client: ERROR_PIPE_LISTENING while waiting
    // for its first client, ERROR_PIPE_NOT_CONNECTED after
    // DisconnectNamedPipe (winerror.h; Learn, ReadFile / DisconnectNamedPipe).
    if (!ep) return error_codes::ERROR_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (!ep->sock || !ep->sock->is_server() || ep->closed) {
        return error_codes::ERROR_INVALID_HANDLE;
    }
    return ep->disconnected ? 233u /*ERROR_PIPE_NOT_CONNECTED*/
                            : 536u /*ERROR_PIPE_LISTENING*/;
}

[[nodiscard]] std::string pipe_metadata_path(std::string_view win_name) {
    return ipc::posix_pipe_path(std::string(win_name)) + ".meta";
}

[[nodiscard]] bool write_all_fd(int fd, const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    usize offset = 0;
    while (offset < size) {
        const ssize_t written =
            ::write(fd, bytes + offset, size - offset);
        if (written > 0) {
            offset += static_cast<usize>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

[[nodiscard]] bool read_all_fd(int fd, void* data, usize size) {
    auto* bytes = static_cast<u8*>(data);
    usize offset = 0;
    while (offset < size) {
        const ssize_t received =
            ::read(fd, bytes + offset, size - offset);
        if (received > 0) {
            offset += static_cast<usize>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

[[nodiscard]] bool publish_pipe_metadata(
    std::string_view win_name, const NamedPipeConfig& config) {
    PipeMetadata metadata{};
    metadata.message_type = config.message_type ? 1u : 0u;
    metadata.input_buffer_size = config.input_buffer_size;
    metadata.output_buffer_size = config.output_buffer_size;
    metadata.max_instances = config.max_instances;
    metadata.default_timeout_ms = config.default_timeout_ms;

    const std::string path = pipe_metadata_path(win_name);
    const int fd = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write_all_fd(fd, &metadata, sizeof(metadata));
    const int close_result = ::close(fd);
    return ok && close_result == 0;
}

[[nodiscard]] std::optional<NamedPipeConfig> read_pipe_metadata(
    std::string_view win_name) {
    const std::string path = pipe_metadata_path(win_name);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;

    PipeMetadata metadata{};
    const bool ok = read_all_fd(fd, &metadata, sizeof(metadata));
    (void)::close(fd);
    if (!ok || metadata.magic != PIPE_METADATA_MAGIC ||
        metadata.version != PIPE_METADATA_VERSION ||
        metadata.message_type > 1u ||
        metadata.max_instances == 0) {
        return std::nullopt;
    }

    NamedPipeConfig config{};
    config.message_type = metadata.message_type != 0;
    // CreateFile always opens a client handle in byte-read, blocking mode.
    config.message_read_mode = false;
    config.blocking = true;
    config.input_buffer_size = metadata.input_buffer_size;
    config.output_buffer_size = metadata.output_buffer_size;
    config.max_instances = metadata.max_instances;
    config.default_timeout_ms = metadata.default_timeout_ms;
    return config;
}

[[nodiscard]] bool read_exact_pipe(
    const std::shared_ptr<ipc::UnixPipe>& io, void* data, usize size) {
    auto* bytes = static_cast<u8*>(data);
    usize offset = 0;
    while (offset < size) {
        const isize received = io->read(bytes + offset, size - offset);
        if (received <= 0) return false;
        offset += static_cast<usize>(received);
    }
    return true;
}

[[nodiscard]] bool write_exact_pipe(
    const std::shared_ptr<ipc::UnixPipe>& io,
    const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    usize offset = 0;
    while (offset < size) {
        const isize written = io->write(bytes + offset, size - offset);
        if (written <= 0) return false;
        offset += static_cast<usize>(written);
    }
    return true;
}

[[nodiscard]] bool write_message_frame(
    const std::shared_ptr<ipc::UnixPipe>& io,
    const u8* data, u32 size) {
    const std::array<u8, 4> header{
        static_cast<u8>(size & 0xFFu),
        static_cast<u8>((size >> 8u) & 0xFFu),
        static_cast<u8>((size >> 16u) & 0xFFu),
        static_cast<u8>((size >> 24u) & 0xFFu),
    };
    if (!write_exact_pipe(io, header.data(), header.size())) {
        return false;
    }
    return size == 0 || write_exact_pipe(io, data, size);
}

[[nodiscard]] bool receive_message_frame(
    PipeEndpoint& endpoint,
    const std::shared_ptr<ipc::UnixPipe>& io) {
    std::array<u8, 4> header{};
    if (!read_exact_pipe(io, header.data(), header.size())) return false;
    const u32 size =
        static_cast<u32>(header[0]) |
        (static_cast<u32>(header[1]) << 8u) |
        (static_cast<u32>(header[2]) << 16u) |
        (static_cast<u32>(header[3]) << 24u);
    if (size > PIPE_MESSAGE_FRAME_LIMIT) return false;

    endpoint.pending_message.resize(size);
    endpoint.pending_message_offset = 0;
    endpoint.pending_message_loaded = true;
    if (size == 0) return true;
    if (read_exact_pipe(io, endpoint.pending_message.data(), size)) {
        return true;
    }
    endpoint.pending_message.clear();
    endpoint.pending_message_loaded = false;
    return false;
}

[[nodiscard]] usize pipe_available_locked(const PipeState& st) {
    if (st.read_pos >= st.buffer.size()) return 0;
    return st.buffer.size() - st.read_pos;
}


std::string encode_pipe_state(const PipeEndpoint& ep) {
    std::string out = "{";
    out += std::to_string(static_cast<unsigned>(ep.kind));
    out += ':';
    out += std::to_string(static_cast<unsigned>(ep.pipe_type));
    out += ':';
    out += std::to_string(static_cast<unsigned>(ep.read_mode));
    out += ':';
    out += (ep.blocking ? "1" : "0");
    out += ':';
    out += std::to_string(ep.input_buffer_size);
    out += ':';
    out += std::to_string(ep.output_buffer_size);
    out += ':';
    out += std::to_string(ep.max_instances);
    out += ':';
    out += std::to_string(ep.default_timeout_ms);
    out += ':';
    out += std::to_string(ep.server_process_id);
    out += ':';
    out += std::to_string(ep.file_mode);
    out += "}";
    return out;
}

std::string_view decode_pipe_state(std::string_view field, PipeEndpoint& ep) {
    if (field.empty() || field.front() != '{') return field;
    const usize close = field.find('}');
    if (close == std::string_view::npos) return field;
    const std::string_view body = field.substr(1, close - 1);
    unsigned values[10]{};
    usize index = 0;
    usize pos = 0;
    while (index < 10 && pos <= body.size()) {
        const usize sep = body.find(':', pos);
        const std::string_view part = body.substr(
            pos, sep == std::string_view::npos ? body.size() - pos : sep - pos);
        if (part.empty()) break;
        values[index++] = static_cast<unsigned>(
            std::strtoul(std::string(part).c_str(), nullptr, 10));
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    if (index >= 8) {
        ep.kind = static_cast<PipeKind>(values[0]);
        ep.pipe_type = static_cast<PipeDataMode>(values[1]);
        ep.read_mode = static_cast<PipeDataMode>(values[2]);
        ep.blocking = values[3] != 0;
        ep.input_buffer_size = values[4];
        ep.output_buffer_size = values[5];
        ep.max_instances = values[6];
        ep.default_timeout_ms = values[7];
        if (index >= 9) {
            ep.server_process_id = values[8];
        }
        if (index >= 10) ep.file_mode = values[9];
    }
    return field.substr(close + 1);
}

} // namespace vitreo::winapi
