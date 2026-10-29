// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_ops_stubs.cpp
/// @brief File Operations stubs banner.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "capsule/vfs/path_encoding.hpp"
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
#include "capsule/vfs/win32_full_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
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
// File Operations (stubs)
// =============================================================================

// CopyFileA was previously unregistered entirely, which sent every ANSI caller
// to the generic missing-export stub: that returns 0xFFFFFFFF (a non-zero, and
// therefore TRUE-looking, BOOL) and pops zero stdcall bytes, leaking 12 bytes
// of the caller's stack per call. Both now run the shared copy engine
// (copy_file_engine.cpp).
void Kernel32ExtManager::copy_file_entry(Marshaler& m, bool wide) {
    // BOOL CopyFile(LPCTSTR lpExistingFileName, LPCTSTR lpNewFileName, BOOL bFailIfExists)
    auto src_res = wide ? m.read_wide_string(m.arg_ptr(0))
                        : m.read_ansi_string(m.arg_ptr(0));
    auto dst_res = wide ? m.read_wide_string(m.arg_ptr(1))
                        : m.read_ansi_string(m.arg_ptr(1));
    if (src_res.is_err() || dst_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    CopyFileRequest r;
    r.api = CopyFileApi::CopyFile;
    r.src = src_res.value();
    r.dst = dst_res.value();
    r.flags = m.arg32(2) != 0 ? 0x1u /*COPY_FILE_FAIL_IF_EXISTS*/ : 0u;
    const u32 err = run_copy_file(m, r);
    // Success sets ERROR_SUCCESS (last_error_on_success_probe CopyFileW_new).
    set_last_error(err);
    m.set_return(err == 0 ? 1u : 0u);
}

void Kernel32ExtManager::impl_CopyFileA(Marshaler& m) { copy_file_entry(m, false); }
void Kernel32ExtManager::impl_CopyFileW(Marshaler& m) { copy_file_entry(m, true); }

void Kernel32ExtManager::impl_MoveFileW(Marshaler& m) {
    // BOOL MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName)
    auto src_res = m.read_wide_string(m.arg_ptr(0));
    auto dst_res = m.read_wide_string(m.arg_ptr(1));

    if (src_res.is_err() || dst_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        VITREO_TRACE("[kernel32_ext] MoveFileW() -> FALSE (bad params)\n");
        m.set_return(0);
        return;
    }

    auto r = m_vfs.rename(src_res.value(), dst_res.value());
    bool ok = r.is_ok();
    if (!ok) {
        using capsule::VfsError;
        auto err = r.error();
        // PathAlreadyExists -> ERROR_ALREADY_EXISTS (183), not FILE_EXISTS (80);
        // FileNotFound vs PathNotFound kept distinct (files_ns_probe
        // MoveFileW_missing_source / _missing_source_dir / _missing_dest_dir).
        if (err == VfsError::FileNotFound || err == VfsError::NotAFile) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        } else if (err == VfsError::PathNotFound) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        } else if (err == VfsError::PathAlreadyExists) {
            set_last_error(error_codes::ERROR_ALREADY_EXISTS);
        } else {
            set_last_error(vfs_error_to_win32(err));
        }
    }
    VITREO_TRACE("[kernel32_ext] MoveFileW(%s, %s) -> %s\n",
                 src_res.value().c_str(), dst_res.value().c_str(),
                 ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::impl_MoveFileA(Marshaler& m) {
    // BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName)
    auto src_res = m.read_ansi_string(m.arg_ptr(0));
    auto dst_res = m.read_ansi_string(m.arg_ptr(1));

    if (src_res.is_err() || dst_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto r = m_vfs.rename(src_res.value(), dst_res.value());
    bool ok = r.is_ok();
    if (!ok) {
        using capsule::VfsError;
        auto err = r.error();
        if (err == VfsError::PathNotFound || err == VfsError::FileNotFound ||
            err == VfsError::NotAFile) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        } else if (err == VfsError::PathAlreadyExists) {
            set_last_error(error_codes::ERROR_ALREADY_EXISTS);
        } else {
            set_last_error(vfs_error_to_win32(err));
        }
    }
    VITREO_TRACE("[kernel32_ext] MoveFileA(%s, %s) -> %s\n",
                 src_res.value().c_str(), dst_res.value().c_str(),
                 ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::impl_SetCurrentDirectoryA(Marshaler& m) {
    // BOOL SetCurrentDirectoryA(LPCSTR lpPathName): the W contract on the
    // ANSI (CP_ACP) spelling of the name.
    GuestAddr path_ptr = m.arg_ptr(0);
    if (path_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto res = m.read_ansi_string(path_ptr);
    if (res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(change_current_directory(capsule::ansi_to_utf8(res.value())) ? 1 : 0);
}

void Kernel32ExtManager::impl_GetShortPathNameA(Marshaler& m) {
    // DWORD GetShortPathNameA(LPCSTR lpszLongPath, LPSTR lpszShortPath, DWORD cchBuffer)
    // Vitreo's VFS has no 8.3 aliases, so the short path equals the long path.
    GuestAddr long_path = m.arg_ptr(0);
    GuestAddr short_path = m.arg_ptr(1);
    u32 cch_buffer = static_cast<u32>(m.arg(2));

    auto res = m.read_ansi_string(long_path);
    if (res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string& path = res.value();
    u32 src_len = static_cast<u32>(path.size());

    // Return value excludes the null terminator; needs cchBuffer > src_len to copy.
    if (short_path != 0 && cch_buffer > src_len) {
        for (u32 i = 0; i < src_len; ++i) {
            (void)m_memory.write_u8(short_path + i, static_cast<u8>(path[i]));
        }
        (void)m_memory.write_u8(short_path + src_len, 0);
        m.set_return(src_len);
    } else {
        // Buffer too small (or size query): return required size incl. null.
        m.set_return(src_len + 1);
    }
}

void Kernel32ExtManager::impl_RemoveDirectoryW(Marshaler& m) {
    // BOOL RemoveDirectoryW(LPCWSTR lpPathName)
    // Learn: path must name an empty directory. A regular file fails with
    // ERROR_DIRECTORY and is left alone (files_ns_probe RemoveDirectoryW_file).
    remove_directory_common(m, /*wide=*/true);
}

void Kernel32ExtManager::remove_directory_common(Marshaler& m, bool wide) {
    auto path_res = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
    if (path_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        VITREO_TRACE("[kernel32_ext] RemoveDirectory() -> FALSE (bad param)\n");
        m.set_return(0);
        return;
    }
    const std::string path = wide ? path_res.value()
                                  : capsule::ansi_to_utf8(path_res.value());
    if (path.empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }

    auto info = m_vfs.stat(path);
    if (info.is_ok() &&
        !capsule::has_attribute(info.value().attributes,
                                capsule::FileAttributes::Directory)) {
        set_last_error(error_codes::ERROR_DIRECTORY);
        VITREO_TRACE("[kernel32_ext] RemoveDirectoryW(%s) -> FALSE (not a directory)\n",
                     path.c_str());
        m.set_return(0);
        return;
    }

    auto r = m_vfs.remove(path);
    bool ok = r.is_ok();
    if (!ok) {
        using capsule::VfsError;
        auto err = r.error();
        // Missing leaf -> FILE_NOT_FOUND; missing parent -> PATH_NOT_FOUND
        // (files_ns_probe RemoveDirectoryW_missing / _missing_parent).
        if (err == VfsError::FileNotFound) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        } else if (err == VfsError::PathNotFound || err == VfsError::NotADirectory) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        } else if (err == VfsError::DirectoryNotEmpty) {
            set_last_error(error_codes::ERROR_DIR_NOT_EMPTY);
        } else if (err == VfsError::AccessDenied) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
        } else if (err == VfsError::SharingViolation) {
            set_last_error(error_codes::ERROR_SHARING_VIOLATION);
        } else {
            set_last_error(error_codes::ERROR_GEN_FAILURE);
        }
    }
    VITREO_TRACE("[kernel32_ext] RemoveDirectoryW(%s) -> %s\n",
                 path.c_str(), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

} // namespace vitreo::winapi
