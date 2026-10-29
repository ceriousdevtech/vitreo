/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/common/object_access.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include <poll.h>
#include "winapi/kernel32/filesystem/file_ops.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/filesystem/file_thunks_internal.hpp"
#include "winapi/kernel32/filesystem/file_profile_internal.hpp"
#include "winapi/kernel32/filesystem/file_path_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

namespace vitreo::winapi {

// Shared VfsError -> Win32 mapping. Lives here (not in an anonymous
// namespace) so every kernel32 file API uses one table; ad-hoc copies in
// file_ops.cpp had drifted and collapsed PathNotFound onto FILE_NOT_FOUND.
[[nodiscard]] u32 vfs_error_to_win32(capsule::VfsError err) noexcept {
    using capsule::VfsError;
    using namespace error_codes;
    switch (err) {
        case VfsError::None:              return ERROR_SUCCESS;
        case VfsError::PathNotFound:      return ERROR_PATH_NOT_FOUND;
        case VfsError::FileNotFound:      return ERROR_FILE_NOT_FOUND;
        case VfsError::PathAlreadyExists: return ERROR_FILE_EXISTS;
        case VfsError::InvalidPath:       return ERROR_INVALID_NAME;
        case VfsError::NotADirectory:     return ERROR_PATH_NOT_FOUND;
        case VfsError::NotAFile:          return ERROR_FILE_NOT_FOUND;
        case VfsError::DirectoryNotEmpty: return ERROR_DIR_NOT_EMPTY;
        case VfsError::AccessDenied:      return ERROR_ACCESS_DENIED;
        case VfsError::SharingViolation:  return ERROR_SHARING_VIOLATION;
        case VfsError::InvalidHandle:     return ERROR_INVALID_HANDLE;
        case VfsError::HandleClosed:      return ERROR_INVALID_HANDLE;
        case VfsError::TooManyOpenFiles:  return ERROR_TOO_MANY_OPEN_FILES;
        case VfsError::DiskFull:          return ERROR_DISK_FULL;
        case VfsError::OutOfMemory:       return ERROR_OUTOFMEMORY;
        case VfsError::InvalidArgument:   return ERROR_INVALID_PARAMETER;
        case VfsError::NotSameDevice:     return ERROR_NOT_SAME_DEVICE;
        case VfsError::NotSupported:      return ERROR_NOT_SUPPORTED;
        case VfsError::DriveNotMounted:   return ERROR_PATH_NOT_FOUND;
        case VfsError::LockViolation:     return ERROR_LOCK_VIOLATION;
        case VfsError::NotLocked:         return ERROR_NOT_LOCKED;
        default:                          return ERROR_GEN_FAILURE;
    }
}

namespace {
using debug::write_sha1_open;
using debug::write_sha1_update;
using debug::write_sha1_close;
using debug::read_sha1_open;
using debug::read_sha1_update;
using debug::read_sha1_close;
using debug::watch_open;
using debug::watch_close;
using debug::is_watched;
using debug::watched_path;

} // namespace


// =============================================================================
// CreateFile Core Implementation
// =============================================================================

[[nodiscard]] static bool contains_utf16le_ascii(
    const std::vector<u8>& bytes, std::string_view text) {
    if (text.empty() || bytes.size() < text.size() * 2) return false;
    for (usize offset = 0; offset + text.size() * 2 <= bytes.size();
         ++offset) {
        bool matches = true;
        for (usize i = 0; i < text.size(); ++i) {
            if (bytes[offset + i * 2] != static_cast<u8>(text[i]) ||
                bytes[offset + i * 2 + 1] != 0) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    return false;
}

Handle FileManager::create_named_pipe_server(const std::string& win_name,
                                             bool inheritable,
                                             const NamedPipeConfig& config) {
    auto srv = ipc::UnixPipe::create_server(win_name);
    if (!srv) return INVALID_HANDLE;
    if (!publish_pipe_metadata(win_name, config)) {
        ipc::ipc_log(
            "named pipe metadata publish failed name='%s' errno=%d (%s)",
            win_name.c_str(), errno, std::strerror(errno));
        return INVALID_HANDLE;
    }
    auto ep = std::make_shared<PipeEndpoint>();
    ep->readable = true;
    ep->writable = true;
    ep->sock = std::move(srv);
    ep->win_name = win_name;
    ep->kind = PipeKind::NamedServer;
    ep->pipe_type =
        config.message_type ? PipeDataMode::Message : PipeDataMode::Byte;
    ep->read_mode =
        config.message_read_mode ? PipeDataMode::Message : PipeDataMode::Byte;
    ep->blocking = config.blocking;
    ep->file_mode = config.overlapped ? 0 : 0x20;
    ep->input_buffer_size = config.input_buffer_size;
    ep->output_buffer_size = config.output_buffer_size;
    ep->max_instances = config.max_instances;
    ep->default_timeout_ms = config.default_timeout_ms;
    // GetCurrentProcessId is host getpid(); keep server identity consistent.
    ep->server_process_id = static_cast<u32>(::getpid());
    const u32 flags = inheritable ? HandleFlags::Inherit : 0;
    return allocate_pipe_endpoint(m_handles, std::move(ep), flags);
}

bool FileManager::wait_named_pipe(const std::string& win_name,
                                  u32 timeout_ms) {
    // MSDN: if no instances of the named pipe exist, WaitNamedPipe returns
    // FALSE immediately regardless of the timeout. When instances exist but
    // are busy, the call waits until one becomes available or the timeout
    // elapses (ERROR_SEM_TIMEOUT). Busy saturation is not modeled yet — an
    // existing AF_UNIX server is treated as an available instance.
    (void)timeout_ms;

    if (!is_named_pipe_path(win_name)) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }

    if (!read_pipe_metadata(win_name)) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return false;
    }
    const std::string path = ipc::posix_pipe_path(win_name);
    if (::access(path.c_str(), F_OK) != 0) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return false;
    }

    return true;
}



// Windows named-pipe mode bits.
namespace {

constexpr u32 PIPE_CLIENT_END_FLAG  = 0x00000000;
constexpr u32 PIPE_SERVER_END_FLAG  = 0x00000001;
constexpr u32 PIPE_TYPE_BYTE_FLAG   = 0x00000000;
constexpr u32 PIPE_TYPE_MESSAGE_FLAG = 0x00000004;
}  // namespace

bool FileManager::set_named_pipe_handle_state(
    Handle handle,
    const u32* mode,
    const u32* max_collection_count,
    const u32* collect_data_timeout,
    u32& error) {
    if (m_handles.get_type(handle) != HandleType::Pipe) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    auto ep = m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe);
    if (!ep) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }

    std::lock_guard<std::mutex> lock(ep->mutex);

    // Validate everything before applying anything, so a rejected request
    // leaves the endpoint untouched.
    PipeDataMode new_read_mode = ep->read_mode;
    bool new_blocking = ep->blocking;
    if (mode != nullptr) {
        const u32 requested = *mode;
        if ((requested & ~PIPE_MODE_SUPPORTED_MASK) != 0) {
            error = error_codes::ERROR_INVALID_PARAMETER;
            return false;
        }
        const bool wants_message =
            (requested & PIPE_READMODE_MESSAGE_FLAG) != 0;
        // A byte-type pipe cannot be read in message mode. Message-type
        // endpoints preserve WriteFile boundaries with Vitreo's transport
        // framing, so either read mode is valid for those endpoints.
        if (wants_message && ep->pipe_type != PipeDataMode::Message) {
            error = error_codes::ERROR_INVALID_PARAMETER;
            return false;
        }
        new_read_mode =
            wants_message ? PipeDataMode::Message : PipeDataMode::Byte;
        new_blocking = (requested & PIPE_NOWAIT_FLAG) == 0;
    }

    ep->read_mode = new_read_mode;
    ep->blocking = new_blocking;
    // lpMaxCollectionCount and lpCollectDataTimeout only govern remote-client
    // read batching. Every Vitreo pipe is local, where Windows accepts and
    // records them without changing transfer behavior; they are stored so a
    // subsequent query reports what the caller set rather than a default.
    if (max_collection_count != nullptr) {
        ep->max_collection_count = *max_collection_count;
    }
    if (collect_data_timeout != nullptr) {
        ep->collect_data_timeout_ms = *collect_data_timeout;
    }

    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool FileManager::named_pipe_handle_state(
    Handle handle,
    u32* state,
    u32* current_instances,
    u32* max_collection_count,
    u32* collect_data_timeout,
    u32& error) {
    if (m_handles.get_type(handle) != HandleType::Pipe) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    auto ep = m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe);
    if (!ep) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (state != nullptr) {
        // lpState carries only the two bits a caller can set: the read mode
        // and the wait mode. The pipe TYPE is reported by GetNamedPipeInfo.
        *state = 0;
        if (ep->read_mode == PipeDataMode::Message) {
            *state |= PIPE_READMODE_MESSAGE_FLAG;
        }
        if (!ep->blocking) *state |= PIPE_NOWAIT_FLAG;
    }
    if (current_instances != nullptr) {
        // Every Vitreo pipe is a single local instance pair.
        *current_instances = 1;
    }
    if (max_collection_count != nullptr) {
        *max_collection_count = ep->max_collection_count;
    }
    if (collect_data_timeout != nullptr) {
        *collect_data_timeout = ep->collect_data_timeout_ms;
    }
    error = error_codes::ERROR_SUCCESS;
    return true;
}

std::optional<u32> FileManager::pipe_file_mode(Handle handle) const {
    auto ep = m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe);
    if (!ep) return std::nullopt;
    std::lock_guard lock(ep->mutex);
    return ep->file_mode;
}

bool FileManager::named_pipe_info(Handle handle,
                                  u32* flags,
                                  u32* out_buffer_size,
                                  u32* in_buffer_size,
                                  u32* max_instances,
                                  u32& error) {
    if (m_handles.get_type(handle) != HandleType::Pipe) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    auto ep = m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe);
    if (!ep) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }

    std::lock_guard<std::mutex> lock(ep->mutex);
    if (flags != nullptr) {
        u32 value = (ep->kind == PipeKind::NamedClient)
                        ? PIPE_CLIENT_END_FLAG
                        : PIPE_SERVER_END_FLAG;
        value |= (ep->pipe_type == PipeDataMode::Message)
                     ? PIPE_TYPE_MESSAGE_FLAG
                     : PIPE_TYPE_BYTE_FLAG;
        *flags = value;
    }
    if (out_buffer_size != nullptr) *out_buffer_size = ep->output_buffer_size;
    if (in_buffer_size != nullptr) *in_buffer_size = ep->input_buffer_size;
    if (max_instances != nullptr) *max_instances = ep->max_instances;
    error = error_codes::ERROR_SUCCESS;
    return true;
}

std::optional<std::string> FileManager::prepare_transferred_handle(
    Handle source, u32 access, bool inheritable, int& out_fd) {
    out_fd = -1;
    const HandleType type = m_handles.get_type(source);
    if (type == HandleType::File) {
        const auto vfs_h = vfs_handle_for(source);
        if (vfs_h == capsule::INVALID_FILE_HANDLE) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
        }
        const u32 source_access = m_handles.get_access(source);
        if (source_access != HandleAccess::All && access != HandleAccess::All &&
            (access & ~source_access) != 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return std::nullopt;
        }
        auto fd_result = m_vfs.duplicate_native_fd(vfs_h);
        if (fd_result.is_err()) {
            set_last_error(vfs_error_to_win32(fd_result.error()));
            return std::nullopt;
        }
        out_fd = fd_result.value();
        const u32 flags = inheritable ? HandleFlags::Inherit : 0;
        std::string descriptor = "F,";
        descriptor += std::to_string(access);
        descriptor.push_back(',');
        descriptor += std::to_string(flags);
        ipc::ipc_log(
            "prepared runtime file transfer h=0x%x access=0x%x",
            static_cast<unsigned>(source), access);
        return descriptor;
    }
    if (type != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    auto ep = m_handles.get_shared<PipeEndpoint>(source, HandleType::Pipe);
    if (!ep) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    const u32 source_access = m_handles.get_access(source);
    if (source_access != HandleAccess::All && access != HandleAccess::All &&
        (access & ~source_access) != 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return std::nullopt;
    }

    int fd = -1;
    char form = 'A';
    {
        std::lock_guard<std::mutex> lock(ep->mutex);
        if (ep->conn) {
            fd = ep->conn->fd();
            form = 'A';
        } else if (ep->sock && ep->sock->is_server()) {
            fd = ep->sock->fd();
            form = 'N';
        } else if (ep->sock) {
            fd = ep->sock->fd();
            form = 'A';
        }
    }
    if (fd < 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }

    // Dup so DUPLICATE_CLOSE_SOURCE in the caller cannot invalidate the FD
    // before SCM_RIGHTS delivery completes.
    const int sent_fd = ::dup(fd);
    if (sent_fd < 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return std::nullopt;
    }
    out_fd = sent_fd;

    const u32 flags = inheritable ? HandleFlags::Inherit : 0;
    std::string descriptor = "P,";
    descriptor += form;
    descriptor.push_back(',');
    descriptor += ep->readable ? '1' : '0';
    descriptor.push_back(',');
    descriptor += ep->writable ? '1' : '0';
    descriptor.push_back(',');
    descriptor += std::to_string(access);
    descriptor.push_back(',');
    descriptor += std::to_string(flags);
    descriptor.push_back(',');
    descriptor += encode_pipe_state(*ep);
    descriptor += ep->win_name;
    ipc::ipc_log(
        "prepared runtime pipe transfer h=0x%x form=%c name='%s' access=0x%x",
        static_cast<unsigned>(source), form, ep->win_name.c_str(), access);
    return descriptor;
}

Handle FileManager::import_transferred_handle(std::string_view descriptor,
                                              int transferred_fd) {
    if (transferred_fd < 0) {
        return NULL_HANDLE;
    }
    if (descriptor.starts_with("F,")) {
        // F,<access>,<flags>
        const std::string_view body = descriptor.substr(2);
        const usize comma = body.find(',');
        if (comma == std::string_view::npos) {
            ::close(transferred_fd);
            return NULL_HANDLE;
        }
        const auto access = static_cast<u32>(
            std::stoul(std::string(body.substr(0, comma))));
        const auto flags = static_cast<u32>(
            std::stoul(std::string(body.substr(comma + 1))));
        auto adopted = m_vfs.adopt_native_fd(
            transferred_fd, to_access_mode(access), capsule::ShareMode::All,
            "transferred");
        if (adopted.is_err()) {
            ::close(transferred_fd);
            set_last_error(vfs_error_to_win32(adopted.error()));
            return NULL_HANDLE;
        }
        Handle win_handle = m_handles.allocate(
            HandleType::File, nullptr, flags, access);
        if (win_handle == NULL_HANDLE || win_handle == INVALID_HANDLE) {
            (void)m_vfs.close(adopted.value());
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return NULL_HANDLE;
        }
        {
            std::lock_guard<std::mutex> lock(m_file_path_mutex);
            m_vfs_handles[win_handle] = adopted.value();
        }
        ipc::ipc_log(
            "imported runtime file h=0x%x fd=%d access=0x%x",
            static_cast<unsigned>(win_handle), transferred_fd, access);
        return win_handle;
    }
    if (!descriptor.starts_with("P,")) {
        ::close(transferred_fd);
        return NULL_HANDLE;
    }
    // P,<form>,<r>,<w>,<access>,<flags>,{state}name
    const std::string_view body = descriptor.substr(2);
    constexpr auto npos = std::string_view::npos;
    const usize c1 = body.find(',');
    const usize c2 = c1 == npos ? npos : body.find(',', c1 + 1);
    const usize c3 = c2 == npos ? npos : body.find(',', c2 + 1);
    const usize c4 = c3 == npos ? npos : body.find(',', c3 + 1);
    const usize c5 = c4 == npos ? npos : body.find(',', c4 + 1);
    if (c1 != 1 || c2 == npos || c3 == npos || c4 == npos || c5 == npos) {
        ::close(transferred_fd);
        return NULL_HANDLE;
    }
    const char form = body[0];
    const bool readable = body.substr(c1 + 1, c2 - c1 - 1) == "1";
    const bool writable = body.substr(c2 + 1, c3 - c2 - 1) == "1";
    const auto access = static_cast<u32>(
        std::stoul(std::string(body.substr(c3 + 1, c4 - c3 - 1))));
    const auto flags = static_cast<u32>(
        std::stoul(std::string(body.substr(c4 + 1, c5 - c4 - 1))));

    auto ep = std::make_shared<PipeEndpoint>();
    ep->readable = readable;
    ep->writable = writable;
    ep->win_name = std::string(decode_pipe_state(body.substr(c5 + 1), *ep));
    if (form == 'N') {
        ep->sock = ipc::UnixPipe::adopt_server(transferred_fd, ep->win_name);
        if (ep->kind == PipeKind::Anonymous) ep->kind = PipeKind::NamedServer;
    } else if (form == 'A') {
        ep->conn = ipc::UnixPipe::adopt_connected(transferred_fd);
        if (ep->kind == PipeKind::Anonymous && !ep->win_name.empty()) {
            ep->kind = PipeKind::NamedClient;
        }
    } else {
        ::close(transferred_fd);
        return NULL_HANDLE;
    }
    if (!ep->sock && !ep->conn) return NULL_HANDLE;

    const Handle handle =
        allocate_pipe_endpoint(m_handles, ep, flags, access);
    if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
        return NULL_HANDLE;
    }
    ipc::ipc_log(
        "imported runtime pipe h=0x%x form=%c name='%s' fd=%d",
        static_cast<unsigned>(handle), form, ep->win_name.c_str(),
        transferred_fd);
    return handle;
}

namespace {
/// Learn (DuplicateHandle): a file handle cannot be duplicated with rights
/// its source lacks. Compared as granted rights, so FILE_READ_DATA is within
/// a GENERIC_READ source and GENERIC_READ within a FILE_GENERIC_READ one.
bool duplicate_access_allowed(u32 source_access, u32 requested) {
    if (source_access == HandleAccess::All) return true;
    const u32 granted = object_access::file_granted_access(source_access, HandleAccess::All);
    const u32 wanted = object_access::file_granted_access(requested, HandleAccess::All);
    return (wanted & ~granted) == 0;
}
}  // namespace

Handle FileManager::duplicate_handle(Handle source, u32 flags, u32 access) {
    const HandleType type = m_handles.get_type(source);
    if (type == HandleType::File) {
        const auto vfs_h = vfs_handle_for(source);
        if (vfs_h == capsule::INVALID_FILE_HANDLE) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        if (!duplicate_access_allowed(m_handles.get_access(source), access)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return NULL_HANDLE;
        }
        auto fd_result = m_vfs.duplicate_native_fd(vfs_h);
        if (fd_result.is_err()) {
            set_last_error(vfs_error_to_win32(fd_result.error()));
            return NULL_HANDLE;
        }
        auto adopted = m_vfs.adopt_native_fd(
            fd_result.value(), to_access_mode(access),
            capsule::ShareMode::All, "duplicated");
        if (adopted.is_err()) {
            ::close(fd_result.value());
            set_last_error(vfs_error_to_win32(adopted.error()));
            return NULL_HANDLE;
        }
        Handle duplicate = m_handles.allocate(
            HandleType::File, nullptr, flags, access);
        if (duplicate == NULL_HANDLE || duplicate == INVALID_HANDLE) {
            (void)m_vfs.close(adopted.value());
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return NULL_HANDLE;
        }
        {
            std::lock_guard<std::mutex> lock(m_file_path_mutex);
            m_vfs_handles[duplicate] = adopted.value();
        }
        if (m_file_ops != nullptr) {
            m_file_ops->on_file_handle_duplicated(source, duplicate);
        }
        return duplicate;
    }
    if (type != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }
    auto ep = m_handles.get_shared<PipeEndpoint>(source, HandleType::Pipe);
    if (!ep) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }
    if (!duplicate_access_allowed(m_handles.get_access(source), access)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return NULL_HANDLE;
    }
    const Handle duplicate =
        allocate_pipe_endpoint(m_handles, ep, flags, access);
    if (duplicate == NULL_HANDLE) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return NULL_HANDLE;
    }
    return duplicate;
}

void FileManager::restore_inherited_pipe_close_on_exec() {
    std::vector<std::shared_ptr<ipc::UnixPipe>> cleared;
    {
        std::lock_guard<std::mutex> lock(m_close_on_exec_cleared_mutex);
        cleared.swap(m_close_on_exec_cleared);
    }
    for (const auto& pipe : cleared) {
        if (pipe) (void)pipe->set_close_on_exec(true);
    }
}


bool FileManager::create_anonymous_pipe(Handle& read_handle,
                                        Handle& write_handle,
                                        bool inheritable) {
    auto [read_stream, write_stream] = ipc::UnixPipe::create_pair();
    if (!read_stream || !write_stream) return false;

    auto read_ep = std::make_shared<PipeEndpoint>();
    read_ep->readable = true;
    read_ep->conn = std::move(read_stream);
    read_ep->kind = PipeKind::Anonymous;
    auto write_ep = std::make_shared<PipeEndpoint>();
    write_ep->writable = true;
    write_ep->conn = std::move(write_stream);
    write_ep->kind = PipeKind::Anonymous;

    const u32 flags = inheritable ? HandleFlags::Inherit : 0;
    // Anonymous pipe ends get opposite access (files_core_probe
    // Flush_pipe_read_end -> ACCESS_DENIED, Flush_pipe_write_end -> TRUE).
    // Windows grants each end one attribute right beyond its direction:
    // read 0x120189 (+FILE_WRITE_ATTRIBUTES), write 0x120196
    // (+FILE_READ_ATTRIBUTES) (file_access_info_probe).
    constexpr u32 kFileReadAttributes = 0x0080u, kFileWriteAttributes = 0x0100u;
    read_handle = allocate_pipe_endpoint(m_handles, std::move(read_ep), flags,
                                         GENERIC_READ | kFileWriteAttributes);
    write_handle = allocate_pipe_endpoint(m_handles, std::move(write_ep), flags,
                                          GENERIC_WRITE | kFileReadAttributes);
    return true;
}

Handle FileManager::create_local_named_pipe(std::string win_name) {
    auto ep = std::make_shared<PipeEndpoint>();
    ep->state = std::make_shared<PipeState>();
    ep->readable = true;
    ep->writable = true;
    ep->win_name = std::move(win_name);
    ep->kind = PipeKind::NamedServer;
    ep->server_process_id = static_cast<u32>(::getpid());
    return allocate_pipe_endpoint(m_handles, std::move(ep));
}

std::optional<std::string> FileManager::named_pipe_file_name(
    Handle h) const {
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep || ep->win_name.empty()) return std::nullopt;

    static constexpr std::string_view PIPE_PREFIX = "\\\\.\\pipe";
    if (ep->win_name.size() >= PIPE_PREFIX.size()) {
        bool prefix_matches = true;
        for (usize index = 0; index < PIPE_PREFIX.size(); ++index) {
            const unsigned char actual =
                static_cast<unsigned char>(ep->win_name[index]);
            const unsigned char expected =
                static_cast<unsigned char>(PIPE_PREFIX[index]);
            if (std::tolower(actual) != std::tolower(expected)) {
                prefix_matches = false;
                break;
            }
        }
        if (prefix_matches) {
            return ep->win_name.substr(PIPE_PREFIX.size());
        }
    }
    return ep->win_name;
}


PipeConnectResult FileManager::begin_async_named_pipe_connect(
    Handle h, GuestAddr overlapped, u64* generation) {
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep) return PipeConnectResult::InvalidHandle;
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (ep->closed || !ep->sock || !ep->sock->is_server()) {
        return PipeConnectResult::InvalidHandle;
    }
    if (ep->conn) return PipeConnectResult::AlreadyConnected;
    if (ep->connect_pending) return PipeConnectResult::TimedOut;

    // Preserve Windows' ERROR_PIPE_CONNECTED race when the client reached the
    // listener before ConnectNamedPipe armed the overlapped operation.
    ep->disconnected = false;
    if (auto queued = ep->sock->accept(/*timeout_ms=*/0)) {
        ep->conn = std::move(queued);
        return PipeConnectResult::AlreadyConnected;
    }
    if (ep->connect_wake[0] < 0 && ::pipe(ep->connect_wake) == 0) {
        for (const int fd : ep->connect_wake) {
            (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    ep->connect_pending = true;
    ep->disconnected = false;
    ep->connect_cancelled = false;
    ep->connect_closed = false;
    ep->connect_overlapped = overlapped;
    ep->connect_handle = h;
    ep->connect_issuer_tid = ThreadManager::get_current_thread_id_static();
    ++ep->connect_generation;
    if (generation) *generation = ep->connect_generation;
    return PipeConnectResult::Pending;
}

namespace {

void wake_pipe_connect(PipeEndpoint& ep) {
    // Caller holds ep.mutex.
    if (ep.connect_wake[1] < 0) return;
    const char byte = 1;
    ssize_t n = 0;
    do {
        n = ::write(ep.connect_wake[1], &byte, 1);
    } while (n < 0 && errno == EINTR);
}

} // namespace

usize FileManager::cancel_named_pipe_connect(
    const std::shared_ptr<PipeEndpoint>& ep, Handle issued_on, GuestAddr overlapped,
    bool only_calling_thread, bool by_close) {
    if (!ep) return 0;
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (!ep->connect_pending || ep->connect_cancelled ||
        ep->connect_overlapped == 0 || ep->connect_handle != issued_on) {
        return 0;
    }
    if (overlapped != 0 && overlapped != ep->connect_overlapped) return 0;
    if (only_calling_thread &&
        ep->connect_issuer_tid != ThreadManager::get_current_thread_id_static()) {
        return 0;
    }
    ep->connect_cancelled = true;
    ep->connect_closed = by_close;
    wake_pipe_connect(*ep);
    return 1;
}

PipeConnectResult FileManager::wait_async_named_pipe_connect(
    Handle h, u64 generation, std::stop_token stop_token) {
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep) return PipeConnectResult::InvalidHandle;

    std::shared_ptr<ipc::UnixPipe> listener;
    int wake_fd = -1;
    {
        std::lock_guard<std::mutex> lock(ep->mutex);
        // DisconnectNamedPipe may already have ended this request.
        if (ep->connect_disconnected_generation == generation) {
            return PipeConnectResult::Disconnected;
        }
        if (ep->closed || !ep->connect_pending || !ep->sock ||
            !ep->sock->is_server() || ep->connect_generation != generation) {
            return PipeConnectResult::InvalidHandle;
        }
        listener = ep->sock;
        wake_fd = ep->connect_wake[0];
    }
    std::stop_callback on_stop(stop_token, [&ep] {
        std::lock_guard<std::mutex> lock(ep->mutex);
        wake_pipe_connect(*ep);
    });
    auto finish = [&](PipeConnectResult result) {
        // Caller holds ep->mutex. A request armed after this one was
        // disconnected owns the endpoint's connect state and wake pipe.
        if (ep->connect_generation != generation) return result;
        ep->connect_pending = false;
        ep->connect_overlapped = 0;
        for (int& fd : ep->connect_wake) {
            if (fd >= 0) (void)::close(fd);
            fd = -1;
        }
        return result;
    };

    // Sleep in poll() on the listening socket and the wake pipe: a client, a
    // cancel (CancelIo/CancelIoEx/CloseHandle) or shutdown ends the wait at
    // once. The previous 100 ms accept loop could neither be cancelled nor
    // stop polling, and once its handle was closed it went on accepting —
    // and dropping — the next client of the name.
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(ep->mutex);
            // A disconnect ends the request first: a later CloseHandle does
            // not change its STATUS_PIPE_DISCONNECTED.
            if (ep->connect_disconnected_generation == generation) {
                return finish(PipeConnectResult::Disconnected);
            }
            if (ep->connect_closed || ep->closed) {
                return finish(PipeConnectResult::Closed);
            }
            if (stop_token.stop_requested() || ep->connect_cancelled) {
                return finish(PipeConnectResult::Cancelled);
            }
            if (!ep->connect_pending) {
                return finish(PipeConnectResult::InvalidHandle);
            }
        }
        struct pollfd fds[2] = {{listener->fd(), POLLIN, 0}, {wake_fd, POLLIN, 0}};
        const nfds_t count = wake_fd >= 0 ? 2 : 1;
        int ready = 0;
        do {
            ready = ::poll(fds, count, wake_fd >= 0 ? -1 : 100);
        } while (ready < 0 && errno == EINTR);
        if (count == 2 && (fds[1].revents & POLLIN) != 0) {
            // Consume the wake so the next armed request's poll blocks.
            char drain[16];
            while (::read(wake_fd, drain, sizeof(drain)) > 0) {
            }
        }
        if (ready <= 0 || (fds[0].revents & POLLIN) == 0) continue;
        auto connection = listener->accept(/*timeout_ms=*/0);
        if (!connection) continue;  // another instance of the name took it
        std::lock_guard<std::mutex> lock(ep->mutex);
        if (ep->connect_disconnected_generation == generation) {
            return finish(PipeConnectResult::Disconnected);
        }
        if (ep->closed || ep->connect_cancelled || !ep->connect_pending) {
            // Too late to hand to this instance; the client sees a broken pipe.
            return finish(ep->closed || ep->connect_closed
                              ? PipeConnectResult::Closed
                              : ep->connect_cancelled
                                  ? PipeConnectResult::Cancelled
                                  : PipeConnectResult::InvalidHandle);
        }
        ep->conn = std::move(connection);
        return finish(PipeConnectResult::Connected);
    }
}

PipeDisconnectResult FileManager::disconnect_named_pipe(Handle h) {
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep) return PipeDisconnectResult::InvalidHandle;
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (ep->closed || !ep->sock || !ep->sock->is_server()) {
        return PipeDisconnectResult::InvalidHandle;
    }
    // Measured x86 == x64 (pipe_pending_connect_end_probe): a connected
    // instance and a listening one (fresh, connect pending, or after a
    // cancelled connect) both disconnect with TRUE; an already disconnected
    // instance fails with ERROR_PIPE_NOT_CONNECTED. A pending connect
    // completes with STATUS_PIPE_DISCONNECTED.
    if (ep->conn) {
        ep->conn.reset();
        ep->disconnected = true;
        return PipeDisconnectResult::Disconnected;
    }
    if (ep->disconnected) return PipeDisconnectResult::NotConnected;
    if (ep->connect_pending) {
        ep->connect_disconnected_generation = ep->connect_generation;
        ep->connect_pending = false;
        wake_pipe_connect(*ep);
    }
    ep->disconnected = true;
    return PipeDisconnectResult::Disconnected;
}

bool FileManager::named_pipe_client_process_id(
    Handle h, u32& out_pid, u32& error) {
    // Learn: GetNamedPipeClientProcessId — Pipe must be a CreateNamedPipe
    // instance. Returns the connected client's process id.
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (ep->closed || ep->kind != PipeKind::NamedServer) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    if (!ep->conn) {
        error = error_codes::ERROR_PIPE_NOT_CONNECTED;
        return false;
    }
    const int peer = ep->conn->peer_pid();
    if (peer <= 0) {
        error = error_codes::ERROR_INVALID_FUNCTION;
        return false;
    }
    out_pid = static_cast<u32>(peer);
    error = error_codes::ERROR_SUCCESS;
    return true;
}



bool FileManager::start_kernel_driver(std::string_view service_name,
                                      std::string_view image_path,
                                      u32& error) {
    const std::string device = ascii_lower(service_name);
    std::ifstream input(std::string(image_path), std::ios::binary);
    std::vector<u8> image((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    if (!input.is_open() || image.size() < 2 || image[0] != 'M' ||
        image[1] != 'Z') {
        error = error_codes::ERROR_BAD_FORMAT;
        return false;
    }

    // This provider is an original, contract-level implementation of the
    // public DeviceIoControl boundary exposed by CPUID's signed CPUZ162
    // kernel driver. Other NT-native images remain unsupported rather than
    // being reported as started.
    const std::string upper_name = [&] {
        std::string value(service_name);
        std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
            return static_cast<char>(
                std::toupper(static_cast<unsigned char>(ch)));
        });
        return value;
    }();
    if (device != "cpuz162" ||
        !contains_utf16le_ascii(image, upper_name)) {
        VITREO_TRACE("[device] no native provider for kernel driver service "
                     "'%.*s' image='%.*s'\n",
                     static_cast<int>(service_name.size()),
                     service_name.data(), static_cast<int>(image_path.size()),
                     image_path.data());
        error = error_codes::ERROR_NOT_SUPPORTED;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_device_mutex);
    m_running_kernel_devices.insert(device);
    VITREO_TRACE("[device] registered native kernel device '%s' from '%.*s'\n",
                 device.c_str(), static_cast<int>(image_path.size()),
                 image_path.data());
    error = error_codes::ERROR_SUCCESS;
    return true;
}

void FileManager::stop_kernel_driver(std::string_view service_name) {
    const std::string device = ascii_lower(service_name);
    std::lock_guard<std::mutex> lock(m_device_mutex);
    m_running_kernel_devices.erase(device);
    VITREO_TRACE("[device] unregistered native kernel device '%s'\n",
                 device.c_str());
}

bool FileManager::kernel_driver_running(
    std::string_view service_name) const {
    const std::string device = ascii_lower(service_name);
    std::lock_guard<std::mutex> lock(m_device_mutex);
    return m_running_kernel_devices.contains(device);
}






} // namespace vitreo::winapi
