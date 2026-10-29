/// @file crt_string.cpp
/// @brief msvcrt string and formatted I/O functions (WIN32-068)

#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win_path.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include <strings.h> // strcasecmp / strncasecmp on macOS
#include <unistd.h>  // getpid for _mktemp

namespace vitreo::winapi {

// =============================================================================
// Case-insensitive comparison
// =============================================================================

int CrtStringManager::stricmp(const char* s1, const char* s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    return ::strcasecmp(s1, s2);
}

int CrtStringManager::wcsicmp(const wchar_t* s1, const wchar_t* s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    return ::wcscasecmp(s1, s2);
}

int CrtStringManager::strnicmp(const char* s1, const char* s2, usize count) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    return ::strncasecmp(s1, s2, count);
}

int CrtStringManager::wcsnicmp(const wchar_t* s1, const wchar_t* s2, usize count) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    // No wcsncasecmp on all platforms; manual loop
    for (usize i = 0; i < count; ++i) {
        wchar_t c1 = static_cast<wchar_t>(towlower(s1[i]));
        wchar_t c2 = static_cast<wchar_t>(towlower(s2[i]));
        if (c1 != c2) return (c1 < c2) ? -1 : 1;
        if (c1 == L'\0') return 0;
    }
    return 0;
}

// =============================================================================
// String duplication (malloc-based, caller must free)
// =============================================================================

char* CrtStringManager::crt_strdup(const char* s) {
    if (!s) return nullptr;
    usize len = ::strlen(s) + 1;
    auto* dup = static_cast<char*>(::malloc(len));
    if (dup) ::memcpy(dup, s, len);
    return dup;
}

wchar_t* CrtStringManager::crt_wcsdup(const wchar_t* s) {
    if (!s) return nullptr;
    usize len = ::wcslen(s) + 1;
    auto* dup = static_cast<wchar_t*>(::malloc(len * sizeof(wchar_t)));
    if (dup) ::memcpy(dup, s, len * sizeof(wchar_t));
    return dup;
}

// =============================================================================
// Integer conversion
// =============================================================================

int CrtStringManager::wtoi(const wchar_t* str) {
    if (!str) return 0;
    return static_cast<int>(::wcstol(str, nullptr, 10));
}

long CrtStringManager::wtol(const wchar_t* str) {
    if (!str) return 0;
    return ::wcstol(str, nullptr, 10);
}

i64 CrtStringManager::atoi64(const char* str) {
    if (!str) return 0;
    return static_cast<i64>(::strtoll(str, nullptr, 10));
}

i64 CrtStringManager::wtoi64(const wchar_t* str) {
    if (!str) return 0;
    return static_cast<i64>(::wcstoll(str, nullptr, 10));
}

// =============================================================================
// Formatted output
// =============================================================================

int CrtStringManager::crt_vsnprintf(char* buffer, usize count,
                                     const char* format, va_list args) {
    if (!format) return -1;
    // MSVC _vsnprintf: if result >= count, buffer is NOT null-terminated
    // and returns -1. Replicate that behavior.
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = ::vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (needed < 0) return -1;

    if (buffer && count > 0) {
        // Write into buffer without the automatic null-termination guarantee
        int written = ::vsnprintf(buffer, count, format, args);
        if (static_cast<usize>(written) >= count) {
            // MSVC behavior: truncate, do NOT null-terminate, return -1
            return -1;
        }
        return written;
    }
    return -1;
}

int CrtStringManager::crt_snprintf(char* buffer, usize count,
                                    const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = crt_vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int CrtStringManager::crt_sprintf(char* buffer, const char* format, ...) {
    if (!buffer || !format) return -1;
    va_list args;
    va_start(args, format);
    int result = ::vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int CrtStringManager::crt_scprintf(const char* format, ...) {
    if (!format) return -1;
    va_list args;
    va_start(args, format);
    int result = ::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    return result;
}

int CrtStringManager::crt_snwprintf(wchar_t* buffer, usize count,
                                     const wchar_t* format, ...) {
    if (!format) return -1;
    va_list args;
    va_start(args, format);
    int result = ::vswprintf(buffer, count, format, args);
    va_end(args);
    // vswprintf returns -1 on truncation, which matches MSVC _snwprintf
    return result;
}

// =============================================================================
// Formatted input
// =============================================================================

int CrtStringManager::crt_sscanf(const char* buffer, const char* format, ...) {
    if (!buffer || !format) return -1;
    va_list args;
    va_start(args, format);
    int result = ::vsscanf(buffer, format, args);
    va_end(args);
    return result;
}

// =============================================================================
// Memory / string ops
// =============================================================================

void* CrtStringManager::crt_memset(void* dest, int c, usize count) {
    return ::memset(dest, c, count);
}

void* CrtStringManager::crt_memcpy(void* dest, const void* src, usize count) {
    return ::memcpy(dest, src, count);
}

void* CrtStringManager::crt_memmove(void* dest, const void* src, usize count) {
    return ::memmove(dest, src, count);
}

int CrtStringManager::crt_memcmp(const void* buf1, const void* buf2, usize count) {
    return ::memcmp(buf1, buf2, count);
}

usize CrtStringManager::crt_strlen(const char* str) {
    if (!str) return 0;
    return ::strlen(str);
}

usize CrtStringManager::crt_wcslen(const wchar_t* str) {
    if (!str) return 0;
    return ::wcslen(str);
}

char* CrtStringManager::crt_strcpy(char* dest, const char* src) {
    return ::strcpy(dest, src);
}

char* CrtStringManager::crt_strcat(char* dest, const char* src) {
    return ::strcat(dest, src);
}

std::optional<std::string> CrtStringManager::resolve_fullpath(
    std::string_view path, std::string_view current_directory) {
    if (capsule::WinPath::is_unc_path(path)) {
        return std::nullopt;
    }

    std::string cwd = current_directory.empty() ? "C:\\" : std::string(current_directory);
    const bool cwd_has_drive = cwd.size() >= 2 && cwd[1] == ':';
    const char cwd_drive = cwd_has_drive
        ? capsule::normalize_drive_letter(cwd[0])
        : 'C';

    std::string candidate;
    const bool has_drive = path.size() >= 2 && path[1] == ':' &&
        capsule::is_valid_drive_letter(path[0]);
    if (has_drive && path.size() >= 3 && (path[2] == '\\' || path[2] == '/')) {
        candidate = path;
    } else if (has_drive) {
        const char drive = capsule::normalize_drive_letter(path[0]);
        if (drive == cwd_drive) {
            candidate = cwd;
            if (!candidate.empty() && candidate.back() != '\\' && candidate.back() != '/') {
                candidate.push_back('\\');
            }
            candidate.append(path.substr(2));
        } else {
            candidate.assign(1, drive);
            candidate += ":\\";
            candidate.append(path.substr(2));
        }
    } else if (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        candidate.assign(1, cwd_drive);
        candidate += ':';
        candidate.append(path);
    } else {
        candidate = cwd;
        if (!candidate.empty() && candidate.back() != '\\' && candidate.back() != '/') {
            candidate.push_back('\\');
        }
        candidate.append(path);
    }

    capsule::WinPath parsed(candidate);
    if (!parsed.is_valid()) {
        return std::nullopt;
    }
    capsule::WinPath normalized = parsed.normalize();
    std::string result = normalized.to_windows_string();
    if (normalized.is_root()) {
        result.push_back('\\');
    }
    return result;
}

// =============================================================================
// Thunk exports
// =============================================================================

/// Maximum buffer size for guest bulk operations

/// Length of a NUL-terminated ANSI string in guest memory.
///
/// Learn strlen: "returns the number of characters in str, excluding the
/// terminal null" -- there is no upper bound. The walk used to stop at 64 KiB
/// (MAX_BULK_SIZE) and report 65536 for any longer string (09_crt.md HIGH 9).
/// It now scans page by page until the terminator, `cap`, or the first
/// unreadable byte (where Windows would raise an access violation).
usize guest_strlen(GuestMemory& mem, GuestAddr addr, usize cap) {
    usize n = 0;
    while (n < cap) {
        const GuestAddr cur = addr + n;
        const usize page_left =
            GUEST_PAGE_SIZE - static_cast<usize>(cur & (GUEST_PAGE_SIZE - 1));
        const usize chunk = std::min(page_left, cap - n);
        if (const Byte* p = mem.host_ptr(cur, chunk)) {
            if (const void* nul = ::memchr(p, 0, chunk)) {
                return n + static_cast<usize>(static_cast<const Byte*>(nul) - p);
            }
        } else {
            for (usize i = 0; i < chunk; ++i) {
                auto r = mem.read_u8(cur + i);
                if (r.is_err() || r.value() == 0) return n + i;
            }
        }
        n += chunk;
    }
    return cap;
}

/// Length in UTF-16 units of a NUL-terminated wide string (see guest_strlen;
/// Learn wcslen has no bound either -- 09_crt.md HIGH 10).
usize guest_wcslen(GuestMemory& mem, GuestAddr addr, usize cap) {
    usize n = 0;
    while (n < cap) {
        const GuestAddr cur = addr + n * 2;
        const usize page_left =
            GUEST_PAGE_SIZE - static_cast<usize>(cur & (GUEST_PAGE_SIZE - 1));
        usize units = std::min(page_left / 2, cap - n);
        if (units == 0) {
            // An odd address with one byte left in the page: this unit
            // straddles two pages.
            auto r = mem.read_u16(cur);
            if (r.is_err() || r.value() == 0) return n;
            ++n;
            continue;
        }
        if (const Byte* p = mem.host_ptr(cur, units * 2)) {
            for (usize i = 0; i < units; ++i) {
                u16 unit;
                std::memcpy(&unit, p + i * 2, sizeof unit);
                if (unit == 0) return n + i;
            }
        } else {
            for (usize i = 0; i < units; ++i) {
                auto r = mem.read_u16(cur + i * 2);
                if (r.is_err() || r.value() == 0) return n + i;
            }
        }
        n += units;
    }
    return cap;
}

/// Read a wide string from guest into a host std::wstring
std::wstring guest_read_wstr(GuestMemory& mem, GuestAddr addr, usize len) {
    std::wstring result;
    result.reserve(len);
    for (usize i = 0; i < len; ++i) {
        auto r = mem.read_u16(addr + i * 2);
        if (r.is_err()) break;
        result.push_back(static_cast<wchar_t>(r.value()));
    }
    return result;
}

} // namespace vitreo::winapi
