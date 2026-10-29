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

namespace vitreo::winapi {

// =============================================================================
// Construction
// =============================================================================

FileOpsManager::FileOpsManager(GuestMemory& memory, capsule::Vfs& vfs)
    : m_memory(memory), m_vfs(vfs) {}

void FileOpsManager::on_file_handle_closed(Handle h) noexcept {
    std::lock_guard<std::mutex> lock(m_directory_enumeration_mutex);
    m_directory_enumerations.erase(h);
}

void FileOpsManager::on_file_handle_duplicated(Handle source,
                                               Handle duplicate) noexcept {
    std::lock_guard<std::mutex> lock(m_directory_enumeration_mutex);
    auto it = m_directory_enumerations.find(source);
    if (it == m_directory_enumerations.end()) {
        return;
    }
    m_directory_enumerations[duplicate] = it->second;
}

// =============================================================================
// Internal Helpers
// =============================================================================

u32 FileOpsManager::write_wide_literal(GuestAddr addr, const char* str) {
    u32 i = 0;
    for (; str[i] != '\0'; ++i) {
        (void)m_memory.write_u16(addr + i * 2, static_cast<u16>(str[i]));
    }
    // Null terminator
    (void)m_memory.write_u16(addr + i * 2, 0);
    return i;
}

u32 FileOpsManager::guest_wstrlen(GuestAddr addr) {
    u32 len = 0;
    for (;;) {
        auto r = m_memory.read_u16(addr + len * 2);
        if (r.is_err() || r.value() == 0) break;
        ++len;
    }
    return len;
}




// =============================================================================
// String Operations
// =============================================================================

void FileOpsManager::impl_lstrcpyW(Marshaler& m) {
    // LPWSTR lstrcpyW(LPWSTR lpString1, LPCWSTR lpString2)
    GuestAddr dst = m.arg_ptr(0);
    GuestAddr src = m.arg_ptr(1);

    // Copy wide chars from src to dst (including null terminator)
    for (u32 i = 0; ; ++i) {
        auto r = m_memory.read_u16(src + i * 2);
        u16 ch = r.is_ok() ? r.value() : 0;
        (void)m_memory.write_u16(dst + i * 2, ch);
        if (ch == 0) break;
    }

    m.set_return(static_cast<u64>(dst));
}

void FileOpsManager::impl_lstrcatW(Marshaler& m) {
    // LPWSTR lstrcatW(LPWSTR lpString1, LPCWSTR lpString2)
    GuestAddr dst = m.arg_ptr(0);
    GuestAddr src = m.arg_ptr(1);

    // Find end of dst string
    u32 dst_len = guest_wstrlen(dst);

    // Copy src after dst (including null terminator)
    for (u32 i = 0; ; ++i) {
        auto r = m_memory.read_u16(src + i * 2);
        u16 ch = r.is_ok() ? r.value() : 0;
        (void)m_memory.write_u16(dst + (dst_len + i) * 2, ch);
        if (ch == 0) break;
    }

    m.set_return(static_cast<u64>(dst));
}



// =============================================================================
// GetFileInformationByName
// =============================================================================

void FileOpsManager::impl_GetFileInformationByName(Marshaler& m) {
    // BOOL GetFileInformationByName(PCWSTR FileName,
    //   FILE_INFO_BY_NAME_CLASS FileInformationClass, PVOID FileInfoBuffer,
    //   ULONG FileInfoBufferSize)                 (WinBase.h, 10.0.26100)
    // Classes (minwinbase.h): FileStatByNameInfo=0 -> FILE_STAT_INFORMATION
    // (72 bytes), FileStatLxByNameInfo=1 -> FILE_STAT_LX_INFORMATION (96),
    // FileCaseSensitiveByNameInfo=2 -> FILE_CASE_SENSITIVE_INFORMATION (4),
    // FileStatBasicByNameInfo=3 -> FILE_STAT_BASIC_INFORMATION (104); layouts
    // from winnt.h. Error rules measured in files_ns_probe (GFIBN_*): other
    // classes -> 87, short buffer -> 24, missing leaf/dir -> 2/3.
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u32 cls = m.arg32(1);
    const GuestAddr buf = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    static constexpr u32 kSizes[4] = {72, 96, 4, 104};
    if (cls > 3) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (size < kSizes[cls]) {
        set_last_error(error_codes::ERROR_BAD_LENGTH);
        m.set_return(0);
        return;
    }
    auto name = m.read_wide_string(name_ptr);
    if (name.is_err() || buf == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (name.value().empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }
    auto st = m_vfs.stat(name.value());
    if (st.is_err()) {
        set_last_error(vfs_error_to_win32(st.error()));
        m.set_return(0);
        return;
    }
    const capsule::FileInfo& info = st.value();
    std::vector<u8> out(kSizes[cls], 0);
    auto put32 = [&](usize off, u32 v) { std::memcpy(out.data() + off, &v, 4); };
    auto put64 = [&](usize off, u64 v) { std::memcpy(out.data() + off, &v, 8); };
    if (cls == 2) {
        // Vitreo directories are case-insensitive (no per-directory
        // FILE_CS_FLAG_CASE_SENSITIVE_DIR; files_ns_probe cs_flags = 0).
        put32(0, 0);
    } else {
        const bool is_dir = capsule::has_attribute(
            info.attributes, capsule::FileAttributes::Directory);
        put64(0, info.file_id);
        put64(8, info.creation_time);
        put64(16, info.last_access_time);
        put64(24, info.last_write_time);
        put64(32, info.change_time != 0 ? info.change_time : info.last_write_time);
        constexpr u64 kCluster = 4096;
        put64(40, is_dir ? 0 : (info.size + kCluster - 1) / kCluster * kCluster);
        put64(48, is_dir ? 0 : info.size);
        put32(56, static_cast<u32>(info.attributes));
        put32(60, 0);  // ReparseTag
        put32(64, info.link_count);
        if (cls == 0 || cls == 1) {
            put32(68, 0x001F01FFu);  // EffectiveAccess: FILE_ALL_ACCESS (GFIBN_class_0)
            // Lx fields stay zero: LxFlags = 0 means none present (GFIBN_class_1).
        } else {
            put32(68, 0x00000007u);  // DeviceType FILE_DEVICE_DISK (GFIBN_class_3)
            put32(72, 0x100u);       // DeviceCharacteristics (GFIBN_class_3 device_chars)
            put32(76, 0);            // Reserved
            put64(80, static_cast<u64>(static_cast<u32>(info.volume_id)));
            put64(88, info.file_id);  // FileId128 low half == FileId
            put64(96, 0);
        }
    }
    if (m_memory.write_bytes(buf, reinterpret_cast<const Byte*>(out.data()),
                             out.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

// =============================================================================
// ReplaceFileW
// =============================================================================

void FileOpsManager::impl_ReplaceFileW(Marshaler& m) {
    // BOOL ReplaceFileW(LPCWSTR lpReplacedFileName, LPCWSTR lpReplacementFileName,
    //   LPCWSTR lpBackupFileName, DWORD dwReplaceFlags, LPVOID lpExclude, LPVOID lpReserved)
    // Learn + files_ns_probe ReplaceFileW_*: both files must exist; on failure
    // other than 1175/1176/1177 the names are unchanged; replaced identity
    // (creation time, attributes) is preserved onto the survivor.
    constexpr u32 kValidFlags =
        0x1u |  // REPLACEFILE_WRITE_THROUGH
        0x2u |  // REPLACEFILE_IGNORE_MERGE_ERRORS
        0x4u;   // REPLACEFILE_IGNORE_ACL_ERRORS

    const GuestAddr replaced_ptr = m.arg_ptr(0);
    const GuestAddr replacement_ptr = m.arg_ptr(1);
    const GuestAddr backup_ptr = m.arg_ptr(2);
    const u32 flags = static_cast<u32>(m.arg(3));
    (void)m.arg_ptr(4);  // lpExclude — reserved, ignored when NULL
    const GuestAddr reserved = m.arg_ptr(5);

    if (reserved != 0 || (flags & ~kValidFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto replaced_res = m.read_wide_string(replaced_ptr);
    auto replacement_res = m.read_wide_string(replacement_ptr);
    if (replaced_res.is_err() || replacement_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string& replaced = replaced_res.value();
    const std::string& replacement = replacement_res.value();

    auto replaced_info = m_vfs.stat(replaced);
    if (replaced_info.is_err()) {
        set_last_error(vfs_error_to_win32(replaced_info.error()));
        m.set_return(0);
        return;
    }
    auto replacement_info = m_vfs.stat(replacement);
    if (replacement_info.is_err()) {
        set_last_error(vfs_error_to_win32(replacement_info.error()));
        m.set_return(0);
        return;
    }

    if (capsule::has_attribute(replaced_info.value().attributes,
                               capsule::FileAttributes::ReadOnly)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }

    // Capture identity to restore onto the survivor. Windows reports the
    // preserved attributes with ARCHIVE also set on the replaced path
    // (files_ns_probe replaced_attrs=0x22 after HIDDEN-only SetFileAttributes).
    const u64 creation = replaced_info.value().creation_time;
    u32 attrs = static_cast<u32>(replaced_info.value().attributes);
    if (!capsule::has_attribute(replaced_info.value().attributes,
                                capsule::FileAttributes::Directory)) {
        attrs |= static_cast<u32>(capsule::FileAttributes::Archive);
    }

    std::string backup;
    if (backup_ptr != 0) {
        auto backup_res = m.read_wide_string(backup_ptr);
        if (backup_res.is_err() || backup_res.value().empty()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        backup = backup_res.value();
        // Existing backup is replaced; ignore missing.
        (void)m_vfs.remove(backup);
        auto br = m_vfs.rename(replaced, backup);
        if (br.is_err()) {
            // Learn: ERROR_UNABLE_TO_REMOVE_REPLACED when the replaced file
            // cannot be moved aside; both inputs stay put
            // (files_ns_probe ReplaceFileW_backup_missing_dir -> 1175).
            set_last_error(error_codes::ERROR_UNABLE_TO_REMOVE_REPLACED);
            m.set_return(0);
            return;
        }
    } else {
        auto rem = m_vfs.remove(replaced);
        if (rem.is_err()) {
            set_last_error(vfs_error_to_win32(rem.error()));
            m.set_return(0);
            return;
        }
    }

    auto r = m_vfs.rename(replacement, replaced);
    if (r.is_err()) {
        // Partial: replaced already moved/removed. Report move-replacement
        // failure; do not invent success.
        set_last_error(error_codes::ERROR_UNABLE_TO_MOVE_REPLACEMENT);
        m.set_return(0);
        return;
    }

    // Preserve creation time and attributes of the original replaced file.
    (void)m_vfs.set_attributes(replaced, attrs);
    if (m_file_manager != nullptr) {
        constexpr u32 kWriteAttrs = 0x0100;  // FILE_WRITE_ATTRIBUTES
        constexpr u32 kShareAll = 0x7;
        constexpr u32 kOpenExisting = 3;
        const Handle h = m_file_manager->create_file(
            replaced, kWriteAttrs, kShareAll, kOpenExisting, 0);
        if (h != INVALID_HANDLE) {
            const auto vfs_h = m_file_manager->vfs_handle_for(h);
            if (vfs_h != capsule::INVALID_FILE_HANDLE) {
                (void)m_vfs.set_file_times_ft(vfs_h, &creation, nullptr, nullptr);
            }
            (void)m_file_manager->close_file(h);
        }
    }

    // Measured on Windows 11 (files_ns_probe): success with a backup name
    // leaves last-error 0xCB; success without a backup leaves 0.
    set_last_error(backup.empty() ? error_codes::ERROR_SUCCESS : 0xCBu);
    VITREO_TRACE("[file_ops] ReplaceFileW(%s, %s) -> TRUE\n",
                 replaced.c_str(), replacement.c_str());
    m.set_return(1);
}

// =============================================================================
// CreateSymbolicLinkW
// =============================================================================

void FileOpsManager::impl_CreateSymbolicLinkW(Marshaler& m) {
    // BOOLEAN CreateSymbolicLinkW(LPCWSTR lpSymlinkFileName,
    //   LPCWSTR lpTargetFileName, DWORD dwFlags)
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    // Symlinks are not supported in the VFS — return FALSE with ERROR_PRIVILEGE_NOT_HELD
    // (the real API requires SeCreateSymbolicLinkPrivilege).
    set_last_error(1314u); // ERROR_PRIVILEGE_NOT_HELD
    VITREO_TRACE("[file_ops] CreateSymbolicLinkW stub -> FALSE\n");
    m.set_return(0);
}

// =============================================================================
// create_file_ops_thunks
// =============================================================================

DllExports create_file_ops_thunks(FileOpsManager& mgr) {
    DllExports exports;
    exports.entries.reserve(11);

    // DeleteFileW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "DeleteFileW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeleteFileW(m); },
        4
    });

    // DeleteFileA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "DeleteFileA", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeleteFileA(m); },
        4
    });

    // CreateDirectoryW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "CreateDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateDirectoryW(m); },
        8
    });

    // GetSystemDirectoryW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetSystemDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemDirectoryW(m); },
        8
    });

    // GetVersion (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetVersion", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetVersion(m); },
        0
    });

    // LoadLibraryExW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "LoadLibraryExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadLibraryExW(m); },
        12
    });

    // GetStartupInfoA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetStartupInfoA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetStartupInfoA(m); },
        4
    });

    // SetFileAttributesW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SetFileAttributesW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileAttributesW(m); },
        8
    });

    // SetFileAttributesA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SetFileAttributesA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileAttributesA(m); },
        8
    });

    // SetFileTime (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "SetFileTime", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_SetFileTime(m); },
        16
    });

    // lstrcatW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcatW", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcatW(m); },
        8
    });

    // lstrcpyW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcpyW", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcpyW(m); },
        8
    });

    // MoveFileExW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "MoveFileExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_MoveFileExW(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "MoveFileExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_MoveFileExA(m); },
        12
    });

    // GetFileInformationByHandleEx (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetFileInformationByHandleEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetFileInformationByHandleEx(m); },
        16
    });

    // SetFileInformationByHandle (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "SetFileInformationByHandle", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileInformationByHandle(m); },
        16
    });

    // CopyFileExA / CopyFileExW / CopyFile2 live in kernel32_ext (the shared
    // copy engine needs the FileManager and the guest-callback bridge).

    // ReplaceFileW (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "ReplaceFileW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ReplaceFileW(m); },
        24
    });

    // GetFileInformationByName (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetFileInformationByName", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetFileInformationByName(m); },
        16
    });

    // CreateSymbolicLinkW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "CreateSymbolicLinkW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateSymbolicLinkW(m); },
        12
    });

    // MoveFileTransactedW (7 args, stdcall_bytes=28)
    // BOOL MoveFileTransactedW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName,
    //   LPPROGRESS_ROUTINE lpProgressRoutine, LPVOID lpData, DWORD dwFlags, HANDLE hTransaction)
    // Ignoring the transaction handle is the natural fallback on filesystems
    // that don't support TxF. An updater uses this API to atomically commit
    // staged files (foo_ -> foo).
    exports.entries.push_back(ExportEntry{
        "MoveFileTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_MoveFileTransactedW(m); },
        24  // six parameters (winbase.h); 28 unbalanced every x86 call
    });

    // DeleteFileTransactedW (2 args, stdcall_bytes=8)
    // BOOL DeleteFileTransactedW(LPCWSTR lpFileName, HANDLE hTransaction)
    exports.entries.push_back(ExportEntry{
        "DeleteFileTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeleteFileTransactedW(m); },
        8
    });

    // SetFileAttributesTransactedW (3 args, stdcall_bytes=12)
    // BOOL SetFileAttributesTransactedW(LPCWSTR, DWORD, HANDLE hTransaction)
    // Prefix-compatible with SetFileAttributesW; transaction handle ignored.
    exports.entries.push_back(ExportEntry{
        "SetFileAttributesTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileAttributesW(m); },
        12
    });

    // CreateDirectoryTransactedW (4 args, stdcall_bytes=16)
    // BOOL CreateDirectoryTransactedW(LPCWSTR lpTemplateDirectory,
    //   LPCWSTR lpNewDirectory, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    //   HANDLE hTransaction)
    // Args are NOT prefix-compatible with CreateDirectoryW — the new-dir path
    // is at arg index 1 here, so we have a dedicated member impl.
    exports.entries.push_back(ExportEntry{
        "CreateDirectoryExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateDirectoryExW(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CreateDirectoryTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateDirectoryTransactedW(m); },
        16
    });

    return exports;
}

} // namespace vitreo::winapi
