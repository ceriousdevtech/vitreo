// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_stream_io_registration.cpp
/// @brief Stream transfer exports: fread, fwrite, fgetc, fputc, fgets, fputs, ...

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"

#include <cerrno>
#include <cstdio>
#include <span>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

/// fread, fwrite, fgetc, putchar, fputc, fgets, puts, fputs and ungetc.
void add_crt_stdio_transfer_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto host_standard_stream = [&mgr, &crt_data](GuestAddr stream) -> FILE* {
        return stdio_host_standard_stream(mgr, crt_data, stream);
    };
    auto vfs_read_translated = [&vfs](VfsFile& vf, std::span<u8> dst) -> usize {
        return stdio_vfs_read_translated(vfs, vf, dst);
    };
    auto vfs_write_translated = [&vfs](VfsFile& vf, std::span<const u8> src) -> usize {
        return stdio_vfs_write_translated(vfs, vf, src);
    };

    exports.entries.push_back(
        {"fread", 4,
         [&mgr, &vfs, &memory_manager, vfs_read_translated,
          host_standard_stream](Marshaler& m) {
            (void)mgr;
            GuestAddr buf_ptr = m.arg_ptr(0);
            usize size  = static_cast<usize>(m.arg(1));
            usize count = static_cast<usize>(m.arg(2));
            u32   fp    = static_cast<u32>(m.arg(3));
            if (size == 0 || count == 0) { m.set_return(0); return; }
            if (buf_ptr == 0 || fp == 0) {
                if (!stdio_host_call([&] { return memory_manager.invalid_parameter(); })) return;
                memory_manager.set_errno(EINVAL);
                m.set_return(0);
                return;
            }
            if (FILE* host_file = host_standard_stream(fp)) {
                const usize total = size * count;
                std::vector<u8> tmp(total);
                const usize got = stdio_host_call([&] { return ::fread(tmp.data(), size, count, host_file); });
                const usize bytes = got * size;
                for (usize i = 0; i < bytes; ++i) {
                    (void)m.write_u8(
                        buf_ptr + static_cast<GuestAddr>(i), tmp[i]);
                }
                m.set_return(got);
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                VITREO_TRACE("[crt] fread(fp=0x%x) -> 0 (unknown stream)\n", fp);
                m.set_return(0);
                return;
            }
            usize total = size * count;
            std::vector<u8> tmp(total);
            usize got = vfs_read_translated(it->second, std::span<u8>(tmp));
            if (got > 0) {
                for (usize i = 0; i < got; ++i)
                    (void)m.write_u8(buf_ptr + static_cast<GuestAddr>(i), tmp[i]);
            }
            VITREO_TRACE("[crt] fread(fp=0x%x size=%zu count=%zu) -> %zu bytes\n",
                         fp, size, count, got);
            m.set_return(static_cast<u64>(size > 0 ? got / size : 0));
        }});
    exports.entries.push_back(
        {"fwrite", 5,
         [&mgr, host_standard_stream, vfs_write_translated,
          &memory_manager](Marshaler& m) {
            (void)mgr;
            GuestAddr buf_ptr = m.arg_ptr(0);
            usize size  = static_cast<usize>(m.arg(1));
            usize count = static_cast<usize>(m.arg(2));
            u32   fp    = static_cast<u32>(m.arg(3));
            if (buf_ptr == 0 || size == 0 || count == 0) { m.set_return(0); return; }
            usize total = size * count;
            std::vector<u8> tmp(total);
            for (usize i = 0; i < total; ++i) {
                auto r = m.read_u8(buf_ptr + static_cast<GuestAddr>(i));
                tmp[i] = r.is_ok() ? r.value() : 0;
            }
            if (FILE* host_file = host_standard_stream(fp)) {
                const usize written =
                    stdio_host_call([&] { return ::fwrite(tmp.data(), size, count, host_file); });
                m.set_return(written);
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) { m.set_return(0); return; }
            if (it->second.host_std_fd >= 0) {
                FILE* hf = std_host_file(mgr, it->second.host_std_fd);
                m.set_return(stdio_host_call([&] { return ::fwrite(tmp.data(), size, count, hf); }));
                return;
            }
            stdio_diag_vfs(m, "fwrite", fp, it->second);
            if (!capsule::has_access(it->second.access,
                                     capsule::AccessMode::Write)) {
                it->second.error_flag = true;
                memory_manager.set_errno(9);  // EBADF
                m.set_return(0);
                return;
            }
            const usize written = vfs_write_translated(
                it->second, std::span<const u8>(tmp));
            m.set_return(static_cast<u64>(size > 0 ? written / size : 0));
        }});
    exports.entries.push_back(
        {"fgetc", 6,
         [&mgr](Marshaler& m) {
            u32 fp = static_cast<u32>(m.arg(0));
            auto it = stdio_arena().files.find(fp);
            if (it != stdio_arena().files.end() &&
                !capsule::has_access(it->second.access,
                                     capsule::AccessMode::Read)) {
                it->second.error_flag = true;
                m.set_return(static_cast<u32>(-1));  // EOF
                return;
            }
            m.set_return(static_cast<u32>(mgr.guest_fgetc(fp)));
        }});
    exports.entries.push_back(
        {"putchar", 0,
         [&mgr](Marshaler& m) {
             const int ch = static_cast<int>(m.arg(0) & 0xFFu);
             stdio_host_call([&] { return std::fputc(ch, std_host_file(mgr, 1)); });
             m.set_return(static_cast<u32>(ch));   // returns the char, not 0/1
         }});
    exports.entries.push_back(
        {"fputc", 7,
         [&mgr, host_standard_stream, vfs_write_translated](Marshaler& m) {
            (void)mgr;
            u8  ch = static_cast<u8>(m.arg(0));
            u32 fp = static_cast<u32>(m.arg(1));
            if (FILE* host_file = host_standard_stream(fp)) {
                m.set_return(static_cast<u32>(stdio_host_call([&] { return ::fputc(ch, host_file); })));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(static_cast<u64>(static_cast<i64>(-1)));
                return;
            }
            if (it->second.host_std_fd >= 0) {
                FILE* hf = std_host_file(mgr, it->second.host_std_fd);
                m.set_return(static_cast<u32>(stdio_host_call([&] { return ::fputc(ch, hf); })));
                return;
            }
            // Text mode: LF → CR-LF (same path as fwrite).
            const usize n = vfs_write_translated(
                it->second, std::span<const u8>(&ch, 1));
            if (n == 0) {
                m.set_return(static_cast<u64>(static_cast<i64>(-1)));
            } else {
                m.set_return(static_cast<u64>(ch));
            }
        }});
    exports.entries.push_back(
        {"fgets", 8,
         [&mgr, &memory_manager, vfs_read_translated, host_standard_stream](Marshaler& m) {
            (void)mgr;
            GuestAddr buf_ptr = m.arg_ptr(0);
            i32 n  = static_cast<i32>(m.arg(1));
            u32 fp = static_cast<u32>(m.arg(2));
            if (buf_ptr == 0 || fp == 0 || n <= 0) {
                if (!stdio_host_call([&] { return memory_manager.invalid_parameter(); })) return;
                memory_manager.set_errno(EINVAL);
                m.set_return(0);
                return;
            }
            if (FILE* host_file = host_standard_stream(fp)) {
                std::vector<char> tmp(static_cast<usize>(n));
                if (stdio_host_call([&] { return ::fgets(tmp.data(), n, host_file); }) == nullptr) {
                    m.set_return(0);
                    return;
                }
                for (i32 i = 0; i < n; ++i) {
                    (void)m.write_u8(buf_ptr + static_cast<GuestAddr>(i),
                                     static_cast<u8>(tmp[static_cast<usize>(i)]));
                    if (tmp[static_cast<usize>(i)] == 0) break;
                }
                m.set_return(static_cast<u64>(buf_ptr));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) { m.set_return(0); return; }
            if (!capsule::has_access(it->second.access,
                                     capsule::AccessMode::Read)) {
                it->second.error_flag = true;
                m.set_return(0);
                return;
            }
            // Read up to n-1 bytes, stopping at newline
            i32 i = 0;
            bool got_any = false;
            while (i < n - 1) {
                u8 ch = 0;
                usize ngot = vfs_read_translated(it->second, std::span<u8>(&ch, 1));
                if (ngot == 0) break;
                got_any = true;
                (void)m.write_u8(buf_ptr + static_cast<GuestAddr>(i++), ch);
                if (ch == '\n') break;
            }
            if (!got_any) { m.set_return(0); return; }
            (void)m.write_u8(buf_ptr + static_cast<GuestAddr>(i), 0);
            m.set_return(static_cast<u64>(buf_ptr));
        }});
    // puts() writes the string to stdout AND appends a newline (fputs does
    // not), returning a non-negative value on success. It was missing, so a
    // guest calling it got 0xFFFFFFFF -- which reads as EOF, i.e. "the write
    // failed" -- and no output at all.
    exports.entries.push_back(
        {"puts", 0,
         [&mgr](Marshaler& m) {
            auto sr = m.read_ansi_string(m.arg_ptr(0));
            if (sr.is_err()) {
                m.set_return(static_cast<u32>(-1));  // EOF
                return;
            }
            FILE* out = std_host_file(mgr, 1);
            const int rc = stdio_host_call([&] { return ::fputs(sr.value().c_str(), out); });
            if (rc < 0 || stdio_host_call([&] { return ::fputc('\n', out); }) < 0) {
                m.set_return(static_cast<u32>(-1));
                return;
            }
            m.set_return(0);  // any non-negative value
        }});
    exports.entries.push_back(
        {"fputs", 9,
         [&mgr, host_standard_stream, vfs_write_translated](Marshaler& m) {
            (void)mgr;
            GuestAddr str_ptr = m.arg_ptr(0);
            u32 fp = static_cast<u32>(m.arg(1));
            if (FILE* host_file = host_standard_stream(fp)) {
                auto sr = m.read_ansi_string(str_ptr);
                if (sr.is_err()) {
                    m.set_return(static_cast<u32>(-1));
                    return;
                }
                m.set_return(static_cast<u32>(
                    stdio_host_call([&] { return ::fputs(sr.value().c_str(), host_file); })));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end() || str_ptr == 0) {
                m.set_return(static_cast<u64>(static_cast<i64>(-1)));
                return;
            }
            auto sr = m.read_ansi_string(str_ptr);
            if (sr.is_err()) {
                m.set_return(static_cast<u64>(static_cast<i64>(-1)));
                return;
            }
            const auto& s = sr.value();
            const usize n = vfs_write_translated(
                it->second,
                std::span<const u8>(
                    reinterpret_cast<const u8*>(s.data()), s.size()));
            // Learn fputs: nonnegative on success, EOF on error.
            m.set_return(n == s.size()
                             ? static_cast<u64>(n)
                             : static_cast<u64>(static_cast<i64>(-1)));
        }});
    exports.entries.push_back(
        {"ungetc", 10, [&mgr](Marshaler& m) {
            const i32 ch = static_cast<i32>(m.arg(0));
            const u32 fp = static_cast<u32>(m.arg(1));
            m.set_return(static_cast<u32>(mgr.guest_ungetc(ch, fp)));
        }});
}

} // namespace vitreo::winapi::crt_stdio_internal
