// SPDX-License-Identifier: LGPL-2.1-only

/// @file handle_ext.cpp
/// @brief The handle calls whose arguments name a process, not just a handle.
///
/// DuplicateHandle and the queries around it. handle.cpp owns the table these
/// read; they live beside it rather than in it because the cross-process forms
/// need the process and IPC layers that handle.cpp deliberately does not.
///
/// Split out of kernel32_ext.cpp, and the cleanest cut in that file by a
/// distance: nothing it defines is used outside it, and it needs nothing
/// file-local from anywhere else — checked in both directions before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/synthetic_handle_registry.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"
#include "winapi/kernel32/console/console.hpp"
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
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/advapi32/security/security.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Handle Operations
// =============================================================================

void Kernel32ExtManager::impl_DuplicateHandle(Marshaler& m) {
    // BOOL DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
    //   HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle,
    //   DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions)
    const Handle source_process = static_cast<Handle>(m.arg(0));
    const Handle source = static_cast<Handle>(m.arg(1));
    const Handle target_process = static_cast<Handle>(m.arg(2));
    const GuestAddr target_handle_ptr = m.arg_ptr(3);
    const u32 desired_access = static_cast<u32>(m.arg(4));
    // BOOL on the stack: only the low dword of the x64 slot is defined.
    const bool inherit = m.arg32(5) != 0;
    const u32 options = static_cast<u32>(m.arg(6));

    ipc::ipc_log(
        "DuplicateHandle ENTER source_process=0x%x source=0x%x "
        "target_process=0x%x access=0x%x inherit=%d options=0x%x type=%s",
        static_cast<unsigned>(source_process), static_cast<unsigned>(source),
        static_cast<unsigned>(target_process), desired_access,
        inherit ? 1 : 0, options,
        m_handle_table == nullptr
            ? "null-table"
            : handle_type_to_string(m_handle_table->get_type(source)));

    constexpr u32 DUPLICATE_CLOSE_SOURCE = 0x00000001;
    constexpr u32 DUPLICATE_SAME_ACCESS = 0x00000002;
    constexpr u32 PROCESS_TERMINATE = 0x00000001;
    constexpr u32 PROCESS_CREATE_THREAD = 0x00000002;
    constexpr u32 PROCESS_VM_OPERATION = 0x00000008;
    constexpr u32 PROCESS_VM_READ = 0x00000010;
    constexpr u32 PROCESS_VM_WRITE = 0x00000020;
    constexpr u32 PROCESS_DUP_HANDLE = 0x00000040;
    constexpr u32 PROCESS_CREATE_PROCESS = 0x00000080;
    constexpr u32 PROCESS_SET_QUOTA = 0x00000100;
    constexpr u32 PROCESS_SET_INFORMATION = 0x00000200;
    constexpr u32 PROCESS_QUERY_INFORMATION = 0x00000400;
    constexpr u32 PROCESS_SUSPEND_RESUME = 0x00000800;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000;
    constexpr u32 SYNCHRONIZE = 0x00100000;
    // Learn: DuplicateHandle may duplicate Process handles. A transfer
    // requests PROCESS_DUP_HANDLE. OpenProcess often uses 0x100451. Honor
    // the common mask;
    // unsupported rights still fail honestly rather than silently truncating.
    // winnt.h: PROCESS_ALL_ACCESS = STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE |
    // 0xFFFF. Every right in it is grantable on a process handle -- including
    // DELETE / READ_CONTROL / WRITE_DAC / WRITE_OWNER and the rights above
    // PROCESS_QUERY_LIMITED_INFORMATION -- so a PROCESS_ALL_ACCESS duplicate
    // of GetCurrentProcess() succeeds on Windows (handles_waits_probe).
    constexpr u32 STANDARD_RIGHTS_REQUIRED = 0x000F0000;
    constexpr u32 SUPPORTED_PROCESS_ACCESS =
        PROCESS_TERMINATE | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_DUP_HANDLE |
        PROCESS_CREATE_PROCESS | PROCESS_SET_QUOTA | PROCESS_SET_INFORMATION |
        PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME |
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE |
        STANDARD_RIGHTS_REQUIRED | 0xFFFFu;
    Handle duplicate = NULL_HANDLE;
    bool duplicate_is_remote = false;

    auto import_process_transfer_descriptor =
        [&](std::string_view descriptor) -> Handle {
        // "R,<access>,<flags>,<pid>" — Process object identity by host pid.
        // Prefix R avoids colliding with pipe transfer descriptors ("P,").
        if (!descriptor.starts_with("R,") || m_handle_table == nullptr) {
            return NULL_HANDLE;
        }
        std::array<std::string_view, 4> fields{};
        usize field_start = 2;
        for (usize index = 1; index < fields.size(); ++index) {
            const usize comma = descriptor.find(',', field_start);
            if (index + 1 == fields.size()) {
                if (comma != std::string_view::npos) return NULL_HANDLE;
                fields[index] = descriptor.substr(field_start);
            } else {
                if (comma == std::string_view::npos) return NULL_HANDLE;
                fields[index] =
                    descriptor.substr(field_start, comma - field_start);
                field_start = comma + 1;
            }
        }
        u32 access = 0;
        u32 flags = 0;
        i64 pid_value = 0;
        const auto access_end =
            std::from_chars(fields[1].data(),
                            fields[1].data() + fields[1].size(), access);
        const auto flags_end =
            std::from_chars(fields[2].data(),
                            fields[2].data() + fields[2].size(), flags);
        const auto pid_end =
            std::from_chars(fields[3].data(),
                            fields[3].data() + fields[3].size(), pid_value);
        if (access_end.ec != std::errc{} ||
            access_end.ptr != fields[1].data() + fields[1].size() ||
            flags_end.ec != std::errc{} ||
            flags_end.ptr != fields[2].data() + fields[2].size() ||
            pid_end.ec != std::errc{} ||
            pid_end.ptr != fields[3].data() + fields[3].size() ||
            pid_value <= 0) {
            return NULL_HANDLE;
        }
        if (access != HandleAccess::All &&
            (access & ~SUPPORTED_PROCESS_ACCESS) != 0) {
            return NULL_HANDLE;
        }
        ipc::ProcessObject* process =
            ipc::track_process(static_cast<pid_t>(pid_value));
        if (process == nullptr) return NULL_HANDLE;
        return m_handle_table->allocate(
            HandleType::Process, process, flags, access);
    };

    auto prepare_process_transfer_descriptor =
        [&](Handle source_handle, u32 access,
            bool inheritable) -> std::optional<std::string> {
        // `access` is already resolved by the caller (including
        // DUPLICATE_SAME_ACCESS). Learn: Process handles are transferable via
        // DuplicateHandle; identity is the target host pid.
        ipc::ProcessObject* process = nullptr;
        if (source_handle == CURRENT_PROCESS_PSEUDO) {
            if (m_process_manager == nullptr) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
            process = ipc::track_process(static_cast<pid_t>(
                m_process_manager->get_current_process_id()));
        } else if (m_handle_table != nullptr &&
                   m_handle_table->get_type(source_handle) ==
                       HandleType::Process) {
            process = static_cast<ipc::ProcessObject*>(
                m_handle_table->get(source_handle, HandleType::Process));
        } else {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
        }
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
        }
        if (access != HandleAccess::All &&
            (access & ~SUPPORTED_PROCESS_ACCESS) != 0) {
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            return std::nullopt;
        }
        const u32 flags = inheritable ? HandleFlags::Inherit : 0;
        return "R," + std::to_string(access) + "," + std::to_string(flags) +
               "," + std::to_string(static_cast<long long>(process->pid));
    };

    if (target_process == NULL_HANDLE) {
        // Learn: hTargetProcessHandle "can be specified as NULL if the
        // DUPLICATE_CLOSE_SOURCE flag is set" -- the documented way to close a
        // handle in this or another process. Measured (handles_waits_probe):
        // returns TRUE and the source is closed.
        if ((options & DUPLICATE_CLOSE_SOURCE) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (source_process == CURRENT_PROCESS_PSEUDO) {
            if (source != CURRENT_PROCESS_PSEUDO &&
                source != CURRENT_THREAD_PSEUDO && m_handle_table != nullptr) {
                (void)m_handle_table->close(source);
            }
        } else {
            ipc::ProcessObject* source_proc = m_handle_table == nullptr
                ? nullptr
                : m_handle_table->get_as<ipc::ProcessObject>(
                      source_process, HandleType::Process);
            if (source_proc == nullptr) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            (void)ipc::remote_close_handle(source_proc->pid,
                                           static_cast<u32>(source));
        }
        m.set_return(1);
        return;
    }

    if (source_process != CURRENT_PROCESS_PSEUDO) {
        // A sender-side transfer issues:
        //   DuplicateHandle(remote_process, handle, GetCurrentProcess(), ...)
        // Export a transfer descriptor from the remote worker and import it
        // locally (mirror of the INTO-remote prepare+remote_import path).
        // Always log entry so a DecodeHandle attempt is visible even when the
        // source_process handle is wrong-typed or export fails.
        ipc::ipc_log(
            "DuplicateHandle FROM-remote attempt source_process=0x%x "
            "source=0x%x target_process=0x%x access=0x%x options=0x%x",
            static_cast<unsigned>(source_process),
            static_cast<unsigned>(source),
            static_cast<unsigned>(target_process), desired_access, options);
        ipc::ProcessObject* source_proc = m_handle_table == nullptr
            ? nullptr
            : m_handle_table->get_as<ipc::ProcessObject>(
                  source_process, HandleType::Process);
        if (source_proc == nullptr) {
            ipc::ipc_log(
                "DuplicateHandle FROM-remote FAIL source_process=0x%x "
                "source=0x%x (not a Process handle) options=0x%x",
                static_cast<unsigned>(source_process),
                static_cast<unsigned>(source), options);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        } else if (target_process != CURRENT_PROCESS_PSEUDO) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        } else if ((options & ~(DUPLICATE_CLOSE_SOURCE |
                                DUPLICATE_SAME_ACCESS)) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        } else {
            const bool same_access = (options & DUPLICATE_SAME_ACCESS) != 0;
            const bool close_source = (options & DUPLICATE_CLOSE_SOURCE) != 0;
            int pipe_fd = -1;
            // Learn DuplicateHandle: create the target handle before closing
            // the source. Export without CLOSE_SOURCE, import locally, then
            // remote-close. Closing the last anonymous section handle during
            // export (before import) destroys shared memory for a buffer the
            // peer never mapped. The import helpers leave the last error alone on success and do
            // not all set one on failure; clear it so a failed import can be
            // told apart below, and give the caller's value back on success
            // (a successful DuplicateHandle leaves it untouched, measured on
            // Windows 11: last_error_on_success_probe.expected).
            const u32 entry_error = get_last_error();
            set_last_error(error_codes::ERROR_SUCCESS);
            auto descriptor = ipc::remote_export_handle(
                source_proc->pid, static_cast<u32>(source),
                desired_access, same_access, /*close_source=*/false,
                &pipe_fd);
            if (!descriptor) {
                if (pipe_fd >= 0) ::close(pipe_fd);
                ipc::ipc_log(
                    "DuplicateHandle FROM-remote FAIL source_process=0x%x "
                    "source=0x%x remote_pid=%d access=0x%x options=0x%x",
                    static_cast<unsigned>(source_process),
                    static_cast<unsigned>(source),
                    static_cast<int>(source_proc->pid), desired_access,
                    options);
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
            } else if (descriptor->starts_with("S,") &&
                       m_sync_manager != nullptr) {
                duplicate = m_sync_manager->import_transferred_handle(
                    *descriptor);
            } else if (descriptor->starts_with("M,") &&
                       m_file_mapping_manager != nullptr) {
                duplicate =
                    m_file_mapping_manager->import_transferred_handle(
                        *descriptor);
            } else if (descriptor->starts_with("R,")) {
                duplicate = import_process_transfer_descriptor(*descriptor);
            } else if (descriptor->starts_with("D,") &&
                       m_handle_table != nullptr) {
                duplicate = import_shared_resource_transfer(*m_handle_table,
                                                            *descriptor);
            } else if ((descriptor->starts_with("P,") ||
                        descriptor->starts_with("F,")) &&
                       m_file_manager != nullptr) {
                duplicate = m_file_manager->import_transferred_handle(
                    *descriptor, pipe_fd);
                pipe_fd = -1;
            } else {
                if (pipe_fd >= 0) ::close(pipe_fd);
                pipe_fd = -1;
                ipc::ipc_log(
                    "DuplicateHandle FROM-remote FAIL source_process=0x%x "
                    "source=0x%x remote_pid=%d unsupported descriptor='%.*s'",
                    static_cast<unsigned>(source_process),
                    static_cast<unsigned>(source),
                    static_cast<int>(source_proc->pid),
                    static_cast<int>(std::min<usize>(descriptor->size(), 64)),
                    descriptor->data());
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            }
            if (pipe_fd >= 0) ::close(pipe_fd);
            if (duplicate != NULL_HANDLE) {
                set_last_error(entry_error);
                ipc::ipc_log(
                    "DuplicateHandle FROM-remote source_process=0x%x "
                    "source=0x%x remote_pid=%d -> local=0x%x options=0x%x",
                    static_cast<unsigned>(source_process),
                    static_cast<unsigned>(source),
                    static_cast<int>(source_proc->pid),
                    static_cast<unsigned>(duplicate), options);
            } else if (descriptor &&
                       (descriptor->starts_with("S,") ||
                        descriptor->starts_with("M,") ||
                        descriptor->starts_with("R,") ||
                        descriptor->starts_with("P,") ||
                        descriptor->starts_with("F,"))) {
                ipc::ipc_log(
                    "DuplicateHandle FROM-remote FAIL import "
                    "source_process=0x%x source=0x%x remote_pid=%d "
                    "descriptor='%.*s' gle=%u",
                    static_cast<unsigned>(source_process),
                    static_cast<unsigned>(source),
                    static_cast<int>(source_proc->pid),
                    static_cast<int>(std::min<usize>(descriptor->size(), 96)),
                    descriptor->data(), get_last_error());
                if (get_last_error() == error_codes::ERROR_SUCCESS) {
                    set_last_error(error_codes::ERROR_INVALID_HANDLE);
                }
            } else if (get_last_error() == error_codes::ERROR_SUCCESS) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
            }
            // DUPLICATE_CLOSE_SOURCE: close remote source regardless of import
            // success (Learn), but only after import has had a chance to attach
            // shared backing while the source handle still pinned lifetime.
            if (close_source) {
                (void)ipc::remote_close_handle(
                    source_proc->pid, static_cast<u32>(source));
            }
        }
    } else if ((options & ~(DUPLICATE_CLOSE_SOURCE |
                            DUPLICATE_SAME_ACCESS)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    } else if (target_process != CURRENT_PROCESS_PSEUDO) {
        ipc::ProcessObject* target = m_handle_table == nullptr
            ? nullptr
            : m_handle_table->get_as<ipc::ProcessObject>(
                  target_process, HandleType::Process);
        const u32 target_access = m_handle_table == nullptr
            ? 0
            : m_handle_table->get_access(target_process);
        if (target == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        } else if (target_access != HandleAccess::All &&
                   (target_access & PROCESS_DUP_HANDLE) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
        } else {
            const bool source_is_process =
                source == CURRENT_PROCESS_PSEUDO ||
                (m_handle_table != nullptr &&
                 m_handle_table->get_type(source) == HandleType::Process);
            const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
                ? (source == CURRENT_PROCESS_PSEUDO
                       ? HandleAccess::All
                       : m_handle_table->get_access(source))
                : desired_access;
            std::optional<std::string> descriptor;
            int pipe_fd = -1;
            const HandleType type = source == CURRENT_PROCESS_PSEUDO
                ? HandleType::Process
                : m_handle_table->get_type(source);
            if (source_is_process) {
                descriptor = prepare_process_transfer_descriptor(
                    source, access, inherit);
            } else if (type == HandleType::SharedResource) {
                descriptor = prepare_shared_resource_transfer(
                    *m_handle_table, source, access, inherit);
            } else if ((type == HandleType::Event ||
                        type == HandleType::Mutex ||
                        type == HandleType::Semaphore) &&
                       m_sync_manager != nullptr) {
                descriptor = m_sync_manager->prepare_transferred_handle(
                    source, access, inherit);
            } else if (type == HandleType::FileMapping &&
                       m_file_mapping_manager != nullptr) {
                descriptor =
                    m_file_mapping_manager->prepare_transferred_handle(
                        source, access, inherit);
            } else if ((type == HandleType::Pipe ||
                        type == HandleType::File) &&
                       m_file_manager != nullptr) {
                descriptor = m_file_manager->prepare_transferred_handle(
                    source, access, inherit, pipe_fd);
            } else {
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            }
            if (!descriptor) {
                if (pipe_fd >= 0) ::close(pipe_fd);
                ipc::ipc_log(
                    "DuplicateHandle cross-process FAIL source=0x%x "
                    "type=%s target_pid=%d access=0x%x inherit=%d "
                    "options=0x%x gle=%u",
                    static_cast<unsigned>(source),
                    handle_type_to_string(type),
                    static_cast<int>(target->pid), access,
                    inherit ? 1 : 0, options, get_last_error());
            }
            if (descriptor) {
                duplicate = static_cast<Handle>(
                    ipc::remote_import_handle(
                        target->pid, *descriptor, pipe_fd));
                pipe_fd = -1;
                duplicate_is_remote = duplicate != NULL_HANDLE;
                if (!duplicate_is_remote) {
                    set_last_error(error_codes::ERROR_GEN_FAILURE);
                } else {
                    // options=0x3 (SAME_ACCESS|CLOSE_SOURCE) is a recipient-side
                    // encode. Other option sets are explicit dups into a
                    // child process handle.
                    ipc::ipc_log(
                        "DuplicateHandle cross-process source=0x%x "
                        "type=%s target_pid=%d -> target=0x%x access=0x%x "
                        "options=0x%x target_process=0x%x",
                        static_cast<unsigned>(source),
                        handle_type_to_string(type),
                        static_cast<int>(target->pid),
                        static_cast<unsigned>(duplicate), access, options,
                        static_cast<unsigned>(target_process));
                }
            }
            if (pipe_fd >= 0) ::close(pipe_fd);
        }
    } else if (source == CURRENT_PROCESS_PSEUDO ||
               (m_handle_table != nullptr &&
                m_handle_table->get_type(source) == HandleType::Process)) {
        ipc::ProcessObject* process = nullptr;
        u32 access = desired_access;
        if (m_handle_table == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        } else if (source == CURRENT_PROCESS_PSEUDO) {
            if (m_process_manager != nullptr) {
                process = ipc::track_process(static_cast<pid_t>(
                    m_process_manager->get_current_process_id()));
            }
            if ((options & DUPLICATE_SAME_ACCESS) != 0) {
                access = HandleAccess::All;
            }
        } else {
            process = static_cast<ipc::ProcessObject*>(
                m_handle_table->get(source, HandleType::Process));
            if ((options & DUPLICATE_SAME_ACCESS) != 0) {
                access = m_handle_table->get_access(source);
            }
        }

        // Learn lists Process among DuplicateHandle object types. Grant the
        // common process access mask; consumers still enforce rights.
        if (m_handle_table == nullptr) {
            // Error set above.
        } else if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        } else if (access != HandleAccess::All &&
                   (access & ~SUPPORTED_PROCESS_ACCESS) != 0) {
            ipc::ipc_log(
                "DuplicateHandle Process access not supported source=0x%x "
                "access=0x%x",
                static_cast<unsigned>(source), access);
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        } else {
            duplicate = m_handle_table->allocate(
                HandleType::Process, process,
                inherit ? HandleFlags::Inherit : 0, access);
            if (duplicate != NULL_HANDLE) {
                ipc::ipc_log(
                    "DuplicateHandle same-process source=0x%x type=Process "
                    "-> 0x%x access=0x%x",
                    static_cast<unsigned>(source),
                    static_cast<unsigned>(duplicate), access);
            } else {
                ipc::ipc_log(
                    "DuplicateHandle same-process FAIL source=0x%x "
                    "type=Process access=0x%x gle=%u",
                    static_cast<unsigned>(source), access, get_last_error());
            }
        }
    } else if (m_handle_table != nullptr &&
               m_handle_table->get_type(source) == HandleType::FileMapping &&
               m_file_mapping_manager != nullptr) {
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        duplicate = m_file_mapping_manager->duplicate_handle(
            source, inherit ? HandleFlags::Inherit : 0, access);
        if (duplicate != NULL_HANDLE) {
            ipc::ipc_log(
                "DuplicateHandle same-process source=0x%x type=FileMapping "
                "-> 0x%x access=0x%x",
                static_cast<unsigned>(source),
                static_cast<unsigned>(duplicate), access);
        } else {
            ipc::ipc_log(
                "DuplicateHandle same-process FAIL source=0x%x "
                "type=FileMapping access=0x%x gle=%u",
                static_cast<unsigned>(source), access, get_last_error());
        }
    } else if (m_handle_table != nullptr &&
               (m_handle_table->get_type(source) == HandleType::Pipe ||
                m_handle_table->get_type(source) == HandleType::File) &&
               m_file_manager != nullptr) {
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        duplicate = m_file_manager->duplicate_handle(
            source, inherit ? HandleFlags::Inherit : 0, access);
    } else if (m_handle_table != nullptr &&
               m_handle_table->get_type(source) == HandleType::Token &&
               m_security_stubs != nullptr) {
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        if (!m_security_stubs->duplicate_token_handle(
                source, access, inherit ? HandleFlags::Inherit : 0,
                duplicate)) {
            duplicate = NULL_HANDLE;
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
        }
    } else if (m_handle_table != nullptr &&
               m_handle_table->get_type(source) == HandleType::Job) {
        std::shared_ptr<JobState> job;
        {
            std::lock_guard lock(m_job_mutex);
            const auto found = m_jobs.find(source);
            if (found != m_jobs.end()) job = found->second;
        }
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        const u32 source_access = m_handle_table->get_access(source);
        if (!job || (source_access != HandleAccess::All &&
                     (access & ~source_access) != 0)) {
            set_last_error(job ? error_codes::ERROR_ACCESS_DENIED
                               : error_codes::ERROR_INVALID_HANDLE);
        } else {
            duplicate = m_handle_table->allocate(
                HandleType::Job, nullptr,
                inherit ? HandleFlags::Inherit : 0, access);
            std::lock_guard lock(m_job_mutex);
            m_jobs[duplicate] = std::move(job);
        }
    } else if (m_handle_table != nullptr &&
               m_handle_table->get_type(source) == HandleType::IoCompletionPort) {
        std::shared_ptr<CompletionPort> port;
        {
            std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
            const auto found = m_iocp_ports.find(source);
            if (found != m_iocp_ports.end()) port = found->second;
        }
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        const u32 source_access = m_handle_table->get_access(source);
        if (!port || (source_access != HandleAccess::All &&
                      (access & ~source_access) != 0)) {
            set_last_error(port ? error_codes::ERROR_ACCESS_DENIED
                                : error_codes::ERROR_INVALID_HANDLE);
        } else {
            duplicate = m_handle_table->allocate(
                HandleType::IoCompletionPort, nullptr,
                inherit ? HandleFlags::Inherit : 0, access);
            if (duplicate != NULL_HANDLE) {
                std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
                m_iocp_ports[duplicate] = std::move(port);
            } else {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            }
        }
    } else if (m_handle_table != nullptr &&
               m_handle_table->get_type(source) ==
                   HandleType::SharedResource) {
        // Learn IDXGIResource1::CreateSharedHandle: "If you need more
        // handles to the same shared resource, call DuplicateHandle" -- the
        // duplicate names the same object (measured: it opens, and closing
        // it leaves the original open).
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        duplicate = duplicate_shared_resource_handle(*m_handle_table, source,
                                                     access, inherit);
        if (duplicate == NULL_HANDLE) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        }
    } else if (m_handle_table != nullptr && m_sync_manager != nullptr &&
               (m_handle_table->get_type(source) == HandleType::Event ||
                m_handle_table->get_type(source) == HandleType::Mutex ||
                m_handle_table->get_type(source) == HandleType::Semaphore)) {
        // A watcher duplicates the event into the same process:
        //   DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
        //                   &dup, SYNCHRONIZE|EVENT_MODIFY_STATE, FALSE, 0);
        // Both handles must alias one kernel object so SetEvent(original)
        // wakes RegisterWait/WFSO on the duplicate.
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        duplicate = m_sync_manager->duplicate_handle(
            source, inherit ? HandleFlags::Inherit : 0, access);
    } else if (m_thread_manager != nullptr &&
               source == CURRENT_THREAD_PSEUDO) {
        // The runtime-attach pattern (Mono, .NET, ...):
        //   DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
        //                   GetCurrentProcess(), &h, THREAD_ALL_ACCESS, ...)
        // Any requested access is granted on the caller's own thread
        // (handles_waits_probe); SAME_ACCESS gives the pseudo handle's full
        // access.
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? HandleAccess::All
            : desired_access;
        duplicate = m_thread_manager->duplicate_thread_handle(
            source, inherit ? HandleFlags::Inherit : 0, access);
        if (duplicate == INVALID_HANDLE) duplicate = NULL_HANDLE;
    } else if (m_handle_table != nullptr &&
               m_handle_table->get_type(source) == HandleType::Thread) {
        // A CreateProcess primary-thread handle represents a thread in the
        // child worker and therefore has no local GuestThread object. It is
        // still a valid thread handle and must be duplicable. Preserve the
        // opaque object pointer for local threads and the NULL marker for
        // external threads.
        duplicate = m_handle_table->allocate(
            HandleType::Thread,
            m_handle_table->get(source, HandleType::Thread),
            inherit ? HandleFlags::Inherit : 0,
            (options & DUPLICATE_SAME_ACCESS) != 0
                ? m_handle_table->get_access(source)
                : desired_access);
        if (duplicate != NULL_HANDLE) {
            std::lock_guard lock(m_external_thread_mutex);
            const auto external = m_external_primary_handles.find(source);
            if (external != m_external_primary_handles.end()) {
                m_external_primary_handles.emplace(
                    duplicate, external->second);
            }
        }
        if (duplicate != NULL_HANDLE && m_security_stubs != nullptr) {
            m_security_stubs->duplicate_thread_token_association(
                source, duplicate);
        }
        ipc::ipc_log(
            "DuplicateHandle thread source=0x%x -> 0x%x external=%d",
            static_cast<unsigned>(source),
            static_cast<unsigned>(duplicate),
            m_handle_table->get(source, HandleType::Thread) == nullptr
                ? 1
                : 0);
    } else if (m_handle_table != nullptr && m_console_manager != nullptr &&
               (m_handle_table->get_type(source) == HandleType::ConsoleInput ||
                m_handle_table->get_type(source) == HandleType::ConsoleOutput)) {
        // Learn lists console input and screen buffers as duplicable.
        const u32 access = (options & DUPLICATE_SAME_ACCESS) != 0
            ? m_handle_table->get_access(source)
            : desired_access;
        duplicate = m_console_manager->duplicate_handle(
            source, inherit ? HandleFlags::Inherit : 0, access);
    } else {
        const HandleType type = m_handle_table == nullptr
            ? HandleType::Invalid
            : m_handle_table->get_type(source);
        if (type == HandleType::Invalid || type == HandleType::Heap ||
            type == HandleType::Module) {
            // Not a kernel object handle (or not a handle at all).
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        } else {
            static std::atomic<bool> s_logged{false};
            if (!s_logged.exchange(true)) {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED kernel32!DuplicateHandle: "
                    "same-process duplicate of a %s handle\n",
                    handle_type_to_string(type));
            }
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        }
    }

    // DUPLICATE_CLOSE_SOURCE applies even if duplication fails, but only to a
    // source handle that lives in *this* process. FROM-remote applies remote
    // CLOSE_SOURCE via remote_close_handle after import (see above).
    if ((options & DUPLICATE_CLOSE_SOURCE) != 0 &&
        source_process == CURRENT_PROCESS_PSEUDO &&
        source != CURRENT_PROCESS_PSEUDO &&
        source != CURRENT_THREAD_PSEUDO && m_handle_table != nullptr) {
        (void)m_handle_table->close(source);
    }

    if (duplicate == NULL_HANDLE) {
        m.set_return(0);
        return;
    }

    if (target_handle_ptr != 0) {
        const auto write_result = m.cpu_is_64bit()
            ? m_memory.write_u64(target_handle_ptr, duplicate)
            : m_memory.write_u32(target_handle_ptr, duplicate);
        if (write_result.is_err()) {
            if (!duplicate_is_remote && m_handle_table != nullptr) {
                (void)m_handle_table->close(duplicate);
            }
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }
    m.set_return(1);
}

/// Handle flags for a Winsock socket, which is a kernel handle Vitreo happens
/// to own outside HandleTable. `socket()` returns an inheritable handle, so an
/// entry that has never been written reads back as HANDLE_FLAG_INHERIT.
u32 Kernel32ExtManager::socket_handle_flags(Handle handle, u32 mask, u32 flags,
                                            bool write) {
    std::lock_guard<std::mutex> lock(m_socket_handle_flags_mutex);
    auto found = m_socket_handle_flags.find(handle);
    u32 current = found == m_socket_handle_flags.end() ? HandleFlags::Inherit
                                                       : found->second;
    if (write) {
        current = (current & ~mask) | (flags & mask);
        m_socket_handle_flags[handle] = current;
    }
    return current;
}

void Kernel32ExtManager::impl_SetHandleInformation(Marshaler& m) {
    // BOOL SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 mask = static_cast<u32>(m.arg(1)) &
                     (HandleFlags::Inherit | HandleFlags::ProtectFromClose);
    const u32 flags = static_cast<u32>(m.arg(2));
    if (m_file_manager &&
        m_file_manager->handle_table().set_flags(handle, mask, flags)) {
        VITREO_TRACE("[handle] SetHandleInformation(0x%x, mask=0x%x, flags=0x%x)"
                     " -> TRUE\n", handle, mask, flags);
        m.set_return(1);
        return;
    }
    // A socket is a handle for this contract. libuv's uv__tcp_set_socket
    // clears HANDLE_FLAG_INHERIT on every socket it creates and returns
    // GetLastError() if the call fails, so a rejection here closes the
    // socket before it is ever connected.
    if (m_socket_handle_probe &&
        m_socket_handle_probe(static_cast<u32>(handle))) {
        const u32 result = socket_handle_flags(handle, mask, flags, true);
        VITREO_TRACE("[handle] SetHandleInformation(socket=0x%x, mask=0x%x,"
                     " flags=0x%x) -> TRUE now=0x%x\n",
                     handle, mask, flags, result);
        m.set_return(1);
        return;
    }
    // Live FACE/CE/CC synthetic handles accept the same Inherit /
    // ProtectFromClose flags as HandleTable entries (Windows).
    if (synthetic_handles::set_flags(handle, mask, flags)) {
        VITREO_TRACE("[handle] SetHandleInformation(synth=0x%x, mask=0x%x,"
                     " flags=0x%x) -> TRUE\n",
                     handle, mask, flags);
        m.set_return(1);
        return;
    }
    ipc::ipc_log("SetHandleInformation FAIL h=0x%x mask=0x%x flags=0x%x",
                 static_cast<unsigned>(handle), mask, flags);
    VITREO_TRACE("[handle] SetHandleInformation(0x%x, mask=0x%x, flags=0x%x)"
                 " -> FALSE (invalid handle)\n", handle, mask, flags);
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetHandleInformation(Marshaler& m) {
    // BOOL GetHandleInformation(HANDLE hObject, LPDWORD lpdwFlags)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr flags_out = m.arg_ptr(1);
    if (flags_out != 0 && m_socket_handle_probe &&
        (m_handle_table == nullptr ||
         m_handle_table->get_type(handle) == HandleType::Invalid) &&
        m_socket_handle_probe(static_cast<u32>(handle))) {
        const u32 socket_flags = socket_handle_flags(handle, 0, 0, false);
        if (m.write_u32(flags_out, socket_flags).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        m.set_return(1);
        return;
    }
    if (flags_out == 0 || m_handle_table == nullptr ||
        m_handle_table->get_type(handle) == HandleType::Invalid) {
        u32 synth_flags = 0;
        if (flags_out != 0 && synthetic_handles::get_flags(handle, synth_flags)) {
            if (m.write_u32(flags_out, synth_flags).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
            return;
        }
        ipc::ipc_log(
            "GetHandleInformation FAIL h=0x%x type=%s",
            static_cast<unsigned>(handle),
            m_handle_table == nullptr
                ? "null-table"
                : handle_type_to_string(m_handle_table->get_type(handle)));
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 flags = m_handle_table->get_flags(handle) &
                      (HandleFlags::Inherit | HandleFlags::ProtectFromClose);
    if (m.write_u32(flags_out, flags).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

bool Kernel32ExtManager::has_iocp_association(Handle handle) {
    std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
    return m_iocp_file_associations.find(handle) != m_iocp_file_associations.end();
}

void Kernel32ExtManager::on_iocp_handle_close(Handle handle) {
    std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
    m_iocp_file_associations.erase(handle);
    m_file_completion_modes.erase(handle);
}

bool Kernel32ExtManager::on_iocp_port_close(Handle handle) {
    std::shared_ptr<CompletionPort> port;
    {
        std::lock_guard<std::mutex> lock(m_iocp_registry_mutex);
        const auto found = m_iocp_ports.find(handle);
        if (found == m_iocp_ports.end()) return true;
        port = found->second;
        m_iocp_ports.erase(found);
        // Drop file associations that target this port so recycled file handles
        // cannot enqueue into a destroyed port identity.
        for (auto it = m_iocp_file_associations.begin();
             it != m_iocp_file_associations.end();) {
            if (it->second.port_handle == handle) {
                it = m_iocp_file_associations.erase(it);
            } else {
                ++it;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(port->mutex);
        port->closed = true;
        port->cv.notify_all();
    }
    VITREO_TRACE("[iocp] CloseHandle port=0x%x\n", handle);
    return true;
}

} // namespace vitreo::winapi
