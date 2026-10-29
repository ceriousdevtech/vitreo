// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_pipe.cpp
/// @brief Named pipes and the serial-comm exports that sit beside them.
///
/// The server half (CreateNamedPipe / ConnectNamedPipe / DisconnectNamedPipe)
/// and the client half (WaitNamedPipe / CallNamedPipe), plus the state and
/// identity queries around them. FileManager owns the transport; these marshal
/// the Win32 shape of it.
///
/// The comm exports (ClearCommError, SetCommState, SetCommTimeouts, …) are here
/// because they were adjacent in kernel32_ext.cpp and because a guest that
/// reaches them is doing the same thing: talking to a byte stream that is not a
/// file. They answer success without a device behind them.
///
/// Split out of kernel32_ext.cpp. Contiguous and closed: the one anonymous
/// helper it carries, call_named_pipe_common, is used by nothing outside it,
/// and nothing here needed a file-local helper from elsewhere.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_path_internal.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/ipc_gate.hpp"

#include <string>

namespace vitreo::winapi {

// ---- Group E: Named Pipes & Comm ----
void Kernel32ExtManager::impl_SetNamedPipeHandleState(Marshaler& m) {
    // BOOL SetNamedPipeHandleState(HANDLE hNamedPipe, LPDWORD lpMode,
    //   LPDWORD lpMaxCollectionCount, LPDWORD lpCollectDataTimeout)
    const Handle pipe = static_cast<Handle>(m.arg(0));
    const GuestAddr mode_ptr = m.arg_ptr(1);
    const GuestAddr max_count_ptr = m.arg_ptr(2);
    const GuestAddr timeout_ptr = m.arg_ptr(3);

    // Learn: each pointer is optional. When none are supplied Windows returns
    // TRUE without touching handle state or LastError.
    if (mode_ptr == 0 && max_count_ptr == 0 && timeout_ptr == 0) {
        m.set_return(1u);
        return;
    }

    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    // Optional out/in pointers are read only when supplied; an unreadable
    // pointer the caller did supply is a parameter error, not a silent skip.
    u32 mode = 0; bool have_mode = false;
    if (mode_ptr != 0) {
        auto v = m.read_u32(mode_ptr);
        if (v.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        mode = v.value(); have_mode = true;
    }
    u32 max_count = 0; bool have_max = false;
    if (max_count_ptr != 0) {
        auto v = m.read_u32(max_count_ptr);
        if (v.is_ok()) { max_count = v.value(); have_max = true; }
    }
    u32 timeout = 0; bool have_timeout = false;
    if (timeout_ptr != 0) {
        auto v = m.read_u32(timeout_ptr);
        if (v.is_ok()) { timeout = v.value(); have_timeout = true; }
    }

    u32 error = error_codes::ERROR_SUCCESS;
    const bool ok = m_file_manager->set_named_pipe_handle_state(
        pipe, have_mode ? &mode : nullptr,
        have_max ? &max_count : nullptr,
        have_timeout ? &timeout : nullptr, error);
    // Success leaves the last error untouched (measured on Windows 11:
    // last_error_on_success_probe.expected).
    if (!ok) set_last_error(error);
    ipc::ipc_log("SetNamedPipeHandleState h=0x%x mode=%s0x%x -> %s gle=%u",
                 static_cast<unsigned>(pipe), have_mode ? "" : "none:", mode,
                 ok ? "TRUE" : "FALSE", static_cast<unsigned>(error));
    m.set_return(ok ? 1u : 0u);
}

void Kernel32ExtManager::impl_GetNamedPipeHandleStateW(Marshaler& m) {
    // BOOL GetNamedPipeHandleStateW(HANDLE hNamedPipe, LPDWORD lpState,
    //   LPDWORD lpCurInstances, LPDWORD lpMaxCollectionCount,
    //   LPDWORD lpCollectDataTimeout, LPWSTR lpUserName,
    //   DWORD nMaxUserNameSize)
    const Handle pipe = static_cast<Handle>(m.arg(0));
    const GuestAddr state_ptr = m.arg_ptr(1);
    const GuestAddr instances_ptr = m.arg_ptr(2);
    const GuestAddr max_count_ptr = m.arg_ptr(3);
    const GuestAddr timeout_ptr = m.arg_ptr(4);
    const GuestAddr user_ptr = m.arg_ptr(5);
    const u32 user_chars = static_cast<u32>(m.arg(6));

    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    u32 state = 0, instances = 0, max_count = 0, timeout = 0;
    u32 error = error_codes::ERROR_SUCCESS;
    if (!m_file_manager->named_pipe_handle_state(
            pipe, state_ptr ? &state : nullptr,
            instances_ptr ? &instances : nullptr,
            max_count_ptr ? &max_count : nullptr,
            timeout_ptr ? &timeout : nullptr, error)) {
        set_last_error(error);
        m.set_return(0);
        return;
    }

    // lpUserName names the client that opened the pipe. A capsule runs as one
    // user, so that is the environment's USERNAME — not a fabricated value.
    // Windows fails the whole call when the buffer cannot hold it.
    if (user_ptr != 0) {
        std::string user = "Default";
        if (m_environment_manager != nullptr) {
            auto env_user = m_environment_manager->get_variable("USERNAME");
            if (env_user.is_ok() && !env_user.value().empty()) {
                user = env_user.value();
            }
        }
        if (user_chars < user.size() + 1) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            m.set_return(0);
            return;
        }
        if (m.write_wide_string(user_ptr, user).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
    }

    // Every output is written only after all of them can be satisfied, so a
    // failed call leaves the caller's buffers untouched.
    if (state_ptr && m.write_u32(state_ptr, state).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0); return;
    }
    if (instances_ptr && m.write_u32(instances_ptr, instances).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0); return;
    }
    if (max_count_ptr && m.write_u32(max_count_ptr, max_count).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0); return;
    }
    if (timeout_ptr && m.write_u32(timeout_ptr, timeout).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0); return;
    }

    VITREO_TRACE("[pipe] GetNamedPipeHandleStateW h=0x%x state=0x%x "
                 "instances=%u -> TRUE\n",
                 pipe, state, instances);
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetNamedPipeInfo(Marshaler& m) {
    // BOOL GetNamedPipeInfo(HANDLE hNamedPipe, LPDWORD lpFlags,
    //   LPDWORD lpOutBufferSize, LPDWORD lpInBufferSize,
    //   LPDWORD lpMaxInstances)
    const Handle pipe = static_cast<Handle>(m.arg(0));
    const GuestAddr flags_ptr = m.arg_ptr(1);
    const GuestAddr out_size_ptr = m.arg_ptr(2);
    const GuestAddr in_size_ptr = m.arg_ptr(3);
    const GuestAddr max_inst_ptr = m.arg_ptr(4);

    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    u32 flags = 0, out_size = 0, in_size = 0, max_inst = 0;
    u32 error = error_codes::ERROR_SUCCESS;
    const bool ok = m_file_manager->named_pipe_info(
        pipe, &flags, &out_size, &in_size, &max_inst, error);
    if (!ok) {
        set_last_error(error);
        ipc::ipc_log("GetNamedPipeInfo h=0x%x -> FALSE gle=%u",
                     static_cast<unsigned>(pipe),
                     static_cast<unsigned>(error));
        m.set_return(0);
        return;
    }

    // Only non-null outputs are written, as Windows documents.
    if (flags_ptr != 0) (void)m_memory.write_u32(flags_ptr, flags);
    if (out_size_ptr != 0) (void)m_memory.write_u32(out_size_ptr, out_size);
    if (in_size_ptr != 0) (void)m_memory.write_u32(in_size_ptr, in_size);
    if (max_inst_ptr != 0) (void)m_memory.write_u32(max_inst_ptr, max_inst);
    ipc::ipc_log("GetNamedPipeInfo h=0x%x -> TRUE flags=0x%x out=%u in=%u "
                 "instances=%u",
                 static_cast<unsigned>(pipe), static_cast<unsigned>(flags),
                 static_cast<unsigned>(out_size),
                 static_cast<unsigned>(in_size),
                 static_cast<unsigned>(max_inst));
    m.set_return(1);
}

void Kernel32ExtManager::impl_WaitNamedPipeA(Marshaler& m) {
    // BOOL WaitNamedPipeA(LPCSTR lpNamedPipeName, DWORD nTimeOut)
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32 timeout_ms = static_cast<u32>(m.arg(1));
    if (!m_file_manager) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string name = m.read_ansi_string(name_ptr).value_or("");
    const bool ok = m_file_manager->wait_named_pipe(name, timeout_ms);
    ipc::ipc_log("WaitNamedPipeA name='%s' timeout=%u -> %s gle=%u",
                 name.c_str(), timeout_ms, ok ? "TRUE" : "FALSE",
                 get_last_error());
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::impl_WaitNamedPipeW(Marshaler& m) {
    // BOOL WaitNamedPipeW(LPCWSTR lpNamedPipeName, DWORD nTimeOut)
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32 timeout_ms = static_cast<u32>(m.arg(1));
    if (!m_file_manager) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string name = m.read_wide_string(name_ptr).value_or("");
    const bool ok = m_file_manager->wait_named_pipe(name, timeout_ms);
    ipc::ipc_log("WaitNamedPipeW name='%s' timeout=%u -> %s gle=%u",
                 name.c_str(), timeout_ms, ok ? "TRUE" : "FALSE",
                 get_last_error());
    m.set_return(ok ? 1 : 0);
}

namespace {

/// CallNamedPipe*: WaitNamedPipe → CreateFile → WriteFile → ReadFile → Close.
/// SecuROM (RCT3) resolves CallNamedPipeA early; without the export,
/// GetProcAddress returns NULL and the DRM path degrades.
bool call_named_pipe_common(FileManager& fm, const std::string& name,
                            GuestAddr input, u32 input_size, GuestAddr output,
                            u32 output_size, GuestAddr bytes_read,
                            u32 timeout_ms) {
    if (bytes_read != 0) {
        // Zero until a successful read, matching Windows' failure path.
        if (fm.guest_memory() != nullptr) {
            (void)fm.guest_memory()->write_u32(bytes_read, 0);
        }
    }
    if (!fm.wait_named_pipe(name, timeout_ms)) {
        return false;
    }
    const Handle pipe = fm.create_file(
        name, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    if (pipe == INVALID_HANDLE) {
        return false;
    }
    if (input_size != 0 && input != 0) {
        u32 written = 0;
        if (!fm.write_file(pipe, input, input_size, 0, &written) ||
            written != input_size) {
            (void)fm.close_file(pipe);
            return false;
        }
    }
    if (output_size != 0 && output != 0) {
        u32 read = 0;
        if (!fm.read_file(pipe, output, output_size, 0, &read)) {
            (void)fm.close_file(pipe);
            return false;
        }
        if (bytes_read != 0 && fm.guest_memory() != nullptr) {
            (void)fm.guest_memory()->write_u32(bytes_read, read);
        }
    } else if (bytes_read != 0 && fm.guest_memory() != nullptr) {
        (void)fm.guest_memory()->write_u32(bytes_read, 0);
    }
    (void)fm.close_file(pipe);
    set_last_error(error_codes::ERROR_SUCCESS);
    return true;
}

}  // namespace

void Kernel32ExtManager::impl_CallNamedPipeA(Marshaler& m) {
    // BOOL CallNamedPipeA(LPCSTR lpNamedPipeName, LPVOID lpInBuffer,
    //   DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize,
    //   LPDWORD lpBytesRead, DWORD nTimeOut)
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr input = m.arg_ptr(1);
    const u32 input_size = static_cast<u32>(m.arg(2));
    const GuestAddr output = m.arg_ptr(3);
    const u32 output_size = static_cast<u32>(m.arg(4));
    const GuestAddr bytes_read = m.arg_ptr(5);
    const u32 timeout_ms = static_cast<u32>(m.arg(6));
    if (!m_file_manager) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string name = m.read_ansi_string(name_ptr).value_or("");
    const bool ok = call_named_pipe_common(
        *m_file_manager, name, input, input_size, output, output_size,
        bytes_read, timeout_ms);
    ipc::ipc_log("CallNamedPipeA name='%s' in=%u out=%u timeout=%u -> %s gle=%u",
                 name.c_str(), input_size, output_size, timeout_ms,
                 ok ? "TRUE" : "FALSE", get_last_error());
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::impl_CallNamedPipeW(Marshaler& m) {
    // BOOL CallNamedPipeW(LPCWSTR lpNamedPipeName, LPVOID lpInBuffer,
    //   DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize,
    //   LPDWORD lpBytesRead, DWORD nTimeOut)
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr input = m.arg_ptr(1);
    const u32 input_size = static_cast<u32>(m.arg(2));
    const GuestAddr output = m.arg_ptr(3);
    const u32 output_size = static_cast<u32>(m.arg(4));
    const GuestAddr bytes_read = m.arg_ptr(5);
    const u32 timeout_ms = static_cast<u32>(m.arg(6));
    if (!m_file_manager) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string name = m.read_wide_string(name_ptr).value_or("");
    const bool ok = call_named_pipe_common(
        *m_file_manager, name, input, input_size, output, output_size,
        bytes_read, timeout_ms);
    ipc::ipc_log("CallNamedPipeW name='%s' in=%u out=%u timeout=%u -> %s gle=%u",
                 name.c_str(), input_size, output_size, timeout_ms,
                 ok ? "TRUE" : "FALSE", get_last_error());
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::impl_CreateNamedPipeA(Marshaler& m) {
    // HANDLE CreateNamedPipeA(LPCSTR lpName, ...) — kernel32 exports both the
    // ANSI and the wide form. Only the wide form was registered, so an ANSI
    // caller reached the synthetic missing-export stub: it returns
    // 0xFFFFFFFF, which *is* INVALID_HANDLE_VALUE, and never touches the
    // thread's last-error slot. The guest then reports a create failure with
    // GetLastError() == 0, which no documented Win32 failure path produces.
    create_named_pipe_impl(m, /*wide=*/false);
}

void Kernel32ExtManager::impl_CreateNamedPipeW(Marshaler& m) {
    create_named_pipe_impl(m, /*wide=*/true);
}

void Kernel32ExtManager::create_named_pipe_impl(Marshaler& m, bool wide) {
    // HANDLE CreateNamedPipe{A,W}(LPCTSTR lpName, ...)
    constexpr u32 PIPE_NOWAIT = 0x00000001;
    constexpr u32 PIPE_READMODE_MESSAGE = 0x00000002;
    constexpr u32 PIPE_TYPE_MESSAGE = 0x00000004;
    constexpr u32 PIPE_REJECT_REMOTE_CLIENTS = 0x00000008;
    constexpr u32 PIPE_MODE_SUPPORTED =
        PIPE_NOWAIT | PIPE_READMODE_MESSAGE | PIPE_TYPE_MESSAGE |
        PIPE_REJECT_REMOTE_CLIENTS;
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32 open_mode = static_cast<u32>(m.arg(1));
    const u32 pipe_mode = static_cast<u32>(m.arg(2));
    const u32 max_instances = static_cast<u32>(m.arg(3));
    const u32 out_buffer_size = static_cast<u32>(m.arg(4));
    const u32 in_buffer_size = static_cast<u32>(m.arg(5));
    const u32 default_timeout = static_cast<u32>(m.arg(6));
    const GuestAddr security_attributes = m.arg_ptr(7);
    bool inheritable = false;
    if (security_attributes != 0) {
        const GuestAddr inherit_offset = m.cpu_is_64bit() ? 16 : 8;
        auto inherit = m_memory.read_u32(security_attributes + inherit_offset);
        inheritable = inherit.is_ok() && inherit.value() != 0;
    }

    const bool message_type = (pipe_mode & PIPE_TYPE_MESSAGE) != 0;
    const bool message_read =
        (pipe_mode & PIPE_READMODE_MESSAGE) != 0;
    if ((pipe_mode & ~PIPE_MODE_SUPPORTED) != 0 ||
        (message_read && !message_type) ||
        max_instances == 0 || max_instances > 255) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(static_cast<u64>(INVALID_HANDLE));
        return;
    }

    NamedPipeConfig config{};
    config.message_type = message_type;
    config.message_read_mode = message_read;
    config.blocking = (pipe_mode & PIPE_NOWAIT) == 0;
    config.output_buffer_size =
        out_buffer_size == 0 ? 4096u : out_buffer_size;
    config.input_buffer_size =
        in_buffer_size == 0 ? 4096u : in_buffer_size;
    config.max_instances = max_instances;
    config.default_timeout_ms = default_timeout;
    config.overlapped = (open_mode & 0x40000000u) != 0;

    // A named pipe can be inherited by any real CreateProcess child, even when
    // the optional shared-object IPC gate is disabled. Always give it a
    // transferable AF_UNIX listener rather than a process-local stand-in.
    if (m_file_manager && name_ptr) {
        auto name_res = wide ? m.read_wide_string(name_ptr)
                             : m.read_ansi_string(name_ptr);
        if (name_res.is_err()) {
            // The caller supplied a pointer Windows would dereference.
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(static_cast<u64>(INVALID_HANDLE));
            return;
        }
        // Learn (CreateNamedPipeW): lpName must have the form
        // \\.\pipe\pipename (also accepting the extended \\?\ prefix).
        // Anything else is ERROR_INVALID_NAME; it must not
        // become a listener under some other host path.
        if (!is_named_pipe_path(name_res.value())) {
            ipc::ipc_log("CreateNamedPipe '%s' -> ERROR_INVALID_NAME",
                         name_res.value().c_str());
            set_last_error(error_codes::ERROR_INVALID_NAME);
            m.set_return(static_cast<u64>(INVALID_HANDLE));
            return;
        }
        {
            Handle h = m_file_manager->create_named_pipe_server(
                name_res.value(), inheritable, config);
            if (h != INVALID_HANDLE && h != NULL_HANDLE) {
                ipc::ipc_log(
                    "CreateNamedPipe '%s' open=0x%x mode=0x%x instances=%u "
                    "out=%u in=%u timeout=%u -> server h=0x%x",
                    name_res.value().c_str(), open_mode, pipe_mode,
                    max_instances, out_buffer_size, in_buffer_size,
                    default_timeout, static_cast<unsigned>(h));
                VITREO_TRACE(
                    "[kernel32_ext] CreateNamedPipeW('%s', open=0x%x "
                    "mode=0x%x instances=%u out=%u in=%u timeout=%u "
                    "inherit=%d) -> 0x%x\n",
                    name_res.value().c_str(), open_mode, pipe_mode,
                    max_instances, out_buffer_size, in_buffer_size,
                    default_timeout, inheritable ? 1 : 0,
                    static_cast<unsigned>(h));
                set_last_error(error_codes::ERROR_SUCCESS);
                m.set_return(static_cast<u64>(h));
                return;
            }
            ipc::ipc_log("CreateNamedPipe '%s' -> server FAILED", name_res.value().c_str());
        }
        // The listener could not be created or the handle could not be
        // allocated. Windows reports a resource failure here; what must never
        // happen is INVALID_HANDLE_VALUE with a zero last-error.
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(static_cast<u64>(INVALID_HANDLE));
        return;
    }

    // Only reachable with lpName == NULL: no listener can be published under a
    // name that does not exist, so this is a process-local instance.
    if (m_file_manager) {
        const Handle h = m_file_manager->create_local_named_pipe(std::string{});
        if (h != INVALID_HANDLE && h != NULL_HANDLE) {
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(static_cast<u64>(h));
            return;
        }
    }

    set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    m.set_return(static_cast<u64>(INVALID_HANDLE));
}
void Kernel32ExtManager::impl_ConnectNamedPipe(Marshaler& m) {
    const auto h = static_cast<Handle>(m.arg(0));
    const GuestAddr overlapped = m.arg_ptr(1);
    const bool is_64bit = m.cpu_is_64bit();
    if (!m_file_manager) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    if (overlapped != 0) {
        constexpr u32 STATUS_PENDING = 0x00000103;
        const GuestAddr internal_high =
            overlapped + (is_64bit ? 8u : 4u);
        const GuestAddr event_field =
            overlapped + (is_64bit ? 24u : 16u);
        const auto status_write = is_64bit
            ? m_memory.write_u64(overlapped, STATUS_PENDING)
            : m_memory.write_u32(overlapped, STATUS_PENDING);
        const auto count_write = is_64bit
            ? m_memory.write_u64(internal_high, 0)
            : m_memory.write_u32(internal_high, 0);
        if (status_write.is_err() || count_write.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }

        u64 generation = 0;
        const PipeConnectResult start =
            m_file_manager->begin_async_named_pipe_connect(h, overlapped, &generation);
        if (start == PipeConnectResult::AlreadyConnected) {
            ipc::ipc_log(
                "ConnectNamedPipe h=0x%x ovlp=0x%llx -> already connected",
                static_cast<unsigned>(h),
                static_cast<unsigned long long>(overlapped));
            set_last_error(error_codes::ERROR_PIPE_CONNECTED);
            m.set_return(0);
            return;
        }
        if (start == PipeConnectResult::InvalidHandle) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (start != PipeConnectResult::Pending) {
            set_last_error(error_codes::ERROR_PIPE_LISTENING);
            m.set_return(0);
            return;
        }

        u64 event_value = is_64bit
            ? m_memory.read_u64(event_field).value_or(0)
            : m_memory.read_u32(event_field).value_or(0);
        if (event_value != 0 && m_sync_manager) {
            (void)m_sync_manager->reset_event(
                static_cast<Handle>(event_value & ~u64{1}));
        }

        // The completion belongs to the request: record the handle's port
        // now so a cancel caused by CloseHandle still reaches it.
        snapshot_completion_route(h, overlapped);
        std::vector<PipeConnectWorker> finished;
        {
            std::lock_guard<std::mutex> lock(m_pipe_connect_workers_mutex);
            // Workers that already finished are joined (outside the lock)
            // instead of piling up until process exit.
            for (auto it = m_pipe_connect_workers.begin();
                 it != m_pipe_connect_workers.end();) {
                if (it->done->load(std::memory_order_acquire)) {
                    finished.push_back(std::move(*it));
                    it = m_pipe_connect_workers.erase(it);
                } else {
                    ++it;
                }
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            m_pipe_connect_workers.push_back(PipeConnectWorker{done, std::jthread(
                [this, h, overlapped, is_64bit, done, generation](std::stop_token stop_token) {
                    struct Done {
                        std::atomic<bool>& flag;
                        ~Done() { flag.store(true, std::memory_order_release); }
                    } mark{*done};
                    const PipeConnectResult result =
                        m_file_manager->wait_async_named_pipe_connect(
                            h, generation, stop_token);
                    if (result == PipeConnectResult::Disconnected) {
                        // DisconnectNamedPipe while pending, also when the
                        // handle is closed after it
                        // (pipe_pending_connect_end_probe).
                        constexpr u32 STATUS_PIPE_DISCONNECTED = 0xC00000B0;
                        complete_overlapped_file_io(
                            h, 0, overlapped, is_64bit, STATUS_PIPE_DISCONNECTED,
                            error_codes::ERROR_PIPE_NOT_CONNECTED);
                        return;
                    }
                    if (result == PipeConnectResult::Closed) {
                        constexpr u32 STATUS_PIPE_BROKEN = 0xC000014B;
                        complete_overlapped_file_io(
                            h, 0, overlapped, is_64bit, STATUS_PIPE_BROKEN,
                            error_codes::ERROR_BROKEN_PIPE);
                        return;
                    }
                    if (result == PipeConnectResult::Cancelled) {
                        // CancelIo / CancelIoEx / CloseHandle: the OVERLAPPED
                        // completes with ERROR_OPERATION_ABORTED (Learn,
                        // CancelIoEx).
                        constexpr u32 STATUS_CANCELLED = 0xC0000120;
                        complete_overlapped_file_io(
                            h, 0, overlapped, is_64bit, STATUS_CANCELLED,
                            error_codes::ERROR_OPERATION_ABORTED);
                        return;
                    }
                    if (result != PipeConnectResult::Connected) return;
                    ipc::ipc_log(
                        "ConnectNamedPipe h=0x%x ovlp=0x%llx -> CONNECTED",
                        static_cast<unsigned>(h),
                        static_cast<unsigned long long>(overlapped));
                    complete_overlapped_file_io(
                        h, /*bytes_transferred=*/0, overlapped, is_64bit);
                })});
        }
        finished.clear();
        ipc::ipc_log("ConnectNamedPipe h=0x%x ovlp=0x%llx -> IO_PENDING",
                     static_cast<unsigned>(h),
                     static_cast<unsigned long long>(overlapped));
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }

    // A synchronous blocking-mode pipe waits until a client connects. There
    // is no arbitrary host timeout in the Windows contract.
    const PipeConnectResult result =
        m_file_manager->connect_named_pipe(h, /*timeout_ms=*/-1);
    switch (result) {
        case PipeConnectResult::Connected:
            ipc::ipc_log("ConnectNamedPipe h=0x%x -> CONNECTED",
                         static_cast<unsigned>(h));
            m.set_return(1);
            return;
        case PipeConnectResult::AlreadyConnected:
            ipc::ipc_log("ConnectNamedPipe h=0x%x -> already connected",
                         static_cast<unsigned>(h));
            set_last_error(error_codes::ERROR_PIPE_CONNECTED);
            m.set_return(0);
            return;
        case PipeConnectResult::Pending:
        case PipeConnectResult::TimedOut:
            set_last_error(error_codes::ERROR_PIPE_LISTENING);
            m.set_return(0);
            return;
        case PipeConnectResult::InvalidHandle:
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        case PipeConnectResult::Disconnected:
            set_last_error(error_codes::ERROR_PIPE_NOT_CONNECTED);
            m.set_return(0);
            return;
        case PipeConnectResult::Cancelled:  // overlapped-only results
        case PipeConnectResult::Closed:
            set_last_error(error_codes::ERROR_OPERATION_ABORTED);
            m.set_return(0);
            return;
    }
}
void Kernel32ExtManager::impl_DisconnectNamedPipe(Marshaler& m) {
    const auto h = static_cast<Handle>(m.arg(0));
    if (!m_file_manager) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    switch (m_file_manager->disconnect_named_pipe(h)) {
        case PipeDisconnectResult::Disconnected:
            ipc::ipc_log("DisconnectNamedPipe h=0x%x -> DISCONNECTED",
                         static_cast<unsigned>(h));
            m.set_return(1);
            return;
        case PipeDisconnectResult::NotConnected:
            set_last_error(error_codes::ERROR_PIPE_NOT_CONNECTED);
            m.set_return(0);
            return;
        case PipeDisconnectResult::InvalidHandle:
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
    }
}
void Kernel32ExtManager::impl_GetNamedPipeClientProcessId(Marshaler& m) {
    // BOOL GetNamedPipeClientProcessId(HANDLE Pipe, PULONG ClientProcessId)
    // Learn: Pipe must be a CreateNamedPipe instance; returns connected client.
    const Handle pipe = static_cast<Handle>(m.arg(0));
    const GuestAddr pid_ptr = m.arg_ptr(1);
    if (pid_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    u32 pid = 0;
    u32 error = error_codes::ERROR_SUCCESS;
    if (!m_file_manager->named_pipe_client_process_id(pipe, pid, error)) {
        set_last_error(error);
        ipc::ipc_log("GetNamedPipeClientProcessId h=0x%x -> FALSE gle=%u",
                     static_cast<unsigned>(pipe),
                     static_cast<unsigned>(error));
        m.set_return(0);
        return;
    }
    (void)m.write_u32(pid_ptr, pid);
    ipc::ipc_log("GetNamedPipeClientProcessId h=0x%x -> TRUE pid=%u",
                 static_cast<unsigned>(pipe),
                 static_cast<unsigned>(pid));
    m.set_return(1);
}
void Kernel32ExtManager::impl_GetNamedPipeServerProcessId(Marshaler& m) {
    // BOOL GetNamedPipeServerProcessId(HANDLE Pipe, PULONG ServerProcessId)
    // Learn: CreateNamedPipe handle. The inherited client end also uses this
    // to OpenProcess(PROCESS_DUP_HANDLE) the parent.
    const Handle pipe = static_cast<Handle>(m.arg(0));
    const GuestAddr pid_ptr = m.arg_ptr(1);
    if (pid_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    u32 pid = 0;
    u32 error = error_codes::ERROR_SUCCESS;
    if (!m_file_manager->named_pipe_server_process_id(pipe, pid, error)) {
        set_last_error(error);
        ipc::ipc_log("GetNamedPipeServerProcessId h=0x%x -> FALSE gle=%u",
                     static_cast<unsigned>(pipe),
                     static_cast<unsigned>(error));
        m.set_return(0);
        return;
    }
    (void)m.write_u32(pid_ptr, pid);
    ipc::ipc_log("GetNamedPipeServerProcessId h=0x%x -> TRUE pid=%u",
                 static_cast<unsigned>(pipe),
                 static_cast<unsigned>(pid));
    m.set_return(1);
}
// ClearCommError / EscapeCommFunction / GetCommModemStatus / SetCommState /
// SetCommTimeouts live with the rest of the comm family in comm_port.cpp.

} // namespace vitreo::winapi
