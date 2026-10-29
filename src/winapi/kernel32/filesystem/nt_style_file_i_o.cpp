// SPDX-License-Identifier: LGPL-2.1-only

/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
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

/// @file nt_style_file_i_o.cpp
/// @brief NT-style File I/O (for NtReadFile/NtWriteFile)
///
/// Closed cut from file.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NT-style File I/O (for NtReadFile/NtWriteFile)
// =============================================================================

u32 FileManager::nt_read_file(Handle handle, GuestAddr buffer_addr, u32 length,
                               i64 byte_offset, u32& bytes_read) {
    bytes_read = 0;
    if (!m_memory) return 0xC0000008; // STATUS_INVALID_HANDLE (no memory = invalid state)

    // Check handle type
    auto type = m_handles.get_type(handle);
    if (type == HandleType::Invalid) {
        return 0xC0000008; // STATUS_INVALID_HANDLE
    }

    // Handle pipes (no offset support)
    if (type == HandleType::Pipe) {
        auto ep = m_handles.get_shared<PipeEndpoint>(
            handle, HandleType::Pipe);
        if (!ep || !ep->state || !ep->readable) {
            return 0xC0000008; // STATUS_INVALID_HANDLE
        }

        std::lock_guard<std::mutex> lock(ep->state->mutex);
        usize avail = pipe_available_locked(*ep->state);

        if (avail == 0) {
            if (!ep->state->write_open) {
                return 0xC0000011; // STATUS_END_OF_FILE (pipe closed)
            }
            return 0x00000000; // STATUS_SUCCESS with 0 bytes (no data available)
        }

        usize to_copy = std::min<usize>(static_cast<usize>(length), avail);
        if (to_copy > 0) {
            auto wr = m_memory->write_bytes(
                buffer_addr,
                reinterpret_cast<const Byte*>(ep->state->buffer.data() + ep->state->read_pos),
                to_copy);
            if (wr.is_err()) {
                return 0xC0000022; // STATUS_ACCESS_DENIED
            }
            ep->state->read_pos += to_copy;

            // Compact buffer occasionally
            if (ep->state->read_pos > 4096 && ep->state->read_pos > (ep->state->buffer.size() / 2)) {
                auto erase_end = ep->state->buffer.begin()
                               + static_cast<std::vector<u8>::difference_type>(ep->state->read_pos);
                ep->state->buffer.erase(ep->state->buffer.begin(), erase_end);
                ep->state->read_pos = 0;
            }
        }

        bytes_read = static_cast<u32>(to_copy);
        return 0x00000000; // STATUS_SUCCESS
    }

    // Regular file
    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        return 0xC0000008; // STATUS_INVALID_HANDLE
    }

    // Seek to offset if specified (byte_offset >= 0)
    if (byte_offset >= 0) {
        auto seek_result = m_vfs.seek(vfs_handle, byte_offset, capsule::SeekOrigin::Begin);
        if (seek_result.is_err()) {
            return 0xC0000022; // STATUS_ACCESS_DENIED
        }
    }

    // Zero-copy path: read directly into guest memory via host pointer
    Byte* guest_host_ptr = m_memory->host_ptr_mut(buffer_addr, length);
    if (guest_host_ptr) {
        auto result = m_vfs.read(vfs_handle,
            std::span<u8>(reinterpret_cast<u8*>(guest_host_ptr), length));
        if (result.is_err()) {
            return 0xC0000022; // STATUS_ACCESS_DENIED
        }
        bytes_read = static_cast<u32>(result.value());
        debug::read_sha1_update(handle, guest_host_ptr, bytes_read);
        if (bytes_read == 0 && length > 0) {
            return 0xC0000011; // STATUS_END_OF_FILE
        }
        return 0x00000000; // STATUS_SUCCESS
    }

    // Fallback for cross-region buffers
    std::vector<u8> host_buf(length);
    auto result = m_vfs.read(vfs_handle, std::span<u8>(host_buf));
    if (result.is_err()) {
        return 0xC0000022; // STATUS_ACCESS_DENIED
    }

    usize n = result.value();
    if (n > 0) {
        auto wr = m_memory->write_bytes(buffer_addr,
                                        reinterpret_cast<const Byte*>(host_buf.data()), n);
        if (wr.is_err()) {
            return 0xC0000022; // STATUS_ACCESS_DENIED
        }
    }
    debug::read_sha1_update(handle, host_buf.data(), n);

    bytes_read = static_cast<u32>(n);
    if (bytes_read == 0 && length > 0) {
        return 0xC0000011; // STATUS_END_OF_FILE
    }
    return 0x00000000; // STATUS_SUCCESS
}

u32 FileManager::nt_write_file(Handle handle, GuestAddr buffer_addr, u32 length,
                                i64 byte_offset, u32& bytes_written) {
    bytes_written = 0;
    if (!m_memory) return 0xC0000008; // STATUS_INVALID_HANDLE

    // Check handle type
    auto type = m_handles.get_type(handle);
    if (type == HandleType::Invalid) {
        return 0xC0000008; // STATUS_INVALID_HANDLE
    }

    // Handle console output
    if (type == HandleType::ConsoleOutput && m_console) {
        GuestAddr bytes_written_addr = 0; // We'll handle this differently
        Marshaler marshaler(*m_memory);
        // Allocate temporary guest memory for bytes_written would be complex,
        // so we'll read directly and write bytes_written after
        bool ok = m_console->write_console_a(handle, buffer_addr, length,
                                             bytes_written_addr, marshaler);
        if (ok) {
            bytes_written = length; // Console writes always succeed fully
            return 0x00000000; // STATUS_SUCCESS
        }
        return 0xC0000022; // STATUS_ACCESS_DENIED
    }

    if (type == HandleType::ConsoleInput) {
        return 0xC0000022; // STATUS_ACCESS_DENIED (can't write to console input)
    }

    // Handle pipes (no offset support)
    if (type == HandleType::Pipe) {
        auto ep = m_handles.get_shared<PipeEndpoint>(
            handle, HandleType::Pipe);
        if (!ep || !ep->state || !ep->writable) {
            return 0xC0000008; // STATUS_INVALID_HANDLE
        }

        std::vector<u8> host_buf(length);
        auto rr = m_memory->read_bytes(buffer_addr,
                                       reinterpret_cast<Byte*>(host_buf.data()), length);
        if (rr.is_err()) {
            return 0xC0000022; // STATUS_ACCESS_DENIED
        }

        std::lock_guard<std::mutex> lock(ep->state->mutex);
        if (!ep->state->read_open) {
            return 0xC000014B; // STATUS_PIPE_BROKEN
        }

        ep->state->buffer.insert(ep->state->buffer.end(), host_buf.begin(), host_buf.end());
        bytes_written = length;
        return 0x00000000; // STATUS_SUCCESS
    }

    // Regular file
    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        return 0xC0000008; // STATUS_INVALID_HANDLE
    }

    // Seek to offset if specified (byte_offset >= 0)
    if (byte_offset >= 0) {
        auto seek_result = m_vfs.seek(vfs_handle, byte_offset, capsule::SeekOrigin::Begin);
        if (seek_result.is_err()) {
            return 0xC0000022; // STATUS_ACCESS_DENIED
        }
    }

    // Zero-copy path: write directly from guest memory via host pointer
    const Byte* guest_host_ptr = m_memory->host_ptr(buffer_addr, length);
    if (guest_host_ptr) {
        auto result = m_vfs.write(vfs_handle,
            std::span<const u8>(reinterpret_cast<const u8*>(guest_host_ptr), length));
        if (result.is_err()) {
            return 0xC0000022; // STATUS_ACCESS_DENIED
        }
        bytes_written = static_cast<u32>(result.value());
        debug::write_sha1_update(handle, guest_host_ptr, bytes_written);
        return 0x00000000; // STATUS_SUCCESS
    }

    // Fallback for cross-region buffers
    std::vector<u8> host_buf(length);
    auto rr = m_memory->read_bytes(buffer_addr,
                                   reinterpret_cast<Byte*>(host_buf.data()), length);
    if (rr.is_err()) {
        return 0xC0000022; // STATUS_ACCESS_DENIED
    }

    auto result = m_vfs.write(vfs_handle, std::span<const u8>(host_buf));
    if (result.is_err()) {
        return 0xC0000022; // STATUS_ACCESS_DENIED
    }

    bytes_written = static_cast<u32>(result.value());
    debug::write_sha1_update(handle, host_buf.data(), bytes_written);
    return 0x00000000; // STATUS_SUCCESS
}

} // namespace vitreo::winapi
