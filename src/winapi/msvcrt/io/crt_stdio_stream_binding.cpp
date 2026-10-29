// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_stream_binding.cpp
/// @brief FILE <-> low-io descriptor binding.
///
/// Helpers more than one export family needs: allocating a descriptor number,
/// binding a freshly opened stream to one, creating a guest FILE object, and
/// recognising the standard streams.

#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>

namespace vitreo::winapi::crt_stdio_internal {

// The Windows CRT owns its stream and low-I/O descriptor tables at
// process scope and synchronizes access to them.  These maps model that
// same shared state, so every exported operation that can observe or
// mutate it must participate in one lock domain.  Recursive locking is
// required because a stdio operation may delegate to another CRT helper.
// Low-level descriptors behind _open/_read/_write, and behind the `_file`
// field of a stream opened by fopen. Declared here because fopen has to
// populate that field (see stdio_bind_stream_descriptor below).
i32 stdio_alloc_fd(CrtStdioManager& mgr) {
    // Native _close releases even descriptors 0-2. Resolve against this
    // manager before examining them, so a previous process cannot donate
    // its closed standard descriptors to this one.
    for (i32 fd = 0; fd <= 2; ++fd) {
        if (std_descriptor(mgr, fd).kind == StdKind::Closed) return fd;
    }
    for (i32 fd = 3; fd < lowio_limit(); ++fd)
        if (!stdio_arena().descriptors.contains(fd)) return fd;
    return -1;
}

void stdio_bind_reused_standard(CrtStdioManager& mgr, i32 fd) {
    if (fd < 0 || fd > 2) return;
    LowIoFd& entry = stdio_arena().descriptors.at(fd);
    FileManager* files = mgr.file_manager();
    if (entry.os_handle == 0 && files != nullptr) {
        entry.os_handle = files->adopt_vfs_handle(
            entry.handle, entry.path, entry.win32_access, entry.owns_handle);
    }
    StdDescriptor& desc = std_descriptor(mgr, fd);
    desc.kind = StdKind::Kernel;
    desc.handle = entry.os_handle;
    desc.translation_mode = entry.translation_mode != 0 ? entry.translation_mode
        : (entry.text_mode ? kStdModeText : kStdModeBinary);
    desc.text_eof = false;
    desc.lookahead_valid = false;
    desc.unicode_lookahead.reset();
    desc.pending_output = false;
    stdio_arena().file_manager = files;
    if (auto* console = mgr.console_manager())
        (void)console->set_std_handle(STD_INPUT_HANDLE - static_cast<u32>(fd), entry.os_handle);
}

u32 stdio_allocate_stream_object(CrtMemoryManager& memory_manager, Marshaler& m) {
    const GuestAddr stream =
        memory_manager.crt_malloc(CrtDataBlock::ACRT_FILE_SIZE);
    if (stream == 0 || stream > 0xFFFFFFFFULL) return 0;
    // crt_malloc recycles freed guest blocks (VITREO_CRT_HEAP_REUSE is on
    // by default), so this object inherits whatever its previous occupant
    // left behind. The x64 MSVC STL reads the ptr/base cells exposed by
    // _get_stream_buffer_pointers as 8-byte pointers and copies straight
    // into *ptr whenever the count cell is positive; a stale function
    // pointer and a stale count there put Ark's filebuf memcpy into
    // .text. Every cell must start in the unbuffered (zero) state.
    for (u64 off = 0; off < CrtDataBlock::ACRT_FILE_SIZE; off += 8) {
        if (m.write_u64(stream + off, 0).is_err()) return 0;
    }
    return static_cast<u32>(stream);
}

void stdio_initialize_guest_binary_buffer(CrtMemoryManager& memory_manager, Marshaler& m,
                                          u32 stream, VfsFile& file) {
    using namespace crt_stdio_buffer;
    if (file.text_mode) return;
    const GuestAddr buffer =
        memory_manager.crt_malloc(kDefaultBufferBytes);
    if (buffer == 0 || buffer > 0xFFFFFFFFULL) return;

    file.guest_memory = &m.memory();
    file.guest_stream = stream;
    file.guest_buffer = static_cast<u32>(buffer);
    const auto public_cells = cells(file.layout);
    // The opaque UCRT cells are pointer-wide slots (0x00/0x08/0x10);
    // the legacy _iobuf packs _cnt/_flag right after 4-byte pointers.
    if (file.layout == Layout::UcrtOpaque32) {
        (void)m.write_u64(stream + public_cells.ptr, file.guest_buffer);
        (void)m.write_u64(stream + public_cells.base, file.guest_buffer);
    } else {
        (void)m.write_u32(stream + public_cells.ptr, file.guest_buffer);
        (void)m.write_u32(stream + public_cells.base, file.guest_buffer);
    }
    (void)m.write_u32(stream + public_cells.count, 0);
    (void)m.write_u32(stream + kCapacity,
                      static_cast<u32>(kDefaultBufferBytes));
    if (file.layout == Layout::LegacyIobuf32) {
        (void)m.write_u32(stream + public_cells.buffer_size,
                          static_cast<u32>(kDefaultBufferBytes));
    }
    (void)m.write_u32(stream + kLock, 0);
    (void)m.write_u32(stream + kHasRead, 0);
    (void)m.write_u32(stream + kPushback, 0);
    (void)m.write_u32(stream + kEof, 0);
    // Publish the marker last: accelerated code cannot enter until
    // every public FILE cell and private sidecar field is initialized.
    (void)m.write_u32(stream + kMagic, kMagicValue);
}

// Give a freshly opened stream a real descriptor and publish it in the
// legacy FILE layout.
//
// `fileno(f)` is a *macro* in both the MSVC and MinGW headers — it expands
// to `((f)->_file)` and reads the struct directly, so no thunk is involved
// and Vitreo cannot intercept it. The CRT arena hands back zeroed memory,
// which made every fopen'd stream report descriptor 0, i.e. stdin. A guest
// that read a file through _read(fileno(f), ...) then hit stdin and got EOF
// immediately, so the file looked empty and its contents never parsed.
//
//   struct _iobuf { char* _ptr; int _cnt; char* _base; int _flag;
//                   int _file; int _charbuf; int _bufsiz; char* _tmpfname; }
i32 stdio_bind_stream_descriptor(capsule::Vfs& vfs, CrtMemoryManager& memory_manager,
                                 CrtStdioManager& mgr, Marshaler& m, u32 stream,
                                 VfsFile& file) {
    constexpr u32 IOREAD  = 0x0001;
    constexpr u32 IOWRT   = 0x0002;

    LowIoFd entry;
    entry.handle = stream_handle(vfs, file);
    entry.text_mode = file.text_mode;
    // The stream owns the VFS handle. Its descriptor nevertheless
    // closes that same handle when _close is called first.
    entry.owns_handle = false;
    entry.aliased_file = stream;
    entry.win32_access =
        (capsule::has_access(file.access, capsule::AccessMode::Read)
             ? 0x80000000u : 0u) |
        (capsule::has_access(file.access, capsule::AccessMode::Write)
             ? 0x40000000u : 0u);
    const i32 fd = stdio_alloc_fd(mgr);
    if (fd < 0) { memory_manager.set_errno(24); return -1; }
    stdio_arena().descriptors[fd] = entry;
    stdio_bind_reused_standard(mgr, fd);

    if (file.layout ==
        crt_stdio_buffer::Layout::LegacyIobuf32) {
        if (m.cpu_is_64bit()) {
            // x64 struct _iobuf: _flag at +24, _file at +28.
            (void)m.write_u32(stream + CrtDataBlock::LEGACY64_FLAG_OFFSET,
                              IOREAD | IOWRT);
            (void)m.write_u32(stream + CrtDataBlock::LEGACY64_FILE_OFFSET,
                              static_cast<u32>(fd));
        } else {
            const auto public_cells = crt_stdio_buffer::cells(file.layout);
            (void)m.write_u32(
                stream + public_cells.flag, IOREAD | IOWRT);
            (void)m.write_u32(
                stream + public_cells.file, static_cast<u32>(fd));
        }
    }
    return fd;
}

i32 stdio_standard_stream_index(CrtDataBlock& crt_data, GuestAddr stream) {
    const i32 acrt_index = crt_data.acrt_stream_index(stream);
    if (acrt_index >= 0) return acrt_index;

    // msvcrt's public _iob[] (32-byte FILE on x86, 48-byte on x64).
    return crt_data.legacy_stream_index(stream);
}

FILE* stdio_host_standard_stream(CrtStdioManager& mgr, CrtDataBlock& crt_data,
                                 GuestAddr stream) {
    const i32 idx = stdio_standard_stream_index(crt_data, stream);
    if (idx < 0 || idx > 2) return nullptr;
    if (std_descriptor(mgr, idx).file_closed) return nullptr;
    return std_host_file(mgr, idx);
}

} // namespace vitreo::winapi::crt_stdio_internal
