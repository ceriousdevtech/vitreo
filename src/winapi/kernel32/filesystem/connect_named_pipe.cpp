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

/// @file connect_named_pipe.cpp
/// @brief FileManager::connect_named_pipe
///
/// Closed method cut from file.cpp.

namespace vitreo::winapi {

PipeConnectResult FileManager::connect_named_pipe(
    Handle h, int timeout_ms) {
    auto ep = m_handles.get_shared<PipeEndpoint>(h, HandleType::Pipe);
    if (!ep) return PipeConnectResult::InvalidHandle;
    std::shared_ptr<ipc::UnixPipe> listener;
    u64 generation = 0;
    {
        std::lock_guard<std::mutex> lock(ep->mutex);
        if (ep->closed || !ep->sock || !ep->sock->is_server()) {
            return PipeConnectResult::InvalidHandle;
        }
        if (ep->conn) return PipeConnectResult::AlreadyConnected;
        if (ep->connect_pending) return PipeConnectResult::Pending;
        ep->disconnected = false;

        // Windows distinguishes a client which connected before this call
        // from one which completes a blocking ConnectNamedPipe.
        if (auto queued = ep->sock->accept(/*timeout_ms=*/0)) {
            ep->conn = std::move(queued);
            return PipeConnectResult::AlreadyConnected;
        }
        if (timeout_ms == 0) return PipeConnectResult::TimedOut;
        ep->connect_pending = true;
        generation = ++ep->connect_generation;
        listener = ep->sock;
    }
    // DisconnectNamedPipe on another thread ends the wait with
    // ERROR_PIPE_NOT_CONNECTED (pipe_pending_connect_end_probe).
    auto ended = [&]() {
        // Caller holds ep->mutex.
        return ep->connect_disconnected_generation == generation
                   ? PipeConnectResult::Disconnected
                   : PipeConnectResult::InvalidHandle;
    };

    std::shared_ptr<ipc::UnixPipe> connection;
    if (timeout_ms < 0) {
        while (!connection) {
            connection = listener->accept(/*timeout_ms=*/100);
            if (connection) break;
            std::lock_guard<std::mutex> lock(ep->mutex);
            if (ep->closed || !ep->connect_pending ||
                ep->connect_generation != generation) {
                return ended();
            }
        }
    } else {
        connection = listener->accept(timeout_ms);
    }

    std::lock_guard<std::mutex> lock(ep->mutex);
    if (ep->closed || !ep->connect_pending ||
        ep->connect_generation != generation) {
        return ended();
    }
    ep->connect_pending = false;
    if (!connection) return PipeConnectResult::TimedOut;
    ep->conn = std::move(connection);
    return PipeConnectResult::Connected;
}

} // namespace vitreo::winapi
