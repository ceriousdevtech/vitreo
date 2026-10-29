/// @file share_access.hpp
/// @brief Windows share-access and byte-range-lock arbitration across every
///        guest process, keyed by the host file identity (st_dev, st_ino).
///
/// Learn (CreateFileW dwShareMode): an open whose requested access conflicts
/// with the share mode of an existing open — or whose share mode refuses the
/// access an existing open holds — fails ERROR_SHARING_VIOLATION. Only data
/// access participates (read = FILE_READ_DATA|FILE_EXECUTE, write =
/// FILE_WRITE_DATA|FILE_APPEND_DATA, delete = DELETE); an attribute-only open
/// neither checks nor records sharing (files_core_probe share0_then_attr_only).
///
/// Guest processes are separate host processes, so the state lives in POSIX
/// record locks on a per-file sidecar in the user's temp directory: a process
/// holds a shared lock on byte b of the sidecar while any of its opens has
/// property b; another process's lock on that byte is visible through
/// F_GETLK. Opens inside one process are counted in memory (POSIX record
/// locks never conflict with their own process). A process that dies releases
/// its record locks with it, so a crashed guest never wedges a file.
///
/// Byte-range locks (LockFile/LockFileEx) use the same sidecar: file range
/// [off, off+len) maps to sidecar bytes [kRangeBase + off, ...), exclusive
/// locks as F_WRLCK, shared as F_RDLCK, plus an in-process table because a
/// Windows lock belongs to a HANDLE, not to a process.

#pragma once

#include <vitreo/common.hpp>

#include <optional>

namespace vitreo::capsule {

namespace share_bits {
inline constexpr u32 Read = 1u << 0;
inline constexpr u32 Write = 1u << 1;
inline constexpr u32 Delete = 1u << 2;
inline constexpr u32 All = Read | Write | Delete;
}  // namespace share_bits

/// Opaque registration of one open's share access; 0 = none held.
using ShareToken = u64;

/// Try to record an open of (dev, ino) with @p access (share_bits) and
/// @p share (share_bits the open allows others). Returns the token, 0 when
/// @p access has no data bits (nothing to record), or nullopt on a sharing
/// conflict with any open in any process.
[[nodiscard]] std::optional<ShareToken> share_access_acquire(
    u64 dev, u64 ino, u32 access, u32 share);

/// Check only (DeleteFile / MoveFile source): true when an open with
/// @p access and @p share would be admitted right now.
[[nodiscard]] bool share_access_would_admit(u64 dev, u64 ino, u32 access,
                                            u32 share);

/// Release a token returned by share_access_acquire (0 is ignored).
void share_access_release(ShareToken token);

/// Byte-range locks. @p owner identifies the Windows file object (handle).
enum class RangeLockResult { Ok, Conflict, NotLocked };
[[nodiscard]] RangeLockResult range_lock(u64 dev, u64 ino, u64 owner,
                                         u64 offset, u64 length,
                                         bool exclusive);
[[nodiscard]] RangeLockResult range_unlock(u64 dev, u64 ino, u64 owner,
                                           u64 offset, u64 length);
/// Drop every range @p owner holds (CloseHandle).
void range_unlock_all(u64 owner);
/// I/O admission: may @p owner read (or write) [offset, offset+length)?
/// Learn (ReadFile/WriteFile): fails when the range overlaps a region locked
/// by another handle (exclusive, or any lock for a write) — files_core_probe
/// ReadFile_h2_locked_region / ReadFile_h1_other_lock.
[[nodiscard]] bool range_io_allowed(u64 dev, u64 ino, u64 owner, u64 offset,
                                    u64 length, bool write);

}  // namespace vitreo::capsule
