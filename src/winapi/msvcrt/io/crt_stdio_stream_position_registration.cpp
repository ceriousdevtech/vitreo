// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_stream_position_registration.cpp
/// @brief Stream positioning and status: fseek, ftell, feof, fflush, fgetpos, ...

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"

#include <cerrno>
#include <cstdio>

namespace vitreo::winapi::crt_stdio_internal {

/// fseek, ftell, rewind, feof, ferror, clearerr and fflush.
void add_crt_stdio_position_exports(DllExports& exports, const StdioContext& ctx) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto standard_stream_index = [&crt_data](GuestAddr stream) -> i32 {
        return stdio_standard_stream_index(crt_data, stream);
    };
    const auto host_standard_stream = [&mgr, &crt_data](GuestAddr stream) -> FILE* {
        return stdio_host_standard_stream(mgr, crt_data, stream);
    };

    exports.entries.push_back(
        {"fseek", 11, [&mgr, &vfs, &memory_manager](Marshaler& m) {
            (void)mgr;
            u32 fp     = static_cast<u32>(m.arg(0));
            i64 offset = static_cast<i64>(static_cast<i32>(m.arg(1)));
            i32 whence = static_cast<i32>(m.arg(2));
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(static_cast<u64>(static_cast<i64>(-1))); return;
            }
            if (whence < 0 || whence > 2) {
                memory_manager.set_errno(22);
                (void)memory_manager.invalid_parameter();
                m.set_return(static_cast<u64>(static_cast<i64>(-1)));
                return;
            }
            capsule::SeekOrigin origin = capsule::SeekOrigin::Begin;
            if (whence == 1) origin = capsule::SeekOrigin::Current;
            else if (whence == 2) origin = capsule::SeekOrigin::End;
            if (whence == 1 && it->second.pushback_valid) --offset;
            it->second.pushback_valid = false;
            it->second.eof_flag = false;   // C: a successful seek clears EOF
            it->second.error_flag = false;
            it->second.has_read_character = false;
            auto res = vfs.seek(stream_handle(vfs, it->second), offset, origin);
            if (it->second.guest_buffer != 0) {
                (void)m.write_u32(fp + crt_stdio_buffer::kHasRead, 0);
                (void)m.write_u32(fp + crt_stdio_buffer::kPushback, 0);
                (void)m.write_u32(fp + crt_stdio_buffer::kEof, 0);
            }
            m.set_return(res.is_ok() ? 0ULL : static_cast<u64>(static_cast<i64>(-1)));
        }});
    exports.entries.push_back(
        {"ftell", 12, [&mgr, &vfs](Marshaler& m) {
            (void)mgr;
            u32 fp = static_cast<u32>(m.arg(0));
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) {
                m.set_return(static_cast<u64>(static_cast<i64>(-1))); return;
            }
            if (it->second.guest_buffer != 0) {
                GuestStreamLock stream_lock(it->second);
                auto res = vfs.seek(it->second.raw_handle, 0,
                                    capsule::SeekOrigin::Current);
                // Both staging layers sit between the raw position and the
                // guest's: bytes fread left in the host read-ahead block and
                // bytes still unread in the guest-visible FILE buffer (which
                // is refilled through that same block). Leaving out the
                // read-ahead reported the end of the block -- after fread of
                // 5 bytes from an 18-byte file, 18 instead of Windows' 5
                // (tests/crt_oracle/crt_fmt_io_runner "ftell.after5").
                const i64 logical = res.is_ok()
                    ? static_cast<i64>(res.value()) -
                          static_cast<i64>(pending_read_ahead(it->second)) -
                          static_cast<i64>(pending_guest_buffer(it->second)) -
                          (it->second.pending_cr ? 1 : 0)
                    : -1;
                m.set_return(res.is_ok() ? static_cast<u64>(logical) :
                    static_cast<u64>(static_cast<i64>(-1)));
                return;
            }
            auto res = vfs.seek(stream_handle(vfs, it->second), 0, capsule::SeekOrigin::Current);
            const i64 logical = res.is_ok()
                ? static_cast<i64>(res.value()) -
                      (it->second.pushback_valid ? 1 : 0) -
                      (it->second.pending_cr ? 1 : 0)
                : -1;
            m.set_return(res.is_ok() ? static_cast<u64>(logical) :
                static_cast<u64>(static_cast<i64>(-1)));
        }});
    exports.entries.push_back(
        {"rewind", 13, [&mgr, &vfs](Marshaler& m) {
            (void)mgr;
            u32 fp = static_cast<u32>(m.arg(0));
            auto it = stdio_arena().files.find(fp);
            if (it != stdio_arena().files.end()) {
                it->second.pushback_valid = false;
                it->second.has_read_character = false;
                it->second.eof_flag = false;  // C: rewind also clears EOF/error
                it->second.error_flag = false;
                (void)vfs.seek(stream_handle(vfs, it->second), 0, capsule::SeekOrigin::Begin);
                if (it->second.guest_buffer != 0) {
                    (void)m.write_u32(fp + crt_stdio_buffer::kHasRead, 0);
                    (void)m.write_u32(fp + crt_stdio_buffer::kPushback, 0);
                    (void)m.write_u32(fp + crt_stdio_buffer::kEof, 0);
                }
            }
            m.set_return(0);
        }});
    exports.entries.push_back(
        {"feof", 14, [&mgr, host_standard_stream](Marshaler& m) {
            (void)mgr;
            const u32 fp = static_cast<u32>(m.arg(0));
            if (FILE* host_file = host_standard_stream(fp)) {
                m.set_return(stdio_host_call([&] { return std::feof(host_file); }) != 0 ? 1u : 0u);
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) { m.set_return(0); return; }
            if (it->second.guest_buffer != 0) {
                m.set_return(m.read_u32(
                    fp + crt_stdio_buffer::kEof).value_or(0) != 0 ? 1u : 0u);
                return;
            }
            m.set_return(it->second.eof_flag ? 1u : 0u);
        }});
    exports.entries.push_back(
        {"ferror", 15, [&mgr, standard_stream_index](Marshaler& m) {
            const u32 fp = static_cast<u32>(m.arg(0));
            const i32 std_idx = standard_stream_index(fp);
            if (std_idx >= 0 && std_idx <= 2) {
                m.set_return(std_stream_error(std_descriptor(mgr, std_idx)) ? 1u : 0u);
                return;
            }
            auto it = stdio_arena().files.find(fp);
            m.set_return(it != stdio_arena().files.end() && it->second.error_flag
                             ? 1u : 0u);
        }});
    // void clearerr(FILE*) — Learn: clears both EOF and error indicators.
    exports.entries.push_back(
        {"clearerr", 0, [&mgr, standard_stream_index](Marshaler& m) {
            const u32 fp = static_cast<u32>(m.arg(0));
            const i32 std_idx = standard_stream_index(fp);
            if (std_idx >= 0 && std_idx <= 2) {
                StdDescriptor& desc = std_descriptor(mgr, std_idx);
                desc.stream_error = false;
                if (desc.stream != nullptr) stdio_host_call([&] { return ::clearerr(desc.stream); });
                return;
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) return;
            it->second.eof_flag = false;
            it->second.error_flag = false;
            if (it->second.guest_buffer != 0) {
                (void)m.write_u32(fp + crt_stdio_buffer::kEof, 0);
            }
        }});
    exports.entries.push_back(
        {"fflush", 16,
         [&mgr, &vfs, host_standard_stream, standard_stream_index,
          &memory_manager](Marshaler& m) {
            constexpr i32 kEof = -1;
            constexpr u32 EINVAL_VALUE = 22;
            const GuestAddr stream = m.arg_ptr(0);
            // fflush(NULL) flushes all streams — Learn; measured r=0.
            if (stream == 0) {
                (void)stdio_host_call([&] { return ::fflush(nullptr); });
                for (auto& [fp, vf] : stdio_arena().files) {
                    (void)fp;
                    (void)vfs.flush(stream_handle(vfs, vf));
                }
                m.set_return(0);
                return;
            }
            if (FILE* host_file = host_standard_stream(stream)) {
                StdDescriptor& desc =
                    std_descriptor(mgr, standard_stream_index(stream));
                if (desc.kind == StdKind::None || desc.kind == StdKind::Closed) {
                    // The buffered output has no handle to go to.
                    const bool pending = desc.pending_output;
                    desc.pending_output = false;
                    if (pending) {
                        desc.stream_error = true;
                        memory_manager.set_errno(9);  // EBADF
                        m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
                        return;
                    }
                    m.set_return(0);
                    return;
                }
                m.set_return(static_cast<u32>(stdio_host_call([&] { return ::fflush(host_file); })));
                return;
            }
            if (stream > 0xFFFFFFFFULL) {
                memory_manager.set_errno(EINVAL_VALUE);
                m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
                return;
            }
            const u32 fp = static_cast<u32>(stream);
            auto it = stdio_arena().files.find(fp);
            if (it != stdio_arena().files.end()) {
                stdio_diag_vfs(m, "fflush", fp, it->second);
                (void)vfs.flush(stream_handle(vfs, it->second));
                m.set_return(0);
                return;
            }
            // Unknown FILE*: Windows AVs; Vitreo continues with EOF+EINVAL.
            memory_manager.set_errno(EINVAL_VALUE);
            m.set_return(static_cast<u64>(static_cast<i64>(kEof)));
        }});
}

/// _fseeki64, _ftelli64, fgetpos and fsetpos.
void add_crt_stdio_seek64_exports(DllExports& exports, const StdioContext& ctx) {
    capsule::Vfs& vfs = ctx.vfs;
    CrtMemoryManager& memory_manager = ctx.memory_manager;

    // =========================================================================
    // Remaining stream functions
    // =========================================================================

    // int _fseeki64(FILE*, __int64 offset, int origin)
    // Learn fseek/_fseeki64: clears EOF and undoes ungetc; invalid origin →
    // EINVAL.
    exports.entries.push_back({"_fseeki64", 0, [&vfs, &memory_manager](Marshaler& m) {
        const auto fp = static_cast<u32>(m.arg(0));
        const bool is_64bit = m.cpu_is_64bit();
        const i64 offset = is_64bit
            ? static_cast<i64>(m.arg(1))
            : static_cast<i64>((m.arg(1) & 0xFFFFFFFFULL) |
                               ((m.arg(2) & 0xFFFFFFFFULL) << 32));
        const auto origin = static_cast<i32>(m.arg(is_64bit ? 2 : 3));

        auto it = stdio_arena().files.find(fp);
        if (it == stdio_arena().files.end()) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (origin < 0 || origin > 2) {
            memory_manager.set_errno(22);  // EINVAL
            (void)memory_manager.invalid_parameter();
            m.set_return(0xFFFFFFFFu);
            return;
        }
        capsule::SeekOrigin seek_origin = capsule::SeekOrigin::Begin;
        i64 adj = offset;
        if (origin == 1) {
            seek_origin = capsule::SeekOrigin::Current;
            if (it->second.pushback_valid) --adj;
        } else if (origin == 2) {
            seek_origin = capsule::SeekOrigin::End;
        }
        it->second.pushback_valid = false;
        it->second.eof_flag = false;
        it->second.error_flag = false;
        it->second.has_read_character = false;
        if (it->second.guest_buffer != 0) {
            (void)m.write_u32(fp + crt_stdio_buffer::kHasRead, 0);
            (void)m.write_u32(fp + crt_stdio_buffer::kPushback, 0);
            (void)m.write_u32(fp + crt_stdio_buffer::kEof, 0);
        }
        auto result = vfs.seek(stream_handle(vfs, it->second), adj, seek_origin);
        m.set_return(result.is_ok() ? 0u : 0xFFFFFFFFu);
    }});

    // __int64 _ftelli64(FILE*)
    exports.entries.push_back({"_ftelli64", 0, [&vfs](Marshaler& m) {
        const auto fp = static_cast<u32>(m.arg(0));
        auto it = stdio_arena().files.find(fp);
        if (it == stdio_arena().files.end()) {
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        auto result = vfs.seek(stream_handle(vfs, it->second), 0,
                               capsule::SeekOrigin::Current);
        if (result.is_err()) {
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        i64 logical = static_cast<i64>(result.value());
        if (it->second.pushback_valid) --logical;
        if (it->second.pending_cr) --logical;
        m.set_return_i64(static_cast<u64>(logical));
    }});

    // int fgetpos(FILE*, fpos_t* pos) — fpos_t is __int64
    exports.entries.push_back({"fgetpos", 0, [&vfs](Marshaler& m) {
        const auto fp = static_cast<u32>(m.arg(0));
        const GuestAddr out = m.arg_ptr(1);
        auto it = stdio_arena().files.find(fp);
        if (it == stdio_arena().files.end() || out == 0) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto result = vfs.seek(stream_handle(vfs, it->second), 0,
                               capsule::SeekOrigin::Current);
        if (result.is_err() || !m.write_u64(out, result.value()).is_ok()) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    }});

    // int fsetpos(FILE*, const fpos_t* pos)
    exports.entries.push_back({"fsetpos", 0, [&vfs](Marshaler& m) {
        const auto fp = static_cast<u32>(m.arg(0));
        const GuestAddr in = m.arg_ptr(1);
        auto it = stdio_arena().files.find(fp);
        if (it == stdio_arena().files.end() || in == 0) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto position = m.read_u64(in);
        if (position.is_err()) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto result = vfs.seek(stream_handle(vfs, it->second),
                               static_cast<i64>(position.value()),
                               capsule::SeekOrigin::Begin);
        m.set_return(result.is_ok() ? 0u : 0xFFFFFFFFu);
    }});
}

} // namespace vitreo::winapi::crt_stdio_internal
