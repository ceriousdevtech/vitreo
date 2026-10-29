// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_fprintf_registration.cpp
/// @brief The fprintf family: UCRT __stdio_common_vf*printf* and msvcrt.dll's
/// fprintf/printf/wprintf exports.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/io/crt_printf.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"

#include <cerrno>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

/// The fprintf family: UCRT __stdio_common_vf*printf* and msvcrt.dll's
/// fprintf/printf/wprintf exports.
void add_crt_stdio_fprintf_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto host_standard_stream = [&mgr, &crt_data](GuestAddr stream) -> FILE* {
        return stdio_host_standard_stream(mgr, crt_data, stream);
    };
    auto vfs_write_translated = [&vfs](VfsFile& vf, std::span<const u8> src) -> usize {
        return stdio_vfs_write_translated(vfs, vf, src);
    };

    // ---- formatted output to a stream: the fprintf family ---------------------
    //
    // crt_printf runs the output processor (crt_printf.hpp); these write what
    // it produced. UCRT common_vfprintf (ucrt/stdio/output.cpp) rejects a null
    // stream or format with EINVAL; the result is the processor's character
    // count, or -1 when the stream write fails. Text-mode streams translate LF
    // to CR-LF (Learn fopen); the count is of formatted characters, not bytes.
    //
    // A wide call's characters are encoded the way the stream stores them:
    // UTF-16LE or UTF-8 (fopen ccs=) or the ANSI code page, where an
    // unmappable character fails after the prefix is written (UCRT fwprintf
    // on a text stream, crt_ucrt_io_probe vfwprintf_text).
    auto write_formatted = [&vfs, &memory_manager, host_standard_stream,
                            vfs_write_translated](
                               u32 fp, const std::vector<u16>& units, bool wide,
                               int result, crt_printf::Flavor flavor) -> int {
        std::string narrow;
        if (wide) {
            narrow = utf16_to_utf8(units.data(), static_cast<int>(units.size()));
        } else {
            narrow.reserve(units.size());
            for (u16 unit : units) narrow.push_back(static_cast<char>(unit));
        }
        if (FILE* host_file = host_standard_stream(fp)) {
            // Host stdout/stderr: the Vitreo console is UTF-8.
            stdio_host_call([&] { return ::fwrite(narrow.data(), 1, narrow.size(), host_file); });
            stdio_host_call([&] { return ::fflush(host_file); });
            return result;
        }
        auto it = stdio_arena().files.find(fp);
        if (it == stdio_arena().files.end()) return -1;
        VfsFile& vf = it->second;
        auto write_bytes = [&](std::span<const u8> bytes, bool translate_lf) {
            if (bytes.empty()) return true;
            if (translate_lf && vf.text_mode) {
                return vfs_write_translated(vf, bytes) == bytes.size();
            }
            auto written = vfs.write(stream_write_handle(vfs, vf), bytes);
            return written.is_ok() && written.value() == bytes.size();
        };
        if (!wide) {
            const bool ok = write_bytes(
                std::span<const u8>(reinterpret_cast<const u8*>(narrow.data()),
                                    narrow.size()),
                true);
            return ok ? result : -1;
        }
        using Enc = VfsFile::Encoding;
        const Enc enc = vf.encoding == Enc::None ? Enc::Acp : vf.encoding;
        if (enc == Enc::Utf16Le) {
            std::vector<u8> bytes;
            bytes.reserve(units.size() * 2 + 8);
            for (u16 ch : units) {
                if (vf.text_mode && ch == u16{'\n'}) {
                    bytes.insert(bytes.end(), {0x0D, 0x00, 0x0A, 0x00});
                } else {
                    bytes.push_back(static_cast<u8>(ch & 0xFF));
                    bytes.push_back(static_cast<u8>(ch >> 8));
                }
            }
            return write_bytes(bytes, false) ? result : -1;
        }
        if (enc == Enc::Utf8) {
            const bool ok = write_bytes(
                std::span<const u8>(reinterpret_cast<const u8*>(narrow.data()),
                                    narrow.size()),
                true);
            return ok ? result : -1;
        }
        // ACP: one character at a time, WC_NO_BEST_FIT, so an unmappable
        // unit fails (EILSEQ in the UCRT) instead of becoming '?'. msvcrt.dll
        // instead drops the character, keeps going and still counts it, with
        // errno EILSEQ (measured: L"x\x263ay\n" writes "xy\r\n", returns 4).
        const bool skip_unmappable = flavor == crt_printf::Flavor::Msvcrt;
        bool dropped = false;
        constexpr u32 kNoBestFit = 0x400;
        std::vector<u8> bytes;
        bytes.reserve(units.size() * 2 + 8);
        bool failed = false;
        for (usize i = 0; i < units.size();) {
            usize n = 1;
            if (units[i] >= 0xD800 && units[i] <= 0xDBFF && i + 1 < units.size() &&
                units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
                n = 2;
            }
            u8 mb[8];
            const auto conv = codepage::wide_to_multi_byte(
                codepage::kCpAcp, kNoBestFit, &units[i], n, mb, sizeof(mb),
                nullptr, true);
            if (!conv.ok() || conv.count == 0 || conv.used_default) {
                if (skip_unmappable) {
                    dropped = true;
                    i += n;
                    continue;
                }
                failed = true;
                break;
            }
            bytes.insert(bytes.end(), mb, mb + conv.count);
            i += n;
        }
        if (dropped) memory_manager.set_errno(42u);  // EILSEQ
        if (!write_bytes(bytes, true)) return -1;
        return failed ? -1 : result;
    };

    // Format then write. @p stream is the guest FILE*.
    auto stream_printf = [&memory_manager, write_formatted](
                             Marshaler& m, crt_printf::Flavor flavor,
                             crt_printf::Processor processor, u64 options,
                             bool wide, u64 stream, GuestAddr format,
                             crt_printf::ArgCursor args) -> int {
        if (stream == 0 || format == 0) {
            crt_printf::report_invalid(memory_manager, flavor, crt_printf::kEINVAL);
            return -1;
        }
        crt_printf::Target target;
        const crt_printf::Request request = crt_printf::make_request(
            memory_manager, flavor, processor, options, wide);
        const int result = crt_printf::format(m, request, format, args, target);
        const int written = write_formatted(static_cast<u32>(stream),
                                            target.stored, wide, result, flavor);
        return result < 0 ? -1 : written;
    };

    // UCRT: int __stdio_common_vf[w]printf[_s|_p](unsigned __int64 options,
    //           FILE* stream, const TCHAR* format, _locale_t, va_list)
    // `options` takes two argument slots on x86.
    {
        struct Variant {
            const char* narrow;
            const char* wide;
            crt_printf::Processor processor;
        };
        static constexpr Variant kVariants[] = {
            {"__stdio_common_vfprintf", "__stdio_common_vfwprintf",
             crt_printf::Processor::Standard},
            {"__stdio_common_vfprintf_s", "__stdio_common_vfwprintf_s",
             crt_printf::Processor::Validating},
            {"__stdio_common_vfprintf_p", "__stdio_common_vfwprintf_p",
             crt_printf::Processor::Positional},
        };
        for (const Variant& v : kVariants) {
            for (const bool wide : {false, true}) {
                const auto processor = v.processor;
                exports.entries.push_back(
                    {wide ? v.wide : v.narrow, 0,
                     [stream_printf, processor, wide](Marshaler& m) {
                         const bool x64 = m.cpu_is_64bit();
                         const usize b = x64 ? 1 : 2;
                         const u64 options =
                             x64 ? m.arg(0)
                                 : ((m.arg(0) & 0xFFFFFFFFULL) | (m.arg(1) << 32));
                         m.set_return(static_cast<u32>(stream_printf(
                             m, crt_printf::Flavor::Ucrt, processor, options, wide,
                             m.arg_ptr(b), m.arg_ptr(b + 1),
                             crt_printf::ArgCursor::va_list_at(m.arg_ptr(b + 3)))));
                     }});
            }
        }
    }

    // msvcrt.dll: fprintf/vfprintf/printf/vprintf and the wide, _s and _p
    // forms (Learn fprintf, fprintf_s, _fprintf_p families; all exported by
    // the system msvcrt.dll on x86 and x64). printf writes to stdout, the
    // FILE msvcrt publishes at _iob[1].
    {
        struct Legacy {
            const char* name;
            bool wide;
            bool to_stdout;
            bool va;
            crt_printf::Processor processor;
        };
        using P = crt_printf::Processor;
        static constexpr Legacy kLegacy[] = {
            {"fprintf", false, false, false, P::Standard},
            {"vfprintf", false, false, true, P::Standard},
            {"printf", false, true, false, P::Standard},
            {"vprintf", false, true, true, P::Standard},
            {"fprintf_s", false, false, false, P::Validating},
            {"vfprintf_s", false, false, true, P::Validating},
            {"printf_s", false, true, false, P::Validating},
            {"vprintf_s", false, true, true, P::Validating},
            {"_fprintf_p", false, false, false, P::Positional},
            {"_vfprintf_p", false, false, true, P::Positional},
            {"_printf_p", false, true, false, P::Positional},
            {"_vprintf_p", false, true, true, P::Positional},
            {"fwprintf", true, false, false, P::Standard},
            {"vfwprintf", true, false, true, P::Standard},
            {"wprintf", true, true, false, P::Standard},
            {"vwprintf", true, true, true, P::Standard},
            {"fwprintf_s", true, false, false, P::Validating},
            {"vfwprintf_s", true, false, true, P::Validating},
            {"wprintf_s", true, true, false, P::Validating},
            {"vwprintf_s", true, true, true, P::Validating},
            {"_fwprintf_p", true, false, false, P::Positional},
            {"_vfwprintf_p", true, false, true, P::Positional},
            {"_wprintf_p", true, true, false, P::Positional},
            {"_vwprintf_p", true, true, true, P::Positional},
        };
        for (const Legacy& l : kLegacy) {
            const Legacy entry = l;
            exports.entries.push_back(
                {entry.name, 0, [stream_printf, entry, &crt_data](Marshaler& m) {
                     const usize fmt_index = entry.to_stdout ? 0 : 1;
                     const u64 stream = entry.to_stdout
                         ? crt_data.legacy_stream_addr(1, m.cpu_is_64bit())
                         : m.arg(0);
                     const crt_printf::ArgCursor args = entry.va
                         ? crt_printf::ArgCursor::va_list_at(m.arg_ptr(fmt_index + 1))
                         : crt_printf::ArgCursor::call_args(fmt_index + 1);
                     m.set_return(static_cast<u32>(stream_printf(
                         m, crt_printf::Flavor::Msvcrt, entry.processor, 0,
                         entry.wide, stream, m.arg_ptr(fmt_index), args)));
                 }});
        }
    }
}

} // namespace vitreo::winapi::crt_stdio_internal
