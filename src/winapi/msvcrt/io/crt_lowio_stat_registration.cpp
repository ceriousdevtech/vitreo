// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_lowio_stat_registration.cpp
/// @brief Descriptor and path stat: _fstat64*, _fstat, _stat.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/io/crt_filesystem.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <cerrno>
#include <unistd.h>

namespace vitreo::winapi::crt_stdio_internal {

/// _fstat64 and _fstat64i32.
void add_crt_lowio_fstat64_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // int _fstat64(int fd, struct _stat64* out)
    auto fstat64_fn = [&vfs, &mgr, set_errno](Marshaler& m, bool size_is_32bit) {
        const auto fd = static_cast<i32>(m.arg(0));
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            // Learn: NULL buffer → EINVAL (IPH may run first).
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto write_std_device = [&](i32 std_fd) {
            // Learn _fstat: `_S_IFCHR` for a device; pipes report `_S_IFIFO`
            // (oracle crt_fstat64_probe on redirected ssh consoles,
            // stdout_pipe_chain_probe for a pipe handed over in STARTUPINFO).
            constexpr u16 kSIfifo = 0x1000;
            constexpr u16 kSIfchr = 0x2000;
            u16 mode = ::isatty(std_fd) ? kSIfchr : kSIfifo;
            const StdDescriptor& desc = std_descriptor(mgr, std_fd);
            if (desc.kind == StdKind::Closed) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            if (desc.kind == StdKind::Console && desc.handle != 0) mode = kSIfchr;
            if (desc.kind == StdKind::Kernel && mgr.file_manager() != nullptr) {
                FileManager& files = *mgr.file_manager();
                const u32 type = files.file_type(desc.handle);
                if (type == 1) {
                    // A disk file: the file's own metadata.
                    auto info = vfs.stat_handle(files.vfs_handle_for(desc.handle));
                    if (info.is_err()) {
                        set_errno(m, EBADF_VALUE);
                        m.set_return(0xFFFFFFFFu);
                        return;
                    }
                    m.set_return(write_stat64(m, out, info.value(), size_is_32bit)
                        ? 0u : 0xFFFFFFFFu);
                    return;
                }
                mode = type == 2 ? kSIfchr : kSIfifo;
            }
            const u32 dev = static_cast<u32>(std_fd);
            const usize total = size_is_32bit ? 48u : 56u;
            const std::array<u8, 56> zero{};
            if (m.memory().write_bytes(out, zero.data(), total).is_err()) {
                m.set_return(0xFFFFFFFFu);
                return;
            }
            (void)m.write_u32(out + 0x00, dev);
            (void)m.write_u16(out + 0x04, 0);       // st_ino
            (void)m.write_u16(out + 0x06, mode);    // st_mode
            (void)m.write_u16(out + 0x08, 1);       // st_nlink
            (void)m.write_u32(out + 0x10, dev);     // st_rdev
            // size and times are not meaningful for devices (left zero).
            m.set_return(0);
        };
        if (fd >= 0 && fd <= 2) {
            write_std_device(fd);
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto info = vfs.stat_handle(it->second.handle);
        if (info.is_err()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(write_stat64(m, out, info.value(), size_is_32bit)
            ? 0u : 0xFFFFFFFFu);
    };
    exports.entries.push_back({"_fstat64", 0,
        [fstat64_fn](Marshaler& m) { fstat64_fn(m, false); }});

    // int _fstat64i32(int fd, struct _stat64i32* out)
    // Same as _fstat64 but with 32-bit st_size (_stat64i32 layout).
    exports.entries.push_back({"_fstat64i32", 0,
        [fstat64_fn](Marshaler& m) { fstat64_fn(m, true); }});
}

/// _fstat and _stat.
void add_crt_lowio_stat_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // ---- _fstat / _chsize ---------------------------------------------------
    //
    // msvcrt.dll's `struct _stat` uses a 32-bit time_t and a `long` st_size:
    //   0  dev  4  ino  6  mode  8  nlink  10 uid  12 gid
    //   16 rdev 20 size 24 atime 28 mtime  32 ctime   (36 bytes)
    const auto write_stat32 = [](Marshaler& m, GuestAddr out,
                                 const capsule::FileInfo& info) {
        constexpr u16 S_IFDIR_VALUE = 0x4000;
        constexpr u16 S_IFREG_VALUE = 0x8000;
        constexpr u16 S_IREAD_VALUE = 0x0100;
        constexpr u16 S_IWRITE_VALUE = 0x0080;
        constexpr u16 S_IEXEC_VALUE = 0x0040;

        const auto attributes = static_cast<u32>(info.attributes);
        const bool is_directory =
            (attributes &
             static_cast<u32>(capsule::FileAttributes::Directory)) != 0;
        const bool read_only =
            (attributes &
             static_cast<u32>(capsule::FileAttributes::ReadOnly)) != 0;

        u16 mode = is_directory ? S_IFDIR_VALUE : S_IFREG_VALUE;
        mode = static_cast<u16>(mode | S_IREAD_VALUE);
        if (!read_only) mode = static_cast<u16>(mode | S_IWRITE_VALUE);
        if (is_directory) mode = static_cast<u16>(mode | S_IEXEC_VALUE);
        // The CRT mirrors the owner bits into the group and other triads.
        mode = static_cast<u16>(mode | ((mode & 0x01C0) >> 3) |
                                ((mode & 0x01C0) >> 6));

        const auto to_time32 = [](u64 filetime) -> u32 {
            // FILETIME (100 ns since 1601) -> time_t (seconds since 1970)
            constexpr u64 EPOCH_DELTA_100NS = 116444736000000000ULL;
            if (filetime < EPOCH_DELTA_100NS) return 0;
            return static_cast<u32>((filetime - EPOCH_DELTA_100NS) / 10000000ULL);
        };

        bool ok = m.write_u32(out + 0, 0).is_ok();
        ok = m.write_u16(out + 4, 0).is_ok() && ok;
        ok = m.write_u16(out + 6, mode).is_ok() && ok;
        ok = m.write_u16(out + 8, 1).is_ok() && ok;
        ok = m.write_u16(out + 10, 0).is_ok() && ok;
        ok = m.write_u16(out + 12, 0).is_ok() && ok;
        ok = m.write_u16(out + 14, 0).is_ok() && ok;  // padding
        ok = m.write_u32(out + 16, 0).is_ok() && ok;
        ok = m.write_u32(out + 20, static_cast<u32>(
                 info.size > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : info.size))
                 .is_ok() && ok;
        ok = m.write_u32(out + 24, to_time32(info.last_access_time)).is_ok() && ok;
        ok = m.write_u32(out + 28, to_time32(info.last_write_time)).is_ok() && ok;
        ok = m.write_u32(out + 32, to_time32(info.creation_time)).is_ok() && ok;
        return ok;
    };

    // int _fstat(int fd, struct _stat* buffer)
    exports.entries.push_back({"_fstat", 0,
        [&vfs, &mgr, set_errno, write_stat32](Marshaler& m) {
            const auto fd = static_cast<i32>(m.arg(0));
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            if (fd >= 0 && fd <= 2 &&
                std_descriptor(mgr, fd).kind != StdKind::None) {
                // Same rule as _fstat64: a disk file reports its metadata, a
                // pipe _S_IFIFO, a character device or console _S_IFCHR.
                constexpr u16 kSIfifo = 0x1000;
                constexpr u16 kSIfchr = 0x2000;
                const StdDescriptor& desc = std_descriptor(mgr, fd);
                u16 mode = ::isatty(fd) ? kSIfchr : kSIfifo;
                if (desc.kind == StdKind::Closed) {
                    set_errno(m, EBADF_VALUE);
                    m.set_return(0xFFFFFFFFu);
                    return;
                }
                if (desc.kind == StdKind::Console && desc.handle != 0) mode = kSIfchr;
                if (desc.kind == StdKind::Kernel && mgr.file_manager() != nullptr) {
                    FileManager& files = *mgr.file_manager();
                    const u32 type = files.file_type(desc.handle);
                    if (type == 1) {
                        auto info = vfs.stat_handle(files.vfs_handle_for(desc.handle));
                        if (info.is_err() || !write_stat32(m, out, info.value())) {
                            set_errno(m, EBADF_VALUE);
                            m.set_return(0xFFFFFFFFu);
                            return;
                        }
                        m.set_return(0);
                        return;
                    }
                    mode = type == 2 ? kSIfchr : kSIfifo;
                }
                const std::array<u8, 36> zero{};
                if (m.memory().write_bytes(out, zero.data(), zero.size()).is_err()) {
                    set_errno(m, EINVAL_LOWIO);
                    m.set_return(0xFFFFFFFFu);
                    return;
                }
                (void)m.write_u32(out + 0x00, static_cast<u32>(fd));  // st_dev
                (void)m.write_u16(out + 0x06, mode);                  // st_mode
                (void)m.write_u16(out + 0x08, 1);                     // st_nlink
                (void)m.write_u32(out + 0x10, static_cast<u32>(fd));  // st_rdev
                m.set_return(0);
                return;
            }
            auto it = stdio_arena().descriptors.find(fd);
            if (it == stdio_arena().descriptors.end()) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            auto info = vfs.stat_handle(it->second.handle);
            if (info.is_err()) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            if (!write_stat32(m, out, info.value())) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            m.set_return(0);
        }});

    // int _stat(const char* path, struct _stat* buffer)
    exports.entries.push_back({"_stat", 0,
        [&vfs, set_errno, write_stat32](Marshaler& m) {
            auto path = m.read_ansi_string(m.arg_ptr(0));
            const GuestAddr out = m.arg_ptr(1);
            if (path.is_err() || out == 0) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            auto info = vfs.stat(path.value());
            if (info.is_err()) {
                VITREO_TRACE("[crt] _stat(\"%s\") -> -1 (not found)\n",
                             path.value().c_str());
                set_errno(m, ENOENT_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            if (!write_stat32(m, out, info.value())) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            VITREO_TRACE("[crt] _stat(\"%s\") -> 0 attrs=0x%x size=%llu\n",
                         path.value().c_str(),
                         static_cast<unsigned>(info.value().attributes),
                         static_cast<unsigned long long>(info.value().size));
            m.set_return(0);
        }});
}

} // namespace vitreo::winapi::crt_stdio_internal
