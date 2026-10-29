/// @file crt_stdio.cpp
/// @brief msvcrt file I/O / stdio functions (WIN32-084)

#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/io/crt_filesystem.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Constructor / Destructor
// =============================================================================

CrtStdioManager::CrtStdioManager() {
    static std::atomic<u64> s_next_instance{1};
    m_instance_id = s_next_instance.fetch_add(1, std::memory_order_relaxed);
    std::memset(m_files, 0, sizeof(m_files));
    m_files[0] = stdin;
    m_files[1] = stdout;
    m_files[2] = stderr;
    for (i32 i = 0; i < MAX_FILES; ++i) {
        m_unget[i] = -1;
    }
}

CrtStdioManager::~CrtStdioManager() {
    // Close any files we opened (skip stdin/stdout/stderr)
    for (i32 i = 3; i < MAX_FILES; ++i) {
        if (m_files[i]) {
            ::fclose(m_files[i]);
            m_files[i] = nullptr;
        }
    }
}

// =============================================================================
// Helpers
// =============================================================================

i32 CrtStdioManager::allocate_fd() {
    for (i32 i = 3; i < MAX_FILES; ++i) {
        if (!m_files[i]) return i;
    }
    return -1;
}

FILE* CrtStdioManager::get_file(i32 fd) const {
    if (fd < 0 || fd >= MAX_FILES) return nullptr;
    return m_files[fd];
}

// =============================================================================
// File open/close
// =============================================================================

i32 CrtStdioManager::crt_fopen(const std::string& filename,
                                const std::string& mode) {
    FILE* f = ::fopen(filename.c_str(), mode.c_str());
    if (!f) return -1;

    i32 fd = allocate_fd();
    if (fd < 0) {
        ::fclose(f);
        return -1;
    }

    m_files[fd] = f;
    m_unget[fd] = -1;
    return fd;
}

i32 CrtStdioManager::crt_fclose(i32 fd) {
    if (fd < 0 || fd >= MAX_FILES || !m_files[fd]) return -1;
    // Don't close stdin/stdout/stderr
    if (fd < 3) return -1;

    i32 rc = ::fclose(m_files[fd]);
    m_files[fd] = nullptr;
    m_unget[fd] = -1;
    return rc;
}

// =============================================================================
// Read/Write
// =============================================================================

usize CrtStdioManager::crt_fread(void* ptr, usize size, usize count, i32 fd) {
    FILE* f = get_file(fd);
    if (!f || !ptr) return 0;
    return ::fread(ptr, size, count, f);
}

usize CrtStdioManager::crt_fwrite(const void* ptr, usize size, usize count,
                                   i32 fd) {
    FILE* f = get_file(fd);
    if (!f || !ptr) return 0;
    return ::fwrite(ptr, size, count, f);
}

i32 CrtStdioManager::crt_fgetc(i32 fd) {
    FILE* f = get_file(fd);
    if (!f) return -1;

    // Check for pushed-back character
    if (m_unget[fd] != -1) {
        i32 ch = m_unget[fd];
        m_unget[fd] = -1;
        return ch;
    }
    return ::fgetc(f);
}

i32 CrtStdioManager::crt_fputc(i32 ch, i32 fd) {
    FILE* f = get_file(fd);
    if (!f) return -1;
    return ::fputc(ch, f);
}

bool CrtStdioManager::crt_fgets(char* str, i32 n, i32 fd) {
    FILE* f = get_file(fd);
    if (!f || !str || n <= 0) return false;
    return ::fgets(str, n, f) != nullptr;
}

i32 CrtStdioManager::crt_fputs(const char* str, i32 fd) {
    FILE* f = get_file(fd);
    if (!f || !str) return -1;
    return ::fputs(str, f);
}

i32 CrtStdioManager::crt_ungetc(i32 ch, i32 fd) {
    if (fd < 0 || fd >= MAX_FILES || !m_files[fd]) return -1;
    m_unget[fd] = ch;
    return ch;
}

void CrtStdioManager::configure_guest_character_io(GuestFgetc fgetc,
                                                    GuestUngetc ungetc) {
    m_guest_fgetc = std::move(fgetc);
    m_guest_ungetc = std::move(ungetc);
}

i32 CrtStdioManager::guest_fgetc(u32 stream) const {
    return m_guest_fgetc ? m_guest_fgetc(stream) : EOF;
}

i32 CrtStdioManager::guest_ungetc(i32 ch, u32 stream) const {
    return m_guest_ungetc ? m_guest_ungetc(ch, stream) : EOF;
}

// =============================================================================
// Positioning
// =============================================================================

i32 CrtStdioManager::crt_fseek(i32 fd, i64 offset, i32 origin) {
    FILE* f = get_file(fd);
    if (!f) return -1;
    return ::fseek(f, static_cast<long>(offset), origin);
}

i64 CrtStdioManager::crt_ftell(i32 fd) {
    FILE* f = get_file(fd);
    if (!f) return -1;
    return static_cast<i64>(::ftell(f));
}

void CrtStdioManager::crt_rewind(i32 fd) {
    FILE* f = get_file(fd);
    if (f) ::rewind(f);
}

// =============================================================================
// Status
// =============================================================================

i32 CrtStdioManager::crt_feof(i32 fd) {
    FILE* f = get_file(fd);
    if (!f) return 0;
    return ::feof(f);
}

i32 CrtStdioManager::crt_ferror(i32 fd) {
    FILE* f = get_file(fd);
    if (!f) return 0;
    return ::ferror(f);
}

i32 CrtStdioManager::crt_fflush(i32 fd) {
    FILE* f = get_file(fd);
    if (!f) return -1;
    return ::fflush(f);
}

// =============================================================================
// Formatted output
// =============================================================================

i32 CrtStdioManager::crt_fprintf(i32 fd, const char* format, ...) {
    FILE* f = get_file(fd);
    if (!f || !format) return -1;

    va_list args;
    va_start(args, format);
    i32 result = ::vfprintf(f, format, args);
    va_end(args);
    return result;
}

// =============================================================================
// tmpfile
// =============================================================================

i32 CrtStdioManager::crt_tmpfile() {
    FILE* f = ::tmpfile();
    if (!f) return -1;

    i32 fd = allocate_fd();
    if (fd < 0) {
        ::fclose(f);
        return -1;
    }

    m_files[fd] = f;
    m_unget[fd] = -1;
    return fd;
}

} // namespace vitreo::winapi
