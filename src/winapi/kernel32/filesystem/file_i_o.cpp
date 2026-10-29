// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_io_ext.cpp
/// @brief Overlapped I/O and completion ports.
///
/// FlushFileBuffers, the overlapped read/write forms, CreateIoCompletionPort,
/// GetQueuedCompletionStatus and the notification-mode controls. file.cpp owns
/// synchronous file access; this is the asynchronous surface layered over it.
///
/// Its one tie back to kernel32_ext.cpp is iocp_diag_log — the VITREO_IOCP_DIAG
/// tracer, which timestamps each line and locks stderr so completions arriving
/// on several threads stay readable. Declared in kernel32_ext_internal.hpp.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/synthetic_handle_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "ipc/process_handle.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

/// @file file_i_o.cpp
/// @brief File I/O
///
/// Closed cut from file_io_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// File I/O
// =============================================================================

void Kernel32ExtManager::impl_FlushFileBuffers(Marshaler& m) {
    // BOOL FlushFileBuffers(HANDLE hFile)
    // Learn: needs GENERIC_WRITE; invalid/non-file handles fail
    // ERROR_INVALID_HANDLE; read-only opens fail ERROR_ACCESS_DENIED
    // (files_core_probe Flush_*).
    const Handle h_file = static_cast<Handle>(m.arg(0));
    constexpr u32 kGenericWrite = 0x40000000u;
    constexpr u32 kFileWriteData = 0x0002u;
    constexpr u32 kFileAppendData = 0x0004u;

    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    HandleTable& ht = m_file_manager->handle_table();
    const HandleType type = ht.get_type(h_file);
    if (type == HandleType::Invalid || type == HandleType::Event ||
        type == HandleType::Mutex || type == HandleType::Semaphore ||
        type == HandleType::ConsoleOutput || type == HandleType::ConsoleInput) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    const u32 access = ht.get_access(h_file);
    const bool can_write =
        (access & (kGenericWrite | kFileWriteData | kFileAppendData)) != 0;
    if (!can_write) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }

    capsule::FileHandle vfs_h = m_file_manager->vfs_handle_for(h_file);
    if (vfs_h == capsule::INVALID_FILE_HANDLE && m_ntdll_ext_manager) {
        auto nt = m_ntdll_ext_manager->vfs_handle_for(h_file);
        if (nt.has_value()) vfs_h = nt.value();
    }
    if (vfs_h != capsule::INVALID_FILE_HANDLE) {
        (void)m_vfs.flush(vfs_h);
    }
    // Named-pipe write ends may not have a VFS handle; success is still
    // required when GENERIC_WRITE was granted (Flush_pipe_write_end).
    static const bool file_trace = std::getenv("VITREO_FILE_TRACE") != nullptr;
    if (file_trace) {
        VITREO_TRACE("[k32ext] FlushFileBuffers(handle=0x%x)\n",
                     static_cast<unsigned>(h_file));
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_SetEndOfFile(Marshaler& m) {
    // BOOL SetEndOfFile(HANDLE hFile)
    const Handle handle = static_cast<Handle>(m.arg(0));

    if (m_file_manager && m_file_manager->vfs_handle_for(handle) != capsule::INVALID_FILE_HANDLE) {
        m.set_return(m_file_manager->set_end_of_file(handle) ? 1 : 0);
        return;
    }
    if (m_ntdll_ext_manager && m_ntdll_ext_manager->vfs_handle_for(handle).has_value()) {
        const u32 status = m_ntdll_ext_manager->nt_set_end_of_file(handle);
        m.set_return(status == 0 ? 1 : 0);
        return;
    }

    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetFileTime(Marshaler& m) {
    // BOOL GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime,
    //                  LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime)
    //
    // Microsoft Learn: every non-NULL pointer receives that time as a FILETIME
    // (100 ns intervals since 1601-01-01 UTC). Reporting TRUE while storing
    // nothing leaves the caller's structures at whatever they held, which
    // callers read back as a zero FILETIME — a value no real file has.
    // Converting that zero FILETIME fails the caller's rotation.
    const Handle h_file = static_cast<Handle>(m.arg(0));
    const GuestAddr creation_ptr = m.arg_ptr(1);
    const GuestAddr access_ptr = m.arg_ptr(2);
    const GuestAddr write_ptr = m.arg_ptr(3);

    capsule::FileHandle vfs_handle = capsule::INVALID_FILE_HANDLE;
    if (m_file_manager != nullptr) {
        vfs_handle = m_file_manager->vfs_handle_for(h_file);
    }
    if (vfs_handle == capsule::INVALID_FILE_HANDLE &&
        m_ntdll_ext_manager != nullptr) {
        if (auto nt_handle = m_ntdll_ext_manager->vfs_handle_for(h_file);
            nt_handle.has_value()) {
            vfs_handle = nt_handle.value();
        }
    }
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        VITREO_TRACE("[file] GetFileTime(0x%x) -> FALSE (unknown handle)\n",
                     h_file);
        m.set_return(0);
        return;
    }

    auto info_result = m_vfs.stat_handle(vfs_handle);
    if (info_result.is_err()) {
        set_last_error(vfs_error_to_win32(info_result.error()));
        VITREO_TRACE("[file] GetFileTime(0x%x) -> FALSE (%s)\n", h_file,
                     capsule::vfs_error_to_string(info_result.error()));
        m.set_return(0);
        return;
    }

    const capsule::FileInfo& info = info_result.value();
    const auto write_filetime = [&](GuestAddr address, u64 value) {
        if (address == 0) return;
        (void)m_memory.write_u32(address, static_cast<u32>(value));
        (void)m_memory.write_u32(address + 4, static_cast<u32>(value >> 32));
    };
    write_filetime(creation_ptr, info.creation_time);
    write_filetime(access_ptr, info.last_access_time);
    write_filetime(write_ptr, info.last_write_time);

    VITREO_TRACE("[file] GetFileTime(0x%x) write=%llu -> TRUE\n", h_file,
                 static_cast<unsigned long long>(info.last_write_time));
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_GetFileInformationByHandle(Marshaler& m) {
    // BOOL GetFileInformationByHandle(HANDLE hFile,
    //   LPBY_HANDLE_FILE_INFORMATION lpFileInformation)
    const Handle h_file = static_cast<Handle>(m.arg(0));
    const GuestAddr info_ptr = m.arg_ptr(1);
    if (info_ptr == 0 || m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const capsule::FileHandle vfs_handle = m_file_manager->vfs_handle_for(h_file);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        VITREO_TRACE("[file] GetFileInformationByHandle(0x%x) -> FALSE (unknown handle)\n",
                     h_file);
        m.set_return(0);
        return;
    }

    auto info_result = m_vfs.stat_handle(vfs_handle);
    if (info_result.is_err()) {
        set_last_error(vfs_error_to_win32(info_result.error()));
        VITREO_TRACE("[file] GetFileInformationByHandle(0x%x) -> FALSE (%s)\n",
                     h_file, capsule::vfs_error_to_string(info_result.error()));
        m.set_return(0);
        return;
    }

    const capsule::FileInfo& info = info_result.value();
    const u32 attrs = static_cast<u32>(info.attributes) == 0
        ? 0x80u : static_cast<u32>(info.attributes);
    const auto write_u64 = [&](GuestAddr address, u64 value) {
        (void)m_memory.write_u32(address, static_cast<u32>(value));
        (void)m_memory.write_u32(address + 4, static_cast<u32>(value >> 32));
    };

    (void)m_memory.write_u32(info_ptr + 0, attrs);
    write_u64(info_ptr + 4, info.creation_time);
    write_u64(info_ptr + 12, info.last_access_time);
    write_u64(info_ptr + 20, info.last_write_time);
    (void)m_memory.write_u32(info_ptr + 28, static_cast<u32>(info.volume_id));
    (void)m_memory.write_u32(info_ptr + 32, static_cast<u32>(info.size >> 32));
    (void)m_memory.write_u32(info_ptr + 36, static_cast<u32>(info.size));
    (void)m_memory.write_u32(info_ptr + 40, info.link_count);
    (void)m_memory.write_u32(info_ptr + 44, static_cast<u32>(info.file_id >> 32));
    (void)m_memory.write_u32(info_ptr + 48, static_cast<u32>(info.file_id));

    VITREO_TRACE("[file] GetFileInformationByHandle(0x%x) size=%llu attrs=0x%x id=%llx:%llx -> TRUE\n",
                 h_file, static_cast<unsigned long long>(info.size), attrs,
                 static_cast<unsigned long long>(info.volume_id),
                 static_cast<unsigned long long>(info.file_id));
    m.set_return(1);
}

void Kernel32ExtManager::impl_FindFirstFileExW(Marshaler& m) {
    // HANDLE FindFirstFileExW(LPCWSTR lpFileName,
    //   FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
    //   FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags)
    //
    // INVALID_HANDLE_VALUE is (HANDLE)(LONG_PTR)-1 — all bits set. Returning
    // only 0xFFFFFFFF to a PE32+ guest leaves RAX with a zero high dword, so
    // `hFind == INVALID_HANDLE_VALUE` is false and callers proceed to
    // FindNextFile on a non-handle.
    const u64 invalid_handle =
        m.cpu_is_64bit() ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu;

    GuestAddr filename_ptr = m.arg_ptr(0);
    const u32 info_level = static_cast<u32>(m.arg(1));
    GuestAddr data_ptr = m.arg_ptr(2);
    const u32 search_op = static_cast<u32>(m.arg(3));
    (void)m.arg_ptr(4);  // lpSearchFilter: Windows ignores it (ex_search_filter_nonnull succeeds)
    const u32 flags = m.arg32(5);

    // Parameter contract measured on Windows 11 (files_meta_probe.expected):
    //   fInfoLevelId: FindExInfoStandard(0) / FindExInfoBasic(1), else 87.
    //   fSearchOp: NameMatch(0) and LimitToDirectories(1, an advisory Learn
    //   lets the file system ignore) run; LimitToDevices(2) -> 50
    //   ERROR_NOT_SUPPORTED; anything else -> 87.
    //   dwAdditionalFlags: CASE_SENSITIVE(1) | LARGE_FETCH(2) |
    //   ON_DISK_ENTRIES_ONLY(4); any other bit -> 87. CASE_SENSITIVE does not
    //   change matching on a case-insensitive directory (ex_case_sensitive_*),
    //   which is what every Vitreo directory is.
    if (info_level > 1 || search_op > 2 || (flags & ~0x7u) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid_handle);
        return;
    }
    if (search_op == 2) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(invalid_handle);
        return;
    }

    if (!m_file_search_manager) {
        VITREO_TRACE("[k32ext] FindFirstFileExW: no FileSearchManager\n");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(invalid_handle);
        return;
    }

    auto path_res = m.read_wide_string(filename_ptr);
    if (path_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid_handle);
        return;
    }
    const std::string& pattern = path_res.value();
    VITREO_TRACE("[k32ext] FindFirstFileExW: pattern=\"%s\"\n", pattern.c_str());

    FindData data{};
    u64 h = m_file_search_manager->find_first_file(pattern, data);
    if (h == 0) {
        VITREO_TRACE("[k32ext] FindFirstFileExW: no match -> INVALID_HANDLE_VALUE\n");
        m.set_return(invalid_handle);
        return;
    }

    write_find_data_w(m, data_ptr, data);

    VITREO_TRACE("[k32ext] FindFirstFileExW: handle=%llu first='%s' attrs=0x%x\n",
                static_cast<unsigned long long>(h), data.file_name.c_str(), data.attributes);
    m.set_return(h);
}

void Kernel32ExtManager::impl_DeviceIoControl(Marshaler& m) {
    // BOOL DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode,
    //   LPVOID lpInBuffer, DWORD nInBufferSize,
    //   LPVOID lpOutBuffer, DWORD nOutBufferSize,
    //   LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 control_code = static_cast<u32>(m.arg(1));
    const GuestAddr input = m.arg_ptr(2);
    const u32 input_size = static_cast<u32>(m.arg(3));
    const GuestAddr output = m.arg_ptr(4);
    const u32 output_size = static_cast<u32>(m.arg(5));
    GuestAddr bytes_returned = m.arg_ptr(6);
    (void)m.arg_ptr(7);

    u32 transferred = 0;
    const bool ok = m_file_manager != nullptr &&
        m_file_manager->device_io_control(
            handle, control_code, input, input_size, output, output_size,
            transferred);
    if (bytes_returned != 0 &&
        m_memory.write_u32(bytes_returned, transferred).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    VITREO_TRACE("[device] DeviceIoControl(h=0x%x code=0x%08x in=%u "
                 "out=%u) -> %s bytes=%u gle=%u\n",
                 handle, control_code, input_size, output_size,
                 ok ? "TRUE" : "FALSE", transferred, get_last_error());
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::complete_overlapped_file_io(
    Handle file_handle, u32 bytes_transferred, GuestAddr overlapped,
    bool is_64bit, u32 nt_status, u32 win_error,
    bool completed_synchronously) {
    if (overlapped == 0) return;

    const GuestAddr internal_high = overlapped + (is_64bit ? 8u : 4u);
    const GuestAddr event_field = overlapped + (is_64bit ? 24u : 16u);
    const auto status_write = is_64bit
        ? m_memory.write_u64(overlapped, nt_status)
        : m_memory.write_u32(overlapped, nt_status);
    const auto count_write = is_64bit
        ? m_memory.write_u64(internal_high, bytes_transferred)
        : m_memory.write_u32(internal_high, bytes_transferred);
    if (status_write.is_err() || count_write.is_err()) {
        VITREO_TRACE("[iocp] completion file=0x%x has invalid OVERLAPPED=0x%llx\n",
                     file_handle, static_cast<unsigned long long>(overlapped));
        return;
    }

    u64 event_value = 0;
    if (is_64bit) {
        event_value = m_memory.read_u64(event_field).value_or(0);
    } else {
        event_value = m_memory.read_u32(event_field).value_or(0);
    }

    std::shared_ptr<CompletionPort> port;
    u32 port_handle = 0;
    u64 completion_key = 0;
    u8 modes = 0;
    bool association_found = false;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        if (auto pending = m_pending_file_completions.find(overlapped);
            pending != m_pending_file_completions.end()) {
            association_found = true;
            port = pending->second.port;
            port_handle = pending->second.port_handle;
            completion_key = pending->second.completion_key;
            modes = pending->second.modes;
            m_pending_file_completions.erase(pending);
        } else {
            if (auto mode = m_file_completion_modes.find(file_handle);
                mode != m_file_completion_modes.end()) {
                modes = mode->second;
            }
            if (auto association = m_iocp_file_associations.find(file_handle);
                association != m_iocp_file_associations.end()) {
                association_found = true;
                port_handle = association->second.port_handle;
                completion_key = association->second.completion_key;
                if (auto found = m_iocp_ports.find(association->second.port_handle);
                    found != m_iocp_ports.end()) {
                    port = found->second;
                }
            }
        }
    }
    record_completion_trace(
        OverlappedIoTraceStage::CompletionPrepared, file_handle, overlapped,
        bytes_transferred, port_handle, completion_key, event_value, modes,
        association_found, nt_status, win_error);

    {
        // Internal/InternalHigh are final: wake GetOverlappedResult calls
        // that wait on the file object (hEvent NULL).
        std::lock_guard done_lock(m_overlapped_done_mutex);
    }
    m_overlapped_done_cv.notify_all();

    constexpr u8 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS = 0x01;
    // FILE_SKIP_SET_EVENT_ON_HANDLE only stops the I/O manager signalling the
    // file object itself; "if an explicit event is provided for the request,
    // it is still signaled" (Learn, SetFileCompletionNotificationModes).
    // Skipping OVERLAPPED.hEvent here left libuv-style waiters on hEvent|1
    // blocked forever.
    if (event_value != 0 && m_sync_manager) {
        (void)m_sync_manager->set_event(static_cast<Handle>(event_value & ~u64{1}));
    }

    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS only suppresses packets for I/O that
    // completed inline (ReadFile/WriteFile returned TRUE). Async completions
    // that previously returned ERROR_IO_PENDING must still reach the port.
    // The low bit of hEvent still suppresses
    // the packet for any completion, matching Windows.
    const bool skip_completion_port =
        !port ||
        (event_value & 1u) != 0 ||
        (completed_synchronously &&
         (modes & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0);
    if (skip_completion_port) {
        VITREO_TRACE("[iocp] completed file=0x%x bytes=%u ovlp=0x%llx event=0x%llx "
                     "sync=%u modes=0x%x (no port packet)\n",
                     file_handle, bytes_transferred,
                     static_cast<unsigned long long>(overlapped),
                     static_cast<unsigned long long>(event_value),
                     completed_synchronously ? 1u : 0u, modes);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(port->mutex);
        port->queue.push_back(
            {bytes_transferred, completion_key, overlapped,
             nt_status, win_error});
    }
    port->cv.notify_one();
    record_completion_trace(
        OverlappedIoTraceStage::PacketQueued, file_handle, overlapped,
        bytes_transferred, port_handle, completion_key, event_value, modes,
        association_found, nt_status, win_error);
    VITREO_TRACE("[iocp] completed file=0x%x bytes=%u key=0x%llx ovlp=0x%llx -> queued\n",
                 file_handle, bytes_transferred,
                 static_cast<unsigned long long>(completion_key),
                 static_cast<unsigned long long>(overlapped));
}

void Kernel32ExtManager::complete_nt_io(
    Handle file_handle, GuestAddr apc_context, Handle event,
    GuestAddr apc_routine, GuestAddr io_status_block, u32 issuing_tid,
    u32 bytes_transferred, u32 nt_status, u32 win_error, bool synchronous) {
    {
        // The status block is final: wake GetOverlappedResult waiters.
        std::lock_guard done_lock(m_overlapped_done_mutex);
    }
    m_overlapped_done_cv.notify_all();

    if (event != NULL_HANDLE && m_sync_manager) {
        (void)m_sync_manager->set_event(event);
    }
    // An APC routine is the request's notification; the I/O manager posts no
    // completion packet for it.
    if (apc_routine != 0) {
        if (m_thread_manager) {
            (void)m_thread_manager->queue_nt_io_apc_to(issuing_tid, apc_routine, apc_context,
                                                       io_status_block);
        }
        return;
    }

    const bool failed = (nt_status & 0xC0000000u) == 0xC0000000u;  // NT_ERROR
    const bool succeeded = (nt_status & 0x80000000u) == 0;         // NT_SUCCESS
    std::shared_ptr<CompletionPort> port;
    u64 completion_key = 0;
    u8 modes = 0;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        if (auto mode = m_file_completion_modes.find(file_handle); mode != m_file_completion_modes.end()) {
            modes = mode->second;
        }
        if (auto association = m_iocp_file_associations.find(file_handle);
            association != m_iocp_file_associations.end()) {
            completion_key = association->second.completion_key;
            if (auto found = m_iocp_ports.find(association->second.port_handle); found != m_iocp_ports.end()) {
                port = found->second;
            }
        }
    }
    constexpr u8 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS = 0x01;
    if (!port || (synchronous && failed) ||
        (synchronous && succeeded && (modes & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0)) {
        VITREO_TRACE("[iocp] nt request file=0x%x status=0x%x bytes=%u sync=%u modes=0x%x (no port packet)\n",
                     file_handle, nt_status, bytes_transferred, synchronous ? 1u : 0u, modes);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(port->mutex);
        port->queue.push_back({bytes_transferred, completion_key, apc_context, nt_status, win_error});
    }
    port->cv.notify_one();
    VITREO_TRACE("[iocp] nt request file=0x%x status=0x%x bytes=%u key=0x%llx ctx=0x%llx -> queued\n",
                 file_handle, nt_status, bytes_transferred, static_cast<unsigned long long>(completion_key),
                 static_cast<unsigned long long>(apc_context));
}

bool Kernel32ExtManager::enqueue_completion_packet(
    u32 port_handle, CompletionEntry entry) {
    std::shared_ptr<CompletionPort> port;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        const auto found = m_iocp_ports.find(port_handle);
        if (found != m_iocp_ports.end()) port = found->second;
    }
    if (!port) return false;
    {
        std::lock_guard<std::mutex> lock(port->mutex);
        if (port->closed) return false;
        port->queue.push_back(entry);
    }
    port->cv.notify_one();
    return true;
}

void Kernel32ExtManager::impl_CreateIoCompletionPort(Marshaler& m) {
    // HANDLE CreateIoCompletionPort(HANDLE FileHandle,
    //   HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey,
    //   DWORD NumberOfConcurrentThreads)
    Handle file_handle = static_cast<Handle>(m.arg(0));
    u32 existing_port  = static_cast<u32>(m.arg(1));
    u64 completion_key = m.arg(2);
    (void)m.arg(3);  // NumberOfConcurrentThreads

    const char* associate_type = "?";
    if (file_handle != INVALID_HANDLE) {
        if (file_handle == NULL_HANDLE || !m_handle_table) {
            ipc::ipc_log(
                "CreateIoCompletionPort reject h=0x%x (null/no table)",
                static_cast<unsigned>(file_handle));
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const HandleType type = m_handle_table->get_type(file_handle);
        // Any file object can be associated, device handles included (an
        // HTTP Server API request queue is one).
        const bool is_file_or_pipe =
            type == HandleType::File || type == HandleType::Pipe ||
            type == HandleType::Device;
        // Vitreo sockets are ws2-managed IDs, not HandleTable File/Pipe
        // entries. Windows still allows CreateIoCompletionPort(SOCKET, …)
        // for overlapped Winsock. The caller associates the socket and does
        // not connect if association fails.
        const bool is_socket =
            !is_file_or_pipe && m_socket_handle_probe &&
            m_socket_handle_probe(static_cast<u32>(file_handle));
        if (!is_file_or_pipe && !is_socket) {
            ipc::ipc_log(
                "CreateIoCompletionPort reject h=0x%x type=%s",
                static_cast<unsigned>(file_handle),
                handle_type_to_string(type));
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        associate_type = is_socket ? "Socket" : handle_type_to_string(type);
    }

    std::shared_ptr<CompletionPort> port;
    u32 port_handle = existing_port;
    Handle allocated_port = NULL_HANDLE;

    if (existing_port != 0) {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        auto found = m_iocp_ports.find(existing_port);
        if (found == m_iocp_ports.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        port = found->second;
        if (file_handle != INVALID_HANDLE) {
            m_iocp_file_associations[file_handle] = {port_handle, completion_key};
        }
    } else {
        // Prefer HandleTable so CloseHandle / DuplicateHandle work. Fall back to
        // the legacy 0xCC namespace only when no table is wired (rare unit paths).
        if (m_handle_table) {
            allocated_port = m_handle_table->allocate(
                HandleType::IoCompletionPort, nullptr);
            if (allocated_port == NULL_HANDLE) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                m.set_return(0);
                return;
            }
            port_handle = allocated_port;
        }
        port = std::make_shared<CompletionPort>();
        {
            std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
            if (!m_handle_table) {
                port_handle = 0xCC000000u + (++m_iocp_counter);
                synthetic_handles::note_issued(port_handle);
            }
            m_iocp_ports[port_handle] = port;
            if (file_handle != INVALID_HANDLE) {
                m_iocp_file_associations[file_handle] =
                    {port_handle, completion_key};
            }
        }
    }

    if (file_handle != INVALID_HANDLE) {
        ipc::ipc_log(
            "CreateIoCompletionPort associate h=0x%x port=0x%x key=0x%llx type=%s",
            static_cast<unsigned>(file_handle), port_handle,
            static_cast<unsigned long long>(completion_key), associate_type);
        VITREO_TRACE("[iocp] CreateIoCompletionPort: associate file=0x%x with port=0x%x key=0x%llx\n",
                     file_handle, port_handle,
                     static_cast<unsigned long long>(completion_key));
    } else {
        ipc::ipc_log("CreateIoCompletionPort new port=0x%x", port_handle);
        VITREO_TRACE("[iocp] CreateIoCompletionPort: new port=0x%x\n", port_handle);
    }
    m.set_return(port_handle);
}

void Kernel32ExtManager::impl_PostQueuedCompletionStatus(Marshaler& m) {
    // BOOL PostQueuedCompletionStatus(HANDLE CompletionPort,
    //   DWORD dwNumberOfBytesTransferred, ULONG_PTR dwCompletionKey,
    //   LPOVERLAPPED lpOverlapped)
    u32 port_handle = static_cast<u32>(m.arg(0));
    u32 bytes       = static_cast<u32>(m.arg(1));
    u64 key         = m.arg(2);          // ULONG_PTR — full width
    GuestAddr ovlp  = m.arg_ptr(3);

    std::shared_ptr<CompletionPort> port;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        if (auto it = m_iocp_ports.find(port_handle); it != m_iocp_ports.end()) {
            port = it->second;
        }
    }
    if (!port) {
        VITREO_TRACE("[iocp] PostQueuedCompletionStatus: unknown port 0x%x\n", port_handle);
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0); // FALSE
        return;
    }

    {
        std::lock_guard<std::mutex> lk(port->mutex);
        port->queue.push_back({bytes, key, ovlp, 0, 0});
    }
    port->cv.notify_one();
    static const bool iocp_diag_posts =
        std::getenv("VITREO_IOCP_DIAG_POSTS") != nullptr;
    if (iocp_diag_posts) {
        iocp_diag_log(
            "stage=ExplicitPacketQueued tid=%u port=0x%x bytes=%u "
            "key=0x%llx ov=0x%llx",
            ThreadManager::get_current_thread_id_static(), port_handle, bytes,
            static_cast<unsigned long long>(key),
            static_cast<unsigned long long>(ovlp));
    }
    VITREO_TRACE("[iocp] PostQueuedCompletionStatus: port=0x%x bytes=%u key=0x%llx ovlp=0x%llx\n",
                 port_handle, bytes, static_cast<unsigned long long>(key),
                 static_cast<unsigned long long>(ovlp));
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_GetQueuedCompletionStatus(Marshaler& m) {
    // BOOL GetQueuedCompletionStatus(HANDLE CompletionPort,
    //   LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey,
    //   LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds)
    u32 port_handle   = static_cast<u32>(m.arg(0));
    GuestAddr bytes_p = m.arg_ptr(1);
    GuestAddr key_p   = m.arg_ptr(2);
    GuestAddr ovlp_p  = m.arg_ptr(3);
    u32 timeout_ms    = static_cast<u32>(m.arg(4));

    // ULONG_PTR / LPOVERLAPPED are pointer-width: 8 bytes in x64, 4 in x32.
    // Writing only 4 bytes in x64 leaves the upper half as stack garbage,
    // so the guest sees a corrupt OVERLAPPED* and never matches its pending
    // async operation.
    const bool is64 = m.cpu_is_64bit();
    auto write_uptr = [&](GuestAddr addr, u64 value) {
        if (is64) (void)m.write_u64(addr, value);
        else      (void)m.write_u32(addr, static_cast<u32>(value));
    };

    std::shared_ptr<CompletionPort> port;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        if (auto it = m_iocp_ports.find(port_handle); it != m_iocp_ports.end()) {
            port = it->second;
        }
    }
    if (!port) {
        VITREO_TRACE("[iocp] GetQueuedCompletionStatus: unknown port 0x%x\n", port_handle);
        if (ovlp_p) write_uptr(ovlp_p, 0);
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    std::unique_lock<std::mutex> lk(port->mutex);

    bool got = false;
    if (timeout_ms == 0) {
        got = !port->queue.empty();
    } else if (timeout_ms == 0xFFFFFFFF) {
        // INFINITE — wait until entry available
        port->cv.wait(lk, [this, &port] {
            return !port->queue.empty() || port->closed ||
                   m_shutdown.load(std::memory_order_acquire);
        });
        got = !port->queue.empty();
    } else {
        got = port->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [this, &port] {
                                    return !port->queue.empty() || port->closed ||
                                           m_shutdown.load(std::memory_order_acquire);
                                });
        if (got) got = !port->queue.empty();
    }

    if (!got) {
        if (ovlp_p) write_uptr(ovlp_p, 0);
        const bool cancelled =
            port->closed || m_shutdown.load(std::memory_order_acquire);
        set_last_error(cancelled ? error_codes::ERROR_OPERATION_ABORTED
                                 : 258u /* WAIT_TIMEOUT */);
        VITREO_TRACE(
            "[iocp] GetQueuedCompletionStatus: port=0x%x %s (%u ms)\n",
            port_handle, cancelled ? "cancelled by process shutdown" : "timeout",
            timeout_ms);
        m.set_return(0);
        return;
    }

    CompletionEntry entry = port->queue.front();
    port->queue.pop_front();
    lk.unlock();

    static const bool iocp_diag_posts =
        std::getenv("VITREO_IOCP_DIAG_POSTS") != nullptr;
    if (iocp_diag_posts) {
        // For ScheduleWork-shaped packets (bytes=0, key==ov), log the guest
        // return address so we can see whether MessagePump resumes into
        // ProcessInternalIOItem → DoWork after the wake.
        u64 guest_ret = 0;
        if (m.has_cpu() && !m.cpu_is_64bit()) {
            const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
            guest_ret = m.read_u32(sp).value_or(0);
        } else if (m.has_cpu()) {
            const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
            guest_ret = m.read_u64(sp).value_or(0);
        }
        iocp_diag_log(
            "stage=AnyPacketDequeued api=GQCS tid=%u port=0x%x bytes=%u "
            "key=0x%llx ov=0x%llx status=0x%x gle=%u ret=0x%llx",
            ThreadManager::get_current_thread_id_static(), port_handle,
            entry.bytes_transferred,
            static_cast<unsigned long long>(entry.completion_key),
            static_cast<unsigned long long>(entry.overlapped), entry.nt_status,
            entry.win_error,
            static_cast<unsigned long long>(guest_ret));
    }
    if (bytes_p) (void)m.write_u32(bytes_p, entry.bytes_transferred);
    if (key_p)   write_uptr(key_p, entry.completion_key);
    if (ovlp_p)  write_uptr(ovlp_p, entry.overlapped);
    record_completion_trace(
        OverlappedIoTraceStage::PacketDequeued, NULL_HANDLE,
        entry.overlapped, entry.bytes_transferred, port_handle,
        entry.completion_key, 0, 0, true, entry.nt_status, entry.win_error);

    VITREO_TRACE("[iocp] GetQueuedCompletionStatus: port=0x%x bytes=%u key=0x%llx ovlp=0x%llx\n",
                 port_handle, entry.bytes_transferred,
                 static_cast<unsigned long long>(entry.completion_key),
                 static_cast<unsigned long long>(entry.overlapped));
    if (entry.nt_status != 0) {
        set_last_error(entry.win_error != 0
                           ? entry.win_error
                           : error_codes::ERROR_GEN_FAILURE);
        m.set_return(0);
        return;
    }
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_GetQueuedCompletionStatusEx(Marshaler& m) {
    // BOOL GetQueuedCompletionStatusEx(HANDLE CompletionPort,
    //   LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount,
    //   PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable)
    // Learn (ioapiset): fAlertable TRUE performs an alertable wait when no
    // entries are available; an APC/completion-routine wake returns FALSE
    // after the APC runs (GetLastError = WAIT_IO_COMPLETION), matching
    // WaitForSingleObjectEx / SleepEx.
    u32 port_handle   = static_cast<u32>(m.arg(0));
    GuestAddr entries = m.arg_ptr(1);
    u32 max_count     = static_cast<u32>(m.arg(2));
    GuestAddr num_out = m.arg_ptr(3);
    u32 timeout_ms    = static_cast<u32>(m.arg(4));
    const bool alertable = m.arg(5) != 0;

    std::shared_ptr<CompletionPort> port;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        if (auto it = m_iocp_ports.find(port_handle); it != m_iocp_ports.end()) {
            port = it->second;
        }
    }
    if (!port) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    auto fail_alertable_apc = [&]() {
        if (num_out) (void)m.write_u32(num_out, 0);
        set_last_error(WAIT_IO_COMPLETION);
        m.set_return(0);
    };

    // Pending APCs end an alertable wait before blocking on the port.
    if (alertable && try_deliver_user_apcs()) {
        fail_alertable_apc();
        return;
    }

    std::unique_lock<std::mutex> lk(port->mutex);

    auto queue_ready = [this, &port] {
        return !port->queue.empty() || port->closed ||
               m_shutdown.load(std::memory_order_acquire);
    };

    bool got = false;
    if (timeout_ms == 0) {
        got = !port->queue.empty();
    } else if (!alertable) {
        if (timeout_ms == 0xFFFFFFFF) {
            port->cv.wait(lk, queue_ready);
            got = !port->queue.empty();
        } else {
            got = port->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                    queue_ready);
            if (got) got = !port->queue.empty();
        }
    } else {
        // Slice so APCs queued after entry are observed promptly.
        const bool infinite = timeout_ms == 0xFFFFFFFF;
        u32 remaining = timeout_ms;
        constexpr u32 kSliceMs = 5u;
        for (;;) {
            if (!port->queue.empty()) {
                got = true;
                break;
            }
            if (port->closed || m_shutdown.load(std::memory_order_acquire)) {
                break;
            }
            lk.unlock();
            if (try_deliver_user_apcs()) {
                fail_alertable_apc();
                return;
            }
            lk.lock();
            if (!port->queue.empty()) {
                got = true;
                break;
            }
            const u32 slice = infinite ? kSliceMs
                                       : (remaining < kSliceMs ? remaining
                                                               : kSliceMs);
            const bool woke = port->cv.wait_for(
                lk, std::chrono::milliseconds(slice), queue_ready);
            if (woke && !port->queue.empty()) {
                got = true;
                break;
            }
            if (!infinite) {
                if (remaining <= slice) break;
                remaining -= slice;
            }
        }
    }

    if (!got) {
        if (num_out) (void)m.write_u32(num_out, 0);
        const bool cancelled =
            port->closed || m_shutdown.load(std::memory_order_acquire);
        set_last_error(cancelled ? error_codes::ERROR_OPERATION_ABORTED
                                 : 258u /* WAIT_TIMEOUT */);
        m.set_return(0);
        return;
    }

    // Dequeue up to max_count entries.
    // OVERLAPPED_ENTRY { ULONG_PTR lpCompletionKey, LPOVERLAPPED lpOverlapped,
    //                    ULONG_PTR Internal, DWORD dwNumberOfBytesTransferred }
    //   x86: 4-byte fields, 16 bytes per entry.
    //   x64: 8-byte pointer fields + 4-byte DWORD with trailing pad, 32 bytes.
    const bool is64 = m.cpu_is_64bit();
    const u32 entry_size = is64 ? 32u : 16u;
    u32 count = 0;
    static const bool iocp_diag_posts =
        std::getenv("VITREO_IOCP_DIAG_POSTS") != nullptr;
    while (count < max_count && !port->queue.empty()) {
        CompletionEntry ce = port->queue.front();
        port->queue.pop_front();
        if (iocp_diag_posts) {
            iocp_diag_log(
                "stage=AnyPacketDequeued api=GQCSEx tid=%u port=0x%x "
                "bytes=%u key=0x%llx ov=0x%llx status=0x%x gle=%u",
                ThreadManager::get_current_thread_id_static(), port_handle,
                ce.bytes_transferred,
                static_cast<unsigned long long>(ce.completion_key),
                static_cast<unsigned long long>(ce.overlapped), ce.nt_status,
                ce.win_error);
        }
        GuestAddr ep = entries + count * entry_size;
        if (is64) {
            (void)m.write_u64(ep + 0,  ce.completion_key);      // lpCompletionKey
            (void)m.write_u64(ep + 8,  ce.overlapped);          // lpOverlapped
            (void)m.write_u64(ep + 16, ce.nt_status);           // Internal
            (void)m.write_u32(ep + 24, ce.bytes_transferred);   // dwNumberOfBytesTransferred
        } else {
            (void)m.write_u32(ep + 0,  static_cast<u32>(ce.completion_key));
            (void)m.write_u32(ep + 4,  static_cast<u32>(ce.overlapped));
            (void)m.write_u32(ep + 8,  ce.nt_status);
            (void)m.write_u32(ep + 12, ce.bytes_transferred);
        }
        record_completion_trace(
            OverlappedIoTraceStage::PacketDequeued, NULL_HANDLE,
            ce.overlapped, ce.bytes_transferred, port_handle,
            ce.completion_key, 0, 0, true, ce.nt_status, ce.win_error);
        ++count;
    }
    lk.unlock();

    if (num_out) (void)m.write_u32(num_out, count);
    VITREO_TRACE("[iocp] GetQueuedCompletionStatusEx: port=0x%x returned %u entries\n",
                 port_handle, count);
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_SetFileCompletionNotificationModes(Marshaler& m) {
    // BOOL SetFileCompletionNotificationModes(HANDLE FileHandle, UCHAR Flags)
    const Handle file_handle = static_cast<Handle>(m.arg(0));
    const u8 flags = static_cast<u8>(m.arg(1));
    const HandleType type = m_handle_table
        ? m_handle_table->get_type(file_handle)
        : HandleType::Invalid;
    const bool is_file_or_pipe =
        type == HandleType::File || type == HandleType::Pipe ||
        type == HandleType::Device;
    // Winsock IFS sockets are file handles for overlapped-I/O notification
    // contracts even though Vitreo owns their values outside HandleTable.
    const bool is_socket =
        !is_file_or_pipe && m_socket_handle_probe &&
        m_socket_handle_probe(static_cast<u32>(file_handle));
    // Windows accepts a no-op (Flags == 0) even on an unrecognized handle and
    // leaves LastError unchanged. Nonzero flags still require a file, pipe, or
    // IFS socket.
    if (flags == 0 && !is_file_or_pipe && !is_socket) {
        m.set_return(1);
        return;
    }
    if ((flags & ~u8{0x03}) != 0 || (!is_file_or_pipe && !is_socket)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        ipc::ipc_log(
            "SetFileCompletionNotificationModes h=0x%x flags=0x%x type=%s -> "
            "FALSE gle=%u",
            static_cast<unsigned>(file_handle), flags,
            is_socket ? "Socket" : handle_type_to_string(type),
            get_last_error());
        m.set_return(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        // Learn: once enabled for a handle, notification modes cannot be
        // removed. Preserve prior bits across repeated calls.
        m_file_completion_modes[file_handle] |= flags;
    }
    ipc::ipc_log("SetFileCompletionNotificationModes h=0x%x flags=0x%x -> TRUE",
                 static_cast<unsigned>(file_handle), flags);
    VITREO_TRACE("[iocp] SetFileCompletionNotificationModes(file=0x%x, flags=0x%x) -> TRUE\n",
                 file_handle, flags);
    m.set_return(1);
}

} // namespace vitreo::winapi
