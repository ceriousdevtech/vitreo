// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_export_locking.cpp
/// @brief Classification and lock-wrapping of every registered stdio export.
///
/// The only place that wraps handlers in StdioArenaScope and StdioStateLock.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/filesystem/file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>

namespace vitreo::winapi::crt_stdio_internal {

/// Wrap every registered entry in the stdio lock domain.
void lock_crt_stdio_exports(DllExports& exports, const StdioContext& ctx,
                            std::shared_ptr<StdioArena> arena) {
    CrtStdioManager& mgr = ctx.mgr;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto standard_stream_index = [&crt_data](GuestAddr stream) -> i32 {
        return stdio_standard_stream_index(crt_data, stream);
    };

    // Keep the synchronization at the table owner rather than relying on each
    // individual thunk to remember which shared CRT structures it touches.
    // This also covers descriptor aliases created by fopen and consumed by
    // the low-I/O exports.
    for (auto& entry : exports.entries) {
        if (entry.name.starts_with("__vitreo_")) entry.internal = true;
        // Compute the binding once from the SDK stdio/io prototypes. The
        // common UCRT options parameter occupies two argument slots on x86.
        std::string_view api(entry.name);
        i32 fixed_index = -1;
        i32 file_argument = -1;
        bool common_options = false;
        constexpr std::string_view fd_apis[] = {
            "_read", "_write", "_close", "_setmode", "_dup", "_dup2",
            "_isatty", "_fstat", "_fstat32", "_fstat64", "_fstat64i32",
            "_fstat32i64", "_fstati64", "_lseek", "_lseeki64", "_tell",
            "_telli64", "_filelength", "_filelengthi64", "_locking",
            "_chsize", "_commit", "_fdopen"};
        const bool descriptor_argument =
            std::find(std::begin(fd_apis), std::end(fd_apis), api) != std::end(fd_apis);
        if (!descriptor_argument) {
            if (api.starts_with("__stdio_common_vf")) {
                common_options = true;
                file_argument = 1;
            } else {
                while (api.starts_with('_')) api.remove_prefix(1);
                if (api.ends_with("_nolock")) api.remove_suffix(7);
                if (api.starts_with('v')) api.remove_prefix(1);
                if (api.starts_with("fprintf") || api.starts_with("fwprintf") ||
                    api.starts_with("fscanf") || api.starts_with("fwscanf")) {
                    file_argument = 0;
                } else if (api.starts_with("printf") || api.starts_with("wprintf") ||
                           api == "putchar" || api == "putwchar" || api == "puts" || api == "putws") {
                    fixed_index = 1;
                } else if (api.starts_with("scanf") || api.starts_with("wscanf") ||
                           api == "getchar" || api == "getwchar" || api == "gets" || api == "gets_s") {
                    fixed_index = 0;
                } else {
                    constexpr std::pair<std::string_view, i32> file_apis[] = {
                        {"fgetc",0},{"getc",0},{"fgetwc",0},{"getwc",0},{"filbuf",0},
                        {"fclose",0},{"fflush",0},{"feof",0},{"ferror",0},{"clearerr",0},
                        {"fseek",0},{"ftell",0},{"fseeki64",0},{"ftelli64",0},{"rewind",0},
                        {"setbuf",0},{"setvbuf",0},{"fgetpos",0},{"fsetpos",0},{"getw",0},
                        {"get_stream_buffer_pointers",0},{"lock_file",0},{"unlock_file",0},
                        {"fputc",1},{"putc",1},{"flsbuf",1},{"fputs",1},{"fputws",1},
                        {"ungetc",1},{"fputwc",1},{"putwc",1},{"ungetwc",1},{"putw",1},
                        {"fgets",2},{"fgetws",2},{"fread",3},{"fwrite",3},{"fread_s",4}};
                    for (const auto& [candidate, argument] : file_apis) {
                        if (api == candidate) { file_argument = argument; break; }
                    }
                }
            }
        }
        const bool duplicate_target = api == "_dup2";
        const bool read_api = api == "_read";
        const bool ordinary_fread = api == "fread";
        const bool ordinary_fgets = api == "fgets";
        ApiHandler handler = std::move(entry.handler);
        entry.handler = [arena, handler = std::move(handler), fixed_index,
                         descriptor_argument, file_argument, common_options,
                         ordinary_fread, ordinary_fgets, duplicate_target, read_api,
                         &memory_manager, &mgr, standard_stream_index](Marshaler& marshaler) {
            StdioArenaScope arena_scope(*arena);
            i32 standard_index = fixed_index;
            bool read_was_open = false;
            if (descriptor_argument || file_argument >= 0) {
                StdioStateLock lookup_lock;
                if (descriptor_argument) {
                    const auto fd = static_cast<i32>(marshaler.arg(0));
                    if (fd >= 0 && fd < lowio_limit()) {
                        standard_index = fd;
                        if (read_api) read_was_open = fd <= 2
                            ? std_descriptor(mgr, fd).kind != StdKind::Closed
                            : stdio_arena().descriptors.contains(fd);
                    }
                } else {
                    const i32 argument = file_argument +
                        (common_options && !marshaler.cpu_is_64bit() ? 1 : 0);
                    const GuestAddr stream = marshaler.arg_ptr(argument);
                    if (stream != 0) {
                        standard_index = standard_stream_index(stream);
                        if (standard_index < 0 && stream <= 0xFFFFFFFFULL) {
                            auto file = stdio_arena().files.find(static_cast<u32>(stream));
                            if (file != stdio_arena().files.end()) standard_index = file->second.host_std_fd;
                            if (standard_index < 0) {
                                for (i32 fd = 0; fd <= 2; ++fd) {
                                    auto descriptor = stdio_arena().descriptors.find(fd);
                                    if (descriptor != stdio_arena().descriptors.end() && descriptor->second.aliased_file == stream) {
                                        standard_index = fd;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Zero-item fread does no I/O or stream validation. Parameter
            // validation also precedes waiting on a FILE's I/O lock.
            if ((ordinary_fread && (marshaler.arg(0) == 0 || marshaler.arg(1) == 0 || marshaler.arg(2) == 0)) ||
                (ordinary_fgets && (marshaler.arg(0) == 0 || static_cast<i32>(marshaler.arg(1)) <= 0))) {
                standard_index = -1;
            }
            i32 second_index = -1;
            if (duplicate_target) {
                const auto target = static_cast<i32>(marshaler.arg(1));
                if (target >= 0 && target < lowio_limit() && target != standard_index) {
                    if (standard_index < 0) standard_index = target;
                    else {
                        second_index = std::max(standard_index, target);
                        standard_index = std::min(standard_index, target);
                    }
                }
            }
            auto descriptor_lock = descriptor_argument ? lock_lowio_descriptor(standard_index)
                                                       : lock_standard_descriptor(standard_index);
            auto second_descriptor_lock = lock_lowio_descriptor(second_index);
            StdioStateLock lock;
            if (read_api && read_was_open && (standard_index <= 2
                    ? std_descriptor(mgr, standard_index).kind == StdKind::Closed
                    : !stdio_arena().descriptors.contains(standard_index))) {
                // Native lowio blocking probe: an already validated read that
                // loses its descriptor while waiting fails EBADF without
                // invoking the invalid-parameter handler a second time.
                memory_manager.set_errno(EBADF_VALUE);
                marshaler.set_return(0xFFFFFFFFu);
                return;
            }
            handler(marshaler);
        };
    }
}

} // namespace vitreo::winapi::crt_stdio_internal
