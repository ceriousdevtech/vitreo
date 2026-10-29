#pragma once

/// @file format_message.hpp
/// @brief kernel32!FormatMessageW / FormatMessageA: one engine, two entries.
///
/// Contract: Learn "FormatMessageW function (winbase.h)" for the documented
/// surface; everything Learn leaves open is measured on Windows 11 by
/// tools/oracle_probe/format_message_probe.c (x64 .expected and x86
/// .x86.expected), and tests/winapi/kernel32/test_format_message.cpp replays
/// those measurements. The measured rules the engine follows:
///
///  - Source text: a CR, LF, CRLF or LFCR line break becomes CRLF when the
///    width byte is 0, and one breakable space otherwise. "%n" is a hard
///    CRLF, "%r" a bare CR that starts a new line, "%t" a tab, "%b" a
///    breakable space, "%0" ends the message, "%" + any other character
///    emits that character (a non-breaking one), a trailing "%" fails with
///    ERROR_INVALID_PARAMETER. With IGNORE_INSERTS "%n %r %t %b %0" are still
///    processed, an insert is copied without a "!s!" spec, and every other
///    escape is copied with its '%'.
///  - Sources: FROM_STRING wins over the other two; FROM_HMODULE picks one
///    table language (exact for a nonzero LANGID, else neutral, thread UI,
///    user, system, en-US, any) and falls back to FROM_SYSTEM only when the
///    message id is missing from it; no table is ERROR_RESOURCE_DATA_NOT_FOUND.
///  - Inserts: "%N" with N in 1..200 (IGNORE_INSERTS copies up to 999), an
///    optional "!spec!" that is a whole printf format after '%' (default
///    "s"), formatted by the ntdll printf: no floating point, no %n, 'll'
///    and 'I64' are 64-bit, unknown conversions print their character.
///  - Argument numbering: ARGUMENT_ARRAY reads %N's first value from
///    element N-1; a va_list is walked lazily, caching one value per insert
///    number. Further values ('*' width/precision, the high half of an x86
///    I64) come from a cursor that each star advances once; a two-star insert
///    seen for the first time also claims insert N+1.
///  - Width byte 1..254: greedy wrapping that breaks at the last breakable
///    space (removing the space run around it) or hard-splits; inserts and
///    "% " never break.
///  - Output: return count excludes the NUL; too-small W buffers receive the
///    first nSize-1 characters and a NUL (ERROR_INSUFFICIENT_BUFFER); an empty
///    result is ERROR_NO_WORK_DONE; ALLOCATE_BUFFER works in 64K-WCHAR units
///    covering max(nSize + 1, 4 * (source length + 1)) and fails with
///    ERROR_MORE_DATA beyond them. FormatMessageA formats in UTF-16 and
///    converts: at most 32766 characters each way, and nSize truncated to 16
///    bits for the conversion.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <vector>

namespace vitreo::winapi {

class Marshaler;
class Kernel32ExtManager;
class GlobalLocalManager;
class LocaleStringManager;

namespace format_message {

// winbase.h
inline constexpr u32 kAllocateBuffer = 0x00000100;
inline constexpr u32 kIgnoreInserts  = 0x00000200;
inline constexpr u32 kFromString     = 0x00000400;
inline constexpr u32 kFromHmodule    = 0x00000800;
inline constexpr u32 kFromSystem     = 0x00001000;
inline constexpr u32 kArgumentArray  = 0x00002000;
inline constexpr u32 kMaxWidthMask   = 0x000000FF;

// winnt.h MESSAGE_RESOURCE_ENTRY.Flags
inline constexpr u16 kMessageResourceUnicode = 0x0001;
inline constexpr u16 kMessageResourceUtf8    = 0x0002;

/// What the engine needs from the rest of the runtime.
struct Environment {
    /// LANGIDs under RT_MESSAGETABLE/1 of a module (NULL = main image);
    /// false when there is no such table.
    std::function<bool(GuestAddr hmodule, std::vector<u16>& languages)>
        message_table_languages;
    /// MESSAGE_RESOURCE_DATA of the table in exactly that language.
    std::function<bool(GuestAddr hmodule, u16 language, std::vector<u8>& data)>
        message_table_data;
    /// The system message table (en-US): UTF-8 text or nullptr.
    std::function<const char*(u32 id)> system_message;
    /// LocalAlloc(LMEM_FIXED, bytes); 0 on failure.
    std::function<GuestAddr(u64 bytes)> local_alloc;
    u16 thread_ui_language = 0x0409;
    u16 user_default_language = 0x0409;
    u16 system_default_language = 0x0409;
};

/// LANGID of the system message table.
inline constexpr u16 kSystemTableLanguage = 0x0409;

/// Runs FormatMessageW (@p wide) or FormatMessageA on the call's seven
/// arguments: sets the return value and the last error, raises the access
/// violation Windows takes on an unreadable source or insert string.
void format_message(Marshaler& m, const Environment& env, bool wide);

/// FormatMessageW and FormatMessageA for kernel32 (and the KernelBase copy).
DllExports make_format_message_exports(Kernel32ExtManager& resources,
                                       GlobalLocalManager& local_heap,
                                       LocaleStringManager& locale);

} // namespace format_message
} // namespace vitreo::winapi
