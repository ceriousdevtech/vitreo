// SPDX-License-Identifier: LGPL-2.1-only

/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
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

/// @file readfile_writefile_setfilepointer_getfilesize_wi.cpp
/// @brief ReadFile / WriteFile / SetFilePointer / GetFileSize  (WIN32-011)
///
/// Closed cut from file.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// ReadFile / WriteFile / SetFilePointer / GetFileSize  (WIN32-011)
// =============================================================================

bool FileManager::read_file(Handle handle, GuestAddr buffer_addr,
                            u32 bytes_to_read, GuestAddr bytes_read_addr,
                            u32* bytes_read_out) {
    if (bytes_read_out) *bytes_read_out = 0;
    if (!m_memory) return false;

    // Anonymous pipes.
    auto type = m_handles.get_type(handle);
    if (type == HandleType::ConsoleInput && m_console) {
        Marshaler marshaler(*m_memory);
        return m_console->read_console_a(handle, buffer_addr, bytes_to_read,
                                         bytes_read_addr, marshaler);
    }
    if (type == HandleType::ConsoleOutput) {
        // Learn: ReadFile on console output fails ERROR_INVALID_HANDLE.
        if (bytes_read_out) *bytes_read_out = 0;
        if (bytes_read_addr != 0 && m_memory) {
            (void)m_memory->write_u32(bytes_read_addr, 0);
        }
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    if (type == HandleType::Pipe) {
        auto ep = m_handles.get_shared<PipeEndpoint>(
            handle, HandleType::Pipe);
        if (!ep) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        // Cross-process named pipe: read from the AF_UNIX stream.
        if (auto io = pipe_io(ep)) {
            if (bytes_to_read == 0) {
                if (bytes_read_addr != 0) {
                    (void)m_memory->write_u32(bytes_read_addr, 0);
                }
                return true;
            }
            if (ep->pipe_type == PipeDataMode::Message) {
                std::lock_guard<std::mutex> read_lock(ep->read_io_mutex);
                if (!ep->pending_message_loaded &&
                    !receive_message_frame(*ep, io)) {
                    set_last_error(error_codes::ERROR_BROKEN_PIPE);
                    return false;
                }

                const usize available =
                    ep->pending_message.size() - ep->pending_message_offset;
                const usize to_copy = std::min<usize>(
                    static_cast<usize>(bytes_to_read), available);
                if (to_copy != 0) {
                    const auto write = m_memory->write_bytes(
                        buffer_addr,
                        ep->pending_message.data() + ep->pending_message_offset,
                        to_copy);
                    if (write.is_err()) {
                        set_last_error(error_codes::ERROR_NOACCESS);
                        return false;
                    }
                }
                ep->pending_message_offset += to_copy;

                const bool message_complete =
                    ep->pending_message_offset == ep->pending_message.size();
                if (message_complete) {
                    ep->pending_message.clear();
                    ep->pending_message_offset = 0;
                    ep->pending_message_loaded = false;
                }
                if (bytes_read_addr != 0) {
                    (void)m_memory->write_u32(
                        bytes_read_addr, static_cast<u32>(to_copy));
                }
                if (bytes_read_out) {
                    *bytes_read_out = static_cast<u32>(to_copy);
                }

                // In message-read mode a short buffer consumes part of the
                // message, reports the byte count, and preserves the remainder
                // for the next read.
                if (ep->read_mode == PipeDataMode::Message &&
                    !message_complete) {
                    set_last_error(error_codes::ERROR_MORE_DATA);
                    return false;
                }
                return true;
            }

            std::vector<u8> tmp(static_cast<usize>(bytes_to_read));
            const u32 sync_tid = ThreadManager::get_current_thread_id_static();
            auto cancel_flag = begin_sync_io(sync_tid);
            isize n = io->read(tmp.data(), tmp.size(), cancel_flag.get());
            end_sync_io(sync_tid, cancel_flag);
            pipe_diag("read", handle, bytes_to_read, n);
            if (n == -2) {
                set_last_error(error_codes::ERROR_OPERATION_ABORTED);
                return false;
            }
            if (n < 0) {
                ipc::ipc_log("pipe ReadFile h=0x%x -> BROKEN n=%zd errno=%d",
                             static_cast<unsigned>(handle),
                             static_cast<isize>(n), errno);
                set_last_error(error_codes::ERROR_BROKEN_PIPE);
                return false;
            }
            if (n == 0 && bytes_to_read != 0) {
                ipc::ipc_log("pipe ReadFile h=0x%x -> EOF (peer closed)",
                             static_cast<unsigned>(handle));
                // recv(2) reports zero only after the peer has closed its
                // stream. Unlike a regular disk file, Win32 pipe EOF is not a
                // successful zero-byte read: ReadFile fails with
                // ERROR_BROKEN_PIPE. The reader uses that failure to leave its
                // shutdown-pipe read loop.
                if (bytes_read_addr != 0) {
                    (void)m_memory->write_u32(bytes_read_addr, 0);
                }
                set_last_error(error_codes::ERROR_BROKEN_PIPE);
                return false;
            }
            if (n > 0) {
                auto wr = m_memory->write_bytes(buffer_addr, tmp.data(),
                                                static_cast<usize>(n));
                if (wr.is_err()) { set_last_error(error_codes::ERROR_NOACCESS); return false; }
                pipe_diag_payload("read", handle, tmp.data(),
                                  static_cast<usize>(n));
            }
            if (bytes_read_addr != 0) {
                (void)m_memory->write_u32(bytes_read_addr, static_cast<u32>(n));
            }
            if (bytes_read_out) *bytes_read_out = static_cast<u32>(n);
            return true;
        }
        if (!ep->state || !ep->readable) {
            set_last_error(unconnected_pipe_error(ep));
            return false;
        }

        std::lock_guard<std::mutex> lock(ep->state->mutex);
        usize avail = pipe_available_locked(*ep->state);

        if (avail == 0) {
            if (!ep->state->write_open) {
                // Writer closed → EOF
                if (bytes_read_addr != 0) {
                    (void)m_memory->write_u32(bytes_read_addr, 0);
                }
                set_last_error(error_codes::ERROR_BROKEN_PIPE);
                return false;
            }

            if (bytes_read_addr != 0) {
                (void)m_memory->write_u32(bytes_read_addr, 0);
            }
            ipc::ipc_log("pipe ReadFile h=0x%x -> NO_DATA blocking=%d write_open=%d",
                         static_cast<unsigned>(handle),
                         ep->blocking ? 1 : 0,
                         ep->state->write_open ? 1 : 0);
            set_last_error(error_codes::ERROR_NO_DATA);
            return false;
        }

        usize to_copy = std::min<usize>(static_cast<usize>(bytes_to_read), avail);
        if (to_copy > 0) {
            auto wr = m_memory->write_bytes(
                buffer_addr,
                reinterpret_cast<const Byte*>(ep->state->buffer.data() + ep->state->read_pos),
                to_copy);
            if (wr.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
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

        if (bytes_read_addr != 0) {
            (void)m_memory->write_u32(bytes_read_addr, static_cast<u32>(to_copy));
        }
        if (bytes_read_out) *bytes_read_out = static_cast<u32>(to_copy);
        return true;
    }

    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        if (bytes_read_out) *bytes_read_out = 0;
        if (bytes_read_addr != 0 && m_memory) {
            (void)m_memory->write_u32(bytes_read_addr, 0);
        }
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    // Zero-copy path: read into guest memory only when the guest may write
    // the range (PAGE_READONLY must fail ERROR_NOACCESS — host_ptr_mut would
    // bypass that and fault as GEN_FAILURE; files_core_probe
    // ReadFile_readonly_buffer).
    Byte* guest_host_ptr = m_memory->host_ptr_store(
        buffer_addr, bytes_to_read);
    if (guest_host_ptr) {
        auto result = m_vfs.read(vfs_handle,
            std::span<u8>(reinterpret_cast<u8*>(guest_host_ptr), bytes_to_read));
        if (result.is_err()) {
            if (bytes_read_out) *bytes_read_out = 0;
            if (bytes_read_addr != 0) {
                (void)m_memory->write_u32(bytes_read_addr, 0);
            }
            set_last_error(vfs_error_to_win32(result.error()));
            return false;
        }
        usize n = result.value();
        if (bytes_read_out) *bytes_read_out = static_cast<u32>(n);
        debug::read_sha1_update(handle, guest_host_ptr, n);
        // EH-06: Only write bytes-read count if pointer is non-null.
        if (bytes_read_addr != 0) {
            auto cr = m_memory->write_u32(bytes_read_addr, static_cast<u32>(n));
            if (cr.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
            return true;
        }
        return true;
    }

    // Fallback for cross-region buffers
    std::vector<u8> host_buf(bytes_to_read);
    auto result = m_vfs.read(vfs_handle, std::span<u8>(host_buf));
    if (result.is_err()) {
        if (bytes_read_out) *bytes_read_out = 0;
        if (bytes_read_addr != 0) {
            (void)m_memory->write_u32(bytes_read_addr, 0);
        }
        set_last_error(vfs_error_to_win32(result.error()));
        return false;
    }

    usize n = result.value();
    if (bytes_read_out) *bytes_read_out = static_cast<u32>(n);
    if (n > 0) {
        auto wr = m_memory->write_bytes(buffer_addr,
                                        reinterpret_cast<const Byte*>(host_buf.data()), n);
        if (wr.is_err()) {
            // VFS already advanced the file pointer; restore so a failed
            // ReadFile into a non-writable buffer leaves the pointer
            // unchanged (files_core_probe ReadFile_readonly_buffer).
            (void)m_vfs.seek(vfs_handle, -static_cast<i64>(n),
                             capsule::SeekOrigin::Current);
            if (bytes_read_out) *bytes_read_out = 0;
            if (bytes_read_addr != 0) {
                (void)m_memory->write_u32(bytes_read_addr, 0);
            }
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
    }
    debug::read_sha1_update(handle, host_buf.data(), n);

    // EH-06: Only write bytes-read count if pointer is non-null.
    if (bytes_read_addr != 0) {
        auto cr = m_memory->write_u32(bytes_read_addr, static_cast<u32>(n));
        if (cr.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
    }
    return true;
}

bool FileManager::read_volume_at(Handle handle, GuestAddr buffer_addr,
                                 u32 bytes_to_read, GuestAddr bytes_read_addr,
                                 u64 offset, u32* bytes_read_out) {
    if (bytes_read_out) {
        *bytes_read_out = 0;
    }
    if (!m_memory) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    if (m_handles.get_type(handle) != HandleType::Device) {
        return false;
    }
    const auto* device = static_cast<const std::string*>(
        m_handles.get(handle, HandleType::Device));
    if (device == nullptr || device->size() != 5 ||
        device->compare(0, 4, "vol:") != 0) {
        return false;
    }
    const char letter = (*device)[4];
    const std::string root =
        std::string(1, static_cast<char>(std::toupper(
                             static_cast<unsigned char>(letter)))) +
        ":\\";
    auto resolved = m_vfs.resolve_components(root);
    if (resolved.is_err() || resolved.value().first == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    capsule::IFileSystem* fs = resolved.value().first;
    if (bytes_to_read == 0) {
        if (bytes_read_addr != 0) {
            (void)m_memory->write_u32(bytes_read_addr, 0);
        }
        return true;
    }

    std::vector<u8> host_buf(bytes_to_read);
    auto result = fs->read_volume(offset, std::span<u8>(host_buf));
    if (result.is_err()) {
        // NotSupported / IoError → treat like a bad sector / no media payload.
        set_last_error(result.error() == capsule::VfsError::NotSupported
                           ? error_codes::ERROR_INVALID_FUNCTION
                           : error_codes::ERROR_READ_FAULT);
        VITREO_TRACE("[device] volume %c: ReadFile offset=%llu req=%u -> "
                     "FALSE (%s)\n",
                     std::toupper(static_cast<unsigned char>(letter)),
                     static_cast<unsigned long long>(offset), bytes_to_read,
                     capsule::vfs_error_to_string(result.error()));
        return false;
    }
    const usize n = result.value();
    if (n > 0) {
        if (m_memory->write_bytes(buffer_addr, host_buf.data(), n).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
    }
    if (bytes_read_addr != 0) {
        (void)m_memory->write_u32(bytes_read_addr, static_cast<u32>(n));
    }
    if (bytes_read_out) {
        *bytes_read_out = static_cast<u32>(n);
    }
    VITREO_TRACE("[device] volume %c: ReadFile offset=%llu req=%u got=%zu "
                 "optical=%d\n",
                 std::toupper(static_cast<unsigned char>(letter)),
                 static_cast<unsigned long long>(offset), bytes_to_read, n,
                 fs->is_optical() ? 1 : 0);
    return true;
}

bool FileManager::read_file_at(Handle handle, GuestAddr buffer_addr,
                               u32 bytes_to_read, GuestAddr bytes_read_addr,
                               u64 offset, u32* bytes_read_out,
                               bool advance_pointer) {
    // Raw volume handles (CreateFile("\\.\D:")) use OVERLAPPED offsets to
    // read logical CD sectors — independent of m_vfs_handles.
    if (m_handles.get_type(handle) == HandleType::Device) {
        return read_volume_at(handle, buffer_addr, bytes_to_read,
                              bytes_read_addr, offset, bytes_read_out);
    }

    std::lock_guard<std::mutex> lock(m_positioned_io_mutex);
    // Learn (ReadFile / OVERLAPPED): Offset+OffsetHigh form an unsigned
    // 64-bit file position, but a value whose high bit is set (including
    // the all-ones "append" sentinel, which is WriteFile-only) fails with
    // ERROR_INVALID_PARAMETER (files_core_probe sync_ReadFile_offset_all_ones /
    // _offset_negative).
    if (offset > static_cast<u64>(INT64_MAX)) {
        if (bytes_read_out) *bytes_read_out = 0;
        if (bytes_read_addr != 0 && m_memory) {
            (void)m_memory->write_u32(bytes_read_addr, 0);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    // Synchronous handle + lpOverlapped: the read starts at Offset and the
    // file pointer advances to Offset + bytes transferred (Learn:
    // "Synchronization and File Position"; files_core_probe
    // sync_ReadFile_overlapped pointer_after). FILE_FLAG_OVERLAPPED handles
    // never move the pointer (async_ReadFile_ok pointer_after == 0).
    // On a past-EOF failure the pointer is left unchanged either way.
    const auto original = m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Current);
    if (original.is_err() ||
        m_vfs.seek(vfs_handle, static_cast<i64>(offset),
                   capsule::SeekOrigin::Begin).is_err()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    u32 got = 0;
    const bool ok = read_file(handle, buffer_addr, bytes_to_read,
                              bytes_read_addr, &got);
    if (bytes_read_out) *bytes_read_out = got;
    if (!ok) {
        (void)m_vfs.seek(vfs_handle, static_cast<i64>(original.value()),
                         capsule::SeekOrigin::Begin);
        return false;
    }
    // At/past EOF: leave the caller's file pointer unchanged. Sync ReadFile
    // with OVERLAPPED turns this into FALSE + ERROR_HANDLE_EOF; ReadFileEx
    // keeps TRUE and queues the EOF completion (files_core_probe /
    // ReadFileExAtEofQueuesHandleEof). The thunk distinguishes the two.
    if (bytes_to_read > 0 && got == 0) {
        (void)m_vfs.seek(vfs_handle, static_cast<i64>(original.value()),
                         capsule::SeekOrigin::Begin);
        if (bytes_read_out) *bytes_read_out = 0;
        if (bytes_read_addr != 0 && m_memory) {
            (void)m_memory->write_u32(bytes_read_addr, 0);
        }
        return true;
    }
    if (!advance_pointer) {
        (void)m_vfs.seek(vfs_handle, static_cast<i64>(original.value()),
                         capsule::SeekOrigin::Begin);
    }
    return true;
}

bool FileManager::read_handle_bytes(Handle handle, std::vector<u8>& out,
                                    u32 max_bytes,
                                    std::optional<u64> absolute_offset) {
    out.clear();
    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    const auto original = m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Current);
    if (original.is_err()) {
        set_last_error(vfs_error_to_win32(original.error()));
        return false;
    }
    const u64 start = absolute_offset.value_or(original.value());
    const auto info = m_vfs.stat_handle(vfs_handle);
    if (info.is_err()) {
        set_last_error(vfs_error_to_win32(info.error()));
        return false;
    }
    const u64 file_size = info.value().size;
    if (start >= file_size) {
        return true;
    }
    const u64 remaining = file_size - start;
    const u64 want = (max_bytes == 0)
                         ? remaining
                         : std::min(remaining, static_cast<u64>(max_bytes));
    if (want == 0) return true;
    if (m_vfs.seek(vfs_handle, static_cast<i64>(start),
                   capsule::SeekOrigin::Begin)
            .is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    out.resize(static_cast<usize>(want));
    auto result = m_vfs.read(vfs_handle, std::span<u8>(out));
    (void)m_vfs.seek(vfs_handle, static_cast<i64>(original.value()),
                     capsule::SeekOrigin::Begin);
    if (result.is_err()) {
        out.clear();
        set_last_error(vfs_error_to_win32(result.error()));
        return false;
    }
    out.resize(result.value());
    return true;
}

bool FileManager::write_handle_bytes(Handle handle, std::string_view bytes) {
    // Host-side counterpart of write_file's pipe path. write_file sources its
    // payload from GUEST memory; the console has host bytes (VT it generated
    // itself) and no guest buffer to point at.
    //
    // Used by ConPTY: a child whose console is a pseudoconsole writes its
    // rendered output straight into the pseudoconsole's pipe. Also used by the
    // CRT for descriptors 0-2 bound to a redirected std handle, which may be a
    // file as well as a pipe.
    if (bytes.empty()) return true;
    const HandleType type = m_handles.get_type(handle);
    if (type == HandleType::File) {
        const capsule::FileHandle vfs_handle = vfs_handle_for(handle);
        if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        // Same rule as write_file: an append-only handle writes at the end.
        if (is_append_only(handle)) {
            (void)m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::End);
        }
        auto result = m_vfs.write(
            vfs_handle, std::span<const u8>(
                reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
        if (result.is_err()) {
            set_last_error(vfs_error_to_win32(result.error()));
            return false;
        }
        return result.value() == bytes.size();
    }
    if (type != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    auto ep = m_handles.get_shared<PipeEndpoint>(handle, HandleType::Pipe);
    if (!ep) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    auto io = pipe_io(ep);
    if (!io) {
        // In-process anonymous pipe: the same buffer write_file appends to.
        if (!ep->state || !ep->writable) {
            set_last_error(unconnected_pipe_error(ep));
            return false;
        }
        std::lock_guard<std::mutex> lock(ep->state->mutex);
        if (!ep->state->read_open) {
            set_last_error(error_codes::ERROR_BROKEN_PIPE);
            return false;
        }
        ep->state->buffer.insert(ep->state->buffer.end(),
                                 reinterpret_cast<const u8*>(bytes.data()),
                                 reinterpret_cast<const u8*>(bytes.data()) +
                                     bytes.size());
        return true;
    }
    const auto* data = reinterpret_cast<const u8*>(bytes.data());
    isize n = -1;
    if (ep->pipe_type == PipeDataMode::Message) {
        std::lock_guard<std::mutex> write_lock(ep->write_io_mutex);
        if (write_message_frame(io, data, static_cast<u32>(bytes.size()))) {
            n = static_cast<isize>(bytes.size());
        }
    } else {
        n = io->write(data, bytes.size());
    }
    pipe_diag("write_bytes", handle, static_cast<u32>(bytes.size()), n);
    if (n < 0) {
        set_last_error(error_codes::ERROR_BROKEN_PIPE);
        return false;
    }
    return true;
}

bool FileManager::write_file(Handle handle, GuestAddr buffer_addr,
                             u32 bytes_to_write, GuestAddr bytes_written_addr,
                             u32* bytes_written_out) {
    if (bytes_written_out) *bytes_written_out = 0;
    if (!m_memory) return false;

    // WIN32-089: Check if this is a console handle and delegate accordingly
    auto type = m_handles.get_type(handle);
    if (type == HandleType::ConsoleOutput && m_console) {
        Marshaler marshaler(*m_memory);
        return m_console->write_console_a(handle, buffer_addr, bytes_to_write,
                                          bytes_written_addr, marshaler);
    }
    if (type == HandleType::ConsoleInput) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    // Anonymous pipes
    if (type == HandleType::Pipe) {
        auto ep = m_handles.get_shared<PipeEndpoint>(
            handle, HandleType::Pipe);
        if (!ep) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        // Cross-process named pipe: write to the AF_UNIX stream.
        if (auto io = pipe_io(ep)) {
            std::vector<u8> hb(bytes_to_write);
            auto rr = m_memory->read_bytes(buffer_addr,
                                           reinterpret_cast<Byte*>(hb.data()), bytes_to_write);
            if (rr.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_USER_BUFFER);
                return false;
            }
            isize n = -1;
            if (ep->pipe_type == PipeDataMode::Message) {
                std::lock_guard<std::mutex> write_lock(ep->write_io_mutex);
                if (write_message_frame(
                        io, hb.data(), static_cast<u32>(hb.size()))) {
                    n = static_cast<isize>(hb.size());
                }
            } else {
                n = io->write(hb.data(), hb.size());
            }
            pipe_diag("write", handle, bytes_to_write, n);
            if (n < 0) { set_last_error(error_codes::ERROR_BROKEN_PIPE); return false; }
            if (bytes_written_addr != 0) {
                (void)m_memory->write_u32(bytes_written_addr, static_cast<u32>(n));
            }
            if (bytes_written_out) *bytes_written_out = static_cast<u32>(n);
            return true;
        }
        if (!ep->state || !ep->writable) {
            set_last_error(unconnected_pipe_error(ep));
            return false;
        }

        std::vector<u8> host_buf(bytes_to_write);
        auto rr = m_memory->read_bytes(buffer_addr,
                                       reinterpret_cast<Byte*>(host_buf.data()), bytes_to_write);
        if (rr.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }

        std::lock_guard<std::mutex> lock(ep->state->mutex);
        if (!ep->state->read_open) {
            set_last_error(error_codes::ERROR_BROKEN_PIPE);
            return false;
        }

        ep->state->buffer.insert(ep->state->buffer.end(), host_buf.begin(), host_buf.end());
        if (bytes_written_addr != 0) {
            (void)m_memory->write_u32(bytes_written_addr, bytes_to_write);
        }
        if (bytes_written_out) *bytes_written_out = bytes_to_write;
        return true;
    }

    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        if (bytes_written_out) *bytes_written_out = 0;
        if (bytes_written_addr != 0 && m_memory) {
            (void)m_memory->write_u32(bytes_written_addr, 0);
        }
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    // A FILE_APPEND_DATA-only handle writes at the current end of file, not at
    // its own file pointer. Re-resolving the end here is what keeps concurrent
    // appenders (several processes sharing one log) from overwriting each other.
    if (is_append_only(handle)) {
        (void)m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::End);
    }

    // Zero-copy path: write directly from guest memory via host pointer
    const Byte* guest_host_ptr = m_memory->host_ptr(
        buffer_addr, bytes_to_write);
    if (guest_host_ptr) {
        auto result = m_vfs.write(vfs_handle,
            std::span<const u8>(reinterpret_cast<const u8*>(guest_host_ptr),
                                bytes_to_write));
        if (result.is_err()) {
            if (bytes_written_out) *bytes_written_out = 0;
            if (bytes_written_addr != 0) {
                (void)m_memory->write_u32(bytes_written_addr, 0);
            }
            set_last_error(vfs_error_to_win32(result.error()));
            return false;
        }
        usize n = result.value();
        if (bytes_written_out) *bytes_written_out = static_cast<u32>(n);
        debug::write_sha1_update(handle, guest_host_ptr, n);
        if (bytes_written_addr != 0) {
            auto cr = m_memory->write_u32(bytes_written_addr, static_cast<u32>(n));
            if (cr.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return false;
            }
        }
        return true;
    }

    // Fallback for cross-region buffers
    std::vector<u8> host_buf(bytes_to_write);
    auto rr = m_memory->read_bytes(buffer_addr,
                                   reinterpret_cast<Byte*>(host_buf.data()), bytes_to_write);
    if (rr.is_err()) {
        if (bytes_written_out) *bytes_written_out = 0;
        if (bytes_written_addr != 0) {
            (void)m_memory->write_u32(bytes_written_addr, 0);
        }
        // Learn / files_core_probe WriteFile_noaccess_buffer: unreadable
        // source buffer -> ERROR_INVALID_USER_BUFFER (1784).
        set_last_error(error_codes::ERROR_INVALID_USER_BUFFER);
        return false;
    }

    auto result = m_vfs.write(vfs_handle, std::span<const u8>(host_buf));
    if (result.is_err()) {
        if (bytes_written_out) *bytes_written_out = 0;
        if (bytes_written_addr != 0) {
            (void)m_memory->write_u32(bytes_written_addr, 0);
        }
        set_last_error(vfs_error_to_win32(result.error()));
        return false;
    }

    usize n = result.value();
    if (bytes_written_out) *bytes_written_out = static_cast<u32>(n);
    debug::write_sha1_update(handle, host_buf.data(), n);
    if (bytes_written_addr != 0) {
        auto cr = m_memory->write_u32(bytes_written_addr, static_cast<u32>(n));
        if (cr.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
    }
    return true;
}

bool FileManager::write_file_at(Handle handle, GuestAddr buffer_addr,
                                u32 bytes_to_write, GuestAddr bytes_written_addr,
                                u64 offset, u32* bytes_written_out,
                                bool advance_pointer) {
    std::lock_guard<std::mutex> lock(m_positioned_io_mutex);
    // NtWriteFile ByteOffset sentinels (also used via OVERLAPPED):
    //   -1 (all ones) = FILE_WRITE_TO_END_OF_FILE — append
    //   -2            = FILE_USE_FILE_POINTER_POSITION — leave the pointer
    // Any other value with the high bit set fails ERROR_INVALID_PARAMETER
    // (files_core_probe sync_WriteFile_append / _offset_ffff_fffe /
    // _offset_negative).
    constexpr u64 kWriteToEof = ~u64{0};
    constexpr u64 kUseFilePointer = kWriteToEof - 1u;
    if (offset != kWriteToEof && offset != kUseFilePointer &&
        offset > static_cast<u64>(INT64_MAX)) {
        if (bytes_written_out) *bytes_written_out = 0;
        if (bytes_written_addr != 0 && m_memory) {
            (void)m_memory->write_u32(bytes_written_addr, 0);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    // Synchronous + lpOverlapped advances the file pointer to the end of the
    // transfer (files_core_probe sync_WriteFile_overlapped / _append).
    const auto original = m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Current);
    if (original.is_err()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    if (offset == kWriteToEof) {
        if (m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::End).is_err()) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
    } else if (offset != kUseFilePointer) {
        if (m_vfs.seek(vfs_handle, static_cast<i64>(offset),
                       capsule::SeekOrigin::Begin).is_err()) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
    }
    const bool ok = write_file(handle, buffer_addr, bytes_to_write,
                               bytes_written_addr, bytes_written_out);
    if (!ok || !advance_pointer) {
        (void)m_vfs.seek(vfs_handle, static_cast<i64>(original.value()),
                         capsule::SeekOrigin::Begin);
    }
    return ok;
}

bool FileManager::transact_named_pipe(Handle handle, GuestAddr input_addr,
                                      u32 input_size, GuestAddr output_addr,
                                      u32 output_size, GuestAddr bytes_read_addr) {
    if (m_handles.get_type(handle) != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    if (!write_file(handle, input_addr, input_size, 0)) {
        return false;
    }
    return read_file(handle, output_addr, output_size, bytes_read_addr);
}

std::optional<u64> FileManager::set_file_pointer64(Handle handle, i64 offset,
                                                   u32 move_method) {
    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) return std::nullopt;

    capsule::SeekOrigin origin{};
    switch (move_method) {
        case FILE_BEGIN:   origin = capsule::SeekOrigin::Begin;   break;
        case FILE_CURRENT: origin = capsule::SeekOrigin::Current; break;
        case FILE_END:     origin = capsule::SeekOrigin::End;     break;
        default: return std::nullopt;
    }

    // FILE_FLAG_NO_BUFFERING: new absolute position must be a multiple of the
    // volume sector size (Windows 11 files_core_probe / nobuf_seek: unaligned
    // -> ERROR_INVALID_PARAMETER, pointer unmoved). Capsule volumes use 512.
    if (is_no_buffering_file(handle)) {
        constexpr u64 kSector = 512;
        u64 target = 0;
        if (origin == capsule::SeekOrigin::Begin) {
            if (offset < 0) return std::nullopt;
            target = static_cast<u64>(offset);
        } else {
            auto cur = m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Current);
            if (cur.is_err()) return std::nullopt;
            if (origin == capsule::SeekOrigin::Current) {
                const i64 sum = static_cast<i64>(cur.value()) + offset;
                if (sum < 0) return std::nullopt;
                target = static_cast<u64>(sum);
            } else { // End
                auto end = m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::End);
                if (end.is_err()) return std::nullopt;
                const i64 sum = static_cast<i64>(end.value()) + offset;
                // Restore current before failing/succeeding.
                (void)m_vfs.seek(vfs_handle, static_cast<i64>(cur.value()),
                                 capsule::SeekOrigin::Begin);
                if (sum < 0) return std::nullopt;
                target = static_cast<u64>(sum);
            }
        }
        if ((target % kSector) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return std::nullopt;
        }
        if (origin != capsule::SeekOrigin::Begin) {
            // Seek to absolute target so we do not double-apply CURRENT.
            auto result = m_vfs.seek(vfs_handle, static_cast<i64>(target),
                                     capsule::SeekOrigin::Begin);
            if (result.is_err()) return std::nullopt;
            return result.value();
        }
    }

    auto result = m_vfs.seek(vfs_handle, offset, origin);
    if (result.is_err()) return std::nullopt;
    return result.value();
}

u32 FileManager::set_file_pointer(Handle handle, i32 distance,
                                  i32* distance_high, u32 move_method) {
    i64 offset = distance;
    if (distance_high) {
        offset = static_cast<i64>(static_cast<u32>(distance))
               | (static_cast<i64>(*distance_high) << 32);
    }
    const auto new_pos = set_file_pointer64(handle, offset, move_method);
    if (!new_pos.has_value()) return 0xFFFFFFFF;
    if (distance_high)
        *distance_high = static_cast<i32>(*new_pos >> 32);
    return static_cast<u32>(*new_pos & 0xFFFFFFFF);
}

std::optional<u64> FileManager::get_file_size64(Handle handle) {
    capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) return std::nullopt;

    // Learn / GetFileSize: do not move the file pointer. Use fstat-equivalent
    // (files_core_probe GetFileSize pointer_unchanged).
    auto info = m_vfs.stat_handle(vfs_handle);
    if (info.is_err()) return std::nullopt;
    return info.value().size;
}

u32 FileManager::get_file_size(Handle handle, u32* high_part) {
    const auto size_result = get_file_size64(handle);
    if (!size_result.has_value()) return 0xFFFFFFFF;

    const u64 size = *size_result;
    if (high_part) *high_part = static_cast<u32>(size >> 32);
    return static_cast<u32>(size & 0xFFFFFFFF);
}

bool FileManager::set_end_of_file(Handle handle) {
    const capsule::FileHandle vfs_handle = vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    auto position = m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Current);
    if (position.is_err()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }

    // Learn (SetEndOfFile): shrinking while a CreateFileMapping object or
    // MapViewOfFile view is still open fails ERROR_USER_MAPPED_FILE; growing
    // is allowed (files_core_probe SetEndOfFile_mapped_*).
    if (m_file_mapping_manager &&
        m_file_mapping_manager->blocks_shrink_end_of_file(
            handle, static_cast<u64>(position.value()))) {
        set_last_error(error_codes::ERROR_USER_MAPPED_FILE);
        return false;
    }

    auto result = m_vfs.truncate(vfs_handle, position.value());
    if (result.is_err()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }

    VITREO_TRACE("[file] SetEndOfFile handle=0x%x -> size=%llu\n",
                 static_cast<unsigned>(handle),
                 static_cast<unsigned long long>(position.value()));
    return true;
}

} // namespace vitreo::winapi
