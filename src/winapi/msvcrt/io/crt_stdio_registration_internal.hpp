#pragma once
/// @file crt_stdio_registration_internal.hpp
/// @brief Private surface between create_crt_stdio_thunks and its registrars.
///
/// create_crt_stdio_thunks was one 3,700-line function whose export handlers
/// captured about twenty local helper lambdas. It is now an orchestrator that
/// hands a StdioContext to one registrar per export family, in the order the
/// entries were always registered (DllExports::entries order decides each
/// thunk's address, which the export inventory records).
///
/// Helpers more than one registrar needs are free functions declared here;
/// a registrar re-creates the old lambda name over them so its handlers are
/// unchanged. Helpers one registrar uses stay local lambdas in it.

#include "winapi/msvcrt/io/crt_stdio_internal.hpp"

namespace vitreo::winapi {
class CrtDataBlock;
}

namespace vitreo::winapi::crt_stdio_internal {

/// The five arguments create_crt_stdio_thunks receives, shared by every
/// registrar.
struct StdioContext {
    CrtStdioManager& mgr;
    capsule::Vfs& vfs;
    CrtMemoryManager& memory_manager;
    CrtDataBlock& crt_data;
    crt_stdio_buffer::Layout file_layout;
};

// ---- helpers shared by several registrars ----------------------------------

/// First free descriptor number (a closed standard descriptor is reused), or -1.
i32 stdio_alloc_fd(CrtStdioManager& mgr);

/// Rebind a reused standard descriptor 0-2 to its new low-io entry.
void stdio_bind_reused_standard(CrtStdioManager& mgr, i32 fd);

/// A zeroed guest FILE object, or 0.
u32 stdio_allocate_stream_object(CrtMemoryManager& memory_manager, Marshaler& m);

/// Give a binary stream its guest-visible buffer and publish the marker.
void stdio_initialize_guest_binary_buffer(CrtMemoryManager& memory_manager, Marshaler& m,
                                          u32 stream, VfsFile& file);

/// Give a freshly opened stream a real descriptor; returns it, or -1.
i32 stdio_bind_stream_descriptor(capsule::Vfs& vfs, CrtMemoryManager& memory_manager,
                                 CrtStdioManager& mgr, Marshaler& m, u32 stream,
                                 VfsFile& file);

/// 0-2 when @p stream is stdin/stdout/stderr of either CRT flavor, else -1.
i32 stdio_standard_stream_index(CrtDataBlock& crt_data, GuestAddr stream);

/// The host FILE* behind a standard stream, or nullptr.
FILE* stdio_host_standard_stream(CrtStdioManager& mgr, CrtDataBlock& crt_data,
                                 GuestAddr stream);

/// Text-mode aware read (CR-LF to LF); returns bytes delivered into @p dst.
usize stdio_vfs_read_translated(capsule::Vfs& vfs, VfsFile& vf, std::span<u8> dst);

/// Text-mode aware write (LF to CR-LF); returns guest bytes consumed.
usize stdio_vfs_write_translated(capsule::Vfs& vfs, VfsFile& vf, std::span<const u8> src);

// ---- low-io constants ------------------------------------------------------

// _O_* flags as the CRT defines them.
constexpr i32 O_RDONLY_VALUE = 0x0000;
constexpr i32 O_WRONLY_VALUE = 0x0001;
constexpr i32 O_RDWR_VALUE   = 0x0002;
constexpr i32 O_APPEND_VALUE = 0x0008;
constexpr i32 O_CREAT_VALUE  = 0x0100;
constexpr i32 O_TRUNC_VALUE  = 0x0200;
constexpr i32 O_EXCL_VALUE   = 0x0400;
constexpr i32 O_TEMPORARY_VALUE = 0x0040;
constexpr i32 O_SHORT_LIVED_VALUE = 0x1000;  // _O_SHORT_LIVED
constexpr i32 O_NOINHERIT_VALUE = 0x0080;    // _O_NOINHERIT
constexpr i32 O_TEXT_VALUE   = 0x4000;
constexpr i32 O_BINARY_VALUE = 0x8000;
constexpr i32 O_WTEXT_VALUE  = 0x10000;
constexpr i32 O_U16TEXT_VALUE = 0x20000;
constexpr i32 O_U8TEXT_VALUE  = 0x40000;

constexpr u32 EBADF_VALUE   = 9;
constexpr u32 ENOENT_LOWIO  = 2;
constexpr u32 EINVAL_LOWIO  = 22;
constexpr u32 EEXIST_LOWIO  = 17;
constexpr u32 EACCES_LOWIO  = 13;
constexpr u32 EMFILE_LOWIO  = 24;
constexpr u32 EPIPE_LOWIO   = 32;  // UCRT errno.h EPIPE

// ---- registrars ------------------------------------------------------------

/// __acrt_iob_func and _get_stream_buffer_pointers.
void add_crt_stdio_buffer_exports(DllExports& exports, const StdioContext& ctx);

/// The guest-visible fgetc/ungetc character path: the binary-buffer helpers,
/// the arena callbacks the execution loop services and the __vitreo_* tails.
void add_crt_stdio_guest_char_exports(DllExports& exports, const StdioContext& ctx,
                                      const std::shared_ptr<StdioArena>& arena);

/// fopen, _fsopen, fopen_s and the wide forms, plus _lock_file/_unlock_file.
void add_crt_stdio_open_exports(DllExports& exports, const StdioContext& ctx);

/// fclose.
void add_crt_stdio_close_exports(DllExports& exports, const StdioContext& ctx);

/// fread, fwrite, fgetc, putchar, fputc, fgets, puts, fputs and ungetc.
void add_crt_stdio_transfer_exports(DllExports& exports, const StdioContext& ctx);

/// fseek, ftell, rewind, feof, ferror, clearerr and fflush.
void add_crt_stdio_position_exports(DllExports& exports, const StdioContext& ctx);

/// tmpfile and the legacy _iob accessors.
void add_crt_stdio_tmpfile_exports(DllExports& exports, const StdioContext& ctx);

/// _open, _sopen, _sopen_s and their wide and _dispatch forms.
void add_crt_lowio_open_exports(DllExports& exports, const StdioContext& ctx);

/// _close, _dup and _dup2.
void add_crt_lowio_close_exports(DllExports& exports, const StdioContext& ctx);

/// _read, _write, _lseeki64, _lseek and _filelengthi64.
void add_crt_lowio_transfer_exports(DllExports& exports, const StdioContext& ctx);

/// _fstat64 and _fstat64i32.
void add_crt_lowio_fstat64_exports(DllExports& exports, const StdioContext& ctx);

/// _locking, _setmode, _get_osfhandle, _isatty, _open_osfhandle, _fileno and
/// _fdopen.
void add_crt_lowio_descriptor_exports(DllExports& exports, const StdioContext& ctx);

/// _fseeki64, _ftelli64, fgetpos and fsetpos.
void add_crt_stdio_seek64_exports(DllExports& exports, const StdioContext& ctx);

/// setvbuf, fgetwc, fputwc and ungetwc.
void add_crt_stdio_wide_exports(DllExports& exports, const StdioContext& ctx);

/// _cputs.
void add_crt_stdio_cputs_exports(DllExports& exports, const StdioContext& ctx);

/// The fprintf family: UCRT __stdio_common_vf*printf* and msvcrt.dll's
/// fprintf/printf/wprintf exports.
void add_crt_stdio_fprintf_exports(DllExports& exports, const StdioContext& ctx);

/// _filbuf, _flsbuf and setbuf.
void add_crt_stdio_legacy_char_exports(DllExports& exports, const StdioContext& ctx);

/// freopen.
void add_crt_stdio_freopen_exports(DllExports& exports, const StdioContext& ctx);

/// getwc and putwc.
void add_crt_stdio_getwc_exports(DllExports& exports, const StdioContext& ctx);

/// _fstat and _stat.
void add_crt_lowio_stat_exports(DllExports& exports, const StdioContext& ctx);

/// _chsize.
void add_crt_lowio_chsize_exports(DllExports& exports, const StdioContext& ctx);

/// Wrap every registered entry in the stdio lock domain.
void lock_crt_stdio_exports(DllExports& exports, const StdioContext& ctx,
                            std::shared_ptr<StdioArena> arena);

} // namespace vitreo::winapi::crt_stdio_internal
