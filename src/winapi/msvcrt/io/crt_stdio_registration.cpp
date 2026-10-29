// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_registration.cpp
/// @brief Export table for msvcrt's stdio surface.
///
/// Composition only: creates the per-DLL stdio arena and runs one registrar
/// per export family, in the order the entries were always registered. Each
/// registrar maps export names to handlers and records each one's
/// stack-cleanup contract. msvcrt is cdecl, so most entries pop zero bytes and
/// the guest cleans its own stack.
///
/// Split out of crt_stdio.cpp, which was 2,870 lines and seven-eighths this
/// table, and then split again by export family:
///  - crt_stdio_stream_registration.cpp, crt_stdio_open_registration.cpp,
///    crt_stdio_stream_io_registration.cpp,
///    crt_stdio_stream_position_registration.cpp,
///    crt_stdio_char_registration.cpp, crt_stdio_guest_char_registration.cpp
///    and crt_stdio_fprintf_registration.cpp register the FILE* functions;
///  - crt_lowio_open_registration.cpp, crt_lowio_transfer_registration.cpp,
///    crt_lowio_descriptor_registration.cpp and crt_lowio_stat_registration.cpp
///    register the descriptor functions;
///  - crt_stdio_export_locking.cpp wraps every entry in the stdio lock domain;
///  - crt_stdio_internal.hpp and crt_stdio_registration_internal.hpp are the
///    private shared surface, with its state in crt_stdio_state.cpp,
///    crt_stdio_stream_state.cpp, crt_stdio_std_descriptors.cpp and
///    crt_stdio_stream_binding.cpp.
/// The fprintf family formats through crt_printf.hpp's output processor and
/// writes the result through the stream helpers.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.

#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/io/crt_stdio_registration_internal.hpp"

#include <memory>

namespace vitreo::winapi {

using namespace crt_stdio_internal;

// =============================================================================
// Thunk exports
// =============================================================================

DllExports create_crt_stdio_thunks(CrtStdioManager& mgr, capsule::Vfs& vfs,
                                  CrtMemoryManager& memory_manager,
                                   CrtDataBlock& crt_data,
                                   crt_stdio_buffer::Layout file_layout) {
    DllExports exports;
    // Each CRT DLL's exports retain their own stream/descriptor arena. API
    // set aliases share the UCRT exports rather than creating another arena.
    auto arena = std::make_shared<StdioArena>();
    arena->memory_manager = &memory_manager;
    StdioArenaScope construction_scope(*arena);

    const StdioContext ctx{mgr, vfs, memory_manager, crt_data, file_layout};

    add_crt_stdio_buffer_exports(exports, ctx);

    // The string-target printf backends (__stdio_common_vsprintf and its
    // _s/_p/wide/vsnprintf_s siblings) live in crt_printf_registration.cpp.

    add_crt_stdio_guest_char_exports(exports, ctx, arena);
    add_crt_stdio_open_exports(exports, ctx);
    add_crt_stdio_close_exports(exports, ctx);
    add_crt_stdio_transfer_exports(exports, ctx);
    add_crt_stdio_position_exports(exports, ctx);
    add_crt_stdio_tmpfile_exports(exports, ctx);

    // =========================================================================
    // Low-level (POSIX descriptor) I/O
    // =========================================================================
    //
    // Descriptors 0/1/2 are the standard streams and are handled by the host.
    // Everything else is a real VFS handle, so `_open`/`_read`/`_lseeki64` stay
    // inside the capsule. `owns_handle` is false for descriptors produced by
    // `_fileno`, which alias a FILE* the stream layer still owns.

    add_crt_lowio_open_exports(exports, ctx);
    add_crt_lowio_close_exports(exports, ctx);
    add_crt_lowio_transfer_exports(exports, ctx);
    add_crt_lowio_fstat64_exports(exports, ctx);
    add_crt_lowio_descriptor_exports(exports, ctx);
    add_crt_stdio_seek64_exports(exports, ctx);
    add_crt_stdio_wide_exports(exports, ctx);
    add_crt_stdio_cputs_exports(exports, ctx);
    add_crt_stdio_fprintf_exports(exports, ctx);
    add_crt_stdio_legacy_char_exports(exports, ctx);
    add_crt_stdio_freopen_exports(exports, ctx);
    add_crt_stdio_getwc_exports(exports, ctx);
    add_crt_lowio_stat_exports(exports, ctx);
    add_crt_lowio_chsize_exports(exports, ctx);

    // The scanf family runs its own input processor (crt_scanf.cpp); its
    // stream forms read through the guest getc/ungetc configured above.
    add_crt_scanf_exports(exports, mgr, memory_manager, crt_data);

    lock_crt_stdio_exports(exports, ctx, arena);

    return exports;
}

} // namespace vitreo::winapi
