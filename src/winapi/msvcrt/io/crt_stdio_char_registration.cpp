// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_char_registration.cpp
/// @brief Single-character and wide-character stream exports: setvbuf, fgetwc,
/// _cputs, _filbuf, _flsbuf, setbuf, getwc and putwc.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"

#include <cstdio>
#include <fcntl.h>
#include <span>
#include <unistd.h>

namespace vitreo::winapi::crt_stdio_internal {

namespace {

// wint_t's end-of-file value, shared by the fgetwc/fputwc/getwc/putwc family.
constexpr u32 WEOF_VALUE = 0xFFFF;

} // namespace

/// setvbuf, fgetwc, fputwc and ungetwc.
void add_crt_stdio_wide_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto host_standard_stream = [&mgr, &crt_data](GuestAddr stream) -> FILE* {
        return stdio_host_standard_stream(mgr, crt_data, stream);
    };

    // int setvbuf(FILE*, char* buffer, int mode, size_t size)
    //
    // Vitreo's streams write straight through to the VFS, so there is no
    // buffering to configure; the call succeeds for the modes the CRT accepts
    // and fails for anything else, which is what a caller can observe.
    exports.entries.push_back({"setvbuf", 0, [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg(1);
        const auto mode = static_cast<i32>(m.arg(2));
        constexpr i32 IOFBF = 0, IOLBF = 64, IONBF = 4;
        if (mode != IOFBF && mode != IOLBF && mode != IONBF) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    }});

    // wint_t fgetwc(FILE*) / fputwc(wchar_t, FILE*) / ungetwc(wint_t, FILE*)
    //
    // Vitreo's streams are byte streams; the wide forms transfer one UTF-16
    // code unit built from two bytes, matching the CRT's binary-mode behaviour.
    exports.entries.push_back({"fgetwc", 0,
        [&vfs, host_standard_stream](Marshaler& m) {
            const auto fp = static_cast<u32>(m.arg(0));
            if (FILE* host_file = host_standard_stream(fp)) {
                const int ch = stdio_host_call([&] { return ::fgetc(host_file); });
                m.set_return(ch < 0 ? WEOF_VALUE : static_cast<u32>(ch));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) { m.set_return(WEOF_VALUE); return; }
            u8 bytes[2] = {0, 0};
            auto result = vfs.read(stream_handle(vfs, it->second), std::span<u8>(bytes, 2));
            if (result.is_err() || result.value() < 2) {
                m.set_return(WEOF_VALUE);
                return;
            }
            m.set_return(static_cast<u32>(bytes[0]) |
                         (static_cast<u32>(bytes[1]) << 8));
        }});
    exports.entries.push_back({"fputwc", 0,
        [&vfs, host_standard_stream](Marshaler& m) {
            const auto ch = static_cast<u16>(m.arg(0));
            const auto fp = static_cast<u32>(m.arg(1));
            if (FILE* host_file = host_standard_stream(fp)) {
                // Narrow to the host stream; ASCII passes through unchanged.
                stdio_host_call([&] { return ::fputc(ch < 0x80 ? static_cast<int>(ch) : '?', host_file); });
                m.set_return(ch);
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) { m.set_return(WEOF_VALUE); return; }
            const u8 bytes[2] = {static_cast<u8>(ch & 0xFF),
                                 static_cast<u8>((ch >> 8) & 0xFF)};
            auto result = vfs.write(stream_write_handle(vfs, it->second),
                                    std::span<const u8>(bytes, 2));
            m.set_return(result.is_ok() ? ch : WEOF_VALUE);
        }});
    exports.entries.push_back({"ungetwc", 0, [&vfs](Marshaler& m) {
        const auto ch = static_cast<u16>(m.arg(0));
        const auto fp = static_cast<u32>(m.arg(1));
        auto it = stdio_arena().files.find(fp);
        if (it == stdio_arena().files.end() || ch == WEOF_VALUE) {
            m.set_return(WEOF_VALUE);
            return;
        }
        // Push back by rewinding one code unit, which is what a byte stream
        // can honestly offer; the CRT guarantees only one pushback.
        auto result = vfs.seek(stream_handle(vfs, it->second), -2,
                               capsule::SeekOrigin::Current);
        m.set_return(result.is_ok() ? ch : WEOF_VALUE);
    }});

    // The fprintf family (UCRT __stdio_common_vf*printf* and msvcrt's
    // fprintf/printf/fwprintf/... exports) is registered below, after the
    // stream helpers it writes through.
}

/// _cputs.
void add_crt_stdio_cputs_exports(DllExports& exports, const StdioContext& /*ctx*/) {
    // int _cputs(const char* str)
    //
    // Learn: writes to the console (CONOUT$), not to stdout — so redirecting
    // stdout leaves _cputs visible on the console. On the Mac host that is
    // the controlling tty (/dev/tty), not fileno(stdout).
    exports.entries.push_back({"_cputs", 0, [](Marshaler& m) {
        const GuestAddr str_ptr = m.arg_ptr(0);
        if (str_ptr == 0) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto text = m.read_ansi_string(str_ptr);
        if (text.is_err()) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const auto& s = text.value();
        int tty = ::open("/dev/tty", O_WRONLY | O_CLOEXEC);
        if (tty < 0) {
            // No controlling tty (rare for interactive guests): fall back to
            // stderr so the bytes are not lost into a redirected stdout.
            tty = STDERR_FILENO;
            const auto written = ::write(tty, s.data(), s.size());
            m.set_return(written < 0 ? 0xFFFFFFFFu : 0u);
            return;
        }
        const auto written = ::write(tty, s.data(), s.size());
        ::close(tty);
        m.set_return(written < 0 ? 0xFFFFFFFFu : 0u);
    }});
}

/// _filbuf, _flsbuf and setbuf.
void add_crt_stdio_legacy_char_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
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

    // ---- legacy getc/putc out-of-line helpers -------------------------------
    //
    // MSVC's <stdio.h> expands getc/putc inline and falls back to _filbuf /
    // _flsbuf when the (unused here) FILE buffer runs dry. Vitreo's streams are
    // unbuffered, so every getc/putc lands in these, which makes them
    // load-bearing rather than a corner case.

    constexpr u32 EOF_VALUE = 0xFFFFFFFFu;

    // int _filbuf(FILE*)
    exports.entries.push_back({"_filbuf", 0,
        [vfs_read_translated, host_standard_stream](Marshaler& m) {
            const auto fp = static_cast<u32>(m.arg(0));
            if (FILE* host_file = host_standard_stream(fp)) {
                m.set_return(static_cast<u32>(stdio_host_call([&] { return ::fgetc(host_file); })));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(EOF_VALUE);
                return;
            }
            u8 ch = 0;
            const usize n = vfs_read_translated(it->second,
                                                std::span<u8>(&ch, 1));
            m.set_return(n == 0 ? EOF_VALUE : static_cast<u64>(ch));
        }});

    // int _flsbuf(int c, FILE*)
    exports.entries.push_back({"_flsbuf", 0,
        [host_standard_stream, vfs_write_translated](Marshaler& m) {
            const auto ch = static_cast<u8>(m.arg(0) & 0xFF);
            const auto fp = static_cast<u32>(m.arg(1));
            if (FILE* host_file = host_standard_stream(fp)) {
                m.set_return(static_cast<u32>(stdio_host_call([&] { return ::fputc(ch, host_file); })));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(EOF_VALUE);
                return;
            }
            const usize n = vfs_write_translated(
                it->second, std::span<const u8>(&ch, 1));
            m.set_return(n != 0 ? static_cast<u64>(ch) : EOF_VALUE);
        }});

    // ---- setbuf -------------------------------------------------------------
    //
    // void setbuf(FILE*, char*) — equivalent to setvbuf(f, buf, buf ? _IOFBF
    // : _IONBF, BUFSIZ). Vitreo's streams write through to the VFS on every
    // call, so the guest's buffer is never used; the observable contract
    // (data reaching the file) is stronger than what buffering would give and
    // setbuf returns void, so there is nothing to report.
    exports.entries.push_back({"setbuf", 0, [](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg(1);
    }});
}

/// getwc and putwc.
void add_crt_stdio_getwc_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto host_standard_stream = [&mgr, &crt_data](GuestAddr stream) -> FILE* {
        return stdio_host_standard_stream(mgr, crt_data, stream);
    };
    auto vfs_read_translated = [&vfs](VfsFile& vf, std::span<u8> dst) -> usize {
        return stdio_vfs_read_translated(vfs, vf, dst);
    };

    // ---- getwc / putwc ------------------------------------------------------
    //
    // The narrow streams underneath are byte streams, so in the CRT's default
    // (single-byte "C") locale a wide character is one byte widened, which is
    // exactly what fgetwc/fputwc do on an unoriented stream.
    exports.entries.push_back({"getwc", 0,
        [vfs_read_translated, host_standard_stream](Marshaler& m) {
            const auto fp = static_cast<u32>(m.arg(0));
            if (FILE* host_file = host_standard_stream(fp)) {
                const int ch = stdio_host_call([&] { return ::fgetc(host_file); });
                m.set_return(ch == EOF ? WEOF_VALUE : static_cast<u32>(ch));
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(WEOF_VALUE);
                return;
            }
            u8 ch = 0;
            const usize n = vfs_read_translated(it->second,
                                                std::span<u8>(&ch, 1));
            m.set_return(n == 0 ? WEOF_VALUE : static_cast<u64>(ch));
        }});

    exports.entries.push_back({"putwc", 0,
        [&vfs, host_standard_stream](Marshaler& m) {
            const auto wide = static_cast<u16>(m.arg(0) & 0xFFFF);
            const auto fp = static_cast<u32>(m.arg(1));
            if (wide > 0xFF) {
                // Outside the single-byte locale there is no honest narrowing.
                VITREO_TRACE("[crt] putwc: U+%04X not representable in the "
                             "\"C\" locale\n", wide);
                m.set_return(WEOF_VALUE);
                return;
            }
            const auto ch = static_cast<u8>(wide);
            if (FILE* host_file = host_standard_stream(fp)) {
                const int written = stdio_host_call([&] { return ::fputc(ch, host_file); });
                m.set_return(written == EOF ? WEOF_VALUE : wide);
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(WEOF_VALUE);
                return;
            }
            auto res = vfs.write(stream_write_handle(vfs, it->second),
                                 std::span<const u8>(&ch, 1));
            m.set_return(res.is_ok() && res.value() != 0 ? wide : WEOF_VALUE);
        }});
}

} // namespace vitreo::winapi::crt_stdio_internal
