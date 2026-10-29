// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_lowio_descriptor_registration.cpp
/// @brief Descriptor <-> HANDLE/FILE exports: _setmode, _get_osfhandle, _isatty,
/// _open_osfhandle, _fileno, _fdopen, _locking.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/handle.hpp"
#include "core/memory/guest_memory.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace vitreo::winapi::crt_stdio_internal {

/// _locking, _setmode, _get_osfhandle, _isatty, _open_osfhandle, _fileno and
/// _fdopen.
void add_crt_lowio_descriptor_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const crt_stdio_buffer::Layout file_layout = ctx.file_layout;
    const auto alloc_fd = [&mgr]() -> i32 {
        return stdio_alloc_fd(mgr);
    };
    const auto bind_reused_standard = [&mgr](i32 fd) {
        stdio_bind_reused_standard(mgr, fd);
    };
    const auto allocate_stream_object = [&memory_manager](Marshaler& m) -> u32 {
        return stdio_allocate_stream_object(memory_manager, m);
    };
    const auto initialize_guest_binary_buffer =
        [&memory_manager](Marshaler& m, u32 stream, VfsFile& file) {
            stdio_initialize_guest_binary_buffer(memory_manager, m, stream, file);
        };
    const auto standard_stream_index = [&crt_data](GuestAddr stream) -> i32 {
        return stdio_standard_stream_index(crt_data, stream);
    };
    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // Descriptors 0/1/2 (not in `stdio_arena().descriptors`) record their translation mode in
    // stdio_arena().standard[fd].translation_mode, text by default.

    auto is_translation_mode = [](i32 mode) -> bool {
        return mode == O_TEXT_VALUE || mode == O_BINARY_VALUE ||
               mode == O_WTEXT_VALUE || mode == O_U16TEXT_VALUE ||
               mode == O_U8TEXT_VALUE;
    };
    auto apply_translation_mode = [](LowIoFd& entry, i32 mode) {
        entry.translation_mode = mode;
        entry.text_mode = mode != O_BINARY_VALUE;
    };

    // int _setmode(int fd, int mode) -> previous mode, or -1
    // Learn (_locking): byte-range lock on an open file descriptor. Vitreo runs
    // one guest process tree against its own capsule filesystem, where no
    // second writer can contend for these ranges, so every mode succeeds. The
    // one thing that must not happen is a failure return: the CRT uses
    // _locking around buffered stdio, and a caller that sees -1 concludes
    // another process holds the file.
    exports.entries.push_back({"_locking", 0, [set_errno](Marshaler& m) {
        const int fd = static_cast<int>(m.arg(0));
        if (fd < 0) {
            set_errno(m, EBADF_VALUE);
            m.set_return(static_cast<u64>(static_cast<u32>(-1)));
            return;
        }
        m.set_return(0);
    }});

    exports.entries.push_back({"_setmode", 0, [set_errno, &mgr, &memory_manager, is_translation_mode,
                                               apply_translation_mode](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        i32 mode = static_cast<i32>(m.arg(1));
        if (!is_translation_mode(mode)) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        // UCRT records `_O_U16TEXT` as `_O_WTEXT` (oracle crt_setmode_probe).
        if (mode == O_U16TEXT_VALUE) mode = O_WTEXT_VALUE;
        if (fd >= 0 && fd <= 2) {
            StdDescriptor& desc = std_descriptor(mgr, fd);
            if (desc.kind == StdKind::Closed) {
                if (!memory_manager.invalid_parameter()) return;
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            const i32 previous = desc.translation_mode;
            desc.translation_mode = mode;
            if (auto it = stdio_arena().descriptors.find(fd); it != stdio_arena().descriptors.end()) apply_translation_mode(it->second, mode);
            m.set_return(static_cast<u32>(previous));
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const i32 previous = it->second.translation_mode != 0
            ? it->second.translation_mode
            : (it->second.text_mode ? O_TEXT_VALUE : O_BINARY_VALUE);
        apply_translation_mode(it->second, mode);
        // Keep an aliased FILE* encoding in sync for wide stdio.
        if (it->second.aliased_file != 0) {
            auto fit = stdio_arena().files.find(it->second.aliased_file);
            if (fit != stdio_arena().files.end()) {
                fit->second.text_mode = mode != O_BINARY_VALUE;
                if (mode == O_U8TEXT_VALUE) {
                    fit->second.encoding = VfsFile::Encoding::Utf8;
                } else if (mode == O_U16TEXT_VALUE || mode == O_WTEXT_VALUE) {
                    fit->second.encoding = VfsFile::Encoding::Utf16Le;
                } else if (mode == O_TEXT_VALUE) {
                    fit->second.encoding = VfsFile::Encoding::Acp;
                } else {
                    fit->second.encoding = VfsFile::Encoding::None;
                }
            }
        }
        m.set_return(static_cast<u32>(previous));
    }});

    // intptr_t _get_osfhandle(int fd) — descriptor to OS handle
    //
    // Learn: returns the operating-system handle behind a valid fd; an invalid
    // fd invokes the invalid parameter handler, then returns
    // INVALID_HANDLE_VALUE with errno EBADF. The fd owns the handle: callers
    // must not CloseHandle it; _close (or fclose of its stream) releases it.
    // A descriptor opened here is a VFS file, so its HANDLE is created on the
    // first request -- owning for a descriptor that owns its file, non-owning
    // for one aliasing a FILE* stream -- and then stays stable.
    exports.entries.push_back({"_get_osfhandle", 0,
        [&mgr, &memory_manager, set_errno](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        if (fd >= 0 && fd <= 2) {
            // The standard handle the descriptor was bound to at start-up.
            // With none, -2 and errno untouched (Learn _get_osfhandle;
            // stdout_pipe_chain_probe bare mode).
            const StdDescriptor& desc = std_descriptor(mgr, fd);
            if (desc.kind == StdKind::Closed) {
                if (!memory_manager.invalid_parameter()) return;
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFFFFFFFFFULL);
                return;
            }
            if (desc.kind == StdKind::Kernel) {
                m.set_return(static_cast<u64>(desc.handle));
                return;
            }
            if (desc.kind == StdKind::None) {
                m.set_return(static_cast<u64>(static_cast<i64>(-2)));
                return;
            }
            const Handle h = desc.handle;
            if (h != NULL_HANDLE && h != INVALID_HANDLE) {
                m.set_return(static_cast<u64>(h));
                return;
            }
            // No kernel32 wiring (unit harness): the GetStdHandle selector.
            m.set_return(static_cast<u64>(static_cast<i64>(-10 - fd)));
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        FileManager* files = mgr.file_manager();
        if (it == stdio_arena().descriptors.end() || files == nullptr) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFFFFFFFFFULL);
            return;
        }
        LowIoFd& entry = it->second;
        if (entry.os_handle == 0) {
            const Handle adopted = files->adopt_vfs_handle(
                entry.handle, entry.path, entry.win32_access,
                /*owns_vfs=*/entry.owns_handle);
            if (adopted == INVALID_HANDLE) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFFFFFFFFFULL);
                return;
            }
            entry.os_handle = adopted;
        }
        m.set_return(static_cast<u64>(entry.os_handle));
    }});

    // int _isatty(int fd) — Learn: nonzero if fd is a character device.
    // Standard descriptors follow the host tty state (ssh redirect → 0);
    // VFS-backed descriptors are never character devices.
    exports.entries.push_back({"_isatty", 0, [set_errno, &mgr](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        if (fd < 0) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0);
            return;
        }
        if (fd >= 0 && fd <= 2 &&
            std_descriptor(mgr, fd).kind != StdKind::Console) {
            // A character device is a tty; a pipe or disk file is not. A
            // descriptor with no handle reports the device flag, 64
            // (stdout_pipe_chain_probe).
            const StdDescriptor& desc = std_descriptor(mgr, fd);
            constexpr u32 kFdev = 0x40;
            if (desc.kind == StdKind::Closed) { m.set_return(0); return; }
            if (desc.kind == StdKind::None) {
                m.set_return(kFdev);
                return;
            }
            FileManager* files = mgr.file_manager();
            m.set_return(files != nullptr && files->file_type(desc.handle) == 2
                             ? kFdev : 0u);
            return;
        }
        if (fd == 0 || fd == 1 || fd == 2) {
            const auto& desc = std_descriptor(mgr, fd);
            m.set_return(desc.handle != 0 ? 64u : (::isatty(fd) ? 1u : 0u));
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0);
            return;
        }
        m.set_return(0);
    }});

    // int _open_osfhandle(intptr_t osfhandle, int flags)
    exports.entries.push_back({"_open_osfhandle", 0, [&mgr, set_errno, alloc_fd, bind_reused_standard](Marshaler& m) {
        // Learn: allocate a descriptor for an existing handle; flags
        // _O_APPEND, _O_RDONLY, _O_TEXT, _O_WTEXT (binary otherwise).
        // Ownership of the handle passes to the descriptor: _close closes it.
        FileManager* files = mgr.file_manager();
        const u64 raw = m.arg(0);
        const Handle h = static_cast<Handle>(raw);
        const HandleType type =
            (files != nullptr && raw == static_cast<u64>(h))
                ? files->handle_table().get_type(h)
                : HandleType::Invalid;
        if (type == HandleType::Invalid) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const i32 flags = static_cast<i32>(m.arg32(1));
        LowIoFd entry;
        entry.os_handle = h;
        entry.owns_handle = true;
        const capsule::FileHandle vfs_file =
            type == HandleType::File ? files->vfs_handle_for(h)
                                     : capsule::INVALID_FILE_HANDLE;
        if (vfs_file != capsule::INVALID_FILE_HANDLE) {
            // The same file object as the HANDLE, so the two share a position.
            entry.handle = vfs_file;
            entry.path = files->path_for_handle(h);
        } else {
            entry.kernel_io = true;
        }
        if ((flags & O_WTEXT_VALUE) != 0) {
            entry.text_mode = true;
            entry.translation_mode = O_WTEXT_VALUE;
        } else if ((flags & O_TEXT_VALUE) != 0) {
            entry.text_mode = true;
            entry.translation_mode = O_TEXT_VALUE;
        } else {
            entry.text_mode = false;
            entry.translation_mode = O_BINARY_VALUE;
        }
        entry.append = (flags & O_APPEND_VALUE) != 0;
        entry.noinherit = (flags & O_NOINHERIT_VALUE) != 0;
        const i32 fd = alloc_fd();
        if (fd < 0) { set_errno(m, EMFILE_LOWIO); m.set_return(0xFFFFFFFFu); return; }
        stdio_arena().descriptors[fd] = std::move(entry);
        bind_reused_standard(fd);
        VITREO_TRACE("[crt] _open_osfhandle(0x%x, 0x%x) -> fd %d%s\n", h, flags,
                     fd, vfs_file == capsule::INVALID_FILE_HANDLE ? " (kernel I/O)"
                                                                 : "");
        m.set_return(static_cast<u32>(fd));
    }});

    // int _fileno(FILE* stream)
    //
    // The FILE and descriptor refer to the same underlying handle. Low-level
    // close releases the handle without freeing the FILE object.
    exports.entries.push_back({"_fileno", 0,
        [&vfs, &mgr, standard_stream_index, alloc_fd, bind_reused_standard, set_errno,
         &memory_manager](Marshaler& m) {
            const GuestAddr stream = m.arg_ptr(0);
            if (stream == 0) {
                if (!memory_manager.invalid_parameter()) return;
                set_errno(m, EINVAL_LOWIO);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            const auto fp = static_cast<u32>(stream);
            const i32 std_idx = standard_stream_index(fp);
            if (std_idx >= 0 && std_idx <= 2 && !std_descriptor(mgr, std_idx).file_closed) {
                // stdin/stdout/stderr are descriptors 0-2, or -2 when the
                // process has no such standard handle (Learn _fileno).
                const StdKind kind = std_descriptor(mgr, std_idx).kind;
                m.set_return(static_cast<u32>(kind == StdKind::None ? -2 : std_idx));
                return;
            }
            for (const auto& [fd, entry] : stdio_arena().descriptors) {
                if (entry.aliased_file == fp) {
                    m.set_return(static_cast<u32>(fd));
                    return;
                }
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            if (it->second.host_std_fd >= 0) {
                m.set_return(static_cast<u32>(it->second.host_std_fd));
                return;
            }
            LowIoFd entry;
            entry.handle = stream_handle(vfs, it->second);
            entry.text_mode = it->second.text_mode;
            entry.owns_handle = false;
            entry.aliased_file = fp;
            const i32 fd = alloc_fd();
            if (fd < 0) { set_errno(m, EMFILE_LOWIO); m.set_return(0xFFFFFFFFu); return; }
            stdio_arena().descriptors[fd] = entry;
            bind_reused_standard(fd);
            m.set_return(static_cast<u32>(fd));
        }});

    // FILE* _fdopen(int fd, const char* mode) — wrap a descriptor in a stream
    exports.entries.push_back({"_fdopen", 0,
        [allocate_stream_object, initialize_guest_binary_buffer,
         file_layout, set_errno](Marshaler& m) {
        const auto fd = static_cast<i32>(m.arg(0));
        auto mode = m.read_ansi_string(m.arg_ptr(1));
        if (mode.is_err() || mode.value().empty()) {
            set_errno(m, EINVAL_LOWIO);
            m.set_return(0);
            return;
        }
        // fds 0-2: new FILE* routing through host std (crt_fdopen_std_probe).
        if (fd >= 0 && fd <= 2) {
            const u32 fake_ptr = allocate_stream_object(m);
            if (fake_ptr == 0) {
                set_errno(m, 12);
                m.set_return(0);
                return;
            }
            VfsFile stream;
            stream.layout = file_layout;
            stream.host_std_fd = fd;
            stream.text_mode = mode.value().find('b') == std::string::npos;
            stream.append = mode.value().find('a') != std::string::npos;
            stdio_arena().files[fake_ptr] = std::move(stream);
            m.set_return(static_cast<u64>(fake_ptr));
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            set_errno(m, EBADF_VALUE);
            m.set_return(0);
            return;
        }
        VfsFile stream;
        stream.layout = file_layout;
        stream.raw_handle = it->second.handle;
        stream.text_mode = mode.value().find('b') == std::string::npos;
        stream.append = mode.value().find('a') != std::string::npos ||
                        it->second.append;
        const u32 fake_ptr = allocate_stream_object(m);
        if (fake_ptr == 0) {
            set_errno(m, 12);
            m.set_return(0);
            return;
        }
        initialize_guest_binary_buffer(m, fake_ptr, stream);
        stdio_arena().files[fake_ptr] = stream;
        it->second.owns_handle = false;
        it->second.aliased_file = fake_ptr;
        m.set_return(static_cast<u64>(fake_ptr));
    }});
}

} // namespace vitreo::winapi::crt_stdio_internal
