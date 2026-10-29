// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_stream_registration.cpp
/// @brief FILE-object exports: the stdio buffer cells, fclose and tmpfile.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/handle.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace vitreo::winapi::crt_stdio_internal {

/// __acrt_iob_func and _get_stream_buffer_pointers.
void add_crt_stdio_buffer_exports(DllExports& exports, const StdioContext& ctx) {
    CrtDataBlock& crt_data = ctx.crt_data;

    // FILE* __cdecl __acrt_iob_func(unsigned index)
    exports.entries.push_back(
        {"__acrt_iob_func", 0, [&crt_data](Marshaler& m) {
            const u32 index = static_cast<u32>(m.arg(0));
            const GuestAddr stream = crt_data.acrt_stream_addr(index);
            if (stream == 0) {
                VITREO_TRACE("[crt] __acrt_iob_func invalid index=%u\n", index);
            }
            m.set_return(stream);
        }});

    // errno_t __cdecl _get_stream_buffer_pointers(
    //     FILE*, char*** base, char*** pointer, int** count)
    exports.entries.push_back(
        {"_get_stream_buffer_pointers", 0,
         [&crt_data](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            const GuestAddr stream = m.arg_ptr(0);
            const i32 index = crt_data.acrt_stream_index(stream);
            const bool is_vfs_stream =
                stream <= 0xFFFFFFFFULL &&
                stdio_arena().files.contains(static_cast<u32>(stream));
            if (index < 0 && !is_vfs_stream) {
                VITREO_TRACE(
                    "[crt] _get_stream_buffer_pointers invalid FILE*=0x%llx\n",
                    static_cast<unsigned long long>(stream));
                m.set_return(EINVAL_VALUE);
                return;
            }

            const GuestAddr base_out = m.arg_ptr(1);
            const GuestAddr pointer_out = m.arg_ptr(2);
            const GuestAddr count_out = m.arg_ptr(3);
            GuestAddr base_cell = stream + CrtDataBlock::ACRT_BASE_OFFSET;
            GuestAddr pointer_cell = stream + CrtDataBlock::ACRT_PTR_OFFSET;
            GuestAddr count_cell = stream + CrtDataBlock::ACRT_COUNT_OFFSET;
            if (is_vfs_stream) {
                const auto it = stdio_arena().files.find(static_cast<u32>(stream));
                const auto public_cells = crt_stdio_buffer::cells(
                    it->second.layout);
                base_cell = stream + public_cells.base;
                pointer_cell = stream + public_cells.ptr;
                count_cell = stream + public_cells.count;
            }
            if (is_vfs_stream) {
                stdio_diag_vfs(m, "_get_stream_buffer_pointers",
                               static_cast<u32>(stream),
                               stdio_arena().files.find(static_cast<u32>(stream))
                                   ->second);
            } else {
                stdio_diag_cells(m, "_get_stream_buffer_pointers(std)",
                                 stream, pointer_cell, count_cell, base_cell,
                                 0, 0);
            }
            bool ok = true;
            if (base_out != 0) {
                ok = m.write_ptr(base_out, base_cell).is_ok() &&
                     ok;
            }
            if (pointer_out != 0) {
                ok = m.write_ptr(pointer_out, pointer_cell).is_ok() &&
                     ok;
            }
            if (count_out != 0) {
                ok = m.write_ptr(count_out, count_cell).is_ok() &&
                     ok;
            }
            m.set_return(ok ? 0 : EINVAL_VALUE);
        }});
}

/// fclose.
void add_crt_stdio_close_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto standard_stream_index = [&crt_data](GuestAddr stream) -> i32 {
        return stdio_standard_stream_index(crt_data, stream);
    };

    const auto release_fd_for_stream = [&mgr](u32 stream) {
        bool ok = true;
        for (auto it = stdio_arena().descriptors.begin(); it != stdio_arena().descriptors.end(); ) {
            if (it->second.aliased_file == stream) {
                const i32 fd = it->first;
                // Stream owned the VFS handle; do not close again here. A
                // kernel32 handle adopted for it (non-owning) goes with it.
                if (it->second.os_handle != 0 && mgr.file_manager() != nullptr) {
                    ok = mgr.file_manager()->close_file(it->second.os_handle) && ok;
                }
                it = stdio_arena().descriptors.erase(it);
                if (fd >= 0 && fd <= 2) {
                    StdDescriptor& desc = std_descriptor(mgr, fd);
                    desc.handle = 0;
                    desc.kind = StdKind::Closed;
                    if (ConsoleManager* console = mgr.console_manager()) {
                        (void)console->set_std_handle(STD_INPUT_HANDLE - static_cast<u32>(fd), NULL_HANDLE);
                    }
                } else {
                }
            } else {
                ++it;
            }
        }
        return ok;
    };

    // fclose(FILE*) -> int
    //
    // Measured (crt_fclose_probe): NULL without a handler → fast-fail
    // 0xC0000409; with a handler → EOF + errno EINVAL. Unknown non-NULL
    // guest pointers AV on Windows; Vitreo returns EOF+EINVAL (continue
    // path) so a wild FILE* cannot take down the host. stdin/stdout/stderr
    // fclose returns 0. After fclose of an fopen'd stream the fd alias is
    // EBADF and the next fopen reuses the same FILE* and fd (heap + free
    // list).
    exports.entries.push_back(
        {"fclose", 3,
         [&mgr, &vfs, &memory_manager, standard_stream_index,
          release_fd_for_stream](Marshaler& m) {
            (void)mgr;
            constexpr i32 kEof = -1;
            constexpr u32 EINVAL_VALUE = 22;
            const GuestAddr stream = m.arg_ptr(0);
            if (stream == 0) {
                if (!memory_manager.invalid_parameter()) {
                    // watson — process already terminating
                    return;
                }
                memory_manager.set_errno(EINVAL_VALUE);
                m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
                return;
            }

            const i32 std_idx = standard_stream_index(stream);
            if (std_idx >= 0 && std_idx <= 2) {
                if (std_descriptor(mgr, std_idx).file_closed) {
                    memory_manager.set_errno(EINVAL_VALUE);
                    m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
                    return;
                }
                StdDescriptor& desc = std_descriptor(mgr, std_idx);
                bool ok = true;
                if (desc.kind == StdKind::Closed) {
                    if (!memory_manager.invalid_parameter()) return;
                    memory_manager.set_errno(EBADF);
                    ok = false;
                }
                if (desc.stream != nullptr) {
                    ok = (stdio_host_call([&] { return ::fclose(desc.stream); }) == 0) && ok;
                    desc.stream = nullptr;
                }
                if (desc.handle != 0) {
                    if (FileManager* files = mgr.file_manager()) {
                        ok = files->close_file(desc.handle) && ok;
                    }
                }
                auto alias = stdio_arena().descriptors.find(std_idx);
                if (alias != stdio_arena().descriptors.end() && !alias->second.owns_handle && alias->second.aliased_file != 0) {
                    ok = vfs.close(alias->second.handle).is_ok() && ok;
                }
                // Guest fclose owns guest handles, never the host log.
                if (ConsoleManager* console = mgr.console_manager()) {
                    (void)console->set_std_handle(STD_INPUT_HANDLE - static_cast<u32>(std_idx), NULL_HANDLE);
                }
                desc.handle = 0;
                desc.kind = StdKind::Closed;
                stdio_arena().descriptors.erase(std_idx);
                desc.file_closed = true;
                if (!ok) memory_manager.set_errno(EBADF);
                m.set_return(ok ? 0u : 0xFFFFFFFFu);
                return;
            }

            if (stream > 0xFFFFFFFFULL) {
                memory_manager.set_errno(EINVAL_VALUE);
                m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
                return;
            }
            const u32 file_ptr = static_cast<u32>(stream);
            auto it = stdio_arena().files.find(file_ptr);
            if (it == stdio_arena().files.end()) {
                // Unknown FILE*: Windows would AV; Vitreo continues with
                // EINVAL like the NULL+handler path.
                memory_manager.set_errno(EINVAL_VALUE);
                m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
                return;
            }

            stdio_diag_vfs(m, "fclose", file_ptr, it->second);
            const bool has_descriptor = std::any_of(stdio_arena().descriptors.begin(), stdio_arena().descriptors.end(),
                [file_ptr](const auto& entry) { return entry.second.aliased_file == file_ptr; });
            if (!has_descriptor && it->second.host_std_fd < 0) {
                if (!memory_manager.invalid_parameter()) return;
            }
            const bool descriptor_closed = release_fd_for_stream(file_ptr);
            if (it->second.guest_buffer != 0) {
                memory_manager.crt_free(it->second.guest_buffer);
                it->second.guest_buffer = 0;
            }
            const bool ok = vfs.close(it->second.raw_handle).is_ok() && descriptor_closed;
            stdio_arena().files.erase(it);
            memory_manager.crt_free(file_ptr);
            VITREO_TRACE("[crt] fclose(0x%x) -> 0\n", file_ptr);
            if (!ok) memory_manager.set_errno(EBADF);
            m.set_return(ok ? 0u : 0xFFFFFFFFu);
        }});
}

/// tmpfile and the legacy _iob accessors.
void add_crt_stdio_tmpfile_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const crt_stdio_buffer::Layout file_layout = ctx.file_layout;
    const auto allocate_stream_object = [&memory_manager](Marshaler& m) -> u32 {
        return stdio_allocate_stream_object(memory_manager, m);
    };
    const auto initialize_guest_binary_buffer =
        [&memory_manager](Marshaler& m, u32 stream, VfsFile& file) {
            stdio_initialize_guest_binary_buffer(memory_manager, m, stream, file);
        };
    const auto bind_stream_descriptor =
        [&vfs, &memory_manager, &mgr](Marshaler& m, u32 stream, VfsFile& file) {
            return stdio_bind_stream_descriptor(vfs, memory_manager, mgr, m, stream, file);
        };

    exports.entries.push_back(
        {"tmpfile", 18,
         [&mgr, &vfs, allocate_stream_object,
          initialize_guest_binary_buffer, file_layout,
          bind_stream_descriptor](Marshaler& m) {
            (void)mgr;
            // Was a hard `return 0`, i.e. "out of temp files" -- callers that
            // check the result simply gave up, and callers that did not
            // dereferenced NULL. A tmpfile is an ordinary read/write stream in
            // the capsule's temp directory; the CRT deletes it on close, which
            // CreationDisposition::CreateAlways plus a unique name approximates
            // closely enough for a guest that only reads back what it wrote.
            static std::atomic<u32> s_counter{0};
            char name[64];
            std::snprintf(name, sizeof(name), "C:\\Temp\\vtmp%04x_%04x.tmp",
                          static_cast<unsigned>(
                              reinterpret_cast<uintptr_t>(&s_counter) & 0xFFFFu),
                          static_cast<unsigned>(s_counter.fetch_add(1) & 0xFFFFu));
            auto open_res = vfs.open(name, capsule::AccessMode::ReadWrite,
                                     capsule::ShareMode::ReadWrite,
                                     capsule::CreationDisposition::CreateAlways);
            if (open_res.is_err()) {
                VITREO_TRACE("[crt] tmpfile: could not create %s\n", name);
                m.set_return(0);
                return;
            }
            const u32 fake_ptr = allocate_stream_object(m);
            if (fake_ptr == 0) {
                (void)vfs.close(open_res.value());
                m.set_return(0);
                return;
            }
            VfsFile vf;
            vf.layout = file_layout;
            vf.raw_handle = open_res.value();
            vf.text_mode = false;          // tmpfile is always binary
            initialize_guest_binary_buffer(m, fake_ptr, vf);
            stdio_arena().files[fake_ptr] = vf;
            (void)bind_stream_descriptor(m, fake_ptr, vf);
            VITREO_TRACE("[crt] tmpfile -> 0x%x (%s)\n", fake_ptr, name);
            m.set_return(static_cast<u64>(fake_ptr));
        }});

    // FILE* __iob_func(void) — base of the legacy _iob[] array
    exports.entries.push_back(ExportEntry{
        "__iob_func", 0,
        [&crt_data](Marshaler& m) {
            m.set_return(crt_data.legacy_iob_addr(m.cpu_is_64bit()));
        }
    });

    // FILE* __p__iob(void) — msvcrt's own spelling of the same accessor, and
    // the one every mingw-built image calls before main to reach stdin/
    // stdout/stderr.
    exports.entries.push_back(ExportEntry{
        "__p__iob", 0,
        [&crt_data](Marshaler& m) {
            m.set_return(crt_data.legacy_iob_addr(m.cpu_is_64bit()));
        }
    });
}

} // namespace vitreo::winapi::crt_stdio_internal
