// SPDX-License-Identifier: LGPL-2.1-only

/// @file overlapped_io.cpp
/// @brief GetOverlappedResult and CancelIo/CancelIoEx.
///
/// CPU-Z Overlapped I/O banner.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "winapi/kernel32/filesystem/directory_changes.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// CPU-Z: Overlapped I/O
// =============================================================================

namespace {

/// NTSTATUS left in OVERLAPPED.Internal by Vitreo's completion paths ->
/// the Win32 error GetOverlappedResult reports (ntstatus.h / winerror.h;
/// RtlNtStatusToDosError). Facility-7 statuses carry the Win32 code.
u32 overlapped_status_to_win32(u32 status) {
    const u32 facility_prefix = status & 0xFFFF0000u;
    if (facility_prefix == 0x80070000u || facility_prefix == 0xC0070000u) {
        return status & 0xFFFFu;
    }
    switch (status) {
        case 0xC0000120u: return error_codes::ERROR_OPERATION_ABORTED;  // STATUS_CANCELLED
        case 0xC0000022u: return error_codes::ERROR_ACCESS_DENIED;      // STATUS_ACCESS_DENIED
        case 0xC0000008u: return error_codes::ERROR_INVALID_HANDLE;     // STATUS_INVALID_HANDLE
        case 0xC0000011u: return error_codes::ERROR_HANDLE_EOF;         // STATUS_END_OF_FILE
        case 0xC000014Bu: return error_codes::ERROR_BROKEN_PIPE;        // STATUS_PIPE_BROKEN
        case 0xC00000B0u: return 233;  // STATUS_PIPE_DISCONNECTED -> ERROR_PIPE_NOT_CONNECTED
        case 0xC00000B3u: return 536;  // STATUS_PIPE_LISTENING -> ERROR_PIPE_LISTENING
        case 0x80000005u: return 234;  // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
        case 0xC0000005u: return error_codes::ERROR_NOACCESS;           // STATUS_ACCESS_VIOLATION
        case 0x0000010Bu: return 745;   // STATUS_NOTIFY_CLEANUP -> ERROR_NOTIFY_CLEANUP
        case 0x0000010Cu: return 1022;  // STATUS_NOTIFY_ENUM_DIR -> ERROR_NOTIFY_ENUM_DIR
        default: return 31;            // ERROR_GEN_FAILURE
    }
}

} // namespace

void Kernel32ExtManager::impl_GetOverlappedResult(Marshaler& m) {
    // BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED, LPDWORD, BOOL bWait)
    get_overlapped_result(m, m.arg32(3) != 0 ? INFINITE_WAIT : 0,
                          /*alertable=*/false);
}

void Kernel32ExtManager::impl_GetOverlappedResultEx(Marshaler& m) {
    // BOOL GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped,
    //   LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds,
    //   BOOL bAlertable)
    // Learn: dwMilliseconds 0 does not wait (ERROR_IO_INCOMPLETE); an elapsed
    // timeout is WAIT_TIMEOUT; an APC or completion routine during an
    // alertable wait is WAIT_IO_COMPLETION (s4_overlapped_probe).
    get_overlapped_result(m, m.arg32(3), m.arg32(4) != 0);
}

void Kernel32ExtManager::get_overlapped_result(Marshaler& m, u32 timeout_ms,
                                               bool alertable) {
    const Handle file_handle = static_cast<Handle>(m.arg(0));
    const GuestAddr overlapped = m.arg_ptr(1);
    const GuestAddr bytes_ptr = m.arg_ptr(2);
    const bool is_64bit = m.cpu_is_64bit();

    if (overlapped == 0 || bytes_ptr == 0 || !m_handle_table ||
        (m_handle_table->get_type(file_handle) != HandleType::File &&
         m_handle_table->get_type(file_handle) != HandleType::Pipe &&
         m_handle_table->get_type(file_handle) != HandleType::NetworkNotification)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto read_uptr = [&](GuestAddr address) -> std::optional<u64> {
        if (is_64bit) {
            auto value = m_memory.read_u64(address);
            if (value.is_err()) return std::nullopt;
            return value.value();
        }
        auto value = m_memory.read_u32(address);
        if (value.is_err()) return std::nullopt;
        return value.value();
    };

    constexpr u32 STATUS_PENDING = 0x00000103;
    auto internal = read_uptr(overlapped);
    if (!internal) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    if (static_cast<u32>(*internal) == STATUS_PENDING && timeout_ms != 0 &&
        m_handle_table->get_type(file_handle) == HandleType::NetworkNotification) {
        auto* notification = m_handle_table->get_as<OverlappedNotificationFile>(
            file_handle, HandleType::NetworkNotification);
        if (notification) notification->wait_for_completion(overlapped);
        internal = read_uptr(overlapped);
    }
    if (static_cast<u32>(*internal) == STATUS_PENDING && timeout_ms != 0) {
        const GuestAddr event_field = overlapped + (is_64bit ? 24u : 16u);
        const auto event_value = read_uptr(event_field);
        u32 wait = WAIT_TIMEOUT;
        if (event_value && *event_value != 0 && m_sync_manager) {
            wait = wait_handle_ex(static_cast<Handle>(*event_value & ~u64{1}),
                                  timeout_ms, alertable);
        } else {
            // No hEvent: Windows waits on the file object, which the
            // completion signals. Vitreo publishes every completion through
            // complete_overlapped_file_io, which wakes this wait.
            wait = wait_overlapped_internal(overlapped, is_64bit, timeout_ms,
                                            alertable);
        }
        if (wait == WAIT_IO_COMPLETION) {
            set_last_error(WAIT_IO_COMPLETION);
            m.set_return(0);
            return;
        }
        internal = read_uptr(overlapped);
        if (internal && static_cast<u32>(*internal) == STATUS_PENDING &&
            wait == WAIT_TIMEOUT) {
            set_last_error(WAIT_TIMEOUT);
            m.set_return(0);
            return;
        }
    }

    const u32 status = internal ? static_cast<u32>(*internal) : STATUS_PENDING;
    if (status == STATUS_PENDING) {
        set_last_error(error_codes::ERROR_IO_INCOMPLETE);
        m.set_return(0);
        return;
    }
    // Windows notification cancellation reports zero transferred bytes even
    // when GetOverlappedResult returns ERROR_OPERATION_ABORTED.
    if (m_handle_table->get_type(file_handle) == HandleType::NetworkNotification) {
        const auto bytes = read_uptr(overlapped + (is_64bit ? 8u : 4u));
        if (!bytes || m_memory.write_u32(bytes_ptr, static_cast<u32>(*bytes)).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
    }
    if (status != 0 && status < 0x80000000u) {
        // An informational status (STATUS_NOTIFY_ENUM_DIR 0x10C, ...) is a
        // success: TRUE with its Win32 mapping as the last error (measured,
        // read_dir_changes_probe "last error after informational status").
        const auto bytes = read_uptr(overlapped + (is_64bit ? 8u : 4u));
        if (!bytes || m_memory.write_u32(bytes_ptr, static_cast<u32>(*bytes)).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        set_last_error(overlapped_status_to_win32(status));
        m.set_return(1);
        return;
    }
    if (status != 0) {
        // Report the transferred count on failure too (async EOF -> n=0;
        // STATUS_BUFFER_OVERFLOW -> the partial message length).
        const auto fail_bytes = read_uptr(overlapped + (is_64bit ? 8u : 4u));
        if (fail_bytes) {
            (void)m_memory.write_u32(bytes_ptr, static_cast<u32>(*fail_bytes));
        }
        set_last_error(overlapped_status_to_win32(status));
        m.set_return(0);
        return;
    }

    const auto bytes = read_uptr(overlapped + (is_64bit ? 8u : 4u));
    if (!bytes || m_memory.write_u32(bytes_ptr, static_cast<u32>(*bytes)).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    // Measured on Windows 11: a completed operation sets ERROR_SUCCESS, both
    // after waiting and on a re-query (last_error_on_success_probe.expected).
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

u32 Kernel32ExtManager::wait_overlapped_internal(GuestAddr overlapped,
                                                 bool is_64bit, u32 timeout_ms,
                                                 bool alertable) {
    constexpr u32 STATUS_PENDING = 0x00000103;
    auto pending = [&] {
        const u64 v = is_64bit ? m_memory.read_u64(overlapped).value_or(0)
                               : m_memory.read_u32(overlapped).value_or(0);
        return static_cast<u32>(v) == STATUS_PENDING;
    };
    const bool infinite = timeout_ms == INFINITE_WAIT;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(infinite ? 0 : timeout_ms);
    std::unique_lock lock(m_overlapped_done_mutex);
    for (;;) {
        if (!pending()) return WAIT_OBJECT_0;
        if (alertable) {
            lock.unlock();
            const bool ran = try_deliver_user_apcs();
            lock.lock();
            if (ran) return WAIT_IO_COMPLETION;
        }
        if (m_shutdown.load(std::memory_order_acquire)) return WAIT_TIMEOUT;
        // Completions notify this cv; the slice bounds how long a queued APC
        // waits to be noticed.
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        if (!infinite) {
            if (std::chrono::steady_clock::now() >= deadline) return WAIT_TIMEOUT;
            until = std::min(until, deadline);
        }
        m_overlapped_done_cv.wait_until(lock, until);
    }
}

void Kernel32ExtManager::impl_CancelIo(Marshaler& m) {
    // BOOL CancelIo(HANDLE hFile);
    // Microsoft Learn: cancel pending I/O issued by the calling thread for
    // hFile; cancelled ops complete with ERROR_OPERATION_ABORTED.
    const Handle handle = static_cast<Handle>(m.arg(0));
    if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    const u32 calling_tid = ThreadManager::get_current_thread_id_static();

    // SOCKET values live outside HandleTable (ws2_32 starts at 0x10000).
    if (m_socket_handle_probe && m_socket_handle_probe(static_cast<u32>(handle))) {
        if (m_socket_io_cancel) {
            (void)m_socket_io_cancel(static_cast<u32>(handle), 0, true,
                                     calling_tid);
        }
        // CancelIo succeeds for a valid handle even when nothing was pending.
        m.set_return(1);
        return;
    }

    if (m_handle_table == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const HandleType type = m_handle_table->get_type(handle);
    const bool cancelable =
        type == HandleType::Pipe || type == HandleType::File ||
        type == HandleType::Device || type == HandleType::ConsoleInput ||
        type == HandleType::ConsoleOutput || type == HandleType::Mailslot;
    if (!cancelable) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (m_file_manager != nullptr && type == HandleType::Pipe) {
        (void)m_file_manager->cancel_pipe_handle_io(handle, 0, true);
    }
    if (type == HandleType::File && m_directory_changes) {
        (void)m_directory_changes->cancel(handle, 0, calling_tid);
    }
    if (type == HandleType::Device && m_device_io_cancel) {
        (void)m_device_io_cancel(handle, 0, true, calling_tid);
    }
    // CancelIo succeeds when the cancel request is accepted for a valid
    // overlapped handle; no-pending is still TRUE on Windows.
    m.set_return(1);
}

void Kernel32ExtManager::impl_CancelIoEx(Marshaler& m) {
    // BOOL CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr overlapped = m.arg_ptr(1);
    if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    if (m_socket_handle_probe && m_socket_handle_probe(static_cast<u32>(handle))) {
        if (!m_socket_io_cancel) {
            set_last_error(error_codes::ERROR_NOT_FOUND);
            m.set_return(0);
            return;
        }
        const usize marked = m_socket_io_cancel(static_cast<u32>(handle),
                                                 overlapped, false, 0);
        if (marked == 0) {
            set_last_error(error_codes::ERROR_NOT_FOUND);
            m.set_return(0);
            return;
        }
        m.set_return(1);
        return;
    }

    if (m_handle_table == nullptr ||
        (m_handle_table->get_type(handle) != HandleType::Pipe &&
         m_handle_table->get_type(handle) != HandleType::File &&
         m_handle_table->get_type(handle) != HandleType::Device &&
         m_handle_table->get_type(handle) != HandleType::ConsoleInput &&
         m_handle_table->get_type(handle) != HandleType::ConsoleOutput &&
         m_handle_table->get_type(handle) != HandleType::Mailslot)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (m_handle_table->get_type(handle) == HandleType::File && m_directory_changes &&
        m_directory_changes->cancel(handle, overlapped, 0) > 0) {
        m.set_return(1);   // a pending ReadDirectoryChangesW (STATUS_CANCELLED)
        return;
    }
    if (m_handle_table->get_type(handle) == HandleType::Device && m_device_io_cancel &&
        m_device_io_cancel(handle, overlapped, false, 0) > 0) {
        m.set_return(1);   // a pending request-queue receive (STATUS_CANCELLED)
        return;
    }
    if (m_file_manager == nullptr ||
        m_handle_table->get_type(handle) != HandleType::Pipe) {
        // Non-pipe CancelIoEx: nothing tracked to cancel.
        set_last_error(error_codes::ERROR_NOT_FOUND);
        m.set_return(0);
        return;
    }
    const usize marked = m_file_manager->cancel_pipe_handle_io(
        handle, overlapped, false);
    if (marked == 0) {
        set_last_error(error_codes::ERROR_NOT_FOUND);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_CancelSynchronousIo(Marshaler& m) {
    // BOOL CancelSynchronousIo(HANDLE hThread)
    // https://learn.microsoft.com/en-us/windows/win32/fileio/cancelsynchronousio-func
    // Requires THREAD_TERMINATE. Idle → ERROR_NOT_FOUND (0x490).
    // Marks an in-flight UnixPipe sync ReadFile so it returns
    // ERROR_OPERATION_ABORTED.
    const Handle thread_handle = static_cast<Handle>(m.arg(0));
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_NOT_FOUND);
        m.set_return(0);
        return;
    }
    constexpr u32 kThreadTerminate = 0x0001;
    u32 err = 0;
    const u32 tid = m_thread_manager->thread_id_for_handle(
        thread_handle, kThreadTerminate, err);
    if (tid == 0) {
        set_last_error(err != 0 ? err : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_NOT_FOUND);
        m.set_return(0);
        return;
    }
    const usize marked = m_file_manager->cancel_synchronous_io(tid);
    if (marked == 0) {
        set_last_error(error_codes::ERROR_NOT_FOUND);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
