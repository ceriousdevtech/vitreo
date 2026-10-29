/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/filesystem/file_thunks_internal.hpp"
#include "winapi/kernel32/filesystem/file_profile_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

/// @file file_thunks_and_pipes.cpp
/// @brief Thunk Implementations + Pipes
///
/// Combined closed cut from file.cpp (Thunks+Pipes share impl_*).

namespace vitreo::winapi {

/// Reads SECURITY_ATTRIBUTES::bInheritHandle. Defined with the pipe helpers
/// below; declared here because the CreateFile thunks need it as well.
static bool read_bool_from_security_attributes(Marshaler& m, GuestAddr sa_ptr);


// =============================================================================
// Thunk Implementations
// =============================================================================

// Win32 INVALID_HANDLE_VALUE is (HANDLE)(LONG_PTR)-1. PE32+ callers compare
// the full 64-bit handle, so zero-extending the internal u32 sentinel would
// turn a failed CreateFile into an apparently valid handle.
static u64 guest_file_handle(const Marshaler& m, Handle handle) noexcept {
    if (handle == INVALID_HANDLE && m.cpu_is_64bit()) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    return static_cast<u64>(handle);
}

/// The shared body of CreateFileW and CreateFile2. The two differ only in how
/// the caller spells the parameters (Learn, CreateFile2: the flags and
/// attributes move into an extended-parameters struct and the disposition
/// moves ahead of them), so the path handling, share modes and disposition
/// logic must not be duplicated between them.
static void create_file_common(Marshaler& m, FileManager& fm, bool wide);
static void create_file_core(Marshaler& m, FileManager& fm, bool wide,
                             GuestAddr name_ptr, u32 access, u32 share_mode,
                             GuestAddr security_attributes, u32 disposition,
                             u32 flags, Handle template_file);

void impl_CreateFileW(Marshaler& m, FileManager& fm) {
    // HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    //   DWORD dwShareMode, LPSECURITY_ATTRIBUTES, DWORD dwCreationDisposition,
    //   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
    create_file_common(m, fm, /*wide=*/true);
}

static void create_file_common(Marshaler& m, FileManager& fm, bool wide) {
    // DWORD arguments past the fourth sit in x64 stack slots whose upper
    // half is undefined: read them as 32-bit (arg32). hTemplateFile is a
    // pointer-sized HANDLE.
    create_file_core(m, fm, wide, m.arg_ptr(0), m.arg32(1), m.arg32(2),
                     m.arg_ptr(3), m.arg32(4), m.arg32(5),
                     static_cast<Handle>(m.arg(6)));
}

static void create_file_core(Marshaler& m, FileManager& fm, bool wide,
                             GuestAddr name_ptr, u32 access, u32 share_mode,
                             GuestAddr security_attributes, u32 disposition,
                             u32 flags, Handle template_file) {
    auto path_res = wide ? m.read_wide_string(name_ptr) : m.read_ansi_string(name_ptr);
    if (path_res.is_err()) {
        // An unreadable name has no NT path (ERROR_PATH_NOT_FOUND, as for "":
        // files_core_probe CreateFileW_empty_name).
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(guest_file_handle(m, INVALID_HANDLE));
        return;
    }
    // *A names are CP_ACP (Learn: Naming Files), never guessed as UTF-8.
    const std::string path = wide ? path_res.value()
                                  : capsule::ansi_to_utf8(path_res.value());
    // lpSecurityAttributes carries bInheritHandle, which decides whether this
    // handle survives into a child created with bInheritHandles=TRUE. Dropping
    // it leaves the handle silently non-inheritable, and a caller that hands
    // the numeric value to its child (WiX Burn's -burn.filehandle.attached)
    // then finds nothing open there.
    const bool inheritable = read_bool_from_security_attributes(
        m, security_attributes);

    auto __t0 = file_profile_wanted() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    Handle h = fm.create_file(path, access, share_mode, disposition, flags);
    if (file_profile_wanted()) {
        auto __t1 = std::chrono::steady_clock::now();
        u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(__t1 - __t0).count());
        file_profile_record(g_file_profile.create_file,
                            diagnostics::PerfFsKind::Create, ns);
    }
    // Learn (hTemplateFile): "When opening a new file, ... the template file
    // supplies file attributes and extended attributes for the file being
    // created"; ignored when opening an existing file, and an invalid handle
    // is ignored too (files_core_probe template_file_invalid_handle).
    if (h != INVALID_HANDLE && h != NULL_HANDLE && template_file != 0 &&
        get_last_error() == error_codes::ERROR_SUCCESS &&
        (disposition == CREATE_NEW || disposition == CREATE_ALWAYS ||
         disposition == OPEN_ALWAYS)) {
        fm.apply_template_attributes(h, template_file);
    }
    if (inheritable && h != INVALID_HANDLE && h != NULL_HANDLE) {
        (void)fm.handle_table().set_flags(h, HandleFlags::Inherit,
                                          HandleFlags::Inherit);
    }
    VITREO_TRACE("[file] CreateFile%c(\"%s\", access=0x%x, disp=%u) -> 0x%x%s\n",
                 wide ? 'W' : 'A', path.c_str(), access, disposition,
                 static_cast<unsigned>(h), inheritable ? " [inheritable]" : "");
    static const bool log_io_c = std::getenv("VITREO_LOG_FILEIO") != nullptr;
    if (log_io_c) {
        std::fprintf(stderr, "[fileio] ret=0x%llx CreateFile(\"%s\") -> 0x%x\n",
                     static_cast<unsigned long long>(m.guest_return_address()),
                     path.c_str(), static_cast<unsigned>(h));
    }
    m.set_return(guest_file_handle(m, h));
}

void impl_CreateFileA(Marshaler& m, FileManager& fm) {
    create_file_common(m, fm, /*wide=*/false);
}

void impl_ReOpenFile(Marshaler& m, FileManager& fm) {
    // HANDLE ReOpenFile(HANDLE hOriginalFile, DWORD dwDesiredAccess,
    //   DWORD dwShareMode, DWORD dwFlagsAndAttributes)
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile
    const Handle original = static_cast<Handle>(m.arg(0));
    const u32 access = static_cast<u32>(m.arg(1));
    const u32 share_mode = static_cast<u32>(m.arg(2));
    const u32 flags = static_cast<u32>(m.arg(3));
    const Handle h = fm.reopen_file(original, access, share_mode, flags);
    VITREO_TRACE("[file] ReOpenFile(0x%x, access=0x%x, share=0x%x, flags=0x%x) "
                 "-> 0x%x\n",
                 static_cast<unsigned>(original), access, share_mode, flags,
                 static_cast<unsigned>(h));
    m.set_return(static_cast<u64>(h));
}

void impl_WriteFile(Marshaler& m, FileManager& fm) {
    Handle handle         = static_cast<Handle>(m.arg(0));  // RCX
    GuestAddr buffer      = m.arg_ptr(1);                   // RDX
    u32 bytes_to_write    = static_cast<u32>(m.arg(2));     // R8
    GuestAddr bytes_written = m.arg_ptr(3);                 // R9
    GuestAddr overlapped    = m.arg_ptr(4);
    // Windows 11 (files_core_probe WriteFile_null_bytes_null_ov): a NULL
    // lpNumberOfBytesWritten with NULL OVERLAPPED succeeds and leaves last
    // error untouched — the out-count write is skipped. Learn's "may be NULL
    // only when overlapped" is not enforced as a failure on current Windows.
    u32 actual = 0;
    auto __t0 = file_profile_wanted() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    bool ok = false;
    u64 offset = 0;
    OverlappedPipeIoResult pipe_result = OverlappedPipeIoResult::Failed;
    if (overlapped != 0 &&
        fm.handle_table().get_type(handle) == HandleType::Pipe) {
        pipe_result = fm.begin_overlapped_pipe_write(
            handle, buffer, bytes_to_write, bytes_written, overlapped,
            m.cpu_is_64bit(), actual);
        ok = pipe_result == OverlappedPipeIoResult::Completed;
    } else if (overlapped != 0) {
        const GuestAddr offset_field = overlapped + (m.cpu_is_64bit() ? 16u : 8u);
        const u32 offset_low = m.read_u32(offset_field).value_or(0);
        const u32 offset_high = m.read_u32(offset_field + 4).value_or(0);
        offset = static_cast<u64>(offset_low) |
                 (static_cast<u64>(offset_high) << 32u);
        // FILE_FLAG_OVERLAPPED files never advance the sync file pointer,
        // and report their count only through the completion
        // (async_writefile_probe: *lpNumberOfBytesWritten = 0).
        const bool async_file = fm.is_overlapped_file(handle);
        ok = fm.write_file_at(handle, buffer, bytes_to_write,
                              async_file ? GuestAddr{0} : bytes_written,
                              offset, &actual, /*advance_pointer=*/!async_file);
        if (ok && async_file) {
            if (bytes_written != 0 && m.write_u32(bytes_written, 0).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            // async_writefile_probe: WriteFile on a FILE_FLAG_OVERLAPPED disk
            // handle returns FALSE + ERROR_IO_PENDING even when the write has
            // already completed (event set, Internal = STATUS_SUCCESS); the
            // result is collected through the event / GetOverlappedResult /
            // the completion port.
            fm.notify_overlapped_completion(handle, actual, overlapped,
                                            m.cpu_is_64bit(), 0, 0,
                                            /*completed_synchronously=*/false);
            set_last_error(error_codes::ERROR_IO_PENDING);
            m.set_return(0);
            return;
        }
    } else {
        ok = fm.write_file(handle, buffer, bytes_to_write, bytes_written, &actual);
    }
    if (pipe_result == OverlappedPipeIoResult::Completed &&
        bytes_written != 0 && m.write_u32(bytes_written, actual).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    if (ok && overlapped != 0 &&
        pipe_result != OverlappedPipeIoResult::Pending) {
        fm.notify_overlapped_completion(handle, actual, overlapped,
                                        m.cpu_is_64bit(), 0, 0,
                                        /*completed_synchronously=*/true);
    }
    if (file_profile_wanted()) {
        auto __t1 = std::chrono::steady_clock::now();
        u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(__t1 - __t0).count());
        file_profile_record(g_file_profile.write_file,
                            diagnostics::PerfFsKind::Write, ns,
                            ok ? bytes_to_write : 0);
    }

    // Watched-handle hex dump: when VITREO_WATCH_PATH is set and this handle's
    // path matched, dump the entire write buffer in hex+ascii so we can see
    // exactly what bytes (LF vs CRLF) the guest is sending to WriteFile.
    if (debug::is_watched(handle) && bytes_to_write > 0) {
        const char* path = debug::watched_path(handle);
        std::fprintf(stderr,
                     "[watch] WriteFile(handle=0x%x, bytes=%u) path=%s\n",
                     static_cast<unsigned>(handle), bytes_to_write,
                     path ? path : "?");
        // Hex dump 64 bytes per line, up to 4 KiB
        u32 dump = bytes_to_write < 4096 ? bytes_to_write : 4096;
        for (u32 off = 0; off < dump; off += 32) {
            char hex[32 * 3 + 1] = {};
            char asc[33] = {};
            char* hp = hex;
            u32 line = (dump - off) < 32 ? (dump - off) : 32;
            for (u32 i = 0; i < line; ++i) {
                auto rb = m.read_u8(buffer + static_cast<GuestAddr>(off + i));
                u8 c = rb.is_ok() ? rb.value() : 0;
                hp += std::snprintf(hp, 4, "%02x ", c);
                asc[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
            }
            std::fprintf(stderr, "[watch]   %04x: %-96s  %s\n", off, hex, asc);
        }
    }

    // Debug: VITREO_WRITE_DUMP dumps the content of every write, which is how
    // an app's own log file gets read back when the app rotates/deletes it
    // before the run ends.
    // Per-WriteFile TRACE is gated on VITREO_FILE_TRACE. A copy phase can
    // issue tens of thousands of ~32 KB writes; logging each one under
    // plain VITREO_TRACE dominates wall time.
    static const bool dump_all_writes = std::getenv("VITREO_WRITE_DUMP") != nullptr;
    static const bool file_trace = std::getenv("VITREO_FILE_TRACE") != nullptr;
    if (dump_all_writes) {
        char preview[256] = {};
        u32 show = bytes_to_write < 250 ? bytes_to_write : 250;
        for (u32 i = 0; i < show; ++i) {
            auto rb = m.read_u8(buffer + static_cast<GuestAddr>(i));
            if (rb.is_ok()) {
                u8 c = rb.value();
                preview[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
            } else {
                preview[i] = '?';
            }
        }
        preview[show] = '\0';
        VITREO_TRACE("[file] WriteFile(handle=0x%x, bytes=%u) content: %s\n",
                     static_cast<unsigned>(handle), bytes_to_write, preview);
    } else if (file_trace) {
        detail::trace_emit(
            "[file] WriteFile(handle=0x%x, bytes=%u, ovlp=0x%llx, offset=%llu) -> %s\n",
            static_cast<unsigned>(handle), bytes_to_write,
            static_cast<unsigned long long>(overlapped),
            static_cast<unsigned long long>(offset),
            ok ? "TRUE" : "FALSE");
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_ReadFile(Marshaler& m, FileManager& fm) {
    // The clock stamp for the VITREO_IO_STATS latency summary is only taken
    // when the knob is set; the profiling __t0 below has its own gate.
    static const bool io_stats = std::getenv("VITREO_IO_STATS") != nullptr;
    const auto io_t0 = io_stats ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    Handle handle        = static_cast<Handle>(m.arg(0));  // RCX
    GuestAddr buffer     = m.arg_ptr(1);                   // RDX
    u32 bytes_to_read    = static_cast<u32>(m.arg(2));     // R8
    GuestAddr bytes_read = m.arg_ptr(3);                   // R9
    GuestAddr overlapped = m.arg_ptr(4);
    // Windows 11 (files_core_probe ReadFile_null_bytes_null_ov): a NULL
    // lpNumberOfBytesRead with NULL OVERLAPPED succeeds and leaves last error
    // untouched — the out-count write is skipped (write_file/read_file already
    // tolerate bytes_*_addr == 0). Learn's "may be NULL only when overlapped"
    // is not enforced as a failure on current Windows.
    u32 actual = 0;
    auto __t0 = file_profile_wanted() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    bool ok = false;
    u64 offset = 0;
    OverlappedPipeIoResult pipe_result = OverlappedPipeIoResult::Failed;
    if (overlapped != 0 &&
        fm.handle_table().get_type(handle) == HandleType::Pipe) {
        pipe_result = fm.begin_overlapped_pipe_read(
            handle, buffer, bytes_to_read, overlapped, m.cpu_is_64bit(),
            actual);
        ok = pipe_result == OverlappedPipeIoResult::Completed;
    } else if (overlapped != 0) {
        const GuestAddr offset_field = overlapped + (m.cpu_is_64bit() ? 16u : 8u);
        const u32 offset_low = m.read_u32(offset_field).value_or(0);
        const u32 offset_high = m.read_u32(offset_field + 4).value_or(0);
        offset = static_cast<u64>(offset_low) |
                 (static_cast<u64>(offset_high) << 32u);
        // FILE_FLAG_OVERLAPPED files never advance the sync file pointer.
        const bool advance = !fm.is_overlapped_file(handle);
        ok = fm.read_file_at(handle, buffer, bytes_to_read, bytes_read,
                             offset, &actual, advance);
    } else {
        ok = fm.read_file(handle, buffer, bytes_to_read, bytes_read, &actual);
    }
    if (pipe_result == OverlappedPipeIoResult::Completed && bytes_read != 0 &&
        m.write_u32(bytes_read, actual).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    if (overlapped != 0 &&
        pipe_result != OverlappedPipeIoResult::Pending) {
        // Sync disk overlapped success writes STATUS_SUCCESS. A past-EOF read
        // returns TRUE from read_file_at with 0 bytes; convert it to FALSE +
        // ERROR_HANDLE_EOF and fill OVERLAPPED.Internal with STATUS_END_OF_FILE
        // (files_core_probe sync_ReadFile_overlapped_past_eof). Pipe completions
        // already notified inside begin_overlapped_pipe_read.
        //
        // FILE_FLAG_OVERLAPPED files always return FALSE + ERROR_IO_PENDING;
        // EOF still sets Internal = STATUS_END_OF_FILE so GetOverlappedResult
        // reports ERROR_HANDLE_EOF (files_core_probe async_ReadFile_*).
        constexpr u32 kStatusEndOfFile = 0xC0000011u;
        const bool disk_overlapped =
            fm.handle_table().get_type(handle) != HandleType::Pipe;
        const bool async_file = fm.is_overlapped_file(handle);
        if (disk_overlapped && async_file) {
            if (ok && bytes_to_read > 0 && actual == 0) {
                fm.notify_overlapped_completion(
                    handle, 0, overlapped, m.cpu_is_64bit(), kStatusEndOfFile,
                    error_codes::ERROR_HANDLE_EOF,
                    /*completed_synchronously=*/false);
                set_last_error(error_codes::ERROR_IO_PENDING);
                m.set_return(0);
                return;
            }
            if (ok) {
                fm.notify_overlapped_completion(
                    handle, actual, overlapped, m.cpu_is_64bit(), 0, 0,
                    /*completed_synchronously=*/false);
                set_last_error(error_codes::ERROR_IO_PENDING);
                m.set_return(0);
                return;
            }
        } else if (disk_overlapped && ok && bytes_to_read > 0 && actual == 0) {
            set_last_error(error_codes::ERROR_HANDLE_EOF);
            fm.notify_overlapped_completion(
                handle, 0, overlapped, m.cpu_is_64bit(), kStatusEndOfFile,
                error_codes::ERROR_HANDLE_EOF,
                /*completed_synchronously=*/true);
            ok = false;
        } else if (disk_overlapped && ok) {
            fm.notify_overlapped_completion(handle, actual, overlapped,
                                            m.cpu_is_64bit(), 0, 0,
                                            /*completed_synchronously=*/true);
        } else if (pipe_result == OverlappedPipeIoResult::Completed) {
            // A pipe read that finds bytes already buffered returns TRUE, and
            // Windows still queues an IOCP packet unless the handle set
            // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS. The caller may still wait
            // on that packet for the invitation even when ReadFile succeeded
            // inline.
            fm.notify_overlapped_completion(handle, actual, overlapped,
                                            m.cpu_is_64bit(), 0, 0,
                                            /*completed_synchronously=*/true);
        }
    }
    if (file_profile_wanted()) {
        auto __t1 = std::chrono::steady_clock::now();
        u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(__t1 - __t0).count());
        file_profile_record(g_file_profile.read_file,
                            diagnostics::PerfFsKind::Read, ns,
                            ok ? actual : 0);
    }
    if (debug::is_watched(handle)) {
        const char* path = debug::watched_path(handle);
        std::fprintf(stderr,
                     "[watch] ReadFile(handle=0x%x, req=%u, got=%u) path=%s -> %s\n",
                     static_cast<unsigned>(handle), bytes_to_read, actual,
                     path ? path : "?", ok ? "TRUE" : "FALSE");
    }
    // VITREO_IO_STATS: per-read latency summary every 500 reads.
    if (io_stats) {
        static std::atomic<u64> n{0}, total_ns{0}, max_ns{0}, over_1ms{0};
        const auto ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - io_t0).count());
        const auto c = n.fetch_add(1) + 1;
        total_ns.fetch_add(ns);
        if (ns > 1000000ULL) over_1ms.fetch_add(1);
        auto mx = max_ns.load();
        while (ns > mx && !max_ns.compare_exchange_weak(mx, ns)) {}
        if ((c % 500) == 0) {
            std::fprintf(stderr,
                "[io-stats] reads=%llu avg=%.3fms max=%.3fms >1ms=%llu (%.1f%%)\n",
                (unsigned long long)c,
                static_cast<double>(total_ns.load()) / static_cast<double>(c) / 1e6,
                static_cast<double>(max_ns.load()) / 1e6,
                (unsigned long long)over_1ms.load(),
                100.0 * static_cast<double>(over_1ms.load()) / static_cast<double>(c));
        }
    }

    static const bool log_io = std::getenv("VITREO_LOG_FILEIO") != nullptr;
    if (log_io) {
        // Position AFTER the read. A streaming loop that re-reads the same
        // chunk forever and one that is merely slow look identical from the
        // request size alone; the file pointer separates them.
        // Seeking 0 from FILE_CURRENT is the standard position query.
        const u32 pos = fm.set_file_pointer(handle, 0, nullptr, /*FILE_CURRENT=*/1);
        std::fprintf(stderr,
                     "[fileio] ret=0x%llx ReadFile(h=0x%x req=%u) -> %s got=%u "
                     "pos=0x%x\n",
                     static_cast<unsigned long long>(m.guest_return_address()),
                     static_cast<unsigned>(handle), bytes_to_read,
                     ok ? "TRUE" : "FALSE", actual, pos);
    }
    // Show first 16 bytes written to buffer
    char hex_preview[64] = "...";
    if (ok && actual > 0 && actual <= 64) {
        char* p = hex_preview;
        usize show = actual < 16 ? actual : 16;
        for (usize i = 0; i < show; ++i) {
            auto rb = m.read_u8(buffer + static_cast<GuestAddr>(i));
            if (rb.is_ok())
                p += std::snprintf(p, 4, "%02x ", rb.value());
        }
    }
    // For the critical LZMA header read (buf=0x414ed0), verify data in guest memory
    if (buffer == 0x414ed0u && actual > 0) {
        char verify[128] = "";
        char* vp = verify;
        usize vshow = actual < 32 ? actual : 32;
        for (usize i = 0; i < vshow; ++i) {
            auto rb = m.read_u8(buffer + static_cast<GuestAddr>(i));
            if (rb.is_ok())
                vp += std::snprintf(vp, 4, "%02x ", rb.value());
        }
        VITREO_TRACE( "[file] ReadFile VERIFY buf=0x414ed0 first %zu bytes: %s\n",
                     vshow, verify);
    }
    VITREO_TRACE( "[file] ReadFile handle=0x%x buf=0x%x req=%u got=%u ovlp=0x%llx offset=%llu -> %s  [%s]\n",
                 static_cast<unsigned>(handle),
                 static_cast<unsigned>(buffer),
                 bytes_to_read, actual,
                 static_cast<unsigned long long>(overlapped),
                 static_cast<unsigned long long>(offset),
                 ok ? "TRUE" : "FALSE",
                 hex_preview);
    if (pipe_result == OverlappedPipeIoResult::Pending) {
        set_last_error(error_codes::ERROR_IO_PENDING);
        m.set_return(0);
        return;
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_ReadFileEx(Marshaler& m, FileManager& fm) {
    // BOOL ReadFileEx(HANDLE, LPVOID, DWORD, LPOVERLAPPED,
    //                 LPOVERLAPPED_COMPLETION_ROUTINE)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfileex
    // The read may complete inline; the completion routine is still queued
    // and runs only when the issuing thread enters an alertable wait.
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 bytes_to_read = static_cast<u32>(m.arg(2));
    const GuestAddr overlapped = m.arg_ptr(3);
    const GuestAddr completion = m.arg_ptr(4);
    if (overlapped == 0 || completion == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr offset_field = overlapped + (is_64bit ? 16u : 8u);
    const u32 offset_low = m.read_u32(offset_field).value_or(0);
    const u32 offset_high = m.read_u32(offset_field + 4).value_or(0);
    const u64 offset = static_cast<u64>(offset_low) |
                       (static_cast<u64>(offset_high) << 32u);

    u32 actual = 0;
    const bool ok = fm.read_file_at(handle, buffer, bytes_to_read, 0, offset,
                                    &actual);
    if (!ok) {
        m.set_return(0);
        return;
    }

    constexpr u32 kStatusEndOfFile = 0xC0000011u;
    const bool eof = bytes_to_read > 0 && actual == 0;
    const u32 nt_status = eof ? kStatusEndOfFile : 0u;
    const u32 win_error = eof ? error_codes::ERROR_HANDLE_EOF : 0u;
    const u32 transferred = eof ? 0u : actual;

    if (is_64bit) {
        (void)m.write_u64(overlapped, nt_status);
        (void)m.write_u64(overlapped + 8, transferred);
    } else {
        (void)m.write_u32(overlapped, nt_status);
        (void)m.write_u32(overlapped + 4, transferred);
    }

    fm.queue_io_completion_routine(completion, win_error, transferred,
                                   overlapped);
    VITREO_TRACE("[file] ReadFileEx(handle=0x%x, bytes=%u, ovlp=0x%llx, "
                 "offset=%llu, pfn=0x%llx) -> TRUE eof=%u got=%u\n",
                 static_cast<unsigned>(handle), bytes_to_read,
                 static_cast<unsigned long long>(overlapped),
                 static_cast<unsigned long long>(offset),
                 static_cast<unsigned long long>(completion),
                 eof ? 1u : 0u, transferred);
    m.set_return(1);
}

void impl_WriteFileEx(Marshaler& m, FileManager& fm) {
    // BOOL WriteFileEx(HANDLE, LPCVOID, DWORD, LPOVERLAPPED,
    //                  LPOVERLAPPED_COMPLETION_ROUTINE)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefileex
    // Completion is reported only via FileIOCompletionRoutine once the issuing
    // thread enters an alertable wait. OVERLAPPED.hEvent is ignored.
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 bytes_to_write = static_cast<u32>(m.arg(2));
    const GuestAddr overlapped = m.arg_ptr(3);
    const GuestAddr completion = m.arg_ptr(4);
    if (overlapped == 0 || completion == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr offset_field = overlapped + (is_64bit ? 16u : 8u);
    const u32 offset_low = m.read_u32(offset_field).value_or(0);
    const u32 offset_high = m.read_u32(offset_field + 4).value_or(0);
    u64 offset = static_cast<u64>(offset_low) |
                 (static_cast<u64>(offset_high) << 32u);
    // Learn: Offset=OffsetHigh=0xFFFFFFFF writes at EOF (FILE_APPEND_DATA).
    if (offset_low == 0xFFFFFFFFu && offset_high == 0xFFFFFFFFu) {
        // The unambiguous 64-bit query: FileManager::get_file_size sets no
        // last error, so testing it after a 0xFFFFFFFF low half read
        // whatever error the thread already held.
        const auto size = fm.get_file_size64(handle);
        if (!size.has_value()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        offset = *size;
    }

    u32 actual = 0;
    // An overlapped handle's file pointer does not move (async_writefile_probe).
    const bool ok = fm.write_file_at(handle, buffer, bytes_to_write, 0, offset,
                                     &actual, !fm.is_overlapped_file(handle));
    if (!ok) {
        m.set_return(0);
        return;
    }

    if (is_64bit) {
        (void)m.write_u64(overlapped, 0);
        (void)m.write_u64(overlapped + 8, actual);
    } else {
        (void)m.write_u32(overlapped, 0);
        (void)m.write_u32(overlapped + 4, actual);
    }

    fm.queue_io_completion_routine(completion, 0, actual, overlapped);
    VITREO_TRACE("[file] WriteFileEx(handle=0x%x, bytes=%u, ovlp=0x%llx, "
                 "offset=%llu, pfn=0x%llx) -> TRUE wrote=%u\n",
                 static_cast<unsigned>(handle), bytes_to_write,
                 static_cast<unsigned long long>(overlapped),
                 static_cast<unsigned long long>(offset),
                 static_cast<unsigned long long>(completion), actual);
    m.set_return(1);
}

// =============================================================================
// Pipes (CreatePipe / PeekNamedPipe)
// =============================================================================

static bool read_bool_from_security_attributes(Marshaler& m, GuestAddr sa_ptr) {
    // SECURITY_ATTRIBUTES follows the guest ABI.  LPVOID widens on Win64,
    // placing bInheritHandle at +16 instead of the Win32 +8 offset.
    if (sa_ptr == 0) return false;
    const GuestAddr inherit_offset = m.cpu_is_64bit() ? 16u : 8u;
    auto inh = m.read_u32(sa_ptr + inherit_offset);
    if (inh.is_err()) return false;
    return inh.value() != 0;
}

static void impl_CreatePipe(Marshaler& m, FileManager& fm) {
    // BOOL CreatePipe(PHANDLE hReadPipe, PHANDLE hWritePipe,
    //                 LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize)
    GuestAddr read_out = m.arg_ptr(0);
    GuestAddr write_out = m.arg_ptr(1);
    GuestAddr sa_ptr = m.arg_ptr(2);
    (void)m.arg(3); // nSize (ignored for now)

    if (read_out == 0 || write_out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    bool inheritable = read_bool_from_security_attributes(m, sa_ptr);
    Handle read_h = NULL_HANDLE;
    Handle write_h = NULL_HANDLE;
    if (!fm.create_anonymous_pipe(read_h, write_h, inheritable)) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }

    // PHANDLE points to a pointer-sized HANDLE.  Zero-extend Vitreo's compact
    // handle IDs into the full guest slot so a Win64 caller never observes
    // stale upper bits from its output variables.
    (void)m.write_ptr(read_out, read_h);
    (void)m.write_ptr(write_out, write_h);

    ipc::ipc_log(
        "CreatePipe read=0x%x write=0x%x inheritable=%d",
        static_cast<unsigned>(read_h), static_cast<unsigned>(write_h),
        inheritable ? 1 : 0);

    m.set_return(1);
}

static void impl_PeekNamedPipe(Marshaler& m, FileManager& fm) {
    // BOOL PeekNamedPipe(HANDLE hNamedPipe, LPVOID lpBuffer, DWORD nBufferSize,
    //                    LPDWORD lpBytesRead, LPDWORD lpTotalBytesAvail,
    //                    LPDWORD lpBytesLeftThisMessage)
    Handle handle = static_cast<Handle>(m.arg(0));
    GuestAddr buf = m.arg_ptr(1);
    u32 buf_size = static_cast<u32>(m.arg(2));
    GuestAddr bytes_read_ptr = m.arg_ptr(3);
    GuestAddr total_avail_ptr = m.arg_ptr(4);
    GuestAddr left_ptr = m.arg_ptr(5);

    if (!fm.guest_memory()) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }

    auto type = fm.handle_table().get_type(handle);
    if (type != HandleType::Pipe) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    auto ep = fm.handle_table().get_shared<PipeEndpoint>(
        handle, HandleType::Pipe);
    if (!ep) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    // Cross-process / anonymous UnixPipe: FIONREAD + MSG_PEEK. Learn +
    // process_contract_probe.expected: drained+closed → FALSE/ERROR_BROKEN_PIPE
    // with out-params unchanged; with a buffer, copy without consuming.
    if (auto io = pipe_io(ep)) {
        const isize a = io->peek_available();
        if (a < 0) {
            // Do not touch *lpBytesRead / *lpTotalBytesAvail / *lpLeft —
            // Windows leaves them as the caller set them (probe: 0x77).
            set_last_error(error_codes::ERROR_BROKEN_PIPE);
            m.set_return(0);
            return;
        }
        const u32 avail32 = static_cast<u32>(a);
        u32 bytes_read = 0;
        if (buf != 0 && buf_size > 0 && avail32 > 0) {
            const u32 n = std::min(buf_size, avail32);
            std::vector<u8> tmp(n);
            const isize got = io->peek_read(tmp.data(), n);
            if (got > 0) {
                bytes_read = static_cast<u32>(got);
                auto wr = fm.guest_memory()->write_bytes(
                    buf, tmp.data(), bytes_read);
                if (wr.is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
        }
        if (bytes_read_ptr != 0) {
            (void)fm.guest_memory()->write_u32(bytes_read_ptr, bytes_read);
        }
        if (total_avail_ptr != 0) {
            (void)fm.guest_memory()->write_u32(total_avail_ptr, avail32);
        }
        if (left_ptr != 0) {
            // Measured on Windows 11 x64 (peek_left.exe): for anonymous
            // CreatePipe peeks, lpBytesLeftThisMessage is 0 with a null
            // buffer, and (DWORD)(-(LONG)bytes_read) when a buffer is used —
            // not the message-remaining value Learn describes for named
            // message-mode pipes (process_contract_probe.expected left=fffffffc).
            const u32 left = (buf != 0 && buf_size > 0)
                ? (0u - bytes_read)
                : 0u;
            (void)fm.guest_memory()->write_u32(left_ptr, left);
        }
        m.set_return(1);
        return;
    }
    if (!ep->state || !ep->readable) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    usize bytes_read = 0;
    usize avail = 0;

    {
        std::lock_guard<std::mutex> lock(ep->state->mutex);
        avail = pipe_available_locked(*ep->state);
        if (buf != 0 && buf_size > 0 && avail > 0) {
            bytes_read = std::min<usize>(avail, static_cast<usize>(buf_size));
            auto wr = fm.guest_memory()->write_bytes(
                buf,
                reinterpret_cast<const Byte*>(ep->state->buffer.data() + ep->state->read_pos),
                bytes_read);
            if (wr.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
        }
    }

    if (bytes_read_ptr != 0) (void)m.write_u32(bytes_read_ptr, static_cast<u32>(bytes_read));
    if (total_avail_ptr != 0) (void)m.write_u32(total_avail_ptr, static_cast<u32>(avail));
    if (left_ptr != 0) (void)m.write_u32(left_ptr, static_cast<u32>(avail - bytes_read));

    m.set_return(1);
}

// =============================================================================
// make_file_exports
// =============================================================================

DllExports make_file_exports(FileManager& fm) {
    DllExports exports;
    exports.entries.reserve(11);

    // CreateFileA
    exports.entries.push_back(ExportEntry{
        "CreateFileA", 0,
        [&fm](Marshaler& m) { impl_CreateFileA(m, fm); },
        28
    });

    // CreateFileW
    exports.entries.push_back(ExportEntry{
        "CreateFileW", 0,
        [&fm](Marshaler& m) { impl_CreateFileW(m, fm); },
        28
    });

    // CreateFile2 (5 args, stdcall_bytes=20)
    // HANDLE CreateFile2(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    //                    DWORD dwShareMode, DWORD dwCreationDisposition,
    //                    LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams)
    //
    // Learn (CreateFile2): the modern spelling of CreateFileW. The flags and
    // attributes that CreateFileW takes as its sixth parameter move into the
    // optional extended-parameters struct
    // { DWORD dwSize; DWORD dwFileAttributes; DWORD dwFileFlags;
    //   DWORD dwSecurityQosFlags; LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    //   HANDLE hTemplateFile; }, and the disposition moves ahead of them.
    exports.entries.push_back(ExportEntry{
        "CreateFile2", 0,
        [&fm](Marshaler& m) {
            const GuestAddr name = m.arg_ptr(0);
            const u32 access = static_cast<u32>(m.arg(1));
            const u32 share = static_cast<u32>(m.arg(2));
            const u32 disposition = static_cast<u32>(m.arg(3));
            const GuestAddr ex_params = m.arg_ptr(4);

            u32 attributes = 0;
            u32 flags = 0;
            GuestAddr security = 0;
            u32 template_file = 0;
            if (ex_params != 0) {
                const bool is64 = m.cpu_is_64bit();
                attributes = m.read_u32(ex_params + 4).value_or(0);
                flags = m.read_u32(ex_params + 8).value_or(0);
                // dwSecurityQosFlags at +12 folds into the flags word on
                // CreateFileW, which is where the SECURITY_* bits already live.
                flags |= m.read_u32(ex_params + 12).value_or(0);
                const GuestAddr sec_off = ex_params + 16;
                security = is64 ? static_cast<GuestAddr>(
                                      m.read_u64(sec_off).value_or(0))
                                : static_cast<GuestAddr>(
                                      m.read_u32(sec_off).value_or(0));
                const GuestAddr tmpl_off = sec_off + (is64 ? 8 : 4);
                template_file = is64
                    ? static_cast<u32>(m.read_u64(tmpl_off).value_or(0))
                    : m.read_u32(tmpl_off).value_or(0);
            }
            // Same body as CreateFileW, called with this spelling's operands.
            create_file_core(m, fm, /*wide=*/true, name, access, share, security,
                             disposition, attributes | flags,
                             static_cast<Handle>(template_file));
        },
        20  // five parameters: 16 left every x86 caller's ESP off by 4
    });

    // CreateFileTransactedW (10 args, stdcall_bytes=40)
    // HANDLE CreateFileTransactedW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
    //   DWORD, DWORD, HANDLE, HANDLE hTransaction, PUSHORT pusMiniVersion,
    //   PVOID lpExtendedParameter)
    // The first 7 args match CreateFileW exactly; impl_CreateFileW only reads
    // those, so we forward and the transaction handle / extras are ignored
    // (no real KTM in Vitreo — see src/winapi/ktmw32/ktmw32.cpp).
    exports.entries.push_back(ExportEntry{
        "CreateFileTransactedW", 0,
        [&fm](Marshaler& m) { impl_CreateFileW(m, fm); },
        40
    });

    // HANDLE ReOpenFile(HANDLE, DWORD, DWORD, DWORD) — 4 args, stdcall_bytes=16
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile
    exports.entries.push_back(ExportEntry{
        "ReOpenFile", 0,
        [&fm](Marshaler& m) { impl_ReOpenFile(m, fm); },
        16
    });

    // ReadFile (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "ReadFile", 0,
        [&fm](Marshaler& m) { impl_ReadFile(m, fm); },
        20
    });

    // BOOL ReadFileEx(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfileex
    exports.entries.push_back(ExportEntry{
        "ReadFileEx", 0,
        [&fm](Marshaler& m) { impl_ReadFileEx(m, fm); },
        20
    });

    // WriteFile (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "WriteFile", 0,
        [&fm](Marshaler& m) { impl_WriteFile(m, fm); },
        20
    });

    // BOOL WriteFileEx(HANDLE, LPCVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefileex
    exports.entries.push_back(ExportEntry{
        "WriteFileEx", 0,
        [&fm](Marshaler& m) { impl_WriteFileEx(m, fm); },
        20
    });

    // SetFilePointer (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "SetFilePointer", 0,
        [&fm](Marshaler& m) { impl_SetFilePointer(m, fm); },
        16
    });

    // GetFileSize (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetFileSize", 0,
        [&fm](Marshaler& m) { impl_GetFileSize(m, fm); },
        8
    });

    // GetFileType (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetFileType", 0,
        [&fm](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            Handle h = static_cast<Handle>(m.arg(0));
            const u32 result = fm.file_type(h);
            if (result == 0) {
                // Windows reports FILE_TYPE_UNKNOWN with
                // ERROR_INVALID_HANDLE for a handle it does not recognize.
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                ipc::ipc_log("GetFileType h=0x%x -> FILE_TYPE_UNKNOWN "
                             "(unrecognized handle)",
                             static_cast<unsigned>(h));
                m.set_return(0); // FILE_TYPE_UNKNOWN
                return;
            }
            ipc::ipc_log("GetFileType h=0x%x -> %u",
                         static_cast<unsigned>(h),
                         static_cast<unsigned>(result));
            m.set_return(result);
        },
        4
    });

    // BOOL SetFileValidData(HANDLE, LONGLONG)
    //
    // This API exposes uninitialized disk contents and Windows requires the
    // caller to hold SE_MANAGE_VOLUME_NAME. Vitreo does not grant that
    // privilege, and zero-filling would not satisfy the API contract.
    exports.entries.push_back(ExportEntry{
        "SetFileValidData", 0,
        [&fm](Marshaler& m) {
            const Handle handle = static_cast<Handle>(m.arg(0));
            if (fm.handle_table().get_type(handle) != HandleType::File ||
                fm.vfs_handle_for(handle) == capsule::INVALID_FILE_HANDLE) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            set_last_error(1314u); // ERROR_PRIVILEGE_NOT_HELD
            VITREO_TRACE("[file] SetFileValidData(0x%x, %llu) -> FALSE"
                         " (ERROR_PRIVILEGE_NOT_HELD)\n",
                         static_cast<unsigned>(handle),
                         static_cast<unsigned long long>(m.arg(1)));
            m.set_return(0);
        },
        12
    });

    // CreatePipe (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "CreatePipe", 0,
        [&fm](Marshaler& m) { impl_CreatePipe(m, fm); },
        16
    });

    // PeekNamedPipe (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "PeekNamedPipe", 0,
        [&fm](Marshaler& m) { impl_PeekNamedPipe(m, fm); },
        24
    });

    return exports;
}

} // namespace vitreo::winapi
