// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_stream_state.cpp
/// @brief VfsFile-level stream primitives and translated transfers.
///
/// Guest-visible FILE cells, VFS read-ahead and the text-mode (CR-LF)
/// translating reads and writes that fread, fgets, fwrite, fputs and the
/// fprintf family share.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

u32* guest_stream_cell(VfsFile& vf, GuestAddr offset) {
    if (vf.guest_memory == nullptr || vf.guest_stream == 0) return nullptr;
    return reinterpret_cast<u32*>(
        vf.guest_memory->host_ptr_mut(vf.guest_stream + offset, sizeof(u32)));
}

void release_guest_stream_lock(VfsFile& vf) {
    if (u32* cell = guest_stream_cell(vf, crt_stdio_buffer::kLock)) {
        std::atomic_ref<u32>(*cell).store(0, std::memory_order_release);
    }
}

usize pending_guest_buffer(VfsFile& vf) {
    if (vf.guest_buffer == 0 || vf.guest_memory == nullptr) return 0;
    const auto public_cells = crt_stdio_buffer::cells(vf.layout);
    auto count = vf.guest_memory->read_u32(
        vf.guest_stream + public_cells.count);
    if (count.is_err() || static_cast<i32>(count.value()) <= 0) return 0;
    return static_cast<usize>(count.value());
}

void clear_guest_buffer_cells(VfsFile& vf) {
    if (vf.guest_buffer == 0 || vf.guest_memory == nullptr) return;
    const auto public_cells = crt_stdio_buffer::cells(vf.layout);
    (void)vf.guest_memory->write_u32(
        vf.guest_stream + public_cells.ptr, vf.guest_buffer);
    (void)vf.guest_memory->write_u32(
        vf.guest_stream + public_cells.count, 0);
    (void)vf.guest_memory->write_u32(
        vf.guest_stream + crt_stdio_buffer::kPushback, 0);
}

namespace {

/// VITREO_STDIO_DIAG=1: print the guest-visible FILE cells at each CRT entry
/// that touches a stream. The x64 MSVC STL filebuf reads the ptr/base cells
/// as 8-byte pointers and copies straight into *ptr, so a stale or foreign
/// value there becomes a wild memcpy in guest code, far from the CRT call.
bool stdio_diag_enabled() {
    static const bool enabled = std::getenv("VITREO_STDIO_DIAG") != nullptr;
    return enabled;
}

} // namespace

void stdio_diag_cells(Marshaler& m, const char* who, GuestAddr stream,
                      GuestAddr ptr_cell, GuestAddr count_cell,
                      GuestAddr base_cell, u64 buffer, u64 capacity) {
    if (!stdio_diag_enabled()) return;
    const u64 ptr = m.read_u64(ptr_cell).value_or(0);
    const u32 count = m.read_u32(count_cell).value_or(0);
    const u64 base = m.read_u64(base_cell).value_or(0);
    const bool wild = buffer != 0 && ptr != 0 &&
                      (ptr < buffer || ptr > buffer + capacity);
    std::fprintf(stderr,
        "[stdio-diag] %s stream=0x%llx caller=0x%llx ptr[0x%llx]=0x%llx "
        "cnt[0x%llx]=%d base[0x%llx]=0x%llx buf=0x%llx cap=%llu%s\n",
        who, static_cast<unsigned long long>(stream),
        static_cast<unsigned long long>(m.guest_return_address()),
        static_cast<unsigned long long>(ptr_cell),
        static_cast<unsigned long long>(ptr),
        static_cast<unsigned long long>(count_cell), static_cast<int>(count),
        static_cast<unsigned long long>(base_cell),
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(buffer),
        static_cast<unsigned long long>(capacity),
        wild ? "  <-- PTR OUTSIDE BUFFER" : "");
}

void stdio_diag_vfs(Marshaler& m, const char* who, u32 stream, VfsFile& vf) {
    if (!stdio_diag_enabled()) return;
    const auto public_cells = crt_stdio_buffer::cells(vf.layout);
    stdio_diag_cells(m, who, stream, stream + public_cells.ptr,
                     stream + public_cells.count, stream + public_cells.base,
                     vf.guest_buffer,
                     vf.guest_buffer != 0
                         ? m.read_u32(stream + crt_stdio_buffer::kCapacity)
                               .value_or(0)
                         : 0);
}

/// Raw bytes already fetched from the VFS but not yet handed to the guest.
usize pending_read_ahead(const VfsFile& vf) {
    return vf.read_ahead.size() - vf.read_ahead_pos;
}

/// Drop any read-ahead, rewind the VFS position to the stream's logical
/// position, and return the handle for direct use.
capsule::FileHandle stream_handle(capsule::Vfs& vfs, VfsFile& vf) {
    // A binary guest buffer advances the raw handle by a whole block. Rewind
    // its unread tail before any host-side seek/read/write observes the file.
    // The per-stream guest lock makes this the same synchronization domain as
    // accelerated fgetc/ungetc.
    GuestStreamLock guest_lock(vf);
    const usize guest_pending = pending_guest_buffer(vf);
    if (guest_pending != 0) {
        (void)vfs.seek(vf.raw_handle, -static_cast<i64>(guest_pending),
                       capsule::SeekOrigin::Current);
    }
    clear_guest_buffer_cells(vf);
    vf.pushback_valid = false;

    const usize pending = pending_read_ahead(vf);
    if (pending != 0) {
        (void)vfs.seek(vf.raw_handle, -static_cast<i64>(pending),
                       capsule::SeekOrigin::Current);
    }
    vf.read_ahead.clear();
    vf.read_ahead_pos = 0;
    return vf.raw_handle;
}

/// The handle to WRITE through: stream_handle() plus the append-mode rule
/// that every write lands at end-of-file.
capsule::FileHandle stream_write_handle(capsule::Vfs& vfs, VfsFile& vf) {
    const capsule::FileHandle h = stream_handle(vfs, vf);
    if (vf.append) (void)vfs.seek(h, 0, capsule::SeekOrigin::End);
    return h;
}

/// Block-buffered raw read for a stream.
///
/// A UCRT stream is buffered, so `fgetc` costs one host read per block rather
/// than one per byte. Serving each byte with its own `vfs.read` turns an
/// ordinary byte-at-a-time scan of a file into one host call per byte.
usize stream_raw_read(capsule::Vfs& vfs, VfsFile& vf, std::span<u8> dst) {
    constexpr usize kBlockBytes = 64 * 1024;
    usize delivered = 0;
    while (delivered < dst.size()) {
        if (const usize pending = pending_read_ahead(vf); pending != 0) {
            const usize take = std::min(pending, dst.size() - delivered);
            std::memcpy(dst.data() + delivered,
                        vf.read_ahead.data() + vf.read_ahead_pos, take);
            vf.read_ahead_pos += take;
            delivered += take;
            continue;
        }
        const usize want = dst.size() - delivered;
        if (want >= kBlockBytes) {
            // A bulk request gains nothing from staging through the block.
            auto direct = vfs.read(vf.raw_handle, dst.subspan(delivered, want));
            const usize got = direct.is_ok() ? direct.value() : 0;
            delivered += got;
            if (got < want) break; // EOF
            continue;
        }
        vf.read_ahead.assign(kBlockBytes, 0);
        vf.read_ahead_pos = 0;
        auto block = vfs.read(vf.raw_handle, std::span<u8>(vf.read_ahead));
        const usize got = block.is_ok() ? block.value() : 0;
        vf.read_ahead.resize(got);
        if (got == 0) break; // EOF
    }
    return delivered;
}

/// Consume bytes already staged in the guest-visible FILE buffer.
///
/// Bulk stdio and the guest-side fgetc accelerator must share one logical
/// read position.  If fread/fgets bypass this buffer, the raw handle is already
/// at the end of the 64 KiB block and the bulk operation silently skips every
/// unread byte in it.  Draining it here also preserves an ungetc character,
/// which can differ from the byte in the underlying file.
usize drain_guest_character_buffer(VfsFile& vf, std::span<u8> dst) {
    using namespace crt_stdio_buffer;
    if (dst.empty() || vf.guest_buffer == 0 || vf.guest_memory == nullptr) {
        return 0;
    }

    GuestStreamLock stream_lock(vf);
    const auto public_cells = cells(vf.layout);
    const u32 count = vf.guest_memory->read_u32(
        vf.guest_stream + public_cells.count).value_or(0);
    const u32 ptr = vf.guest_memory->read_u32(
        vf.guest_stream + public_cells.ptr).value_or(0);
    const u32 capacity = vf.guest_memory->read_u32(
        vf.guest_stream + kCapacity).value_or(0);
    if (count == 0 || ptr < vf.guest_buffer ||
        ptr > vf.guest_buffer + capacity ||
        count > vf.guest_buffer + capacity - ptr) {
        return 0;
    }

    const usize take = std::min<usize>(count, dst.size());
    if (vf.guest_memory->read_bytes(ptr, dst.data(), take).is_err()) return 0;
    (void)vf.guest_memory->write_u32(
        vf.guest_stream + public_cells.ptr, ptr + static_cast<u32>(take));
    (void)vf.guest_memory->write_u32(
        vf.guest_stream + public_cells.count,
        count - static_cast<u32>(take));
    if (take != 0) {
        // A pushed-back character is always the first pending byte.
        (void)vf.guest_memory->write_u32(vf.guest_stream + kPushback, 0);
        (void)vf.guest_memory->write_u32(vf.guest_stream + kHasRead, 1);
        vf.has_read_character = true;
        vf.pushback_valid = false;
        vf.eof_flag = false;
    }
    return take;
}

namespace {

// Per-FILE state also tracks text-mode CRLF translation. On Windows,
// fopen(...,"r"/"w") (no 'b') performs CRLF<->LF translation; callers
// rely on it for SHA1 stability.
// Helper: text-mode aware read. Translates CRLF->LF as on Windows
// text-mode fread. Returns number of translated bytes delivered into dst.
usize vfs_read_translated_raw(capsule::Vfs& vfs, VfsFile& vf, std::span<u8> dst) {
    if (!vf.text_mode) {
        return stream_raw_read(vfs, vf, dst);
    }
    usize delivered = 0;
    // Scratch holds raw bytes; we may have a pending '\r' from prior call.
    while (delivered < dst.size()) {
        usize want = dst.size() - delivered;
        std::vector<u8> raw(want);
        usize got = stream_raw_read(vfs, vf, std::span<u8>(raw.data(), want));
        if (got == 0) {
            // EOF: emit a deferred standalone CR (no following LF).
            if (vf.pending_cr) {
                dst[delivered++] = '\r';
                vf.pending_cr = false;
            }
            break;
        }
        for (usize i = 0; i < got; ++i) {
            u8 c = raw[i];
            if (vf.pending_cr) {
                // Previous chunk ended on '\r'. Resolve it now.
                vf.pending_cr = false;
                if (c == '\n') {
                    // CRLF -> LF
                    if (delivered < dst.size()) dst[delivered++] = '\n';
                    continue;
                } else {
                    // Lone CR; emit it.
                    if (delivered < dst.size()) dst[delivered++] = '\r';
                    // fall through to handle current c
                }
            }
            if (c == '\r') {
                if (i + 1 < got) {
                    if (raw[i + 1] == '\n') {
                        // Skip CR; emit LF on next iteration.
                        continue;
                    }
                    // Lone CR followed by non-LF; emit it.
                    if (delivered < dst.size()) dst[delivered++] = '\r';
                    continue;
                } else {
                    // CR at end of chunk; defer.
                    vf.pending_cr = true;
                    continue;
                }
            }
            if (delivered < dst.size()) dst[delivered++] = c;
        }
        // If we delivered less than wanted in this chunk (because we
        // dropped CRs), loop and read more raw to fill dst.
        if (got < want) {
            // EOF reached; flush any deferred CR.
            if (vf.pending_cr) {
                if (delivered < dst.size()) dst[delivered++] = '\r';
                vf.pending_cr = false;
            }
            break;
        }
    }
    return delivered;
}

} // namespace

// All byte-oriented reads consume the same one-character pushback slot.
// The C contract guarantees at least one character of pushback, including
// when the underlying VFS read-ahead buffer is at a refill boundary.
usize stdio_vfs_read_translated(capsule::Vfs& vfs, VfsFile& vf, std::span<u8> dst) {
    if (dst.empty()) return 0;

    usize delivered = 0;
    if (vf.pushback_valid) {
        dst[delivered++] = vf.pushback_character;
        vf.pushback_valid = false;
    }
    if (delivered < dst.size()) {
        delivered += drain_guest_character_buffer(
            vf, dst.subspan(delivered));
    }
    if (delivered < dst.size()) {
        delivered += vfs_read_translated_raw(
            vfs, vf, dst.subspan(delivered));
    }
    if (delivered != 0) vf.has_read_character = true;
    // A short read means the stream hit end-of-file. This is the ONLY
    // thing that may set the EOF indicator -- feof() reports this flag
    // and must not infer EOF from position.
    if (delivered < dst.size()) vf.eof_flag = true;
    return delivered;
}

// Text-mode writes must expand LF to CRLF: a stream opened "w" (not "wb")
// is a TEXT stream on Windows, so "a\nb\n" occupies 6 bytes on disk, not
// 4. Writing raw produced LF-only files that every Windows reader -- and
// our own text-mode reader, which already un-translates CRLF -- disagrees
// with. Returns the number of GUEST bytes consumed, not bytes written, so
// fwrite's item count stays correct.
usize stdio_vfs_write_translated(capsule::Vfs& vfs, VfsFile& vf, std::span<const u8> src) {
    if (src.empty()) return 0;
    if (!vf.text_mode) {
        auto res = vfs.write(stream_write_handle(vfs, vf), src);
        return res.is_ok() ? res.value() : 0;
    }
    std::vector<u8> out;
    out.reserve(src.size() + src.size() / 8 + 8);
    for (u8 b : src) {
        if (b == '\n') out.push_back('\r');
        out.push_back(b);
    }
    auto res = vfs.write(stream_write_handle(vfs, vf),
                         std::span<const u8>(out));
    if (!res.is_ok()) return 0;
    // Map bytes-written back to guest bytes consumed. A partial write
    // must not claim more guest bytes than actually made it out.
    usize wrote = res.value();
    if (wrote >= out.size()) return src.size();
    usize guest = 0, emitted = 0;
    for (u8 b : src) {
        const usize step = (b == '\n') ? 2u : 1u;
        if (emitted + step > wrote) break;
        emitted += step; ++guest;
    }
    return guest;
}

} // namespace vitreo::winapi::crt_stdio_internal
