// SPDX-License-Identifier: LGPL-2.1-only

/// @file volume_info.cpp
/// @brief Drive/volume queries plus a few early Additional-stubs exports.
///
/// GetDriveType, GetDiskFreeSpaceEx, GetVolumeInformation, MoveFileEx,
/// and the condition-variable / semaphore / GetProcessId stubs that
/// shared the same closed Additional Win32 stubs run.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/synthetic_handle_registry.hpp"
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
#include "winapi/kernel32/synchronization/srw_word.hpp"
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
// Additional Win32 stubs
// =============================================================================

void Kernel32ExtManager::impl_InitializeConditionVariable(Marshaler& m) {
    GuestAddr cv = m.arg_ptr(0);
    if (cv != 0) {
        // Zero pointer-sized field (CONDITION_VARIABLE = PVOID Ptr).
        (void)m.write_ptr(cv, 0);
    }
    VITREO_TRACE("[k32ext] InitializeConditionVariable(0x%llx)\n",
                 static_cast<unsigned long long>(cv));
}

void Kernel32ExtManager::impl_WakeConditionVariable(Marshaler& m) {
    GuestAddr cv = m.arg_ptr(0);
    {
        srw_word::Word cvw;
        if (srw_word::resolve(m_memory, m.arg_ptr(0), m.cpu_is_64bit(), cvw)) {
            srw_word::cv_wake(cvw, /*all=*/false);
            m.set_return(0);
            return;
        }
    }
    if (m_sync_manager != nullptr) {
        m_sync_manager->wake_condition_variable(cv);
    }
    // The native API is VOID.  Do not expose Vitreo's dispatch-gateway value
    // as an incidental pointer-sized return value.
    m.set_return(0);
    VITREO_TRACE("[k32ext] WakeConditionVariable(0x%llx)\n",
                 static_cast<unsigned long long>(cv));
}

void Kernel32ExtManager::impl_FreeLibraryAndExitThread(Marshaler& m) {
    // BOOL FreeLibraryAndExitThread(HMODULE hLibModule, DWORD dwExitCode)
    // Decrements the module refcount then ExitThread — never returns.
    const GuestAddr module = m.arg_ptr(0);
    const u32 exit_code = static_cast<u32>(m.arg(1));
    if (m_module_manager != nullptr && module != 0) {
        (void)m_module_manager->free_library(module);
    }
    if (m_thread_manager != nullptr) {
        VITREO_TRACE(
            "[k32ext] FreeLibraryAndExitThread(module=0x%llx exit=%u)\n",
            static_cast<unsigned long long>(module), exit_code);
        m_thread_manager->exit_current_thread_noreturn(m, exit_code);
        return;
    }
    VITREO_TRACE(
        "[k32ext] FreeLibraryAndExitThread(module=0x%llx exit=%u) -> "
        "no ThreadManager; cannot stop guest thread\n",
        static_cast<unsigned long long>(module), exit_code);
}

void Kernel32ExtManager::impl_GetProcessId(Marshaler& m) {
    // DWORD GetProcessId(HANDLE Process)
    //
    // Windows returns the identifier of the process the handle refers to, or 0
    // with LastError set on failure. The distinction between failure codes is
    // load-bearing: a 0 result paired with ERROR_INVALID_HANDLE is treated
    // as a handle-lifetime bug. A handle Vitreo genuinely tracks must
    // therefore resolve to its real pid.
    auto h_process = static_cast<Handle>(m.arg(0));

    // The GetCurrentProcess() pseudo-handle names this process. Report the same
    // identifier GetCurrentProcessId does so both agree.
    if (h_process == CURRENT_PROCESS_PSEUDO) {
        m.set_return(m_process_manager != nullptr
                         ? m_process_manager->get_current_process_id()
                         : static_cast<u32>(::getpid()));
        return;
    }

    // A real process handle from CreateProcess/OpenProcess carries the pid of
    // the host process backing that guest process.
    if (m_sync_manager != nullptr) {
        u32 pid = 0;
        if (m_sync_manager->process_pid(h_process, pid)) {
            m.set_return(pid);
            return;
        }
    }

    // Anything else is not a process handle: NULL, a closed handle, or a handle
    // of another type. Windows fails these with ERROR_INVALID_HANDLE.
    VITREO_TRACE("[k32ext] GetProcessId(0x%x) -> 0 ERROR_INVALID_HANDLE "
                 "(not a tracked process handle)\n",
                 static_cast<unsigned>(h_process));
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0u);
}



void Kernel32ExtManager::impl_CreateSemaphoreA(Marshaler& m) {
    (void)m.arg_ptr(0);
    i32 initial = static_cast<i32>(m.arg(1));
    i32 maximum = static_cast<i32>(m.arg(2));
    GuestAddr name_ptr = m.arg_ptr(3);
    std::string name;
    if (name_ptr) {
        auto r = m.read_ansi_string(name_ptr, 260);
        if (r.is_ok()) name = r.value();
    }
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_sync_manager) {
        Handle h = m_sync_manager->create_semaphore(initial, maximum, name);
        VITREO_TRACE("[k32ext] CreateSemaphoreA(init=%d,max=%d,name='%s') -> 0x%x\n",
                     initial, maximum, name.c_str(), h);
        m.set_return(static_cast<u64>(h));
        return;
    }
    u32 handle = 0xCE000000u + (++m_semaphore_counter);
    synthetic_handles::note_issued(static_cast<Handle>(handle));
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(handle);
}

void Kernel32ExtManager::impl_GetDriveTypeA(Marshaler& m) {
    std::string path;
    if (GuestAddr path_ptr = m.arg_ptr(0); path_ptr != 0) {
        auto value = m.read_ansi_string(path_ptr, 16);
        if (value.is_ok()) path = value.value();
    }
    auto resolved = m_vfs.resolve_components(path);
    m.set_return(resolved.is_ok() && resolved.value().first->is_optical()
                     ? 5u  // DRIVE_CDROM
                     : 3u); // DRIVE_FIXED
}

void Kernel32ExtManager::impl_GetDiskFreeSpaceExA(Marshaler& m) {
    (void)m.arg_ptr(0);
    GuestAddr free_avail = m.arg_ptr(1);
    GuestAddr total_bytes_ptr = m.arg_ptr(2);
    GuestAddr total_free = m.arg_ptr(3);

    u64 total_bytes = 0, free_bytes = 0;
    get_host_disk_stats(total_bytes, free_bytes);

    auto write_u64 = [&](GuestAddr addr, u64 val) {
        (void)m_memory.write_u32(addr,     static_cast<u32>(val & 0xFFFFFFFF));
        (void)m_memory.write_u32(addr + 4, static_cast<u32>(val >> 32));
    };

    if (free_avail != 0)      write_u64(free_avail, free_bytes);
    if (total_bytes_ptr != 0) write_u64(total_bytes_ptr, total_bytes);
    if (total_free != 0)      write_u64(total_free, free_bytes);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetVolumeInformationA(Marshaler& m) {
    std::string path;
    if (GuestAddr path_ptr = m.arg_ptr(0); path_ptr != 0) {
        auto value = m.read_ansi_string(path_ptr, 260);
        if (value.is_ok()) path = value.value();
    }
    GuestAddr vol_buf      = m.arg_ptr(1);
    u32 vol_buf_size       = static_cast<u32>(m.arg(2));
    GuestAddr serial_ptr   = m.arg_ptr(3);
    GuestAddr max_comp_ptr = m.arg_ptr(4);
    GuestAddr fs_flags_ptr = m.arg_ptr(5);
    GuestAddr fs_buf       = m.arg_ptr(6);
    u32 fs_buf_size        = static_cast<u32>(m.arg(7));

    auto write_ansi_z = [&](GuestAddr addr, u32 buf_size, const char* str) {
        if (addr == 0 || buf_size == 0) return;
        u32 len = static_cast<u32>(std::strlen(str));
        u32 to_copy = (len + 1 <= buf_size) ? len : (buf_size - 1);
        for (u32 i = 0; i < to_copy; ++i) {
            (void)m_memory.write_u8(addr + i, static_cast<u8>(str[i]));
        }
        (void)m_memory.write_u8(addr + to_copy, 0);
    };

    capsule::VolumeInfo volume;
    auto resolved = m_vfs.resolve_components(path);
    const bool has_volume_info = resolved.is_ok() &&
        resolved.value().first->volume_info(volume);
    const std::string& label = has_volume_info ? volume.label : std::string("VITREO");
    const std::string& filesystem = has_volume_info
        ? volume.filesystem_name : std::string("NTFS");

    write_ansi_z(vol_buf, vol_buf_size, label.c_str());
    u32 volume_serial = 0x12345678u;
    if (auto st = m_vfs.stat(path.empty() ? std::string("C:\\") : path);
        st.is_ok()) {
        volume_serial = static_cast<u32>(st.value().volume_id);
    } else if (path.size() >= 2 && path[1] == ':') {
        const std::string root = path.substr(0, 2) + "\\";
        if (auto st = m_vfs.stat(root); st.is_ok()) {
            volume_serial = static_cast<u32>(st.value().volume_id);
        }
    }
    if (serial_ptr   != 0) (void)m_memory.write_u32(serial_ptr,   volume_serial);
    if (max_comp_ptr != 0) (void)m_memory.write_u32(
        max_comp_ptr, has_volume_info ? volume.max_component_length : 255u);
    if (fs_flags_ptr != 0) (void)m_memory.write_u32(
        fs_flags_ptr, has_volume_info ? volume.filesystem_flags : 0x0000000Fu);
    write_ansi_z(fs_buf, fs_buf_size, filesystem.c_str());
    VITREO_TRACE("[k32ext] GetVolumeInformationA('%s') -> label='%s' fs='%s' flags=0x%x\n",
                 path.c_str(), label.c_str(), filesystem.c_str(),
                 has_volume_info ? volume.filesystem_flags : 0x00020005u);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetLogicalDriveStringsA(Marshaler& m) {
    u32 buf_size  = static_cast<u32>(m.arg(0));
    GuestAddr buf = m.arg_ptr(1);

    const auto drives = m_vfs.mounted_drives();
    const u32 required = static_cast<u32>(drives.size() * 4 + 1);
    if (buf == 0 || buf_size < required) {
        m.set_return(required);
        return;
    }
    GuestAddr cursor = buf;
    for (char drive : drives) {
        (void)m_memory.write_u8(cursor++, static_cast<u8>(drive));
        (void)m_memory.write_u8(cursor++, ':');
        (void)m_memory.write_u8(cursor++, '\\');
        (void)m_memory.write_u8(cursor++, 0);
    }
    (void)m_memory.write_u8(cursor, 0);
    m.set_return(required - 1);
}

void Kernel32ExtManager::impl_GetVolumePathNameW(Marshaler& m) {
    (void)m.arg_ptr(0); // lpszFileName
    GuestAddr out_buf = m.arg_ptr(1);
    u32 cch = static_cast<u32>(m.arg(2));
    if (out_buf == 0 || cch < 4) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0u);
        return;
    }
    (void)m_memory.write_u16(out_buf + 0, u'C');
    (void)m_memory.write_u16(out_buf + 2, u':');
    (void)m_memory.write_u16(out_buf + 4, u'\\');
    (void)m_memory.write_u16(out_buf + 6, 0);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetVolumeNameForVolumeMountPointW(Marshaler& m) {
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
    (void)m.arg(2);
    set_last_error(error_codes::ERROR_NOT_SUPPORTED);
    m.set_return(0u);
}

} // namespace vitreo::winapi
