// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_lowio_transfer_registration.cpp
/// @brief Low-level transfer: _read, _write, _lseek*, _filelengthi64, _chsize.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/io/crt_stdio_text_read.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/guest_memory.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <span>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

/// _read, _write, _lseeki64, _lseek and _filelengthi64.
void add_crt_lowio_transfer_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // int _read(int fd, void* buffer, unsigned count)
    // Text mode (crt_read_text_probe): collapse CRLF→LF across reads via
    // pending_cr; Ctrl+Z (0x1A) is EOF; NULL buffer / count>INT_MAX →
    // EINVAL (-1).
    exports.entries.push_back({"_read", 0, [&vfs, &mgr, set_errno, &memory_manager](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        const GuestAddr buffer = m.arg_ptr(1);
        const auto count = static_cast<u32>(m.arg(2));
        constexpr u32 INT_MAX_U = 0x7FFFFFFFu;
        // crt_read_validation_probe: invalid descriptors win over NULL/count
        // validation, including a zero-length read. The special no-console
        // descriptor (-2) reports EBADF without the invalid-parameter handler.
        if (fd == -2) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const bool standard = fd >= 0 && fd <= 2;
        if (fd < 0 || (standard
                ? std_descriptor(mgr, fd).kind == StdKind::Closed
                : stdio_arena().descriptors.find(fd) == stdio_arena().descriptors.end())) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (count == 0) { m.set_return(0); return; }
        if (buffer == 0 || count > INT_MAX_U) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const i32 mode = standard ? std_descriptor(mgr, fd).translation_mode
                                  : stdio_arena().descriptors.at(fd).translation_mode;
        const bool unicode = mode != kStdModeBinary && mode != kStdModeText;
        if (unicode && (count & 1u) != 0) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (unicode && ((standard && std_descriptor(mgr, fd).kind == StdKind::Console)
                        || mode == kStdModeU8Text)) {
            static std::once_flag logged_console, logged_utf8;
            std::call_once(mode == kStdModeU8Text ? logged_utf8 : logged_console, [&] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ucrtbase!_read: %s\n",
                    mode == kStdModeU8Text ? "UTF-8 input translation"
                                          : "Unicode console input");
            });
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }

        if (fd >= 0 && fd <= 2 &&
            std_descriptor(mgr, fd).kind != StdKind::Console) {
            StdDescriptor& desc = std_descriptor(mgr, fd);
            if (desc.kind == StdKind::None) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            // A pipe read blocks until the writer produces something. Windows
            // holds only this descriptor's lock meanwhile, so the CRT's
            // process-wide state lock is released around it: another thread
            // writing stdout must not wait for this thread's input.
            std::vector<u8> host(count);
            i64 got = -1;
            {
                StdioStateSuspension unlocked;
                got = std_read(desc, host.data(), count);
            }
            if (got < 0) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            if (got > 0 &&
                m.memory().write_bytes(buffer, reinterpret_cast<const Byte*>(host.data()),
                                       static_cast<usize>(got)).is_err()) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            m.set_return(static_cast<u64>(got));
            return;
        }

        if (fd == 0) {  // stdin
            std::vector<u8> host(count);
            const auto got = stdio_host_call([&] { return ::read(STDIN_FILENO, host.data(), count); });
            if (got <= 0) { m.set_return(got == 0 ? 0u : 0xFFFFFFFFu); return; }
            for (isize i = 0; i < got; ++i) {
                (void)m.write_u8(buffer + static_cast<GuestAddr>(i), host[
                    static_cast<usize>(i)]);
            }
            m.set_return(static_cast<u64>(got));
            return;
        }

        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        LowIoFd& entry = it->second;
        if (entry.text_mode) {
            FileManager* files = mgr.file_manager();
            const bool seekable = !entry.kernel_io ||
                (files != nullptr && files->file_type(entry.os_handle) == 1);
            // Kernel ReadFile marshals through guest storage. File descriptors
            // backed by the VFS can fill the host translation buffer directly.
            const GuestAddr scratch = entry.kernel_io ? memory_manager.crt_malloc(count) : 0;
            if (entry.kernel_io && (files == nullptr || scratch == 0)) {
                if (scratch != 0) memory_manager.crt_free(scratch);
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            std::vector<u8> host(count);
            const i64 got = read_translated_text(host.data(), count, entry.text_eof,
                entry.unicode_lookahead, seekable, unicode,
                [&](u8* bytes, u32 size) -> i64 {
                    if (!entry.kernel_io) {
                        const auto result = vfs.read(entry.handle, std::span<u8>(bytes, size));
                        return result.is_ok() ? static_cast<i64>(result.value()) : -1;
                    }
                    u32 n = 0;
                    const bool read_ok = entry.aliased_file == 0
                        ? stdio_host_call([&] { return files->read_file(entry.os_handle, scratch, size, 0, &n); })
                        : files->read_file(entry.os_handle, scratch, size, 0, &n);
                    if (!read_ok)
                        return get_last_error() == error_codes::ERROR_BROKEN_PIPE ? 0 : -1;
                    return m.memory().read_bytes(scratch, bytes, n).is_ok()
                        ? static_cast<i64>(n) : i64{-1};
                },
                [&](i64 size) {
                    return entry.kernel_io
                        ? files->set_file_pointer64(entry.os_handle, -size, 1).has_value()
                        : vfs.seek(entry.handle, -size, capsule::SeekOrigin::Current).is_ok();
                });
            if (scratch != 0) memory_manager.crt_free(scratch);
            if (got < 0 || (got > 0 &&
                    m.memory().write_bytes(buffer, host.data(), static_cast<usize>(got)).is_err())) {
                set_errno(m, got < 0 ? EBADF_VALUE : EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            m.set_return(static_cast<u64>(got));
            return;
        }
        if (entry.kernel_io) {
            FileManager* files = mgr.file_manager();
            u32 got = 0;
            const bool read_ok = files != nullptr && (entry.aliased_file == 0
                ? stdio_host_call([&] { return files->read_file(entry.os_handle, buffer, count, 0, &got); })
                : files->read_file(entry.os_handle, buffer, count, 0, &got));
            if (!read_ok) {
                if (files != nullptr && get_last_error() == error_codes::ERROR_BROKEN_PIPE) {
                    m.set_return(0); return;
                }
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            m.set_return(got);
            return;
        }
        std::vector<u8> host(count);
        const auto result = vfs.read(entry.handle, std::span<u8>(host.data(), count));
        if (result.is_err()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (result.value() != 0 &&
            m.memory().write_bytes(buffer, host.data(), result.value()).is_err()) {
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(result.value());
    }});

    // int _write(int fd, const void* buffer, unsigned count)
    // Learn: text mode expands LF→CRLF; return is guest bytes consumed.
    // _O_APPEND seeks to EOF before every write.
    exports.entries.push_back({"_write", 0, [&vfs, &mgr, set_errno](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        const GuestAddr buffer = m.arg_ptr(1);
        const auto count = static_cast<u32>(m.arg(2));
        if (count == 0) { m.set_return(0); return; }

        std::vector<u8> host(count);
        for (u32 i = 0; i < count; ++i) {
            host[i] = m.read_u8(buffer + i).value_or(0);
        }

        if (fd >= 0 && fd <= 2 &&
            std_descriptor(mgr, fd).kind != StdKind::Console) {
            StdDescriptor& desc = std_descriptor(mgr, fd);
            if (desc.kind == StdKind::None) {
                // No standard handle: Windows fails EBADF (oracle bare mode).
                // The bytes still reach the host log for diagnosis.
                if (fd != 0) {
                    (void)::write(fd == 2 ? STDERR_FILENO : STDOUT_FILENO,
                                  host.data(), count);
                }
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            const auto encoded = lowio_encode(
                desc.translation_mode, std::span<const u8>(host.data(), count));
            if (!encoded) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            FileManager* files = mgr.file_manager();
            if (files == nullptr ||
                !stdio_host_call([&] { return files->write_handle_bytes(
                    desc.handle,
                    std::string_view(reinterpret_cast<const char*>(encoded->data()),
                                     encoded->size())); })) {
                set_errno(m, get_last_error() == error_codes::ERROR_BROKEN_PIPE
                                 ? EPIPE_LOWIO : EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            // The count of the caller's bytes, as for text-mode files.
            m.set_return(count);
            return;
        }
        if (fd == 1 || fd == 2) {
            const int host_fd = fd == 2 ? STDERR_FILENO : STDOUT_FILENO;
            const auto written = ::write(host_fd, host.data(), count);
            m.set_return(static_cast<u64>(written));
            return;
        }

        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (it->second.kernel_io) {
            // A pipe or device from _open_osfhandle: WriteFile on the handle.
            FileManager* files = mgr.file_manager();
            if (files == nullptr || it->second.text_mode) {
                if (it->second.text_mode) {
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        std::fprintf(stderr,
                            "[vitreo] UNIMPLEMENTED ucrtbase!_write: text-mode "
                            "translation on a descriptor over a non-file "
                            "HANDLE\n");
                    }
                }
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            u32 written = 0;
            const bool write_ok = it->second.aliased_file == 0
                ? stdio_host_call([&] { return files->write_file(it->second.os_handle, buffer, count, 0, &written); })
                : files->write_file(it->second.os_handle, buffer, count, 0, &written);
            if (!write_ok) {
                set_errno(m, get_last_error() == error_codes::ERROR_BROKEN_PIPE
                                 ? EPIPE_LOWIO : EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            m.set_return(written);
            return;
        }
        if (it->second.append) {
            (void)vfs.seek(it->second.handle, 0, capsule::SeekOrigin::End);
        }
        std::span<const u8> out_span(host.data(), count);
        std::vector<u8> translated;
        if (it->second.text_mode) {
            translated.reserve(count + count / 8 + 8);
            for (u8 b : host) {
                if (b == '\n') translated.push_back('\r');
                translated.push_back(b);
            }
            out_span = std::span<const u8>(translated);
        }
        auto result = vfs.write(it->second.handle, out_span);
        if (result.is_err()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // Return guest bytes requested (count), matching UCRT text-mode
        // write (probe write_text w=3 for "a\nb").
        m.set_return(static_cast<u64>(count));
    }});

    // __int64 _lseeki64(int fd, __int64 offset, int origin)
    //
    // The offset is 64-bit, so on x86 it occupies two argument slots and the
    // result comes back in EDX:EAX.
    exports.entries.push_back({"_lseeki64", 0, [&vfs, set_errno, &memory_manager](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        const bool is_64bit = m.cpu_is_64bit();
        const i64 offset = is_64bit
            ? static_cast<i64>(m.arg(1))
            : static_cast<i64>((m.arg(1) & 0xFFFFFFFFULL) |
                               ((m.arg(2) & 0xFFFFFFFFULL) << 32));
        const auto origin = static_cast<i32>(m.arg(is_64bit ? 2 : 3));
        if (origin < 0 || origin > 2) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EINVAL_LOWIO);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }

        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        capsule::SeekOrigin seek_origin = capsule::SeekOrigin::Begin;
        if (origin == 1) seek_origin = capsule::SeekOrigin::Current;
        else if (origin == 2) seek_origin = capsule::SeekOrigin::End;

        auto result = vfs.seek(it->second.handle, offset, seek_origin);
        if (result.is_err()) {
            set_errno(m, EINVAL_LOWIO);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        m.set_return_i64(result.value());
    }});

    // long _lseek(int fd, long offset, int origin)
    exports.entries.push_back({"_lseek", 0, [&vfs, set_errno, &memory_manager](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        const auto offset = static_cast<i64>(static_cast<i32>(m.arg(1)));
        const auto origin = static_cast<i32>(m.arg(2));
        if (origin < 0 || origin > 2) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        capsule::SeekOrigin seek_origin = capsule::SeekOrigin::Begin;
        if (origin == 1) seek_origin = capsule::SeekOrigin::Current;
        else if (origin == 2) seek_origin = capsule::SeekOrigin::End;
        auto result = vfs.seek(it->second.handle, offset, seek_origin);
        m.set_return(result.is_ok() ? static_cast<u32>(result.value())
                                    : 0xFFFFFFFFu);
    }});

    // Learn _filelengthi64: descriptor length without changing its position;
    // invalid descriptors invoke the configured invalid-parameter handler.
    exports.entries.push_back({"_filelengthi64", 0, [&vfs, &memory_manager, set_errno](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        const auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EBADF_VALUE);
            memory_manager.set_doserrno(0);
            m.set_return_i64(~u64{0});
            return;
        }
        const auto info = vfs.stat_handle(it->second.handle);
        if (info.is_err()) {
            set_errno(m, EBADF_VALUE);
            m.set_return_i64(~u64{0});
            return;
        }
        m.set_return_i64(info.value().size);
    }});
}

/// _chsize.
void add_crt_lowio_chsize_exports(DllExports& exports, const StdioContext& ctx) {
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // int _chsize(int fd, long size)
    exports.entries.push_back({"_chsize", 0,
        [&vfs, set_errno](Marshaler& m) {
            const auto fd = static_cast<i32>(m.arg(0));
            const auto size = static_cast<i32>(m.arg(1));
            if (size < 0) {
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            auto it = stdio_arena().descriptors.find(fd);
            if (it == stdio_arena().descriptors.end()) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            auto result = vfs.truncate(it->second.handle,
                                       static_cast<u64>(size));
            if (result.is_err()) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            m.set_return(0);
        }});
}

} // namespace vitreo::winapi::crt_stdio_internal
