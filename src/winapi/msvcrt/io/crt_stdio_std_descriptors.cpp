// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_std_descriptors.cpp
/// @brief Descriptors 0-2 and their host FILE* bridge.
///
/// Resolves stdin/stdout/stderr against the process's standard handles, reads
/// and encodes through them, and exposes the host FILE* every stream function
/// that targets a standard stream goes through.

#include "winapi/msvcrt/io/crt_stdio_internal.hpp"
#include "winapi/msvcrt/io/crt_stdio_text_read.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

// ---- descriptors 0-2 ---------------------------------------------------------
//
// At start-up the CRT binds descriptors 0, 1 and 2 to the process's standard
// handles (GetStdHandle) and stdin/stdout/stderr to those descriptors (Learn
// _get_osfhandle, _fileno, _setmode). Measured on Windows 11 by
// tools/oracle_probe/stdout_pipe_chain_probe, identically for msvcrt.dll and
// the UCRT, x86 and x64:
//  - a pipe or file handle: the descriptor IS that HANDLE. _get_osfhandle
//    returns it, _fileno(stdout) is 1, _isatty 0, _fstat _S_IFIFO for a pipe,
//    and text mode writes LF as CR-LF. A launcher captures a child's output
//    this way, including through a GUI relay (a one-file packer) that hands
//    _get_osfhandle(_fileno(stdout)) on to the program it starts.
//  - no handle (a GUI process started without STARTF_USESTDHANDLES): _fileno
//    and _get_osfhandle report -2 without touching errno, _write fails with
//    EBADF, _isatty returns 64.
//  - a console handle: the host console.

namespace {

constexpr u32 kStdScratchSize = 4096;

} // namespace

/// Bytes a descriptor in @p mode puts on its HANDLE for @p in (Learn _write,
/// _setmode): text turns LF into CR-LF; the Unicode modes take UTF-16 input
/// (an odd byte count is EINVAL) and _O_U8TEXT encodes it as UTF-8.
std::optional<std::vector<u8>> lowio_encode(i32 mode, std::span<const u8> in) {
    std::vector<u8> out;
    if (mode == kStdModeBinary) {
        out.assign(in.begin(), in.end());
        return out;
    }
    if (mode == kStdModeText) {
        out.reserve(in.size() + in.size() / 8 + 8);
        for (u8 b : in) {
            if (b == '\n') out.push_back('\r');
            out.push_back(b);
        }
        return out;
    }
    if ((in.size() & 1u) != 0) return std::nullopt;
    std::vector<u16> units;
    units.reserve(in.size() / 2 + in.size() / 16 + 4);
    for (usize i = 0; i + 1 < in.size(); i += 2) {
        const u16 unit = static_cast<u16>(in[i] | (in[i + 1] << 8));
        if (unit == u16{'\n'}) units.push_back(u16{'\r'});
        units.push_back(unit);
    }
    if (mode == kStdModeU8Text) {
        const std::string utf8 =
            utf16_to_utf8(units.data(), static_cast<int>(units.size()));
        out.assign(utf8.begin(), utf8.end());
        return out;
    }
    out.reserve(units.size() * 2);
    for (u16 unit : units) {
        out.push_back(static_cast<u8>(unit & 0xFF));
        out.push_back(static_cast<u8>(unit >> 8));
    }
    return out;
}

namespace {

/// The same for bytes a host-side stream function produced: narrow text, in
/// UTF-8 for the wide functions, so the Unicode modes re-encode it.
std::vector<u8> stream_encode(i32 mode, std::span<const u8> in) {
    if (mode == kStdModeBinary || mode == kStdModeText) {
        return *lowio_encode(mode, in);
    }
    std::vector<u16> units = utf8_to_utf16(std::string_view(
        reinterpret_cast<const char*>(in.data()), in.size()));
    if (!units.empty() && units.back() == 0 && (in.empty() || in.back() != 0)) {
        units.pop_back();
    }
    std::vector<u8> wide;
    wide.reserve(units.size() * 2);
    for (u16 unit : units) {
        wide.push_back(static_cast<u8>(unit & 0xFF));
        wide.push_back(static_cast<u8>(unit >> 8));
    }
    return *lowio_encode(mode, wide);
}

} // namespace

/// Classify descriptor @p index from the process's std handle. Done on first
/// use rather than at DLL attach: a child's STARTUPINFO handles are installed
/// after the CRT's export table is built, and before any guest code runs.
StdDescriptor& std_descriptor(CrtStdioManager& mgr, i32 index) {
    // One CRT per process; a second manager (a test harness building another
    // process's CRT) starts from unresolved descriptors.
    if (stdio_arena().owner != mgr.instance_id()) {
        for (StdDescriptor& old : stdio_arena().standard) {
            if (old.stream != nullptr) (void)stdio_host_call([&] { return ::fclose(old.stream); });
            old = StdDescriptor{};
        }
        stdio_arena().owner = mgr.instance_id();
    }
    StdDescriptor& d = stdio_arena().standard[index];
    d.index = index;
    d.files = mgr.file_manager();
    d.memory = stdio_arena().memory_manager;
    if (d.kind != StdKind::Unresolved) return d;
    ConsoleManager* console = mgr.console_manager();
    FileManager* files = mgr.file_manager();
    if (console == nullptr || files == nullptr) {
        d.kind = StdKind::Console;  // no kernel32 wiring: the host console
        return d;
    }
    const Handle h = console->std_slot(static_cast<u32>(index));
    const HandleType type = h == NULL_HANDLE || h == INVALID_HANDLE
        ? HandleType::Invalid
        : files->handle_table().get_type(h);
    if (type == HandleType::ConsoleInput || type == HandleType::ConsoleOutput) {
        d.kind = StdKind::Console;
        d.handle = h;
    } else if (h == NULL_HANDLE || h == INVALID_HANDLE || files->file_type(h) == 0) {
        d.kind = StdKind::None;
    } else {
        d.kind = StdKind::Kernel;
        d.handle = h;
        stdio_arena().file_manager = files;
    }
    return d;
}

namespace {

/// One raw ReadFile on a Kernel descriptor into @p dst: bytes read, 0 at end
/// of input (a broken pipe is end of input for the CRT), -1 on failure.
i64 std_read_raw(StdDescriptor& d, u8* dst, u32 count) {
    if (count == 0) return 0;
    if (d.lookahead_valid) {
        dst[0] = d.lookahead;
        d.lookahead_valid = false;
        return 1;
    }
    if (d.files == nullptr || d.memory == nullptr) return -1;
    if (d.scratch == 0) {
        d.scratch = d.memory->crt_malloc(kStdScratchSize);
        if (d.scratch == 0) return -1;
    }
    GuestMemory* memory = d.files->guest_memory();
    if (memory == nullptr) return -1;
    const u32 want = std::min(count, kStdScratchSize);
    u32 got = 0;
    if (!d.files->read_file(d.handle, d.scratch, want, 0, &got)) {
        return get_last_error() == error_codes::ERROR_BROKEN_PIPE ? 0 : -1;
    }
    if (got != 0 &&
        memory->read_bytes(d.scratch, reinterpret_cast<Byte*>(dst), got).is_err()) {
        return -1;
    }
    return got;
}

} // namespace

/// _read on a Kernel descriptor (Learn _read): text mode collapses CR-LF to
/// LF, a lone CR passes through, and Ctrl+Z ends the input. A CR that ends a
/// read is resolved by reading one byte more, kept for the next call when it
/// is not LF.
i64 std_read(StdDescriptor& d, u8* dst, u32 count) {
    if (count == 0) return 0;
    if (d.translation_mode == kStdModeBinary) return std_read_raw(d, dst, count);
    if (d.translation_mode == kStdModeU8Text) {
        static std::once_flag logged;
        std::call_once(logged, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ucrtbase!_read: UTF-8 input translation\n");
        });
        return -1;
    }
    const bool seekable = d.files != nullptr && d.files->file_type(d.handle) == 1;
    return read_translated_text(dst, count, d.text_eof, d.unicode_lookahead, seekable,
        d.translation_mode != kStdModeText,
        [&](u8* bytes, u32 size) { return std_read_raw(d, bytes, size); },
        [&](i64 size) { return d.files->set_file_pointer64(d.handle, -size, 1).has_value(); });
}

namespace {

/// Host FILE* callbacks over a Kernel descriptor, so every stream function
/// that writes or reads through `FILE*` reaches the HANDLE with the
/// descriptor's translation.
int std_stream_read(void* cookie, char* buf, int n) {
    auto& d = *static_cast<StdDescriptor*>(cookie);
    if (n <= 0) return 0;
    const i64 got = std_read(d, reinterpret_cast<u8*>(buf), static_cast<u32>(n));
    if (got < 0) {
        errno = EBADF;
        return -1;
    }
    return static_cast<int>(got);
}

int std_stream_write(void* cookie, const char* buf, int n) {
    auto& d = *static_cast<StdDescriptor*>(cookie);
    if (n <= 0) return 0;
    if (d.files == nullptr) {
        errno = EBADF;
        return -1;
    }
    const std::vector<u8> bytes = stream_encode(
        d.translation_mode,
        std::span<const u8>(reinterpret_cast<const u8*>(buf), static_cast<usize>(n)));
    if (!d.files->write_handle_bytes(
            d.handle, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                       bytes.size()))) {
        errno = get_last_error() == error_codes::ERROR_BROKEN_PIPE ? EPIPE : EBADF;
        return -1;
    }
    return n;
}

/// A standard stream with no handle: Windows buffers its output and the
/// flush fails (printf succeeds, fflush returns EOF with EBADF and sets the
/// error indicator; stdout_pipe_chain_probe bare mode). Vitreo keeps the
/// output as pending for that fflush, and copies it to the host log so it is
/// not lost to diagnosis. Input is end-of-file with an error.
int std_none_read(void*, char*, int) {
    errno = EBADF;
    return -1;
}

int std_none_write(void* cookie, const char* buf, int n) {
    auto& d = *static_cast<StdDescriptor*>(cookie);
    if (n <= 0) return 0;
    const int host_fd = d.index == 2 ? STDERR_FILENO : STDOUT_FILENO;
    (void)::write(host_fd, buf, static_cast<usize>(n));
    d.pending_output = true;
    return n;
}

} // namespace

/// The host FILE* a standard stream writes and reads through.
FILE* std_host_file(CrtStdioManager& mgr, i32 index) {
    StdDescriptor& d = std_descriptor(mgr, index);
    if (d.kind == StdKind::Kernel || d.kind == StdKind::None || d.kind == StdKind::Closed) {
        if (d.stream == nullptr) {
            d.stream = d.kind == StdKind::Kernel
                ? ::funopen(&d, std_stream_read, std_stream_write, nullptr, nullptr)
                : ::funopen(&d, std_none_read, std_none_write, nullptr, nullptr);
            if (d.stream != nullptr) (void)::setvbuf(d.stream, nullptr, _IONBF, 0);
        }
        if (d.stream != nullptr) return d.stream;
    }
    // A console handle is the host console.
    return index == 0 ? stdin : index == 1 ? stdout : stderr;
}

/// ferror for a standard stream; Console streams keep reporting no error.
bool std_stream_error(const StdDescriptor& d) {
    if (d.kind == StdKind::None || d.kind == StdKind::Closed) return d.stream_error;
    return d.kind == StdKind::Kernel && d.stream != nullptr && stdio_host_call([&] { return ::ferror(d.stream); }) != 0;
}

} // namespace vitreo::winapi::crt_stdio_internal
