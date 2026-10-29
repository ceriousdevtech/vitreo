// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_ops.cpp
/// @brief Extended file and system operation stubs for kernel32 (7-Zip installer)

#include "winapi/kernel32/filesystem/file_ops.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

/// @file movefiletransactedw_deletefiletransactedw_passth.cpp
/// @brief MoveFileTransactedW / DeleteFileTransactedW — passthrough stubs
///
/// Closed cut from file_ops.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// MoveFileTransactedW / DeleteFileTransactedW — passthrough stubs
// =============================================================================

void FileOpsManager::impl_MoveFileTransactedW(Marshaler& m) {
    // BOOL MoveFileTransactedW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName,
    //   LPPROGRESS_ROUTINE lpProgressRoutine, LPVOID lpData, DWORD dwFlags, HANDLE hTransaction)
    auto src_res = m.read_wide_string(m.arg_ptr(0));
    auto dst_ptr = m.arg_ptr(1);
    u32 flags = static_cast<u32>(m.arg(4));

    if (src_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Destination NULL => delete source (same semantics as MoveFileExW)
    if (dst_ptr == 0) {
        auto r = m_vfs.remove(src_res.value());
        bool ok = r.is_ok();
        if (!ok) set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        VITREO_TRACE("[file_ops] MoveFileTransactedW(%s, NULL, 0x%x) -> %s\n",
                     src_res.value().c_str(), flags, ok ? "TRUE" : "FALSE");
        m.set_return(ok ? 1 : 0);
        return;
    }

    auto dst_res = m.read_wide_string(dst_ptr);
    if (dst_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // MOVEFILE_REPLACE_EXISTING (0x1): delete destination first
    if (flags & 0x1) {
        (void)m_vfs.remove(dst_res.value());
    }

    auto r = m_vfs.rename(src_res.value(), dst_res.value());
    bool ok = r.is_ok();
    if (!ok) {
        using capsule::VfsError;
        auto err = r.error();
        // MoveFile reports ERROR_ALREADY_EXISTS (not ERROR_FILE_EXISTS) for a
        // colliding destination; everything else uses the shared table so a
        // missing directory stays ERROR_PATH_NOT_FOUND.
        if (err == VfsError::PathAlreadyExists) {
            set_last_error(error_codes::ERROR_ALREADY_EXISTS);
        } else {
            set_last_error(vfs_error_to_win32(err));
        }
    } else {
    }
    VITREO_TRACE("[file_ops] MoveFileTransactedW(%s, %s, 0x%x, hTx=0x%llx) -> %s\n",
                 src_res.value().c_str(), dst_res.value().c_str(),
                 flags, m.arg(5), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

void FileOpsManager::impl_DeleteFileTransactedW(Marshaler& m) {
    // BOOL DeleteFileTransactedW(LPCWSTR lpFileName, HANDLE hTransaction)
    // Ignore hTransaction, delegate to DeleteFileW logic.
    VITREO_TRACE("[file_ops] DeleteFileTransactedW(hTx=0x%llx) -> forwarding\n", m.arg(1));
    impl_DeleteFileW(m);
}

// Create the directory named by the wide string at @p new_dir_arg, mapping
// the VFS result onto the Windows contract. Shared by CreateDirectoryExW and
// CreateDirectoryTransactedW, which both take the new-directory path as their
// second argument (unlike CreateDirectoryW's first) and both ignore the
// template directory, which the VFS has no per-directory attributes to copy.
void FileOpsManager::create_directory_from_template(Marshaler& m,
                                                    usize new_dir_arg,
                                                    const char* api) {
    (void)m.arg(0); // lpTemplateDirectory — no attribute copy in the VFS
    auto path_res = m.read_wide_string(m.arg_ptr(new_dir_arg));

    if (path_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto result = m_vfs.create_dir(path_res.value());
    bool ok = result.is_ok();
    if (!ok && result.error() == capsule::VfsError::PathAlreadyExists) {
        set_last_error(error_codes::ERROR_ALREADY_EXISTS);
    } else if (ok) {
    } else {
        using capsule::VfsError;
        auto err = result.error();
        if (err == VfsError::PathNotFound) set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        else if (err == VfsError::AccessDenied) set_last_error(error_codes::ERROR_ACCESS_DENIED);
        else set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
    }

    VITREO_TRACE("[file_ops] %s(%s) -> %s\n", api,
                 path_res.value().c_str(), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

void FileOpsManager::impl_CreateDirectoryExW(Marshaler& m) {
    // BOOL CreateDirectoryExW(LPCWSTR lpTemplateDirectory,
    //   LPCWSTR lpNewDirectory, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
    create_directory_from_template(m, /*new_dir_arg=*/1, "CreateDirectoryExW");
}

void FileOpsManager::impl_CreateDirectoryTransactedW(Marshaler& m) {
    // BOOL CreateDirectoryTransactedW(LPCWSTR lpTemplateDirectory,
    //   LPCWSTR lpNewDirectory, LPSECURITY_ATTRIBUTES, HANDLE hTransaction)
    create_directory_from_template(m, /*new_dir_arg=*/1,
                                   "CreateDirectoryTransactedW");
}

} // namespace vitreo::winapi
