// SPDX-License-Identifier: LGPL-2.1-only
/// @file crt_find.cpp
/// @brief _findfirst / _findnext / _findclose and their wide and sized forms.
///
/// Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/findfirst-functions
/// (and findnext-functions, findclose). Struct layouts are io.h's
/// _finddata32_t / 32i64 / 64i32 / 64 (and the wchar_t twins). msvcrt's
/// legacy names pick a layout by bitness, measured on Windows
/// (tools/oracle_probe/crt_legacy_exports_probe{32,64}.expected):
///   x86 _findfirst = 32-bit times + 32-bit size, x64 _findfirst = 64i32;
///   x86 _findfirsti64 = 32i64, x64 _findfirsti64 = 64; _findfirst64 = 64.
/// Measured rules the pages leave out:
///   - attrib is the Win32 attribute word, except FILE_ATTRIBUTE_NORMAL
///     alone, which becomes _A_NORMAL (0);
///   - only the documented fields and the name's terminator are written
///     (the x64 padding after attrib, and the name buffer tail, keep their
///     bytes);
///   - a time that does not fit a 32-bit time_t is stored as -1 and sets
///     errno to EINVAL, but the call still succeeds; a size that does not
///     fit 32 bits is truncated without an error;
///   - errors: no match / missing directory / empty pattern -> ENOENT, the
///     end of a search -> ENOENT, a bad handle or a pattern Windows rejects
///     with ERROR_INVALID_PARAMETER -> EINVAL (crt_findfirst_errors_probe:
///     the Win32 error is mapped, _doserrno is left alone).

#include "winapi/msvcrt/io/crt_find.hpp"

#include "capsule/vfs/path_encoding.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <algorithm>
#include <string>

namespace vitreo::winapi {

namespace {

constexpr u32 ENOENT_VALUE = 2;
constexpr u32 ENOMEM_VALUE = 12;
constexpr u32 EINVAL_VALUE = 22;
constexpr u32 FILE_ATTRIBUTE_NORMAL_VALUE = 0x80;
constexpr u32 NAME_CHARS = 260;  // char/wchar_t name[260]
constexpr u64 FILETIME_UNIX_EPOCH = 116444736000000000ULL;
constexpr u64 FILETIME_TICKS_PER_SECOND = 10000000ULL;

struct Layout {
    bool time64;
    bool size64;
    u32 time_offset[3];  // create, access, write
    u32 size_offset;
    u32 name_offset;
};

// io.h: _finddata32_t, _finddata32i64_t, _finddata64i32_t, __finddata64_t.
constexpr Layout k32_32{false, false, {4, 8, 12}, 16, 20};
constexpr Layout k32_64{false, true, {4, 8, 12}, 16, 24};
constexpr Layout k64_32{true, false, {8, 16, 24}, 32, 36};
constexpr Layout k64_64{true, true, {8, 16, 24}, 32, 40};

enum class Form { Msvcrt, MsvcrtI64, Fixed };

const Layout& layout_for(Form form, const Layout& fixed, bool is_64bit) {
    switch (form) {
    case Form::Msvcrt: return is_64bit ? k64_32 : k32_32;
    case Form::MsvcrtI64: return is_64bit ? k64_64 : k32_64;
    case Form::Fixed: break;
    }
    return fixed;
}

u32 errno_for_win32(u32 error) {
    switch (error) {
    case 2:   // ERROR_FILE_NOT_FOUND
    case 3:   // ERROR_PATH_NOT_FOUND
    case 18:  // ERROR_NO_MORE_FILES
        return ENOENT_VALUE;
    case 8:   // ERROR_NOT_ENOUGH_MEMORY
        return ENOMEM_VALUE;
    default:
        return EINVAL_VALUE;
    }
}

/// FILETIME -> time_t seconds; -1 for "no time" (0) or a time before 1970.
i64 time_from_filetime(u64 filetime) {
    if (filetime == 0 || filetime < FILETIME_UNIX_EPOCH) return -1;
    return static_cast<i64>((filetime - FILETIME_UNIX_EPOCH) / FILETIME_TICKS_PER_SECOND);
}

/// Store one record; false when guest memory rejects the write.
bool write_record(Marshaler& m, CrtMemoryManager& memory_manager, GuestAddr out,
                  const Layout& layout, bool wide, const FindData& data) {
    const u32 attrib =
        data.attributes == FILE_ATTRIBUTE_NORMAL_VALUE ? 0 : data.attributes;
    if (m.write_u32(out, attrib).is_err()) return false;
    const u64 times[3] = {data.creation_time, data.last_access_time,
                          data.last_write_time};
    for (int i = 0; i < 3; ++i) {
        i64 t = time_from_filetime(times[i]);
        const GuestAddr at = out + layout.time_offset[i];
        if (layout.time64) {
            if (m.write_u64(at, static_cast<u64>(t)).is_err()) return false;
        } else {
            if (t > 0x7FFFFFFFLL) {
                t = -1;
                memory_manager.set_errno(EINVAL_VALUE);
            }
            if (m.write_u32(at, static_cast<u32>(t)).is_err()) return false;
        }
    }
    const GuestAddr size_at = out + layout.size_offset;
    if (layout.size64) {
        if (m.write_u64(size_at, data.file_size).is_err()) return false;
    } else if (m.write_u32(size_at, static_cast<u32>(data.file_size)).is_err()) {
        return false;
    }
    const GuestAddr name_at = out + layout.name_offset;
    if (wide) {
        const std::vector<u16> name = utf8_to_utf16(data.file_name);
        const usize n = std::min<usize>(name.size(), NAME_CHARS - 1);
        for (usize i = 0; i < n; ++i) {
            if (m.write_u16(name_at + i * 2, name[i]).is_err()) return false;
        }
        return m.write_u16(name_at + n * 2, 0).is_ok();
    }
    const std::string name = utf8_to_acp(data.file_name);
    const usize n = std::min<usize>(name.size(), NAME_CHARS - 1);
    for (usize i = 0; i < n; ++i) {
        if (m.write_u8(name_at + i, static_cast<u8>(name[i])).is_err()) return false;
    }
    return m.write_u8(name_at + n, 0).is_ok();
}

void set_failure(Marshaler& m, u32 errno_value, CrtMemoryManager& memory_manager) {
    memory_manager.set_errno(errno_value);
    m.set_return(m.cpu_is_64bit() ? ~u64{0} : 0xFFFFFFFFu);
}

ApiHandler find_first(FileSearchManager& search, CrtMemoryManager& memory_manager,
                      Form form, Layout fixed, bool wide) {
    return [&search, &memory_manager, form, fixed, wide](Marshaler& m) {
        const GuestAddr spec = m.arg_ptr(0);
        const GuestAddr out = m.arg_ptr(1);
        if (spec == 0 || out == 0) {
            if (!memory_manager.invalid_parameter()) return;
            set_failure(m, EINVAL_VALUE, memory_manager);
            return;
        }
        std::string pattern;
        if (wide) {
            auto text = m.read_wide_string(spec);
            if (text.is_err()) {
                (void)m.raise_access_violation(false, spec);
                return;
            }
            pattern = std::move(text.value());
        } else {
            auto text = m.read_ansi_string(spec);
            if (text.is_err()) {
                (void)m.raise_access_violation(false, spec);
                return;
            }
            pattern = capsule::ansi_to_utf8(text.value());
        }
        FindData data{};
        const u64 handle = pattern.empty() ? 0 : search.find_first_file(pattern, data);
        if (handle == 0) {
            // An empty pattern names no path (ERROR_PATH_NOT_FOUND on Windows).
            const u32 error = pattern.empty() ? 3u : get_last_error();
            set_failure(m, errno_for_win32(error), memory_manager);
            return;
        }
        const Layout& layout = layout_for(form, fixed, m.cpu_is_64bit());
        if (!write_record(m, memory_manager, out, layout, wide, data)) {
            (void)search.find_close(handle);
            (void)m.raise_access_violation(true, out);
            return;
        }
        m.set_return(handle);
    };
}

ApiHandler find_next(FileSearchManager& search, CrtMemoryManager& memory_manager,
                     Form form, Layout fixed, bool wide) {
    return [&search, &memory_manager, form, fixed, wide](Marshaler& m) {
        const u64 handle = m.cpu_is_64bit() ? m.arg(0) : (m.arg(0) & 0xFFFFFFFFu);
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            if (!memory_manager.invalid_parameter()) return;
            memory_manager.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        FindData data{};
        if (!search.find_next_file(handle, data)) {
            memory_manager.set_errno(errno_for_win32(get_last_error()));
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const Layout& layout = layout_for(form, fixed, m.cpu_is_64bit());
        if (!write_record(m, memory_manager, out, layout, wide, data)) {
            (void)m.raise_access_violation(true, out);
            return;
        }
        m.set_return(0);
    };
}

} // namespace

DllExports create_crt_find_thunks(FileSearchManager& search,
                                  CrtMemoryManager& memory_manager) {
    DllExports exports;
    struct Variant {
        const char* first;
        const char* next;
        Form form;
        Layout layout;
        bool wide;
    };
    static constexpr Variant kVariants[] = {
        // msvcrt.dll
        {"_findfirst", "_findnext", Form::Msvcrt, k32_32, false},
        {"_findfirsti64", "_findnexti64", Form::MsvcrtI64, k32_64, false},
        {"_findfirst64", "_findnext64", Form::Fixed, k64_64, false},
        {"_wfindfirst", "_wfindnext", Form::Msvcrt, k32_32, true},
        {"_wfindfirsti64", "_wfindnexti64", Form::MsvcrtI64, k32_64, true},
        {"_wfindfirst64", "_wfindnext64", Form::Fixed, k64_64, true},
        // ucrtbase.dll
        {"_findfirst32", "_findnext32", Form::Fixed, k32_32, false},
        {"_findfirst32i64", "_findnext32i64", Form::Fixed, k32_64, false},
        {"_findfirst64i32", "_findnext64i32", Form::Fixed, k64_32, false},
        {"_wfindfirst32", "_wfindnext32", Form::Fixed, k32_32, true},
        {"_wfindfirst32i64", "_wfindnext32i64", Form::Fixed, k32_64, true},
        {"_wfindfirst64i32", "_wfindnext64i32", Form::Fixed, k64_32, true},
    };
    for (const Variant& v : kVariants) {
        exports.entries.push_back(
            {v.first, 0, find_first(search, memory_manager, v.form, v.layout, v.wide)});
        exports.entries.push_back(
            {v.next, 0, find_next(search, memory_manager, v.form, v.layout, v.wide)});
    }
    // int _findclose(intptr_t handle): 0, or -1 with EINVAL for a handle
    // FindClose rejects (crt_legacy_exports_probe close_minus1).
    exports.entries.push_back({"_findclose", 0, [&search, &memory_manager](Marshaler& m) {
        const u64 handle = m.cpu_is_64bit() ? m.arg(0) : (m.arg(0) & 0xFFFFFFFFu);
        if (!search.find_close(handle)) {
            memory_manager.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    }});
    return exports;
}

} // namespace vitreo::winapi
