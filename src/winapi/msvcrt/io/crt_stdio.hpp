#pragma once
/// @file crt_stdio.hpp
/// @brief msvcrt file I/O / stdio functions (WIN32-084)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/io/crt_stdio_buffer.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class ConsoleManager;
class ThreadManager;
class CrtDataBlock;
class FileManager;
class Marshaler;

/// msvcrt stdio function implementations
class CrtStdioManager {
public:
    CrtStdioManager();
    ~CrtStdioManager();

    // File open/close
    [[nodiscard]] i32 crt_fopen(const std::string& filename,
                                const std::string& mode);
    i32 crt_fclose(i32 fd);

    // Read/Write
    [[nodiscard]] usize crt_fread(void* ptr, usize size, usize count, i32 fd);
    [[nodiscard]] usize crt_fwrite(const void* ptr, usize size, usize count,
                                   i32 fd);
    [[nodiscard]] i32 crt_fgetc(i32 fd);
    i32 crt_fputc(i32 ch, i32 fd);
    [[nodiscard]] bool crt_fgets(char* str, i32 n, i32 fd);
    i32 crt_fputs(const char* str, i32 fd);
    i32 crt_ungetc(i32 ch, i32 fd);

    // Guest FILE* character I/O is configured by create_crt_stdio_thunks so
    // the execution loop can service the two hottest CRT imports without a
    // full generic host-call transition. The callbacks are the same stream
    // implementation used by the registered handlers; this is only a faster
    // route to that contract.
    using GuestFgetc = std::function<i32(u32)>;
    using GuestUngetc = std::function<i32(i32, u32)>;
    void configure_guest_character_io(GuestFgetc fgetc,
                                      GuestUngetc ungetc);
    [[nodiscard]] i32 guest_fgetc(u32 stream) const;
    [[nodiscard]] i32 guest_ungetc(i32 ch, u32 stream) const;

    /// How a stream stores wide characters, which decides what fgetwc reads
    /// (Learn fgetc/fgetwc, fopen ccs=, _setmode): Ansi is a text stream in
    /// the locale's multibyte code page, Binary reads UTF-16 units as byte
    /// pairs, Utf8/Utf16Le are ccs= or _O_U8TEXT/_O_U16TEXT/_O_WTEXT.
    enum class WideEncoding : u8 { Ansi, Binary, Utf8, Utf16Le };
    using GuestStreamEncoding = std::function<WideEncoding(u32)>;
    void configure_guest_stream_encoding(GuestStreamEncoding encoding) {
        m_guest_stream_encoding = std::move(encoding);
    }
    /// Ansi when no stream table is configured.
    [[nodiscard]] WideEncoding guest_stream_encoding(u32 stream) const {
        return m_guest_stream_encoding ? m_guest_stream_encoding(stream)
                                       : WideEncoding::Ansi;
    }

    // Positioning
    i32 crt_fseek(i32 fd, i64 offset, i32 origin);
    [[nodiscard]] i64 crt_ftell(i32 fd);
    void crt_rewind(i32 fd);

    // Status
    [[nodiscard]] i32 crt_feof(i32 fd);
    [[nodiscard]] i32 crt_ferror(i32 fd);
    i32 crt_fflush(i32 fd);

    // Formatted
    i32 crt_fprintf(i32 fd, const char* format, ...);

    // tmpfile
    [[nodiscard]] i32 crt_tmpfile();

    // Get host FILE* for fd
    [[nodiscard]] FILE* get_file(i32 fd) const;

    /// kernel32's file-handle owner. A Windows CRT descriptor is a thin layer
    /// over a kernel32 HANDLE: _get_osfhandle returns it and _open_osfhandle
    /// wraps one (Learn). Set by the runtime before the thunks are created;
    /// without it both fail with EBADF.
    void set_file_manager(FileManager* files) noexcept { m_file_manager = files; }
    [[nodiscard]] FileManager* file_manager() const noexcept {
        return m_file_manager;
    }

    /// DuplicateHandle also accepts the no-console -2 current-thread pseudo
    /// handle stored in an allocated standard descriptor (native CRT probe).
    void set_thread_manager(ThreadManager* threads) noexcept { m_thread_manager = threads; }
    [[nodiscard]] ThreadManager* thread_manager() const noexcept { return m_thread_manager; }

    /// The process's std handle slots. At start-up the CRT binds descriptors
    /// 0-2 to GetStdHandle's values (stdout_pipe_chain_probe): a console
    /// handle is the host console, a pipe or file is read and written through
    /// kernel32, and no handle leaves the descriptor without a stream.
    void set_console_manager(ConsoleManager* console) noexcept {
        m_console_manager = console;
    }
    [[nodiscard]] ConsoleManager* console_manager() const noexcept {
        return m_console_manager;
    }
    /// Distinct for every manager ever constructed: process-wide CRT state
    /// keyed to it starts over for a new process's CRT, even at a reused
    /// address.
    [[nodiscard]] u64 instance_id() const noexcept { return m_instance_id; }

private:
    static constexpr i32 MAX_FILES = 512;
    FILE* m_files[MAX_FILES];
    i32 m_unget[MAX_FILES]; // -1 if none
    GuestFgetc m_guest_fgetc;
    GuestUngetc m_guest_ungetc;
    GuestStreamEncoding m_guest_stream_encoding;
    FileManager* m_file_manager{nullptr};
    ThreadManager* m_thread_manager{nullptr};
    ConsoleManager* m_console_manager{nullptr};
    u64 m_instance_id{0};

    i32 allocate_fd();
};

class CrtMemoryManager;

DllExports create_crt_stdio_thunks(CrtStdioManager& mgr, capsule::Vfs& vfs,
                                   CrtMemoryManager& memory_manager,
                                   CrtDataBlock& crt_data,
                                   crt_stdio_buffer::Layout layout =
                                       crt_stdio_buffer::Layout::LegacyIobuf32);

/// The scanf family (crt_scanf.cpp): UCRT __stdio_common_vsscanf /
/// vswscanf / vfscanf and msvcrt.dll's sscanf, swscanf, _snscanf, _snwscanf,
/// fscanf and scanf with their _s and _l forms. Stream input goes through
/// @p io's guest getc/ungetc; scanf reads msvcrt's stdin from @p crt_data.
void add_crt_scanf_exports(DllExports& exports, CrtStdioManager& io,
                           CrtMemoryManager& memory_manager, CrtDataBlock& crt_data);

} // namespace vitreo::winapi
