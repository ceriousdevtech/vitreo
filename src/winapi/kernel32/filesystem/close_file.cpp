// SPDX-License-Identifier: LGPL-2.1-only

/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_ops.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
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

/// @file close_file.cpp
/// @brief Close File
///
/// Closed cut from file.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Close File
// =============================================================================

bool FileManager::close_file(Handle h) {
    // Defer actual cleanup to the HandleTable close callback (installed by
    // RuntimeContext). This ensures CloseHandle works uniformly for files,
    // pipes, and other object types and avoids duplicate close logic.
    return m_handles.close(h);
}

bool FileManager::is_delete_pending(std::string_view path) const {
    std::lock_guard<std::mutex> lock(m_file_path_mutex);
    return m_paths_delete_pending.find(std::string(path)) !=
           m_paths_delete_pending.end();
}

bool FileManager::set_delete_disposition(Handle h, bool delete_on_close) {
    // Only a tracked file handle can carry a disposition; the caller maps a
    // false return onto ERROR_INVALID_HANDLE rather than reporting success for
    // a delete that could never happen. The disposition belongs to the object,
    // so it is recorded against the path every handle to it shares.
    std::lock_guard<std::mutex> lock(m_file_path_mutex);
    auto it = m_handle_paths.find(h);
    if (it == m_handle_paths.end()) return false;
    if (delete_on_close) {
        m_paths_delete_pending.insert(it->second);
    } else {
        m_paths_delete_pending.erase(it->second);
    }
    return true;
}

bool FileManager::on_handle_close(Handle h, HandleType type, void* object) noexcept {
    // File handles: close underlying VFS handle if we have a mapping.
    if (type == HandleType::File) {
        capsule::FileHandle vfs_handle = capsule::INVALID_FILE_HANDLE;
        std::string delete_path;
        bool remove_on_close = false;
        {
            std::lock_guard<std::mutex> lock(m_file_path_mutex);
            auto it = m_vfs_handles.find(h);
            if (it != m_vfs_handles.end()) {
                vfs_handle = it->second;
                m_vfs_handles.erase(it);
            }
            // An adopted handle whose file belongs to someone else (a CRT
            // FILE* stream) releases only itself.
            if (m_unowned_vfs_handles.erase(h) != 0) {
                vfs_handle = capsule::INVALID_FILE_HANDLE;
            }
            m_append_only_handles.erase(h);
            m_overlapped_file_handles.erase(h);
            m_no_buffering_file_handles.erase(h);

            // Carry out a pending-delete disposition. Windows removes the object
            // when its last handle closes, so the descriptor above is released
            // first — the host refuses to unlink a directory that is still open.
            // Vfs::remove covers files and empty directories, matching the
            // Windows rule that a non-empty directory survives (the guest then
            // sees it still present, which is the honest outcome).
            auto path_it = m_handle_paths.find(h);
            if (path_it != m_handle_paths.end()) {
                const std::string path = path_it->second;
                m_handle_paths.erase(path_it);
                if (m_delete_on_close_handles.erase(h) != 0) {
                    m_paths_delete_pending.insert(path);
                }

                // Windows removes a delete-pending object when its LAST handle
                // closes, so drop this handle's reference first and only act at
                // zero. Deleting on whichever handle happened to carry the
                // disposition would tear the object out from under handles the
                // guest still holds.
                bool last_handle = true;
                auto count_it = m_path_open_counts.find(path);
                if (count_it != m_path_open_counts.end()) {
                    if (count_it->second > 1) {
                        count_it->second -= 1;
                        last_handle = false;
                    } else {
                        m_path_open_counts.erase(count_it);
                    }
                }

                if (last_handle && m_paths_delete_pending.erase(path) != 0) {
                    delete_path = path;
                    remove_on_close = true;
                }
            }
        }

        if (vfs_handle != capsule::INVALID_FILE_HANDLE) {
            (void)m_vfs.close(vfs_handle);
        }
        if (m_file_ops != nullptr) {
            m_file_ops->on_file_handle_closed(h);
        }
        if (remove_on_close) {
            auto removed = m_vfs.remove(delete_path);
            VITREO_TRACE("[file] delete-on-close \"%s\" -> %s\n",
                         delete_path.c_str(),
                         removed.is_ok() ? "removed" : "not removed");
        }

        debug::write_sha1_close(h);
        debug::read_sha1_close(h);
        debug::watch_close(h);
        return true;
    }

    // Pipes: release this handle's reference to the endpoint. Microsoft Learn
    // CloseHandle: invalidate the handle, decrement the object handle count,
    // and only destroy the object after the *last* handle closes.
    // DuplicateHandle shares PipeEndpoint; tearing down on the first close
    // marked remaining aliases closed and closed the AF_UNIX fd early —
    // A child then saw ERROR_BROKEN_PIPE while the peer was still open.
    if (type == HandleType::Pipe) {
        auto* holder = static_cast<PipeEndpointHolder*>(object);
        auto ep = holder ? *holder : PipeEndpointHolder{};
        delete holder;
        // Pending requests retain the endpoint for memory safety; they are
        // not open Windows handles. Cancel this handle's requests even when
        // those references (or another duplicated handle) keep it alive.
        (void)cancel_overlapped_pipe_io(h, 0, false);
        (void)cancel_named_pipe_connect(ep, h, 0, false, /*by_close=*/true);
        const bool last_handle = ep && ep.use_count() == 1;
        if (last_handle) {
            // Cancel outstanding OVERLAPPED I/O before tearing down the
            // transport so completions use ERROR_OPERATION_ABORTED (CancelIo /
            // CloseHandle contract) rather than ERROR_BROKEN_PIPE.
            {
                std::lock_guard<std::mutex> lock(ep->mutex);
                ep->closed = true;
                ep->connect_pending = false;
            }
            if (ep->state) {
                std::lock_guard<std::mutex> lock(ep->state->mutex);
                if (ep->readable) ep->state->read_open = false;
                if (ep->writable) ep->state->write_open = false;
            }
        }
        if (std::getenv("VITREO_PIPE_DIAG") != nullptr) {
            ipc::ipc_log(
                "CloseHandle pipe h=0x%x last=%u name='%s' kind=%u",
                static_cast<unsigned>(h), last_handle ? 1u : 0u,
                ep ? ep->win_name.c_str() : "",
                ep ? static_cast<unsigned>(ep->kind) : 0u);
        }
        return true;
    }

    if (type == HandleType::Device) {
        delete static_cast<std::string*>(object);
        return true;
    }

    return true;
}

} // namespace vitreo::winapi
