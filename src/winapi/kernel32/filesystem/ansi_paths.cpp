// SPDX-License-Identifier: LGPL-2.1-only

/// @file ansi_paths.cpp
/// @brief ANSI directory and Windows/System path helpers.
///
/// CPU-Z ANSI File/Directory APIs banner.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
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
// CPU-Z: ANSI File/Directory APIs
// =============================================================================

void Kernel32ExtManager::impl_SetDllDirectoryA(Marshaler& m) {
    const GuestAddr path_ptr = m.arg_ptr(0);
    if (m_module_manager == nullptr) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    if (path_ptr == 0) {
        m_module_manager->set_dll_directory(std::nullopt);
        VITREO_TRACE("[k32ext] SetDllDirectoryA(NULL) -> TRUE\n");
        m.set_return(1);
        return;
    }

    auto path = m.read_ansi_string(path_ptr, 32768);
    if (path.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m_module_manager->set_dll_directory(path.value());
    VITREO_TRACE("[k32ext] SetDllDirectoryA('%s') -> TRUE\n", path.value().c_str());
    m.set_return(1);
}

void Kernel32ExtManager::impl_SetDllDirectoryW(Marshaler& m) {
    const GuestAddr path_ptr = m.arg_ptr(0);
    if (m_module_manager == nullptr) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    if (path_ptr == 0) {
        m_module_manager->set_dll_directory(std::nullopt);
        VITREO_TRACE("[k32ext] SetDllDirectoryW(NULL) -> TRUE\n");
        m.set_return(1);
        return;
    }

    const std::string path = read_wide_ascii_z(path_ptr, 32768);
    m_module_manager->set_dll_directory(path);
    VITREO_TRACE("[k32ext] SetDllDirectoryW('%s') -> TRUE\n", path.c_str());
    m.set_return(1);
}

void Kernel32ExtManager::impl_CreateDirectoryA(Marshaler& m) {
    GuestAddr path_ptr = m.arg_ptr(0);
    (void)m.arg_ptr(1); // lpSecurityAttributes

    if (path_ptr == 0) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }
    auto r = m.read_ansi_string(path_ptr, 260);
    if (r.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // MS Learn: empty / missing intermediate path → ERROR_PATH_NOT_FOUND.
    // VFS maps "" to the capsule root, so create_dir("") would mkdir the root
    // and return PathAlreadyExists (seen as -5006 : 0x800700b7 after the
    // ERROR_ALREADY_EXISTS fix). Reject before touching the VFS.
    if (r.value().empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        VITREO_TRACE("[k32ext] CreateDirectoryA('') -> FALSE (PATH_NOT_FOUND)\n");
        m.set_return(0);
        return;
    }
    const auto created = m_vfs.create_dir(r.value());
    if (created.is_err()) {
        // MS Learn CreateDirectoryA: existing directory → FALSE +
        // ERROR_ALREADY_EXISTS (183). vfs_error_to_win32 maps PathAlreadyExists
        // to ERROR_FILE_EXISTS (80) for CreateFile(CREATE_NEW). Callers treat
        // 80 as fatal.
        const auto err = created.error();
        if (err == capsule::VfsError::PathAlreadyExists) {
            set_last_error(error_codes::ERROR_ALREADY_EXISTS);
        } else {
            set_last_error(vfs_error_to_win32(err));
        }
        VITREO_TRACE("[k32ext] CreateDirectoryA('%s') -> FALSE (%s) gle=%u\n",
                     r.value().c_str(),
                     capsule::vfs_error_to_string(err),
                     err == capsule::VfsError::PathAlreadyExists
                         ? error_codes::ERROR_ALREADY_EXISTS
                         : vfs_error_to_win32(err));
        m.set_return(0);
        return;
    }
    VITREO_TRACE("[k32ext] CreateDirectoryA('%s') -> TRUE\n", r.value().c_str());
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetTempPathA(Marshaler& m) {
    // DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer): the same path
    // as GetTempPathW (Learn), in CP_ACP. Fits: the string and NUL, returning
    // its length; too small (or no buffer): the size needed including NUL.
    const u32 buf_len = static_cast<u32>(m.arg(0));
    const GuestAddr buf_ptr = m.arg_ptr(1);
    const std::vector<u16> wide = utf8_to_utf16(resolve_temp_path());
    std::vector<u8> ansi(wide.size() * 4 + 4, 0);
    const auto conv = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, wide.data(), wide.size(),
                                                   ansi.data(), ansi.size(), nullptr, false);
    const u32 len = conv.count;
    if (buf_ptr == 0 || buf_len < len + 1) {
        m.set_return(len + 1);
        return;
    }
    (void)m_memory.write_bytes(buf_ptr, ansi.data(), len);
    (void)m_memory.write_u8(buf_ptr + len, 0);
    VITREO_TRACE("[k32ext] GetTempPathA -> %u\n", len);
    m.set_return(len);
}

void Kernel32ExtManager::impl_GetWindowsDirectoryA(Marshaler& m) {
    GuestAddr buf_ptr = m.arg_ptr(0);
    (void)m.arg(1); // uSize

    const char* windir = "C:\\Windows";
    u32 len = 10;

    if (buf_ptr != 0) {
        for (u32 i = 0; i < len; ++i) {
            (void)m_memory.write_u8(buf_ptr + i, static_cast<u8>(windir[i]));
        }
        (void)m_memory.write_u8(buf_ptr + len, 0);
    }
    VITREO_TRACE("[k32ext] GetWindowsDirectoryA -> %u\n", len);
    m.set_return(len);
}

void Kernel32ExtManager::impl_GetSystemDirectoryA(Marshaler& m) {
    // UINT GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize)
    GuestAddr buf_ptr = m.arg_ptr(0);
    u32 size = static_cast<u32>(m.arg(1));

    const char* sysdir = "C:\\Windows\\system32";
    constexpr u32 len = 19;

    if (buf_ptr == 0 || size == 0 || size <= len) {
        m.set_return(len + 1);
        return;
    }
    for (u32 i = 0; i < len; ++i) {
        (void)m_memory.write_u8(buf_ptr + i, static_cast<u8>(sysdir[i]));
    }
    (void)m_memory.write_u8(buf_ptr + len, 0);
    VITREO_TRACE("[k32ext] GetSystemDirectoryA -> %u\n", len);
    m.set_return(len);
}

} // namespace vitreo::winapi
