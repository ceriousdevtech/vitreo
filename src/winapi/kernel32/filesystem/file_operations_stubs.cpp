// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_ops.cpp
/// @brief Extended file and system operation stubs for kernel32 (7-Zip installer)

#include "winapi/kernel32/filesystem/file_ops.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "ipc/ipc_gate.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

/// @file file_operations_stubs.cpp
/// @brief File Operations (stubs)
///
/// Closed cut from file_ops.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// File Operations (stubs)
// =============================================================================

void FileOpsManager::delete_file_common(Marshaler& m, bool wide) {
    // BOOL DeleteFileA/W(LPCSTR/W lpFileName)
    //
    // One implementation for both variants: they previously carried separate
    // copies of the VfsError -> Win32 mapping and had already drifted apart,
    // with the wide form collapsing PathNotFound onto ERROR_FILE_NOT_FOUND.
    const char* api = wide ? "DeleteFileW" : "DeleteFileA";
    static const bool diag = std::getenv("VITREO_FILE_DIAG") != nullptr;

    auto path_res = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
    if (path_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        if (diag) {
            std::fprintf(stderr, "[filediag] %s: unreadable path pointer\n", api);
        }
        m.set_return(0);
        return;
    }
    const std::string& path = path_res.value();

    // Windows DeleteFile removes files and file symlinks only. A directory
    // (including a directory reparse point / symlink-to-dir) fails with
    // ERROR_ACCESS_DENIED — RemoveDirectory removes those (Windows
    // symlink_find2: DeleteFileW_linkdir err=5, RemoveDirectoryW ok).
    // Use lstat so a file symlink is not confused with its target.
    auto info = m_vfs.lstat(path);
    const bool is_dir = info.is_ok() &&
        capsule::has_attribute(info.value().attributes,
                               capsule::FileAttributes::Directory);

    if (diag) {
        // Report the parent's existence separately: it is what decides
        // FILE_NOT_FOUND vs PATH_NOT_FOUND, so it must be visible when the
        // guest's error code looks wrong.
        std::string parent = path;
        auto slash = parent.find_last_of("\\/");
        parent = (slash == std::string::npos) ? std::string() : parent.substr(0, slash);
        const bool parent_ok = parent.empty() || m_vfs.stat(parent).is_ok();
        std::fprintf(stderr,
            "[filediag] %s path='%s' stat=%s is_dir=%d parent='%s' parent_exists=%d\n",
            api, path.c_str(), info.is_ok() ? "ok" : "err",
            is_dir ? 1 : 0, parent.c_str(), parent_ok ? 1 : 0);
    }

    if (is_dir) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        VITREO_TRACE("[file_ops] %s(%s) -> FALSE (is a directory)\n", api, path.c_str());
        if (diag) {
            std::fprintf(stderr, "[filediag] %s -> FALSE last_error=5 (directory)\n", api);
        }
        m.set_return(0);
        return;
    }

    auto r = m_vfs.remove(path);
    const bool ok = r.is_ok();
    if (ok) {
    } else {
        // Single shared table (winapi/kernel32/file.hpp) — notably it keeps
        // PathNotFound (ERROR_PATH_NOT_FOUND) distinct from FileNotFound
        // (ERROR_FILE_NOT_FOUND).
        set_last_error(vfs_error_to_win32(r.error()));
    }
    if (diag) {
        std::fprintf(stderr, "[filediag] %s -> %s vfs_err=%s last_error=%u\n",
                     api, ok ? "TRUE" : "FALSE",
                     ok ? "none" : capsule::vfs_error_to_string(r.error()),
                     get_last_error());
    }
    VITREO_TRACE("[file_ops] %s(%s) -> %s (last_error=%u)\n",
                 api, path.c_str(), ok ? "TRUE" : "FALSE", get_last_error());
    m.set_return(ok ? 1 : 0);
}

void FileOpsManager::impl_DeleteFileW(Marshaler& m) {
    delete_file_common(m, /*wide=*/true);
}

void FileOpsManager::impl_DeleteFileA(Marshaler& m) {
    delete_file_common(m, /*wide=*/false);
}

void FileOpsManager::impl_CreateDirectoryW(Marshaler& m) {
    // BOOL CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
    auto path_res = m.read_wide_string(m.arg_ptr(0));
    (void)m.arg(1); // security attributes (ignored)

    if (path_res.is_err()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0); // FALSE
        return;
    }
    if (path_res.value().empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        VITREO_TRACE("[file_ops] CreateDirectoryW('') -> FALSE (PATH_NOT_FOUND)\n");
        m.set_return(0);
        return;
    }

    auto result = m_vfs.create_dir(path_res.value());
    bool ok = result.is_ok();
    if (!ok && result.is_err() && result.error() == capsule::VfsError::PathAlreadyExists) {
        // Windows contract: fail with ERROR_ALREADY_EXISTS even when existing path is a directory.
        set_last_error(error_codes::ERROR_ALREADY_EXISTS);
        VITREO_TRACE( "[file_ops] CreateDirectoryW(%s) -> FALSE (already exists)\n",
                     path_res.value().c_str());
        m.set_return(0);
        return;
    }
    if (ok) {
    } else {
        using capsule::VfsError;
        auto err = result.error();
        if (err == VfsError::PathNotFound) set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        else if (err == VfsError::AccessDenied) set_last_error(error_codes::ERROR_ACCESS_DENIED);
        else set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
    }

    VITREO_TRACE( "[file_ops] CreateDirectoryW(%s) -> %s\n",
                 path_res.value().c_str(), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

namespace {

/// SetFileAttributesA/W body. Learn (SetFileAttributesW): sets the settable
/// attribute set on a file or directory; the stored word reads back exactly
/// as files_meta_probe.expected records (NORMAL alone, DIRECTORY and
/// COMPRESSED ignored, READONLY makes the file refuse writers and DeleteFile).
void set_file_attributes_common(Marshaler& m, capsule::Vfs& vfs, bool wide) {
    auto path_res = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
    const u32 attrs = m.arg32(1);
    if (path_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
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
    auto r = vfs.set_attributes(path, attrs);
    if (r.is_err()) {
        // Missing leaf -> 2, missing directory -> 3 (files_meta_probe
        // SetFileAttributesW_missing_file / _missing_dir).
        set_last_error(vfs_error_to_win32(r.error()));
        VITREO_TRACE("[file_ops] SetFileAttributes%c(%s, 0x%x) -> FALSE (%s)\n",
                     wide ? 'W' : 'A', path.c_str(), attrs,
                     capsule::vfs_error_to_string(r.error()));
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

}  // namespace

void FileOpsManager::impl_SetFileAttributesW(Marshaler& m) {
    set_file_attributes_common(m, m_vfs, /*wide=*/true);
}

void FileOpsManager::impl_SetFileAttributesA(Marshaler& m) {
    set_file_attributes_common(m, m_vfs, /*wide=*/false);
}

void FileOpsManager::impl_SetFileTime(Marshaler& m) {
    // BOOL SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime,
    //                  const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime)
    // Learn: needs FILE_WRITE_ATTRIBUTES; NULL or a zero FILETIME leaves a
    // time unchanged; 0xFFFFFFFF/0xFFFFFFFF stops I/O through this handle
    // from updating that time. Full 100 ns precision, pre-1970 included
    // (files_meta_probe "# times").
    const auto h = static_cast<Handle>(m.arg(0));
    const GuestAddr pointers[3] = {m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3)};
    u64 values[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (pointers[i] == 0) continue;
        auto lo = m_memory.read_u32(pointers[i]);
        auto hi = m_memory.read_u32(pointers[i] + 4);
        if (lo.is_err() || hi.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        values[i] = (static_cast<u64>(hi.value()) << 32) | lo.value();
    }
    const capsule::FileHandle vfs_h = m_file_manager != nullptr
        ? m_file_manager->vfs_handle_for(h) : capsule::INVALID_FILE_HANDLE;
    if (vfs_h == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    constexpr u32 FILE_WRITE_ATTRIBUTES = 0x0100;
    constexpr u32 GENERIC_WRITE_RIGHT = 0x40000000u;
    constexpr u32 GENERIC_ALL_RIGHT = 0x10000000u;
    constexpr u32 MAXIMUM_ALLOWED_RIGHT = 0x02000000u;
    const u32 access = m_file_manager->handle_table().get_access(h);
    if ((access & (FILE_WRITE_ATTRIBUTES | GENERIC_WRITE_RIGHT | GENERIC_ALL_RIGHT |
                   MAXIMUM_ALLOWED_RIGHT)) == 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);  // SetFileTime_read_only_handle
        m.set_return(0);
        return;
    }
    auto r = m_vfs.set_file_times_ft(vfs_h, pointers[0] ? &values[0] : nullptr,
                                     pointers[1] ? &values[1] : nullptr,
                                     pointers[2] ? &values[2] : nullptr);
    if (r.is_err()) {
        set_last_error(vfs_error_to_win32(r.error()));
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
