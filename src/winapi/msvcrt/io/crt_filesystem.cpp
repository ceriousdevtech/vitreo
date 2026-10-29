// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
/// @file crt_filesystem.cpp
/// @brief UCRT filesystem API set (api-ms-win-crt-filesystem-l1-1-0)
///
/// These are the path-based CRT functions. Every one of them resolves through
/// the capsule VFS rather than the host filesystem, so guest paths stay guest
/// paths and nothing here can escape the capsule. The Windows-visible contract
/// is the POSIX-shaped one the CRT publishes: 0/-1 returns with `errno` set,
/// not Win32 error codes.

#include "winapi/msvcrt/io/crt_filesystem.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"

#include <string>
#include <array>
#include <algorithm>
#include <cctype>
#include <limits>

namespace vitreo::winapi {

namespace {

constexpr u32 EPERM_VALUE  = 1;
constexpr u32 ENOENT_VALUE = 2;
constexpr u32 EACCES_VALUE = 13;
constexpr u32 EEXIST_VALUE = 17;
constexpr u32 ENOMEM_VALUE = 12;
constexpr u32 EINVAL_VALUE = 22;
constexpr u32 ERANGE_VALUE = 34;
constexpr u32 ENOTEMPTY_VALUE = 41;

/// Map a VFS error onto the CRT `errno` value Windows would report.
u32 errno_for(capsule::VfsError error) {
    using capsule::VfsError;
    switch (error) {
        case VfsError::FileNotFound:
        case VfsError::PathNotFound:
        case VfsError::DriveNotMounted:
            return ENOENT_VALUE;
        case VfsError::AccessDenied:
        case VfsError::SharingViolation:
            return EACCES_VALUE;
        case VfsError::PathAlreadyExists:
            return EEXIST_VALUE;
        case VfsError::DirectoryNotEmpty:
            return ENOTEMPTY_VALUE;
        case VfsError::NotADirectory:
        case VfsError::NotAFile:
            return EINVAL_VALUE;
        default:
            return EPERM_VALUE;
    }
}

/// The `_stat` family's mode bits.
constexpr u16 S_IFDIR_VALUE = 0x4000;
constexpr u16 S_IFREG_VALUE = 0x8000;
constexpr u16 S_IREAD_VALUE = 0x0100;
constexpr u16 S_IWRITE_VALUE = 0x0080;
constexpr u16 S_IEXEC_VALUE = 0x0040;

/// Convert a Windows FILETIME (100 ns units since 1601) to a `__time64_t`.
i64 filetime_to_time64(u64 filetime) {
    constexpr u64 TICKS_PER_SECOND = 10000000ULL;
    constexpr u64 EPOCH_DIFFERENCE = 11644473600ULL;  // 1601 -> 1970, seconds
    if (filetime == 0) return 0;
    const u64 seconds = filetime / TICKS_PER_SECOND;
    if (seconds < EPOCH_DIFFERENCE) return -1;
    return static_cast<i64>(seconds - EPOCH_DIFFERENCE);
}

u16 mode_bits_for(const capsule::FileInfo& info, bool executable = false) {
    const auto attributes = static_cast<u32>(info.attributes);
    const bool is_directory =
        (attributes & static_cast<u32>(capsule::FileAttributes::Directory)) != 0;
    const bool read_only =
        (attributes & static_cast<u32>(capsule::FileAttributes::ReadOnly)) != 0;

    u16 mode = is_directory ? S_IFDIR_VALUE : S_IFREG_VALUE;
    mode = static_cast<u16>(mode | S_IREAD_VALUE);
    if (!read_only) mode = static_cast<u16>(mode | S_IWRITE_VALUE);
    if (is_directory || executable) mode = static_cast<u16>(mode | S_IEXEC_VALUE);
    // The CRT mirrors the owner bits into the group and other triads.
    mode = static_cast<u16>(mode | ((mode & 0x01C0) >> 3) |
                            ((mode & 0x01C0) >> 6));
    return mode;
}

} // namespace

/// Write a `struct _stat64` (or `_stat64i32`) into guest memory.
///
/// Shared MSVC CRT layout through `st_rdev` (x86/x64; explicit field sizes):
///   +0x00 _dev_t         st_dev    (u32)
///   +0x04 _ino_t         st_ino    (u16)
///   +0x06 unsigned short st_mode   (u16)
///   +0x08 short          st_nlink  (i16)
///   +0x0A short          st_uid    (i16)
///   +0x0C short          st_gid    (i16)
///   +0x0E (2 bytes pad to align st_rdev)
///   +0x10 _dev_t         st_rdev   (u32)
///
/// `_stat64` then places `__int64 st_size` at +0x18 (with pad at +0x14) and
/// three `__time64_t` stamps at +0x20/+0x28/+0x30 (56 bytes total).
///
/// `_stat64i32` places `_off_t st_size` at +0x14 and the same three stamps at
/// +0x18/+0x20/+0x28 (48 bytes). Writing the 56-byte `_stat64` layout into a
/// 48-byte `_stat64i32` stack slot overflows the caller's frame.
bool write_crt_stat(Marshaler& m, GuestAddr out,
                    const capsule::FileInfo& info, bool size_is_32bit,
                    bool time_is_32bit, u32 drive_number, bool executable,
                    bool& time_overflow) {
    if (out == 0) return false;

    const usize total_size = time_is_32bit ? (size_is_32bit ? 36u : 48u) : (size_is_32bit ? 48u : 56u);
    const std::array<Byte, 56> zero{};
    if (m.memory().write_bytes(out, zero.data(), total_size).is_err()) return false;
    time_overflow = false;
    auto put_u32 = [&](usize offset, u32 value) {
        return m.write_u32(out + static_cast<GuestAddr>(offset), value).is_ok();
    };
    auto put_u16 = [&](usize offset, u16 value) {
        return m.write_u16(out + static_cast<GuestAddr>(offset), value).is_ok();
    };
    auto put_u64 = [&](usize offset, u64 value) {
        return m.write_u64(out + static_cast<GuestAddr>(offset), value).is_ok();
    };

    bool ok = true;
    ok = put_u32(0x00, drive_number) && ok;
    ok = put_u16(0x04, 0) && ok;
    ok = put_u16(0x06, mode_bits_for(info, executable)) && ok;
    ok = put_u16(0x08, 1) && ok;
    ok = put_u16(0x0A, 0) && ok;  // st_uid
    ok = put_u16(0x0C, 0) && ok;  // st_gid
    ok = put_u16(0x0E, 0) && ok;  // padding before st_rdev
    ok = put_u32(0x10, drive_number) && ok;

    usize time_base = 0x20;
    if (size_is_32bit) {
        // `_stat64i32`: 32-bit size immediately after st_rdev; times follow.
        ok = put_u32(0x14, static_cast<u32>(info.size)) && ok;
        time_base = 0x18;
    } else {
        ok = put_u32(0x14, 0) && ok;  // pad to 8-align __int64 st_size
        ok = put_u64(0x18, info.size) && ok;
        time_base = 0x20;
    }
    const u64 times[] = {info.last_access_time, info.last_write_time, info.creation_time};
    for (usize i = 0; i < 3; ++i) {
        const i64 value = filetime_to_time64(times[i]);
        if (time_is_32bit) {
            const bool overflow = value < 0 || value > std::numeric_limits<i32>::max();
            time_overflow |= overflow;
            ok = put_u32(time_base + i * 4, overflow ? ~u32{0} : static_cast<u32>(value)) && ok;
        } else {
            time_overflow |= value < 0;
            ok = put_u64(time_base + i * 8, static_cast<u64>(value)) && ok;
        }
    }
    return ok;
}

bool write_stat64(Marshaler& m, GuestAddr out,
                  const capsule::FileInfo& info, bool size_is_32bit) {
    bool overflow = false;
    return write_crt_stat(m, out, info, size_is_32bit, false,
                         static_cast<u32>(info.volume_id), false, overflow);
}

DllExports create_crt_filesystem_thunks(capsule::Vfs& vfs,
                                        CrtMemoryManager& memory_manager) {
    DllExports exports;
    exports.entries.reserve(16);

    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // Read a path argument in either encoding.
    auto read_path = [](Marshaler& m, usize index, bool wide)
        -> VResult<std::string> {
        const GuestAddr ptr = m.arg_ptr(index);
        if (ptr == 0) return VResult<std::string>::err(Error::InvalidArgument);
        return wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
    };

    // ---- existence and permission checks ------------------------------------

    // int _waccess(const wchar_t* path, int mode) / _access(const char*, int)
    //
    // mode: 0 = existence, 2 = write, 4 = read, 6 = read+write.
    auto access_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        const auto mode = static_cast<i32>(m.arg(1));
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto info = vfs.stat(path.value());
        if (info.is_err()) {
            set_errno(m, errno_for(info.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // Learn _access: directories always report read+write access when they
        // exist; only the existence check matters.
        const bool is_dir =
            (static_cast<u32>(info.value().attributes) &
             static_cast<u32>(capsule::FileAttributes::Directory)) != 0;
        if ((mode & ~0x6) != 0) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (!is_dir) {
            const bool read_only =
                (static_cast<u32>(info.value().attributes) &
                 static_cast<u32>(capsule::FileAttributes::ReadOnly)) != 0;
            if ((mode & 2) != 0 && read_only) {
                set_errno(m, EACCES_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
        }
        m.set_return(0);
    };
    exports.entries.push_back({"_waccess", 0,
        [access_fn](Marshaler& m) { access_fn(m, true); }});
    exports.entries.push_back({"_access", 0,
        [access_fn](Marshaler& m) { access_fn(m, false); }});

    // errno_t _waccess_s(const wchar_t*, int) — same check, errno_t result
    auto access_s_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        const auto mode = static_cast<i32>(m.arg(1));
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        auto info = vfs.stat(path.value());
        if (info.is_err()) {
            const u32 error = errno_for(info.error());
            set_errno(m, error);
            m.set_return(error);
            return;
        }
        const bool is_dir =
            (static_cast<u32>(info.value().attributes) &
             static_cast<u32>(capsule::FileAttributes::Directory)) != 0;
        if ((mode & ~0x6) != 0) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        if (!is_dir) {
            const bool read_only =
                (static_cast<u32>(info.value().attributes) &
                 static_cast<u32>(capsule::FileAttributes::ReadOnly)) != 0;
            if ((mode & 2) != 0 && read_only) {
                set_errno(m, EACCES_VALUE);
                m.set_return(EACCES_VALUE);
                return;
            }
        }
        m.set_return(0);
    };
    exports.entries.push_back({"_waccess_s", 0,
        [access_s_fn](Marshaler& m) { access_s_fn(m, true); }});
    exports.entries.push_back({"_access_s", 0,
        [access_s_fn](Marshaler& m) { access_s_fn(m, false); }});

    // ---- stat ---------------------------------------------------------------

    auto stat_fn = [&vfs, &memory_manager, set_errno, read_path](Marshaler& m, bool wide,
                                                bool size_is_32bit, bool time_is_32bit) {
        const GuestAddr out = m.arg_ptr(1);
        const usize bytes = time_is_32bit ? (size_is_32bit ? 36u : 48u) : (size_is_32bit ? 48u : 56u);
        const std::array<Byte, 56> zero{};
        if (out != 0 && m.memory().write_bytes(out, zero.data(), bytes).is_err()) return;
        if (m.arg_ptr(0) == 0 || out == 0) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EINVAL_VALUE);
            m.set_return(~u32{0});
            return;
        }
        auto path = read_path(m, 0, wide);
        if (path.is_err()) return;
        auto info = vfs.stat(path.value());
        if (info.is_err()) {
            set_errno(m, errno_for(info.error()));
            m.set_return(~u32{0});
            return;
        }
        // Measured UCRT EOVERFLOW, including zeroed output, rather than
        // truncating/saturating a 64-bit size into a signed _off_t.
        if (size_is_32bit && info.value().size > static_cast<u64>(std::numeric_limits<i32>::max())) {
            set_errno(m, 132u);
            m.set_return(~u32{0});
            return;
        }
        auto parsed = capsule::parse_windows_path(path.value());
        const char drive = parsed.drive_letter ? parsed.drive_letter : vfs.default_drive();
        std::string extension;
        const auto dot = path.value().find_last_of('.');
        if (dot != std::string::npos) extension = path.value().substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool executable = extension == ".exe" || extension == ".com" || extension == ".bat" || extension == ".cmd";
        bool time_overflow = false;
        if (!write_crt_stat(m, out, info.value(), size_is_32bit, time_is_32bit,
                            static_cast<u32>(capsule::normalize_drive_letter(drive) - 'A'),
                            executable, time_overflow)) return;
        if (time_overflow) set_errno(m, EINVAL_VALUE);
        m.set_return(0);
    };
    for (bool wide : {false, true}) {
        const std::string prefix = wide ? "_wstat" : "_stat";
        for (const auto& shape : {std::pair{"64", std::pair{false, false}},
                                  std::pair{"64i32", std::pair{true, false}},
                                  std::pair{"32i64", std::pair{false, true}},
                                  std::pair{"32", std::pair{true, true}}}) {
            exports.entries.push_back({prefix + shape.first, 0,
                [stat_fn, wide, shape](Marshaler& m) { stat_fn(m, wide, shape.second.first, shape.second.second); }});
        }
    }

    // ---- delete / rename / directories --------------------------------------

    auto unlink_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // Learn _unlink: directory (or read-only file) → -1 / EACCES.
        auto info = vfs.stat(path.value());
        if (info.is_ok() &&
            capsule::has_attribute(info.value().attributes,
                                   capsule::FileAttributes::Directory)) {
            set_errno(m, EACCES_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto result = vfs.remove(path.value());
        if (result.is_err()) {
            set_errno(m, errno_for(result.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    };
    auto rmdir_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto info = vfs.stat(path.value());
        if (info.is_ok() &&
            !capsule::has_attribute(info.value().attributes,
                                    capsule::FileAttributes::Directory)) {
            // Oracle: _rmdir(file) → -1 / EINVAL (22), not ENOTDIR.
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto result = vfs.remove(path.value());
        if (result.is_err()) {
            set_errno(m, errno_for(result.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    };
    exports.entries.push_back({"_unlink", 0,
        [unlink_fn](Marshaler& m) { unlink_fn(m, false); }});
    exports.entries.push_back({"_wunlink", 0,
        [unlink_fn](Marshaler& m) { unlink_fn(m, true); }});
    // remove() on Windows CRT can delete files; directories still EACCES.
    exports.entries.push_back({"remove", 0,
        [unlink_fn](Marshaler& m) { unlink_fn(m, false); }});
    exports.entries.push_back({"_wremove", 0,
        [unlink_fn](Marshaler& m) { unlink_fn(m, true); }});
    exports.entries.push_back({"_rmdir", 0,
        [rmdir_fn](Marshaler& m) { rmdir_fn(m, false); }});
    exports.entries.push_back({"_wrmdir", 0,
        [rmdir_fn](Marshaler& m) { rmdir_fn(m, true); }});

    auto rename_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto from = read_path(m, 0, wide);
        auto to   = read_path(m, 1, wide);
        if (from.is_err() || to.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // HostFileSystem::rename refuses an existing distinct destination
        // (PathAlreadyExists → EEXIST=17, measured crt_rename_probe) and
        // allows same-inode case-only renames; directory moves to another
        // parent succeed under UCRT (same probe).
        auto result = vfs.rename(from.value(), to.value());
        if (result.is_err()) {
            set_errno(m, errno_for(result.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    };
    exports.entries.push_back({"rename", 0,
        [rename_fn](Marshaler& m) { rename_fn(m, false); }});
    exports.entries.push_back({"_wrename", 0,
        [rename_fn](Marshaler& m) { rename_fn(m, true); }});

    auto mkdir_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto result = vfs.create_dir(path.value());
        if (result.is_err()) {
            set_errno(m, errno_for(result.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    };
    exports.entries.push_back({"_mkdir", 0,
        [mkdir_fn](Marshaler& m) { mkdir_fn(m, false); }});
    exports.entries.push_back({"_wmkdir", 0,
        [mkdir_fn](Marshaler& m) { mkdir_fn(m, true); }});

    // ---- current directory --------------------------------------------------

    // int _wchdir(const wchar_t* path)
    auto chdir_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto info = vfs.stat(path.value());
        if (info.is_err()) {
            set_errno(m, errno_for(info.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if ((static_cast<u32>(info.value().attributes) &
             static_cast<u32>(capsule::FileAttributes::Directory)) == 0) {
            set_errno(m, ENOENT_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        vfs.set_cwd(path.value());
        m.set_return(0);
    };
    exports.entries.push_back({"_chdir", 0,
        [chdir_fn](Marshaler& m) { chdir_fn(m, false); }});
    exports.entries.push_back({"_wchdir", 0,
        [chdir_fn](Marshaler& m) { chdir_fn(m, true); }});

    // wchar_t* _wgetcwd(wchar_t* buffer, int maxlen)
    //
    // A NULL buffer asks the CRT to allocate one; the caller frees it.
    // Learn: path longer than maxlen → NULL + errno=ERANGE; allocation
    // failure with NULL buffer → errno=ENOMEM; maxlen<=0 → EINVAL.
    auto getcwd_fn = [&vfs, &memory_manager, set_errno](Marshaler& m, bool wide) {
        const GuestAddr buffer = m.arg_ptr(0);
        const auto maxlen = static_cast<i32>(m.arg(1));
        const std::string cwd = vfs.cwd();
        const usize needed = cwd.size() + 1;

        GuestAddr destination = buffer;
        if (buffer == 0) {
            const usize bytes = wide ? needed * 2 : needed;
            destination = memory_manager.crt_malloc(bytes);
            if (destination == 0) {
                set_errno(m, ENOMEM_VALUE);
                m.set_return(0);
                return;
            }
        } else if (maxlen <= 0) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0);
            return;
        } else if (static_cast<usize>(maxlen) < needed) {
            set_errno(m, ERANGE_VALUE);
            m.set_return(0);
            return;
        }

        const bool ok = wide ? m.write_wide_string(destination, cwd).is_ok()
                             : m.write_ansi_string(destination, cwd).is_ok();
        m.set_return(ok ? destination : GuestAddr{0});
    };
    exports.entries.push_back({"_getcwd", 0,
        [getcwd_fn](Marshaler& m) { getcwd_fn(m, false); }});
    exports.entries.push_back({"_wgetcwd", 0,
        [getcwd_fn](Marshaler& m) { getcwd_fn(m, true); }});

    // wchar_t* _wgetdcwd(int drive, wchar_t* buffer, int maxlen)
    //
    // Vitreo keeps one current directory per process rather than the per-drive
    // set MS-DOS inherited, so drive 0 ("current drive") is the only value that
    // can be answered; any other drive is reported as unavailable.
    auto getdcwd_fn = [&vfs, &memory_manager, set_errno](Marshaler& m, bool wide) {
        const auto drive = static_cast<i32>(m.arg(0));
        const GuestAddr buffer = m.arg_ptr(1);
        const auto maxlen = static_cast<i32>(m.arg(2));
        const std::string cwd = vfs.cwd();

        if (drive != 0 && !cwd.empty()) {
            const char requested = static_cast<char>('A' + (drive - 1));
            const char current = static_cast<char>(std::toupper(cwd[0]));
            if (requested != current) {
                set_errno(m, ENOENT_VALUE);
                m.set_return(0);
                return;
            }
        }

        const usize needed = cwd.size() + 1;
        GuestAddr destination = buffer;
        if (buffer == 0) {
            destination = memory_manager.crt_malloc(wide ? needed * 2 : needed);
            if (destination == 0) {
                set_errno(m, ENOMEM_VALUE);
                m.set_return(0);
                return;
            }
        } else if (maxlen <= 0) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0);
            return;
        } else if (static_cast<usize>(maxlen) < needed) {
            set_errno(m, ERANGE_VALUE);
            m.set_return(0);
            return;
        }

        const bool ok = wide ? m.write_wide_string(destination, cwd).is_ok()
                             : m.write_ansi_string(destination, cwd).is_ok();
        m.set_return(ok ? destination : GuestAddr{0});
    };
    exports.entries.push_back({"_getdcwd", 0,
        [getdcwd_fn](Marshaler& m) { getdcwd_fn(m, false); }});
    exports.entries.push_back({"_wgetdcwd", 0,
        [getdcwd_fn](Marshaler& m) { getdcwd_fn(m, true); }});

    // int _getdrive(void) — 1-based index of the current drive
    exports.entries.push_back({"_getdrive", 0, [&vfs](Marshaler& m) {
        const std::string cwd = vfs.cwd();
        char letter = vfs.default_drive();
        if (cwd.size() >= 2 && cwd[1] == ':') letter = cwd[0];
        letter = static_cast<char>(std::toupper(letter));
        if (letter < 'A' || letter > 'Z') {
            m.set_return(0);
            return;
        }
        m.set_return(static_cast<u32>(letter - 'A' + 1));
    }});

    // ---- attributes ---------------------------------------------------------

    // int _wchmod(const wchar_t* path, int mode)
    //
    // The CRT's chmod only toggles the read-only attribute: clearing the write
    // bit sets FILE_ATTRIBUTE_READONLY, setting it clears the attribute.
    auto chmod_fn = [&vfs, set_errno, read_path](Marshaler& m, bool wide) {
        auto path = read_path(m, 0, wide);
        if (path.is_err()) {
            set_errno(m, EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto info = vfs.stat(path.value());
        if (info.is_err()) {
            set_errno(m, errno_for(info.error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // The VFS has no attribute setter yet, so the only honest answer for a
        // request that would actually change the attribute is a failure.
        const auto mode = static_cast<i32>(m.arg(1));
        const bool wants_writable = (mode & S_IWRITE_VALUE) != 0;
        const bool is_read_only =
            (static_cast<u32>(info.value().attributes) &
             static_cast<u32>(capsule::FileAttributes::ReadOnly)) != 0;
        if (wants_writable == !is_read_only) {
            m.set_return(0);  // already in the requested state
            return;
        }
        set_errno(m, EACCES_VALUE);
        m.set_return(0xFFFFFFFFu);
    };
    exports.entries.push_back({"_chmod", 0,
        [chmod_fn](Marshaler& m) { chmod_fn(m, false); }});
    exports.entries.push_back({"_wchmod", 0,
        [chmod_fn](Marshaler& m) { chmod_fn(m, true); }});

    // ---- path canonicalisation ----------------------------------------------

    // wchar_t* _wfullpath(wchar_t* buffer, const wchar_t* path, size_t maxlen)
    //
    // Resolves a guest path against the guest current directory and collapses
    // `.` and `..`. Purely textual: the target need not exist.
    auto fullpath_fn = [&vfs, &memory_manager](Marshaler& m, bool wide) {
        const GuestAddr buffer = m.arg_ptr(0);
        const GuestAddr path_ptr = m.arg_ptr(1);
        const auto maxlen = static_cast<usize>(m.arg(2));
        if (path_ptr == 0) {
            m.set_return(0);
            return;
        }
        auto relative = wide ? m.read_wide_string(path_ptr)
                             : m.read_ansi_string(path_ptr);
        if (relative.is_err()) {
            m.set_return(0);
            return;
        }

        std::string absolute = relative.value();
        const bool is_absolute =
            absolute.size() >= 2 && absolute[1] == ':';
        if (!is_absolute) {
            std::string base = vfs.cwd();
            if (!base.empty() && base.back() != '\\') base += '\\';
            if (!absolute.empty() && (absolute[0] == '\\' || absolute[0] == '/')) {
                base = base.substr(0, 2) + "\\";
                absolute.erase(0, 1);
            }
            absolute = base + absolute;
        }

        // Collapse separators and the `.` / `..` components.
        for (char& ch : absolute) {
            if (ch == '/') ch = '\\';
        }
        std::vector<std::string> parts;
        std::string component;
        const std::string prefix = absolute.substr(0, 2);
        for (usize i = 2; i <= absolute.size(); ++i) {
            const char ch = i < absolute.size() ? absolute[i] : '\\';
            if (ch != '\\') { component += ch; continue; }
            if (component.empty() || component == ".") {
                component.clear();
                continue;
            }
            if (component == "..") {
                if (!parts.empty()) parts.pop_back();
                component.clear();
                continue;
            }
            parts.push_back(component);
            component.clear();
        }

        std::string result = prefix + "\\";
        for (usize i = 0; i < parts.size(); ++i) {
            result += parts[i];
            if (i + 1 < parts.size()) result += '\\';
        }

        GuestAddr destination = buffer;
        if (buffer == 0) {
            destination = memory_manager.crt_malloc(
                wide ? (result.size() + 1) * 2 : result.size() + 1);
            if (destination == 0) {
                m.set_return(0);
                return;
            }
        } else if (maxlen != 0 && result.size() + 1 > maxlen) {
            m.set_return(0);
            return;
        }

        const bool ok = wide ? m.write_wide_string(destination, result).is_ok()
                             : m.write_ansi_string(destination, result).is_ok();
        m.set_return(ok ? destination : GuestAddr{0});
    };
    exports.entries.push_back({"_wfullpath", 0,
        [fullpath_fn](Marshaler& m) { fullpath_fn(m, true); }});

    return exports;
}

} // namespace vitreo::winapi
