/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
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

/// @file prepare_inherited_named_pipes.cpp
/// @brief FileManager::prepare_inherited_named_pipes
///
/// Closed method cut from file.cpp.

namespace vitreo::winapi {

std::string FileManager::prepare_inherited_named_pipes() {
    const auto handles = m_handles.handles_with_flags(HandleFlags::Inherit);
    return prepare_inherited_named_pipes(handles);
}

std::string FileManager::prepare_inherited_named_pipes(
    std::span<const Handle> selected_handles, bool require_inheritable) {
    std::string descriptors;
    for (Handle handle : selected_handles) {
        if (require_inheritable &&
            (m_handles.get_flags(handle) & HandleFlags::Inherit) == 0) {
            ipc::ipc_log(
                "HANDLE_LIST skip h=0x%x type=%s (not inheritable)",
                static_cast<unsigned>(handle),
                handle_type_to_string(m_handles.get_type(handle)));
            continue;
        }
        const HandleType type = m_handles.get_type(handle);

        // PROC_THREAD_ATTRIBUTE_HANDLE_LIST can carry a File write end
        // (often CreatePipe's write handle typed as Pipe, or a disk File).
        // Mapping and sync are prepared elsewhere; Pipe and File must travel
        // here so the child sees the
        // same numeric handle values Windows would inherit.
        if (type == HandleType::File) {
            const auto vfs_h = vfs_handle_for(handle);
            if (vfs_h == capsule::INVALID_FILE_HANDLE) {
                ipc::ipc_log(
                    "HANDLE_LIST skip h=0x%x type=File (no VFS backing)",
                    static_cast<unsigned>(handle));
                continue;
            }
            auto fd_result = m_vfs.duplicate_native_fd(vfs_h);
            if (fd_result.is_err()) {
                ipc::ipc_log(
                    "HANDLE_LIST skip h=0x%x type=File (dup fd failed)",
                    static_cast<unsigned>(handle));
                continue;
            }
            const int fd = fd_result.value();
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags < 0 ||
                ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
                ::close(fd);
                ipc::ipc_log(
                    "HANDLE_LIST skip h=0x%x type=File (clear CLOEXEC failed)",
                    static_cast<unsigned>(handle));
                continue;
            }
            const u32 access = m_handles.get_access(handle);
            const u32 hflags = m_handles.get_flags(handle);
            std::string descriptor = "F," + std::to_string(handle) + "," +
                                     std::to_string(fd) + "," +
                                     std::to_string(access) + "," +
                                     std::to_string(hflags);
            if (!descriptors.empty()) descriptors.push_back(';');
            descriptors += descriptor;
            ipc::ipc_log(
                "prepared inherited file h=0x%x fd=%d access=0x%x",
                static_cast<unsigned>(handle), fd, access);
            continue;
        }

        if (type != HandleType::Pipe) {
            // Expected for FileMapping / Event / etc. — those use other
            // prepare_inherited_* paths. Log so a missed type is visible.
            ipc::ipc_log(
                "HANDLE_LIST defer h=0x%x type=%s (not pipe/file)",
                static_cast<unsigned>(handle), handle_type_to_string(type));
            continue;
        }
        auto ep = m_handles.get_shared<PipeEndpoint>(
            handle, HandleType::Pipe);
        std::string descriptor;
        if (ep && !ep->conn && ep->sock && ep->sock->is_server() && !ep->win_name.empty()) {
            if (!ep->sock->set_close_on_exec(false)) {
                ipc::ipc_log(
                    "HANDLE_LIST skip h=0x%x type=Pipe (server CLOEXEC failed)",
                    static_cast<unsigned>(handle));
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_close_on_exec_cleared_mutex);
                m_close_on_exec_cleared.push_back(ep->sock);
            }
            descriptor = "N," + std::to_string(handle) + "," +
                         std::to_string(ep->sock->fd()) + "," +
                         encode_pipe_state(*ep) + ep->win_name;
            ep->sock->release_path_ownership();
        } else if (ep && ep->conn) {
            // Any connected endpoint transfers, whether it was accepted by a
            // server or opened by name with CreateFile. Requiring an empty
            // win_name here silently dropped the client side of a named pipe,
            // which is how a parent hands a child a named-pipe channel: the
            // parent opens \\.\pipe\<name>, marks the handle inheritable, and
            // passes its numeric value on the command line. Re-opening by
            // name in the child
            // would create a *different* connection, so the live socket has to
            // travel. The name is carried only to preserve the endpoint's
            // Windows-visible identity; I/O uses the inherited descriptor.
            if (!ep->conn->set_close_on_exec(false)) {
                ipc::ipc_log(
                    "HANDLE_LIST skip h=0x%x type=Pipe (conn CLOEXEC failed)",
                    static_cast<unsigned>(handle));
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_close_on_exec_cleared_mutex);
                m_close_on_exec_cleared.push_back(ep->conn);
            }
            descriptor = "A," + std::to_string(handle) + "," +
                         std::to_string(ep->conn->fd()) + "," +
                         (ep->readable ? "1" : "0") + "," +
                         (ep->writable ? "1" : "0") + "," +
                         encode_pipe_state(*ep) + ep->win_name;
        } else {
            ipc::ipc_log(
                "HANDLE_LIST skip h=0x%x type=Pipe (no sock/conn transport)",
                static_cast<unsigned>(handle));
        }
        if (descriptor.empty()) continue;
        if (!descriptors.empty()) descriptors.push_back(';');
        descriptors += descriptor;
        ipc::ipc_log(
            "prepared inherited pipe h=0x%x form=%c name='%s' type=%s "
            "readmode=%s blocking=%d",
            static_cast<unsigned>(handle), descriptor.front(),
            ep ? ep->win_name.c_str() : "",
            (ep && ep->pipe_type == PipeDataMode::Message) ? "message" : "byte",
            (ep && ep->read_mode == PipeDataMode::Message) ? "message" : "byte",
            (ep && ep->blocking) ? 1 : 0);
    }
    return descriptors;
}

} // namespace vitreo::winapi
