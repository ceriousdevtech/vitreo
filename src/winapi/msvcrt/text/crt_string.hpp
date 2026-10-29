#pragma once
/// @file crt_string.hpp
/// @brief msvcrt string and formatted I/O functions (WIN32-068)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <cstdarg>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class Marshaler;
class CrtMemoryManager;

/// msvcrt string/IO function implementations
class CrtStringManager {
public:
    CrtStringManager() = default;

    // String comparison (case-insensitive)
    [[nodiscard]] static int stricmp(const char* s1, const char* s2);
    [[nodiscard]] static int wcsicmp(const wchar_t* s1, const wchar_t* s2);
    [[nodiscard]] static int strnicmp(const char* s1, const char* s2, usize count);
    [[nodiscard]] static int wcsnicmp(const wchar_t* s1, const wchar_t* s2, usize count);

    // String duplication
    [[nodiscard]] static char* crt_strdup(const char* s);
    [[nodiscard]] static wchar_t* crt_wcsdup(const wchar_t* s);

    // Integer conversion
    [[nodiscard]] static int wtoi(const wchar_t* str);
    [[nodiscard]] static long wtol(const wchar_t* str);
    [[nodiscard]] static i64 atoi64(const char* str);
    [[nodiscard]] static i64 wtoi64(const wchar_t* str);

    // Formatted output
    [[nodiscard]] static int crt_snprintf(char* buffer, usize count,
                                           const char* format, ...);
    [[nodiscard]] static int crt_vsnprintf(char* buffer, usize count,
                                            const char* format, va_list args);
    [[nodiscard]] static int crt_sprintf(char* buffer, const char* format, ...);
    [[nodiscard]] static int crt_scprintf(const char* format, ...);
    [[nodiscard]] static int crt_snwprintf(wchar_t* buffer, usize count,
                                            const wchar_t* format, ...);

    // Formatted input
    [[nodiscard]] static int crt_sscanf(const char* buffer, const char* format, ...);

    // Memory/string ops
    static void* crt_memset(void* dest, int c, usize count);
    static void* crt_memcpy(void* dest, const void* src, usize count);
    static void* crt_memmove(void* dest, const void* src, usize count);
    [[nodiscard]] static int crt_memcmp(const void* buf1, const void* buf2, usize count);
    [[nodiscard]] static usize crt_strlen(const char* str);
    [[nodiscard]] static usize crt_wcslen(const wchar_t* str);
    static char* crt_strcpy(char* dest, const char* src);
    static char* crt_strcat(char* dest, const char* src);

    /// Resolve an ANSI Windows path against the process current directory.
    [[nodiscard]] static std::optional<std::string> resolve_fullpath(
        std::string_view path, std::string_view current_directory);
};

DllExports create_crt_string_thunks(CrtStringManager& mgr,
                                    CrtMemoryManager& mem_mgr,
                                    vitreo::capsule::Vfs& vfs);

/// strcpy_s/strcat_s/strncpy_s/strncat_s and the wcs* forms
/// (crt_secure_string.cpp); appended by create_crt_string_thunks.
void add_crt_secure_string_exports(DllExports& exports,
                                   CrtMemoryManager& mem_mgr);

/// strtod/strtof/wcstod/wcstof/atof/_wtof (+_l, long double spellings;
/// crt_strtod.cpp); appended by create_crt_string_thunks.
void add_crt_strtod_exports(DllExports& exports, CrtMemoryManager& mem_mgr);

} // namespace vitreo::winapi
