// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_open_registration.cpp
/// @brief Stream open family: fopen, _fsopen, fopen_s, _wfopen*, freopen.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "core/memory/guest_memory.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>

namespace vitreo::winapi::crt_stdio_internal {

/// fopen, _fsopen, fopen_s and the wide forms, plus _lock_file/_unlock_file.
void add_crt_stdio_open_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    const crt_stdio_buffer::Layout file_layout = ctx.file_layout;
    const auto alloc_fd_early = [&mgr]() -> i32 {
        return stdio_alloc_fd(mgr);
    };
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

    // Open a stream and register a guest-visible FILE*. Returns 0 on failure.
    // `share` is the _SH_DENY* flag from _fsopen; the plain fopen family passes
    // _SH_DENYNO (0x40). On failure, `*out_errno` is set when non-null; a
    // return of 0 with *out_errno==EINVAL may also need the invalid-parameter
    // handler (caller decides via `*out_ip`).
    auto open_stream = [&vfs, &memory_manager, alloc_fd_early, allocate_stream_object,
                        initialize_guest_binary_buffer, file_layout,
                        bind_stream_descriptor](
                           Marshaler& m, const std::string& path,
                           const std::string& mode, i32 share,
                           u32* out_errno = nullptr,
                           bool* out_ip = nullptr) -> u32 {
        using capsule::AccessMode;
        using capsule::CreationDisposition;
        using capsule::ShareMode;

        if (out_errno) *out_errno = 0;
        if (out_ip) *out_ip = false;

        // Skip leading whitespace (UCRT accepts " r").
        usize i = 0;
        while (i < mode.size() &&
               (mode[i] == ' ' || mode[i] == '\t')) ++i;
        if (i >= mode.size()) {
            if (out_errno) *out_errno = 22;
            if (out_ip) *out_ip = true;
            return 0;
        }

        const char access_ch = mode[i];
        if (access_ch != 'r' && access_ch != 'w' && access_ch != 'a') {
            if (out_errno) *out_errno = 22;
            if (out_ip) *out_ip = true;
            return 0;
        }
        ++i;
        bool has_plus = false;
        bool has_b = false;
        bool has_t = false;
        bool has_x = false;
        // Optional modifiers after the access letter: + b t x and CRT
        // extensions T/D/N/S (ignored for open semantics but accepted).
        while (i < mode.size()) {
            const char c = mode[i];
            if (c == '+') { has_plus = true; ++i; continue; }
            if (c == 'b') { has_b = true; ++i; continue; }
            if (c == 't') { has_t = true; ++i; continue; }
            if (c == 'x') { has_x = true; ++i; continue; }
            if (c == 'T' || c == 'D' || c == 'N' || c == 'S' ||
                c == 'c' /* ccs= handled below */) {
                // Temporary-file / commit / noinherit / sequential — accepted.
                if (c == 'c') {
                    // ", ccs=..." or "ccs=..." — skip until end or next comma
                    // for BOGUS detection below.
                    break;
                }
                ++i;
                continue;
            }
            if (c == ',' || c == ' ') { ++i; continue; }
            // Unexpected character (e.g. second access letter in "rw").
            if (out_errno) *out_errno = 22;
            if (out_ip) *out_ip = true;
            return 0;
        }
        if (has_b && has_t) {
            if (out_errno) *out_errno = 22;
            if (out_ip) *out_ip = true;
            return 0;
        }
        // 'x' (exclusive create) is only valid with 'w' or 'a'.
        if (has_x && access_ch == 'r') {
            if (out_errno) *out_errno = 22;
            if (out_ip) *out_ip = true;
            return 0;
        }

        // Optional ", ccs=ENCODING" — accept known encodings; reject BOGUS.
        // UTF-8 / UTF-16LE / UNICODE select the stream translation used by
        // fwprintf/fgetwc (Learn fopen ccs= and _O_U8TEXT/_O_U16TEXT).
        VfsFile::Encoding ccs_encoding = VfsFile::Encoding::None;
        const auto ccs_pos = mode.find("ccs=");
        if (ccs_pos != std::string::npos) {
            std::string enc;
            for (usize j = ccs_pos + 4; j < mode.size(); ++j) {
                if (mode[j] == ',' || mode[j] == ' ') break;
                enc.push_back(static_cast<char>(
                    mode[j] >= 'A' && mode[j] <= 'Z' ? mode[j] + 32 : mode[j]));
            }
            if (enc == "utf-8") {
                ccs_encoding = VfsFile::Encoding::Utf8;
            } else if (enc == "utf-16le" || enc == "unicode") {
                ccs_encoding = VfsFile::Encoding::Utf16Le;
            } else {
                if (out_errno) *out_errno = 22;
                if (out_ip) *out_ip = true;
                return 0;
            }
        }

        AccessMode access = AccessMode::Read;
        CreationDisposition disp = CreationDisposition::OpenExisting;
        if (access_ch == 'w') {
            access = has_plus ? AccessMode::ReadWrite : AccessMode::Write;
            disp = has_x ? CreationDisposition::CreateNew
                         : CreationDisposition::CreateAlways;
        } else if (access_ch == 'a') {
            access = has_plus ? AccessMode::ReadWrite : AccessMode::Write;
            disp = has_x ? CreationDisposition::CreateNew
                         : CreationDisposition::OpenAlways;
        } else { // 'r'
            access = has_plus ? AccessMode::ReadWrite : AccessMode::Read;
            disp = CreationDisposition::OpenExisting;
        }

        // _SH_DENYRW=0x10, _SH_DENYWR=0x20, _SH_DENYRD=0x30, _SH_DENYNO=0x40
        ShareMode share_mode = ShareMode::ReadWrite;
        switch (share) {
            case 0x10: share_mode = ShareMode::None;      break;
            case 0x20: share_mode = ShareMode::Read;      break;
            case 0x30: share_mode = ShareMode::Write;     break;
            case 0x40: share_mode = ShareMode::ReadWrite; break;
            default:   share_mode = ShareMode::ReadWrite; break;
        }

        if (alloc_fd_early() < 0) {
            memory_manager.set_errno(24);
            if (out_errno) *out_errno = 24;
            return 0;
        }
        auto open_res = vfs.open(path, access, share_mode, disp);
        if (open_res.is_err()) {
            if (out_errno) {
                using capsule::VfsError;
                switch (open_res.error()) {
                    case VfsError::PathAlreadyExists: *out_errno = 17; break;
                    case VfsError::FileNotFound:
                    case VfsError::PathNotFound: *out_errno = 2; break;
                    case VfsError::AccessDenied:
                    case VfsError::SharingViolation: *out_errno = 13; break;
                    case VfsError::OutOfMemory: *out_errno = 12; break;
                    default: *out_errno = 5; break;
                }
            }
            return 0;
        }

        const u32 fake_ptr = allocate_stream_object(m);
        if (fake_ptr == 0) {
            (void)vfs.close(open_res.value());
            if (out_errno) *out_errno = 12;
            return 0;
        }
        VfsFile vf;
        vf.layout = file_layout;
        vf.raw_handle = open_res.value();
        vf.text_mode = !has_b || ccs_encoding != VfsFile::Encoding::None;
        if (ccs_encoding != VfsFile::Encoding::None) {
            vf.encoding = ccs_encoding;
        } else if (vf.text_mode) {
            vf.encoding = VfsFile::Encoding::Acp;
        } else {
            vf.encoding = VfsFile::Encoding::None;
        }
        vf.append = (access_ch == 'a');
        vf.access = access;
        // ccs= write modes start with a BOM (Learn fopen / Unicode stream I/O).
        if (ccs_encoding != VfsFile::Encoding::None &&
            (access_ch == 'w' || access_ch == 'a')) {
            bool need_bom = (access_ch == 'w');
            if (access_ch == 'a') {
                auto end = vfs.seek(vf.raw_handle, 0, capsule::SeekOrigin::End);
                need_bom = end.is_ok() && end.value() == 0;
                (void)vfs.seek(vf.raw_handle, 0, capsule::SeekOrigin::Begin);
            }
            if (need_bom) {
                if (ccs_encoding == VfsFile::Encoding::Utf8) {
                    static constexpr u8 kBom8[] = {0xEF, 0xBB, 0xBF};
                    (void)vfs.write(vf.raw_handle, std::span<const u8>(kBom8, 3));
                } else if (ccs_encoding == VfsFile::Encoding::Utf16Le) {
                    static constexpr u8 kBom16[] = {0xFF, 0xFE};
                    (void)vfs.write(vf.raw_handle, std::span<const u8>(kBom16, 2));
                }
            }
        }
        initialize_guest_binary_buffer(m, fake_ptr, vf);
        stdio_arena().files[fake_ptr] = vf;
        (void)bind_stream_descriptor(m, fake_ptr, vf);
        return fake_ptr;
    };

    // FILE* _fsopen(const char* path, const char* mode, int shflag)
    exports.entries.push_back(
        {"_fsopen", 0, [open_stream](Marshaler& m) {
            auto path_res = m.read_ansi_string(m.arg_ptr(0));
            auto mode_res = m.read_ansi_string(m.arg_ptr(1));
            if (path_res.is_err() || mode_res.is_err()) {
                m.set_return(0);
                return;
            }
            const u32 file = open_stream(m, path_res.value(), mode_res.value(),
                                         static_cast<i32>(m.arg(2)));
            VITREO_TRACE("[crt] _fsopen(%s, %s) -> 0x%x\n",
                         path_res.value().c_str(), mode_res.value().c_str(),
                         file);
            m.set_return(static_cast<u64>(file));
        }});

    // FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int shflag)
    exports.entries.push_back(
        {"_wfsopen", 0, [open_stream](Marshaler& m) {
            auto path_res = m.read_wide_string(m.arg_ptr(0));
            auto mode_res = m.read_wide_string(m.arg_ptr(1));
            if (path_res.is_err() || mode_res.is_err()) {
                m.set_return(0);
                return;
            }
            const u32 file = open_stream(m, path_res.value(), mode_res.value(),
                                         static_cast<i32>(m.arg(2)));
            VITREO_TRACE("[crt] _wfsopen(%s, %s) -> 0x%x\n",
                         path_res.value().c_str(), mode_res.value().c_str(),
                         file);
            m.set_return(static_cast<u64>(file));
        }});

    // errno_t fopen_s(FILE** out, const char* path, const char* mode)
    exports.entries.push_back(
        {"fopen_s", 0, [open_stream](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            constexpr u32 ENOENT_VALUE = 2;
            const GuestAddr out = m.arg_ptr(0);
            auto path_res = m.read_ansi_string(m.arg_ptr(1));
            auto mode_res = m.read_ansi_string(m.arg_ptr(2));
            if (out == 0 || path_res.is_err() || mode_res.is_err()) {
                if (out != 0) (void)m.write_ptr(out, 0);
                m.set_return(EINVAL_VALUE);
                return;
            }
            // fopen_s always opens with exclusive access (_SH_DENYRW).
            const u32 file = open_stream(m, path_res.value(), mode_res.value(),
                                         0x10);
            (void)m.write_ptr(out, file);
            m.set_return(file != 0 ? 0u : ENOENT_VALUE);
        }});

    // errno_t _wfopen_s(FILE** out, const wchar_t* path, const wchar_t* mode)
    exports.entries.push_back(
        {"_wfopen_s", 0, [open_stream](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            constexpr u32 ENOENT_VALUE = 2;
            const GuestAddr out = m.arg_ptr(0);
            auto path_res = m.read_wide_string(m.arg_ptr(1));
            auto mode_res = m.read_wide_string(m.arg_ptr(2));
            if (out == 0 || path_res.is_err() || mode_res.is_err()) {
                if (out != 0) (void)m.write_ptr(out, 0);
                m.set_return(EINVAL_VALUE);
                return;
            }
            const u32 file = open_stream(m, path_res.value(), mode_res.value(),
                                         0x10);
            (void)m.write_ptr(out, file);
            m.set_return(file != 0 ? 0u : ENOENT_VALUE);
        }});

    // void _lock_file(FILE*) / void _unlock_file(FILE*)
    //
    // Vitreo's FILE* table is guarded by the thunk layer, so the guest-visible
    // contract is just that these nest and never fail.
    static std::recursive_mutex s_file_lock;
    exports.entries.push_back(
        {"_lock_file", 0, [](Marshaler& m) {
            (void)m.arg(0);
            s_file_lock.lock();
        }});
    exports.entries.push_back(
        {"_unlock_file", 0, [](Marshaler& m) {
            (void)m.arg(0);
            s_file_lock.unlock();
        }});

    // Microsoft CRT fopen/_wfopen report the reason for NULL through guest
    // errno. Missing optional files must remain distinguishable from I/O errors.
    auto set_stream_open_error = [&memory_manager](Marshaler& m,
                                                    capsule::VfsError error) {
        using capsule::VfsError;
        u32 value = 5; // Windows EIO
        switch (error) {
            case VfsError::FileNotFound:
            case VfsError::PathNotFound:
            case VfsError::DriveNotMounted: value = 2; break; // ENOENT
            case VfsError::AccessDenied:
            case VfsError::SharingViolation:
            case VfsError::NotAFile: value = 13; break; // EACCES
            case VfsError::PathAlreadyExists: value = 17; break; // EEXIST
            case VfsError::TooManyOpenFiles: value = 24; break; // EMFILE
            case VfsError::OutOfMemory: value = 12; break; // ENOMEM
            case VfsError::DiskFull: value = 28; break; // ENOSPC
            case VfsError::InvalidPath:
            case VfsError::InvalidArgument: value = 22; break; // EINVAL
            default: break;
        }
        (void)m.memory().write_u32(memory_manager.errno_addr(), value);
    };

    exports.entries.push_back(
        {"fopen", 1,
         [open_stream, &memory_manager](Marshaler& m) {
            auto path_res = m.read_ansi_string(m.arg_ptr(0));
            auto mode_res = m.read_ansi_string(m.arg_ptr(1));
            if (path_res.is_err() || mode_res.is_err()) {
                VITREO_TRACE("[crt] fopen: failed to read args\n");
                m.set_return(0);
                return;
            }
            u32 err = 0;
            bool ip = false;
            const u32 file = open_stream(m, path_res.value(), mode_res.value(),
                                         0x40, &err, &ip);
            if (file == 0) {
                if (err != 0) {
                    (void)m.memory().write_u32(memory_manager.errno_addr(), err);
                }
                if (ip) (void)memory_manager.invalid_parameter();
                VITREO_TRACE("[crt] fopen(%s, %s) -> NULL\n",
                             path_res.value().c_str(), mode_res.value().c_str());
                m.set_return(0);
                return;
            }
            VITREO_TRACE("[crt] fopen(%s, %s) -> 0x%x\n",
                         path_res.value().c_str(), mode_res.value().c_str(), file);
            m.set_return(static_cast<u64>(file));
        }});

    // _wfopen(const wchar_t* filename, const wchar_t* mode) -> FILE*
    exports.entries.push_back(
        {"_wfopen", 2,
         [open_stream, &memory_manager](Marshaler& m) {
            auto path_res = m.read_wide_string(m.arg_ptr(0));
            auto mode_res = m.read_wide_string(m.arg_ptr(1));
            if (path_res.is_err() || mode_res.is_err()) {
                VITREO_TRACE("[crt] _wfopen: failed to read args\n");
                m.set_return(0);
                return;
            }
            u32 err = 0;
            bool ip = false;
            const u32 file = open_stream(m, path_res.value(), mode_res.value(),
                                         0x40, &err, &ip);
            if (file == 0) {
                if (err != 0) {
                    (void)m.memory().write_u32(memory_manager.errno_addr(), err);
                }
                if (ip) (void)memory_manager.invalid_parameter();
                m.set_return(0);
                return;
            }
            m.set_return(static_cast<u64>(file));
        }});
}

/// freopen.
void add_crt_stdio_freopen_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const crt_stdio_buffer::Layout file_layout = ctx.file_layout;
    const auto initialize_guest_binary_buffer =
        [&memory_manager](Marshaler& m, u32 stream, VfsFile& file) {
            stdio_initialize_guest_binary_buffer(memory_manager, m, stream, file);
        };
    const auto bind_stream_descriptor =
        [&vfs, &memory_manager, &mgr](Marshaler& m, u32 stream, VfsFile& file) {
            return stdio_bind_stream_descriptor(vfs, memory_manager, mgr, m, stream, file);
        };
    const auto standard_stream_index = [&crt_data](GuestAddr stream) -> i32 {
        return stdio_standard_stream_index(crt_data, stream);
    };

    // ---- freopen ------------------------------------------------------------
    //
    // FILE* freopen(const char* path, const char* mode, FILE* stream)
    //
    // Reuses the caller's FILE* identity, which matters because guests
    // reassign stdout/stderr this way and keep using the old pointer.
    exports.entries.push_back({"freopen", 0,
        [&vfs, standard_stream_index,
         bind_stream_descriptor, initialize_guest_binary_buffer,
         file_layout](Marshaler& m) {
            auto path_res = m.read_ansi_string(m.arg_ptr(0));
            auto mode_res = m.read_ansi_string(m.arg_ptr(1));
            const auto stream = static_cast<u32>(m.arg(2));
            if (path_res.is_err() || mode_res.is_err() || stream == 0) {
                m.set_return(0);
                return;
            }
            const std::string& path = path_res.value();
            const std::string& mode = mode_res.value();

            using capsule::AccessMode;
            using capsule::CreationDisposition;
            using capsule::ShareMode;
            const bool has_w = mode.find('w') != std::string::npos;
            const bool has_a = mode.find('a') != std::string::npos;
            const bool has_plus = mode.find('+') != std::string::npos;
            const bool has_b = mode.find('b') != std::string::npos;

            AccessMode access = AccessMode::Read;
            CreationDisposition disp = CreationDisposition::OpenExisting;
            if (has_w) {
                access = has_plus ? AccessMode::ReadWrite : AccessMode::Write;
                disp = CreationDisposition::CreateAlways;
            } else if (has_a) {
                access = has_plus ? AccessMode::ReadWrite : AccessMode::Write;
                disp = CreationDisposition::OpenAlways;
            } else if (has_plus) {
                access = AccessMode::ReadWrite;
            }

            auto opened = vfs.open(path, access, ShareMode::Read, disp);
            if (opened.is_err()) {
                // freopen closes the original stream even when the reopen
                // fails, so the guest must not keep using it either way.
                auto old = stdio_arena().files.find(stream);
                if (old != stdio_arena().files.end()) {
                    (void)vfs.close(old->second.raw_handle);
                    stdio_arena().files.erase(old);
                }
                VITREO_TRACE("[crt] freopen(\"%s\", \"%s\") -> NULL\n",
                             path.c_str(), mode.c_str());
                m.set_return(0);
                return;
            }
            if (has_a) {
                (void)vfs.seek(opened.value(), 0, capsule::SeekOrigin::End);
            }

            auto existing = stdio_arena().files.find(stream);
            if (existing != stdio_arena().files.end()) {
                (void)vfs.close(existing->second.raw_handle);
            }
            // Redirecting a standard stream is legitimate: the FILE* object
            // lives in the CRT data block, and binding it here makes later
            // fwrite/fprintf calls hit the file rather than the host console.
            VfsFile vf;
            vf.layout = file_layout;
            vf.raw_handle = opened.value();
            vf.text_mode = !has_b;
            vf.append = has_a;
            initialize_guest_binary_buffer(m, stream, vf);
            stdio_arena().files[stream] = vf;
            (void)bind_stream_descriptor(m, stream, vf);
            VITREO_TRACE("[crt] freopen(\"%s\", \"%s\", 0x%x) -> 0x%x%s\n",
                         path.c_str(), mode.c_str(), stream, stream,
                         standard_stream_index(stream) >= 0
                             ? " (standard stream redirected)" : "");
            m.set_return(static_cast<u64>(stream));
        }});
}

} // namespace vitreo::winapi::crt_stdio_internal
