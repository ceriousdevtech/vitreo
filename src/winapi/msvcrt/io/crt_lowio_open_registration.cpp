// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_lowio_open_registration.cpp
/// @brief Low-level open, close and dup: _open, _sopen*, _close, _dup, _dup2.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/handle.hpp"
#include "core/memory/guest_memory.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <string>

namespace vitreo::winapi::crt_stdio_internal {

namespace {

// pmode bits, read only by _O_CREAT handling and the _s forms' validation.
constexpr i32 S_IREAD_VALUE  = 0x0100;
constexpr i32 S_IWRITE_VALUE = 0x0080;

} // namespace

/// _open, _sopen, _sopen_s and their wide and _dispatch forms.
void add_crt_lowio_open_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto alloc_fd = [&mgr]() -> i32 {
        return stdio_alloc_fd(mgr);
    };
    const auto bind_reused_standard = [&mgr](i32 fd) {
        stdio_bind_reused_standard(mgr, fd);
    };

    auto vfs_open_errno = [](capsule::VfsError err) -> u32 {
        using capsule::VfsError;
        switch (err) {
        case VfsError::PathAlreadyExists:
            return EEXIST_LOWIO;
        case VfsError::AccessDenied:
        case VfsError::SharingViolation:
        case VfsError::NotAFile:          // directory → EACCES per Learn _open
            return EACCES_LOWIO;
        case VfsError::TooManyOpenFiles:
            return EMFILE_LOWIO;
        case VfsError::InvalidArgument:
            return EINVAL_LOWIO;
        case VfsError::PathNotFound:
        case VfsError::FileNotFound:
        case VfsError::DriveNotMounted:
        default:
            return ENOENT_LOWIO;
        }
    };

    // ---- _open / _sopen / _sopen_s and the wide forms --------------------------
    //
    // One implementation of the UCRT's _wsopen_nolock (ucrt/lowio/open.cpp):
    // _open is _sopen with _SH_DENYNO, _sopen passes its sharing flag through,
    // and only the _s forms (secure) validate pmode. pmode matters only with
    // _O_CREAT, where a mode without _S_IWRITE makes the new file read-only
    // (umask 0: _umask is not exported). Failures that are parameter
    // validations (_VALIDATE_RETURN) run the invalid-parameter handler; I/O
    // failures only set errno.
    constexpr i32 SH_DENYRW = 0x10, SH_DENYWR = 0x20, SH_DENYRD = 0x30,
                  SH_DENYNO = 0x40, SH_SECURE = 0x80;
    auto sopen_core = [&vfs, &memory_manager, alloc_fd, bind_reused_standard, vfs_open_errno, &crt_data](
                          Marshaler& m, GuestAddr path_ptr, bool wide, i32 oflag,
                          i32 shflag, i32 pmode, bool secure, i32& fd_out) -> u32 {
        fd_out = -1;
        auto invalid = [&]() -> u32 {
            memory_manager.set_errno(EINVAL_LOWIO);
            (void)memory_manager.invalid_parameter();
            return EINVAL_LOWIO;
        };
        if (path_ptr == 0) return invalid();
        if (secure && (pmode & ~(S_IREAD_VALUE | S_IWRITE_VALUE)) != 0) return invalid();
        using capsule::AccessMode;
        using capsule::CreationDisposition;
        using capsule::ShareMode;
        AccessMode access = AccessMode::Read;
        switch (oflag & (O_WRONLY_VALUE | O_RDWR_VALUE)) {
        case O_RDONLY_VALUE: access = AccessMode::Read; break;
        case O_WRONLY_VALUE:
            // Append to a Unicode-text file also reads, to find the BOM.
            access = ((oflag & O_APPEND_VALUE) != 0 &&
                      (oflag & (O_WTEXT_VALUE | O_U16TEXT_VALUE | O_U8TEXT_VALUE)) != 0)
                         ? AccessMode::ReadWrite : AccessMode::Write;
            break;
        case O_RDWR_VALUE: access = AccessMode::ReadWrite; break;
        default:
            // "Invalid open flag". The UCRT reports it and then carries on
            // with an access mask of 0xFFFFFFFF, which CreateFileW refuses, so
            // the caller sees EACCES; msvcrt.dll stops at EINVAL (measured,
            // crt_sopen_probe bad_access).
            if (CrtDllFlavorScope::current() != CrtDllFlavor::Msvcrt) {
                (void)invalid();
                memory_manager.set_errno(EACCES_LOWIO);
                return EACCES_LOWIO;
            }
            return invalid();
        }
        ShareMode share = ShareMode::ReadWrite;
        switch (shflag) {
        case SH_DENYRW: share = ShareMode::None; break;
        case SH_DENYWR: share = ShareMode::Read; break;
        case SH_DENYRD: share = ShareMode::Write; break;
        case SH_DENYNO: share = ShareMode::ReadWrite; break;
        case SH_SECURE:
            share = access == AccessMode::Read ? ShareMode::Read : ShareMode::None;
            break;
        default: return invalid();  // "Invalid sharing flag"
        }
        CreationDisposition disposition = CreationDisposition::OpenExisting;
        switch (oflag & (O_CREAT_VALUE | O_EXCL_VALUE | O_TRUNC_VALUE)) {
        case O_CREAT_VALUE: disposition = CreationDisposition::OpenAlways; break;
        case O_CREAT_VALUE | O_EXCL_VALUE:
        case O_CREAT_VALUE | O_TRUNC_VALUE | O_EXCL_VALUE:
            disposition = CreationDisposition::CreateNew; break;
        case O_TRUNC_VALUE:
        case O_TRUNC_VALUE | O_EXCL_VALUE:
            disposition = CreationDisposition::TruncateExisting; break;
        case O_CREAT_VALUE | O_TRUNC_VALUE:
            disposition = CreationDisposition::CreateAlways; break;
        default: break;  // _O_EXCL without _O_CREAT is ignored
        }
        auto path = wide ? m.read_wide_string(path_ptr) : m.read_ansi_string(path_ptr);
        if (path.is_err()) {
            memory_manager.set_errno(ENOENT_LOWIO);
            return ENOENT_LOWIO;
        }
        const i32 fd = alloc_fd();
        if (fd < 0) {
            memory_manager.set_errno(EMFILE_LOWIO);
            return EMFILE_LOWIO;
        }
        auto opened = vfs.open(path.value(), access, share, disposition);
        if (opened.is_err()) {
            const u32 err = vfs_open_errno(opened.error());
            memory_manager.set_errno(err);
            return err;
        }
        LowIoFd entry;
        entry.handle = opened.value();
        entry.path = path.value();
        entry.win32_access =
            (capsule::has_access(access, AccessMode::Read) ? 0x80000000u : 0u) |
            (capsule::has_access(access, AccessMode::Write) ? 0x40000000u : 0u);
        // is_text_mode: explicit _O_BINARY, then the text/Unicode flags, then
        // the process `_fmode`.
        if ((oflag & O_BINARY_VALUE) != 0) {
            entry.text_mode = false;
            entry.translation_mode = O_BINARY_VALUE;
        } else if ((oflag & O_U8TEXT_VALUE) != 0) {
            entry.text_mode = true;
            entry.translation_mode = O_U8TEXT_VALUE;
        } else if ((oflag & (O_U16TEXT_VALUE | O_WTEXT_VALUE)) != 0) {
            entry.text_mode = true;
            entry.translation_mode = O_WTEXT_VALUE;
        } else if ((oflag & O_TEXT_VALUE) != 0) {
            entry.text_mode = true;
            entry.translation_mode = O_TEXT_VALUE;
        } else {
            const u32 fmode = m.memory().read_u32(crt_data.fmode_addr()).value_or(0);
            if (fmode == static_cast<u32>(O_BINARY_VALUE)) {
                entry.text_mode = false;
                entry.translation_mode = O_BINARY_VALUE;
            } else if (fmode == static_cast<u32>(O_WTEXT_VALUE)) {
                entry.text_mode = true;
                entry.translation_mode = O_WTEXT_VALUE;
            } else {
                entry.text_mode = true;
                entry.translation_mode = O_TEXT_VALUE;
            }
        }
        entry.append = (oflag & O_APPEND_VALUE) != 0;
        if (entry.append) (void)vfs.seek(entry.handle, 0, capsule::SeekOrigin::End);
        entry.temporary = (oflag & O_TEMPORARY_VALUE) != 0;
        if (entry.temporary) entry.delete_path = path.value();
        entry.noinherit = (oflag & O_NOINHERIT_VALUE) != 0;
        entry.short_lived = (oflag & O_SHORT_LIVED_VALUE) != 0;
        if ((oflag & O_CREAT_VALUE) != 0 && (pmode & S_IWRITE_VALUE) == 0) {
            (void)vfs.set_attributes(
                path.value(), static_cast<u32>(capsule::FileAttributes::ReadOnly));
        }
        stdio_arena().descriptors[fd] = std::move(entry);
        bind_reused_standard(fd);
        VITREO_TRACE("[crt] _sopen(%s, 0x%x, 0x%x) -> fd %d\n",
                     path.value().c_str(), oflag, shflag, fd);
        fd_out = fd;
        return 0;
    };
    // int _open(const char* path, int oflag, ...) / _wopen: pmode is read
    // from the varargs, as the UCRT does.
    for (const bool wide : {false, true}) {
        exports.entries.push_back({wide ? "_wopen" : "_open", 0,
            [sopen_core, wide](Marshaler& m) {
                i32 fd = -1;
                const u32 err = sopen_core(m, m.arg_ptr(0), wide,
                                           static_cast<i32>(m.arg(1)), SH_DENYNO,
                                           static_cast<i32>(m.arg(2)), false, fd);
                m.set_return(err != 0 ? 0xFFFFFFFFu : static_cast<u32>(fd));
            }});
        // int _sopen(const char* path, int oflag, int shflag, ...)
        exports.entries.push_back({wide ? "_wsopen" : "_sopen", 0,
            [sopen_core, wide](Marshaler& m) {
                i32 fd = -1;
                const u32 err = sopen_core(m, m.arg_ptr(0), wide,
                                           static_cast<i32>(m.arg(1)),
                                           static_cast<i32>(m.arg(2)),
                                           static_cast<i32>(m.arg(3)), false, fd);
                m.set_return(err != 0 ? 0xFFFFFFFFu : static_cast<u32>(fd));
            }});
        // errno_t _sopen_s(int* pfh, const char* path, int oflag, int shflag,
        //                  int pmode)
        // errno_t _sopen_dispatch(const char* path, int oflag, int shflag,
        //                         int pmode, int* pfh, int secure)  (UCRT)
        for (const bool dispatch : {false, true}) {
            const char* name = dispatch ? (wide ? "_wsopen_dispatch" : "_sopen_dispatch")
                                        : (wide ? "_wsopen_s" : "_sopen_s");
            exports.entries.push_back({name, 0,
                [sopen_core, wide, dispatch, &memory_manager](Marshaler& m) {
                    const usize p = dispatch ? 0 : 1;
                    const GuestAddr pfh = dispatch ? m.arg_ptr(4) : m.arg_ptr(0);
                    const bool secure = dispatch ? static_cast<u32>(m.arg(5)) != 0 : true;
                    if (pfh == 0) {
                        memory_manager.set_errno(EINVAL_LOWIO);
                        (void)memory_manager.invalid_parameter();
                        m.set_return(EINVAL_LOWIO);
                        return;
                    }
                    (void)m.write_u32(pfh, 0xFFFFFFFFu);
                    i32 fd = -1;
                    const u32 err = sopen_core(m, m.arg_ptr(p), wide,
                                               static_cast<i32>(m.arg(p + 1)),
                                               static_cast<i32>(m.arg(p + 2)),
                                               static_cast<i32>(m.arg(p + 3)),
                                               secure, fd);
                    (void)m.write_u32(pfh, static_cast<u32>(err != 0 ? -1 : fd));
                    m.set_return(err);
                }});
        }
    }
}

/// _close, _dup and _dup2.
void add_crt_lowio_close_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    const auto alloc_fd = [&mgr]() -> i32 {
        return stdio_alloc_fd(mgr);
    };
    const auto bind_reused_standard = [&mgr](i32 fd) {
        stdio_bind_reused_standard(mgr, fd);
    };
    auto set_errno = [&memory_manager](Marshaler& m, u32 value) {
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    // int _close(int fd)
    const auto close_lowio = [&vfs, &mgr, &memory_manager, set_errno](Marshaler& m, i32 fd) {
        if (fd >= 0 && fd <= 2) {
            StdDescriptor& desc = std_descriptor(mgr, fd);
            if (desc.kind == StdKind::Closed) {
                if (!memory_manager.invalid_parameter()) return;
                set_errno(m, EBADF_VALUE);
                m.set_return(0xFFFFFFFFu);
                return;
            }
            bool ok = true;
            if (desc.handle != 0) {
                // Learn _close: the descriptor's HANDLE is closed with it.
                if (FileManager* files = mgr.file_manager()) {
                    ok = files->close_file(desc.handle);
                }
            }
            auto alias = stdio_arena().descriptors.find(fd);
            if (alias != stdio_arena().descriptors.end()) {
                if (!alias->second.owns_handle && alias->second.aliased_file != 0) {
                    ok = vfs.close(alias->second.handle).is_ok() && ok;
                }
                if (alias->second.temporary && !alias->second.delete_path.empty()) {
                    (void)vfs.remove(alias->second.delete_path);
                }
            }
            // Low-level close does not close or flush the FILE object. Its
            // descriptor number survives, but the OS standard slot is null.
            if (ConsoleManager* console = mgr.console_manager()) {
                (void)console->set_std_handle(STD_INPUT_HANDLE - static_cast<u32>(fd), NULL_HANDLE);
            }
            desc.handle = 0;
            desc.kind = StdKind::Closed;
            stdio_arena().descriptors.erase(fd);
            if (!ok) set_errno(m, EBADF_VALUE);
            m.set_return(ok ? 0u : 0xFFFFFFFFu);
            return;
        }
        auto it = stdio_arena().descriptors.find(fd);
        if (it == stdio_arena().descriptors.end()) {
            if (!memory_manager.invalid_parameter()) return;
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const bool temporary = it->second.temporary;
        const std::string delete_path = it->second.delete_path;
        bool ok = true;
        if (it->second.os_handle != 0) {
            // Learn _close / _open_osfhandle: closing the descriptor closes the
            // HANDLE it owns, and that handle releases the file.
            if (mgr.file_manager() != nullptr) {
                ok = mgr.file_manager()->close_file(it->second.os_handle);
                if (!it->second.owns_handle && it->second.aliased_file != 0) {
                    ok = vfs.close(it->second.handle).is_ok() && ok;
                }
            }
        } else {
            ok = vfs.close(it->second.handle).is_ok();
        }
        stdio_arena().descriptors.erase(it);
        if (temporary && !delete_path.empty()) {
            (void)vfs.remove(delete_path);
        }
        if (!ok) set_errno(m, EBADF_VALUE);
        m.set_return(ok ? 0u : 0xFFFFFFFFu);
    };
    exports.entries.push_back({"_close", 0, [close_lowio](Marshaler& m) {
        close_lowio(m, static_cast<i32>(m.arg(0)));
    }});

    // Learn _dup/_dup2 and crt_dup_probe.expected. Duplication owns a new
    // inheritable HANDLE with the same access and shared file position. EOF
    // and translation modes are copied; unread pipe lookahead is not.
    const auto duplicate_lowio = [&mgr, &memory_manager, set_errno, alloc_fd,
                                  bind_reused_standard, close_lowio](Marshaler& m, bool forced) {
        const i32 source = static_cast<i32>(m.arg(0));
        const i32 requested = forced ? static_cast<i32>(m.arg(1)) : -1;
        const bool standard = source >= 0 && source <= 2;
        const auto bad_descriptor = [&](bool handler) {
            if (handler && !memory_manager.invalid_parameter()) return;
            set_errno(m, EBADF_VALUE);
            m.set_return(0xFFFFFFFFu);
        };
        if (source == -2) { bad_descriptor(false); return; }
        if (source < 0 || source >= lowio_limit() || (standard
                ? std_descriptor(mgr, source).kind == StdKind::Closed
                : !stdio_arena().descriptors.contains(source))) {
            bad_descriptor(true); return;
        }
        if (forced && requested == -2) { bad_descriptor(false); return; }
        if (forced && (requested < 0 || requested >= lowio_limit())) {
            bad_descriptor(true); return;
        }
        if (forced && requested == source) { m.set_return(0); return; }
        const i32 target = forced ? requested : alloc_fd();
        if (target < 0) { set_errno(m, EMFILE_LOWIO); m.set_return(0xFFFFFFFFu); return; }
        FileManager* files = mgr.file_manager();
        LowIoFd original;
        if (auto found = stdio_arena().descriptors.find(source);
            found != stdio_arena().descriptors.end()) original = found->second;
        if (standard) {
            const auto& descriptor = std_descriptor(mgr, source);
            // Native no-console slots contain -2, the current-thread pseudo
            // handle. DuplicateHandle produces a real thread handle for it.
            original.os_handle = descriptor.kind == StdKind::None
                ? ThreadManager::get_current_thread() : descriptor.handle;
            original.translation_mode = descriptor.translation_mode;
            original.text_mode = descriptor.translation_mode != kStdModeBinary;
            original.text_eof = descriptor.text_eof;
        }
        if (files == nullptr) {
            static std::once_flag logged;
            std::call_once(logged, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ucrtbase!_dup: descriptor without kernel handle wiring\n");
            });
            bad_descriptor(false); return;
        }
        if (!standard && original.os_handle == 0) {
            const Handle adopted = files->adopt_vfs_handle(original.handle, original.path,
                original.win32_access, original.owns_handle);
            if (adopted == INVALID_HANDLE) { bad_descriptor(false); return; }
            stdio_arena().descriptors.at(source).os_handle = adopted;
            original.os_handle = adopted;
        }
        const HandleType type = files->handle_table().get_type(original.os_handle);
        const u32 access = original.os_handle == ThreadManager::get_current_thread()
            ? HandleAccess::All : files->handle_table().get_access(original.os_handle);
        Handle handle = NULL_HANDLE;
        if (type == HandleType::Thread || original.os_handle == ThreadManager::get_current_thread()) {
            if (auto* threads = mgr.thread_manager())
                handle = threads->duplicate_thread_handle(original.os_handle, HandleFlags::Inherit, access);
        } else if (type == HandleType::ConsoleInput || type == HandleType::ConsoleOutput) {
            if (auto* console = mgr.console_manager())
                handle = console->duplicate_handle(original.os_handle, HandleFlags::Inherit, access);
        } else handle = files->duplicate_handle(original.os_handle, HandleFlags::Inherit, access);
        if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
            bad_descriptor(false); return;
        }
        if (forced && (target <= 2
                ? (std_descriptor(mgr, target).kind != StdKind::Closed &&
                   std_descriptor(mgr, target).kind != StdKind::None)
                : stdio_arena().descriptors.contains(target))) {
            close_lowio(m, target);
        }
        LowIoFd copy = original;
        copy.os_handle = handle;
        copy.handle = files->vfs_handle_for(handle);
        copy.kernel_io = copy.handle == capsule::INVALID_FILE_HANDLE;
        copy.owns_handle = true;
        copy.aliased_file = 0;
        copy.noinherit = false;
        copy.pushback_valid = false;
        copy.pending_cr = false;
        copy.unicode_lookahead.reset();
        stdio_arena().descriptors[target] = std::move(copy);
        bind_reused_standard(target);
        if (target <= 2) std_descriptor(mgr, target).text_eof = original.text_eof;
        m.set_return(forced ? 0u : static_cast<u32>(target));
    };
    exports.entries.push_back({"_dup", 0, [duplicate_lowio](Marshaler& m) { duplicate_lowio(m, false); }});
    exports.entries.push_back({"_dup2", 0, [duplicate_lowio](Marshaler& m) { duplicate_lowio(m, true); }});
}

} // namespace vitreo::winapi::crt_stdio_internal
