// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_guest_char_registration.cpp
/// @brief The guest-visible fgetc/ungetc path.
///
/// Installs the arena character callbacks the execution loop services and
/// exports the __vitreo_* tails the guest accelerators branch to.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

/// The guest-visible fgetc/ungetc character path: the binary-buffer helpers,
/// the arena callbacks the execution loop services and the __vitreo_* tails.
void add_crt_stdio_guest_char_exports(DllExports& exports, const StdioContext& ctx,
                                      const std::shared_ptr<StdioArena>& arena) {
    CrtStdioManager& mgr = ctx.mgr;
    capsule::Vfs& vfs = ctx.vfs;
    CrtDataBlock& crt_data = ctx.crt_data;
    const auto standard_stream_index = [&crt_data](GuestAddr stream) -> i32 {
        return stdio_standard_stream_index(crt_data, stream);
    };
    const auto host_standard_stream = [&mgr, &crt_data](GuestAddr stream) -> FILE* {
        return stdio_host_standard_stream(mgr, crt_data, stream);
    };
    auto vfs_read_translated = [&vfs](VfsFile& vf, std::span<u8> dst) -> usize {
        return stdio_vfs_read_translated(vfs, vf, dst);
    };

    const auto refill_guest_binary_buffer_locked =
        [&vfs](VfsFile& vf) -> i32 {
            using namespace crt_stdio_buffer;
            if (vf.guest_buffer == 0 || vf.guest_memory == nullptr) return EOF;
            const auto public_cells = cells(vf.layout);

            std::vector<u8> block(kDefaultBufferBytes);
            // A preceding fread/fgets may have staged bytes in the ordinary
            // host read-ahead block.  Refill through the shared raw reader so
            // returning to accelerated fgetc cannot skip that pending tail.
            const usize got = stream_raw_read(vfs, vf, std::span<u8>(block));
            if (got == 0) {
                // Keep _ptr at the byte just past the last character. ungetc
                // is required to work after EOF when a character was read;
                // resetting it to _base would erase the one pushback slot.
                (void)vf.guest_memory->write_u32(
                    vf.guest_stream + public_cells.count, 0);
                (void)vf.guest_memory->write_u32(
                    vf.guest_stream + kPushback, 0);
                (void)vf.guest_memory->write_u32(vf.guest_stream + kEof, 1);
                vf.eof_flag = true;
                return EOF;
            }

            if (vf.guest_memory->write_bytes(
                    vf.guest_buffer, block.data(), got).is_err()) {
                clear_guest_buffer_cells(vf);
                return EOF;
            }
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + public_cells.ptr, vf.guest_buffer + 1);
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + public_cells.count,
                static_cast<u32>(got - 1));
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + kHasRead, 1);
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + kPushback, 0);
            (void)vf.guest_memory->write_u32(vf.guest_stream + kEof, 0);
            vf.has_read_character = true;
            vf.pushback_valid = false;
            vf.eof_flag = false;
            return static_cast<i32>(block[0]);
        };

    const auto consume_guest_binary_buffer_locked =
        [refill_guest_binary_buffer_locked](VfsFile& vf) -> i32 {
            using namespace crt_stdio_buffer;
            if (vf.guest_buffer == 0 || vf.guest_memory == nullptr) return EOF;
            const auto public_cells = cells(vf.layout);
            const i32 count = static_cast<i32>(
                vf.guest_memory->read_u32(
                    vf.guest_stream + public_cells.count)
                    .value_or(0));
            if (count <= 0) return refill_guest_binary_buffer_locked(vf);
            const u32 ptr = vf.guest_memory->read_u32(
                vf.guest_stream + public_cells.ptr).value_or(0);
            const auto ch = vf.guest_memory->read_u8(ptr);
            if (ch.is_err()) return EOF;
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + public_cells.ptr, ptr + 1);
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + public_cells.count,
                static_cast<u32>(count - 1));
            (void)vf.guest_memory->write_u32(vf.guest_stream + kHasRead, 1);
            (void)vf.guest_memory->write_u32(vf.guest_stream + kPushback, 0);
            vf.has_read_character = true;
            vf.pushback_valid = false;
            return static_cast<i32>(ch.value());
        };

    const auto push_guest_binary_character_locked =
        [](VfsFile& vf, i32 ch) -> i32 {
            using namespace crt_stdio_buffer;
            if (ch == EOF || vf.guest_buffer == 0 ||
                vf.guest_memory == nullptr) return EOF;
            const auto public_cells = cells(vf.layout);
            const bool has_read = vf.guest_memory->read_u32(
                vf.guest_stream + kHasRead).value_or(0) != 0;
            const bool pushed = vf.guest_memory->read_u32(
                vf.guest_stream + kPushback).value_or(0) != 0;
            const u32 ptr = vf.guest_memory->read_u32(
                vf.guest_stream + public_cells.ptr).value_or(0);
            if (!has_read || pushed || ptr <= vf.guest_buffer) return EOF;
            const u32 pushed_ptr = ptr - 1;
            const u8 value = static_cast<u8>(ch);
            if (vf.guest_memory->write_u8(pushed_ptr, value).is_err()) return EOF;
            const u32 count = vf.guest_memory->read_u32(
                vf.guest_stream + public_cells.count).value_or(0);
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + public_cells.ptr, pushed_ptr);
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + public_cells.count, count + 1);
            (void)vf.guest_memory->write_u32(
                vf.guest_stream + kPushback, 1);
            (void)vf.guest_memory->write_u32(vf.guest_stream + kEof, 0);
            vf.pushback_valid = false; // represented by the guest count now
            vf.eof_flag = false;
            return static_cast<i32>(value);
        };

    // fgetc/ungetc can dominate parser startup with tens of millions of
    // imported calls. The host callbacks remain the semantic reference and
    // operate on the same guest-visible buffer as the x86 accelerator.
    arena->get_character =
        [vfs_read_translated, host_standard_stream, standard_stream_index,
         consume_guest_binary_buffer_locked](u32 fp) -> i32 {
            auto descriptor_lock = lock_standard_descriptor(standard_stream_index(fp));
            StdioStateLock lock;
            if (FILE* host_file = host_standard_stream(fp)) {
                return stdio_host_call([&] { return ::fgetc(host_file); });
            }
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) return EOF;
            if (it->second.guest_buffer != 0) {
                GuestStreamLock stream_lock(it->second);
                return consume_guest_binary_buffer_locked(it->second);
            }
            u8 ch = 0;
            const usize n = vfs_read_translated(
                it->second, std::span<u8>(&ch, 1));
            return n == 0 ? EOF : static_cast<i32>(ch);
        };
    arena->unget_character =
        [host_standard_stream, standard_stream_index,
         push_guest_binary_character_locked](i32 ch, u32 fp) -> i32 {
            auto descriptor_lock = lock_standard_descriptor(standard_stream_index(fp));
            StdioStateLock lock;
            if (FILE* host_file = host_standard_stream(fp)) {
                return stdio_host_call([&] { return ::ungetc(ch, host_file); });
            }
            auto it = stdio_arena().files.find(fp);
            if (it != stdio_arena().files.end() && it->second.guest_buffer != 0) {
                GuestStreamLock stream_lock(it->second);
                return push_guest_binary_character_locked(it->second, ch);
            }
            if (ch == EOF || it == stdio_arena().files.end() ||
                !it->second.has_read_character || it->second.pushback_valid) {
                return EOF;
            }
            it->second.pushback_character = static_cast<u8>(ch);
            it->second.pushback_valid = true;
            return static_cast<i32>(it->second.pushback_character);
        };
    // Every registered FILE operation establishes its owning DLL arena. The
    // dispatcher is independent of registration order and buffer layout.
    mgr.configure_guest_character_io(
        [](u32 fp) { return stdio_arena().get_character(fp); },
        [](i32 ch, u32 fp) { return stdio_arena().unget_character(ch, fp); });

    // The wide-character encoding of a stream, for the wide input functions:
    // fopen's mode and ccs= for a file stream, the descriptor's _setmode
    // translation for stdin/stdout/stderr.
    mgr.configure_guest_stream_encoding(
        [&mgr, standard_stream_index](u32 fp) -> CrtStdioManager::WideEncoding {
            using W = CrtStdioManager::WideEncoding;
            const i32 index = standard_stream_index(fp);
            if (index >= 0) {
                auto descriptor_lock = lock_standard_descriptor(index);
                switch (std_descriptor(mgr, index).translation_mode) {
                    case kStdModeText: return W::Ansi;
                    case kStdModeBinary: return W::Binary;
                    case kStdModeU8Text: return W::Utf8;
                    default: return W::Utf16Le;  // _O_WTEXT / _O_U16TEXT
                }
            }
            StdioStateLock lock;
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end()) return W::Ansi;
            switch (it->second.encoding) {
                case VfsFile::Encoding::None: return W::Binary;
                case VfsFile::Encoding::Utf8: return W::Utf8;
                case VfsFile::Encoding::Utf16Le: return W::Utf16Le;
                case VfsFile::Encoding::Acp: break;
            }
            return W::Ansi;
        });

    // Tail targets for the guest accelerators. The refill target is entered
    // with the per-stream guest lock already held and therefore releases it
    // on every return path. Unknown/non-buffered FILE objects take the normal
    // synchronized callbacks instead.
    exports.entries.push_back(
        {"__vitreo_fgetc_refill", 0,
         [refill_guest_binary_buffer_locked](Marshaler& m) {
            const u32 fp = static_cast<u32>(m.arg(0));
            auto it = stdio_arena().files.find(fp);
            if (it == stdio_arena().files.end() || it->second.guest_buffer == 0) {
                (void)m.write_u32(fp + crt_stdio_buffer::kLock, 0);
                m.set_return(static_cast<u32>(EOF));
                return;
            }
            const i32 result = refill_guest_binary_buffer_locked(it->second);
            release_guest_stream_lock(it->second);
            m.set_return(static_cast<u32>(result));
         }});
    exports.entries.push_back(
        {"__vitreo_fgetc_slow", 0, [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(mgr.guest_fgetc(
                static_cast<u32>(m.arg(0)))));
         }});
    exports.entries.push_back(
        {"__vitreo_ungetc_slow", 0, [&mgr](Marshaler& m) {
            m.set_return(static_cast<u32>(mgr.guest_ungetc(
                static_cast<i32>(m.arg(0)),
                static_cast<u32>(m.arg(1)))));
         }});
}

} // namespace vitreo::winapi::crt_stdio_internal
