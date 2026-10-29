// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_lock.cpp
/// @brief LockFile / UnlockFile — exclusive byte-range locks on a file object.
///
/// Learn (LockFile / UnlockFile) + files_core_probe.expected. Exclusive locks
/// cannot overlap any existing lock (including one this handle already holds);
/// UnlockFile requires an exact match on this handle. I/O into a locked range
/// fails ERROR_LOCK_VIOLATION (enforced in HostFileSystem::read/write via
/// range_io_allowed).

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"

#include <vitreo/diagnostics.hpp>

namespace vitreo::winapi {
namespace {

[[nodiscard]] u64 make_offset(u32 low, u32 high) noexcept {
    return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
}

[[nodiscard]] u64 make_length(u32 low, u32 high) noexcept {
    return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
}

bool lock_file_common(Kernel32ExtManager& mgr, Handle handle, u64 offset,
                      u64 length, bool exclusive) {
    if (handle == 0 || mgr.file_manager() == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    const capsule::FileHandle vfs =
        mgr.file_manager()->vfs_handle_for(handle);
    if (vfs == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    auto r = mgr.vfs().lock_range(vfs, offset, length, exclusive);
    if (r.is_err()) {
        set_last_error(vfs_error_to_win32(r.error()));
        return false;
    }
    return true;
}

bool unlock_file_common(Kernel32ExtManager& mgr, Handle handle, u64 offset,
                        u64 length) {
    if (handle == 0 || mgr.file_manager() == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    const capsule::FileHandle vfs =
        mgr.file_manager()->vfs_handle_for(handle);
    if (vfs == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    auto r = mgr.vfs().unlock_range(vfs, offset, length);
    if (r.is_err()) {
        set_last_error(vfs_error_to_win32(r.error()));
        return false;
    }
    return true;
}

}  // namespace

void Kernel32ExtManager::impl_LockFile(Marshaler& m) {
    // BOOL LockFile(HANDLE hFile, DWORD dwFileOffsetLow, DWORD dwFileOffsetHigh,
    //               DWORD nNumberOfBytesToLockLow, DWORD nNumberOfBytesToLockHigh)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u64 offset = make_offset(static_cast<u32>(m.arg(1)),
                                   static_cast<u32>(m.arg(2)));
    // Args 3/4 are DWORD; on x64 stack args use the low dword only.
    const u64 length = make_length(m.arg32(3), m.arg32(4));
    const bool ok = lock_file_common(*this, handle, offset, length,
                                     /*exclusive=*/true);
    VITREO_TRACE("[k32ext] LockFile(h=0x%x off=%llu len=%llu) -> %s\n",
                 static_cast<unsigned>(handle),
                 static_cast<unsigned long long>(offset),
                 static_cast<unsigned long long>(length),
                 ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

void Kernel32ExtManager::impl_UnlockFile(Marshaler& m) {
    // BOOL UnlockFile(HANDLE hFile, DWORD dwFileOffsetLow, DWORD dwFileOffsetHigh,
    //                 DWORD nNumberOfBytesToUnlockLow,
    //                 DWORD nNumberOfBytesToUnlockHigh)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u64 offset = make_offset(static_cast<u32>(m.arg(1)),
                                   static_cast<u32>(m.arg(2)));
    const u64 length = make_length(m.arg32(3), m.arg32(4));
    const bool ok = unlock_file_common(*this, handle, offset, length);
    VITREO_TRACE("[k32ext] UnlockFile(h=0x%x off=%llu len=%llu) -> %s\n",
                 static_cast<unsigned>(handle),
                 static_cast<unsigned long long>(offset),
                 static_cast<unsigned long long>(length),
                 ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

} // namespace vitreo::winapi
