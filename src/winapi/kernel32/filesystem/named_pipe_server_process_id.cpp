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

/// @file named_pipe_server_process_id.cpp
/// @brief FileManager::named_pipe_server_process_id
///
/// Closed method cut from file.cpp.

namespace vitreo::winapi {

bool FileManager::named_pipe_server_process_id(
    Handle h, u32& out_pid, u32& error) {
    // Learn documents CreateNamedPipe handles. The inherited CreateFile
    // client end also calls this to learn the parent's pid before
    // OpenProcess(PROCESS_DUP_HANDLE).
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    std::lock_guard<std::mutex> lock(ep->mutex);
    if (ep->closed) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    if (ep->kind == PipeKind::NamedServer) {
        out_pid = ep->server_process_id != 0
                      ? ep->server_process_id
                      : static_cast<u32>(::getpid());
        error = error_codes::ERROR_SUCCESS;
        return true;
    }
    if (ep->kind == PipeKind::NamedClient) {
        std::shared_ptr<ipc::UnixPipe> io = ep->conn;
        if (!io && ep->sock && !ep->sock->is_server()) {
            io = ep->sock;
        }
        if (!io) {
            error = error_codes::ERROR_PIPE_NOT_CONNECTED;
            return false;
        }
        // Prefer the peer's live identity; fall back to a cached creator id
        // carried across inheritance when LOCAL_PEERPID is unavailable.
        const int peer = io->peer_pid();
        if (peer > 0) {
            out_pid = static_cast<u32>(peer);
            error = error_codes::ERROR_SUCCESS;
            return true;
        }
        if (ep->server_process_id != 0) {
            out_pid = ep->server_process_id;
            error = error_codes::ERROR_SUCCESS;
            return true;
        }
        error = error_codes::ERROR_INVALID_FUNCTION;
        return false;
    }
    error = error_codes::ERROR_INVALID_HANDLE;
    return false;
}

} // namespace vitreo::winapi
