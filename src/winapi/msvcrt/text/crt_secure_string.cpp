/// @file crt_secure_string.cpp
/// @brief strcpy_s / strcat_s / strncpy_s / strncat_s and their wide forms.
///
/// Contract: the Windows SDK's UCRT source, ucrt/inc/
/// corecrt_internal_string_templates.h (common_tcscpy_s, common_tcscat_s,
/// common_tcsncpy_s, common_tcsncat_s) and corecrt_internal_securecrt.h for
/// the error macros. The copy runs character by character and the overflow is
/// detected when the buffer runs out, so a too-small destination holds the
/// copied prefix with [0] reset (strcpy_s(buf, 2, "hello") leaves "\0e";
/// measured identically for msvcrt.dll by crt_msvcrt_stdio_probe). Every
/// failure sets errno and runs the invalid-parameter handler, which inside a
/// UCRT export with no handler fast-fails (CrtMemoryManager).

#include "winapi/msvcrt/text/crt_string.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <limits>

namespace vitreo::winapi {

namespace {

constexpr u32 kEINVAL = 22;
constexpr u32 kERANGE = 34;
constexpr u32 kSTRUNCATE = 80;

/// One destination or source string of 1- or 2-byte characters.
struct Chars {
    Marshaler& m;
    GuestAddr base;
    usize width;
    u32 get(usize i) const {
        const GuestAddr a = base + i * width;
        if (width == 1) return m.read_u8(a).value_or(0);
        return m.read_u16(a).value_or(0);
    }
    void set(usize i, u32 c) const {
        const GuestAddr a = base + i * width;
        if (width == 1) (void)m.write_u8(a, static_cast<u8>(c));
        else (void)m.write_u16(a, static_cast<u16>(c));
    }
};

u64 truncate_value(const Marshaler& m) {
    return m.cpu_is_64bit() ? std::numeric_limits<u64>::max()
                            : std::numeric_limits<u32>::max();
}

/// _VALIDATE_RETURN / _RETURN_BUFFER_TOO_SMALL: errno, the handler, the code.
u32 fail(CrtMemoryManager& mm, u32 error) {
    mm.set_errno(error);
    (void)mm.invalid_parameter();
    return error;
}

u32 tcscpy_s(Marshaler& m, CrtMemoryManager& mm, usize width) {
    const Chars dst{m, m.arg_ptr(0), width};
    const usize size = static_cast<usize>(m.arg(1));
    const Chars src{m, m.arg_ptr(2), width};
    if (dst.base == 0 || size == 0) return fail(mm, kEINVAL);
    if (src.base == 0) {
        dst.set(0, 0);
        return fail(mm, kEINVAL);
    }
    usize available = size;
    for (usize i = 0;; ++i) {
        const u32 c = src.get(i);
        dst.set(i, c);
        if (c == 0 || --available == 0) break;
    }
    if (available == 0) {
        dst.set(0, 0);
        return fail(mm, kERANGE);
    }
    return 0;
}

u32 tcscat_s(Marshaler& m, CrtMemoryManager& mm, usize width) {
    const Chars dst{m, m.arg_ptr(0), width};
    const usize size = static_cast<usize>(m.arg(1));
    const Chars src{m, m.arg_ptr(2), width};
    if (dst.base == 0 || size == 0) return fail(mm, kEINVAL);
    if (src.base == 0) {
        dst.set(0, 0);
        return fail(mm, kEINVAL);
    }
    usize d = 0;
    usize available = size;
    while (available > 0 && dst.get(d) != 0) {
        ++d;
        --available;
    }
    if (available == 0) {  // destination not NUL-terminated
        dst.set(0, 0);
        return fail(mm, kEINVAL);
    }
    for (usize s = 0;; ++s) {
        const u32 c = src.get(s);
        dst.set(d++, c);
        if (c == 0 || --available == 0) break;
    }
    if (available == 0) {
        dst.set(0, 0);
        return fail(mm, kERANGE);
    }
    return 0;
}

u32 tcsncpy_s(Marshaler& m, CrtMemoryManager& mm, usize width) {
    const Chars dst{m, m.arg_ptr(0), width};
    const usize size = static_cast<usize>(m.arg(1));
    const Chars src{m, m.arg_ptr(2), width};
    const u64 count = m.arg(3) & (m.cpu_is_64bit() ? ~0ULL : 0xFFFFFFFFULL);
    const bool truncate = count == truncate_value(m);
    if (count == 0 && dst.base == 0 && size == 0) return 0;
    if (dst.base == 0 || size == 0) return fail(mm, kEINVAL);
    if (count == 0) {
        dst.set(0, 0);
        return 0;
    }
    if (src.base == 0) {
        dst.set(0, 0);
        return fail(mm, kEINVAL);
    }
    usize available = size;
    u64 remaining = count;
    usize d = 0;
    if (truncate) {
        for (;;) {
            const u32 c = src.get(d);
            dst.set(d++, c);
            if (c == 0 || --available == 0) break;
        }
    } else {
        for (;;) {
            const u32 c = src.get(d);
            dst.set(d++, c);
            if (c == 0 || --available == 0 || --remaining == 0) break;
        }
        if (remaining == 0) dst.set(d, 0);
    }
    if (available == 0) {
        if (truncate) {
            dst.set(size - 1, 0);
            return kSTRUNCATE;
        }
        dst.set(0, 0);
        return fail(mm, kERANGE);
    }
    return 0;
}

u32 tcsncat_s(Marshaler& m, CrtMemoryManager& mm, usize width) {
    const Chars dst{m, m.arg_ptr(0), width};
    const usize size = static_cast<usize>(m.arg(1));
    const Chars src{m, m.arg_ptr(2), width};
    const u64 count = m.arg(3) & (m.cpu_is_64bit() ? ~0ULL : 0xFFFFFFFFULL);
    const bool truncate = count == truncate_value(m);
    if (count == 0 && dst.base == 0 && size == 0) return 0;
    if (dst.base == 0 || size == 0) return fail(mm, kEINVAL);
    if (count != 0 && src.base == 0) {
        dst.set(0, 0);
        return fail(mm, kEINVAL);
    }
    usize d = 0;
    usize available = size;
    u64 remaining = count;
    while (available > 0 && dst.get(d) != 0) {
        ++d;
        --available;
    }
    if (available == 0) {
        dst.set(0, 0);
        return fail(mm, kEINVAL);
    }
    usize s = 0;
    if (truncate) {
        for (;;) {
            const u32 c = src.get(s++);
            dst.set(d++, c);
            if (c == 0 || --available == 0) break;
        }
    } else {
        while (remaining > 0) {
            const u32 c = src.get(s++);
            dst.set(d++, c);
            if (c == 0 || --available == 0) break;
            --remaining;
        }
        if (remaining == 0) dst.set(d, 0);
    }
    if (available == 0) {
        if (truncate) {
            dst.set(size - 1, 0);
            return kSTRUNCATE;
        }
        dst.set(0, 0);
        return fail(mm, kERANGE);
    }
    return 0;
}

/// common_makepath_s (ucrt/filesystem/makepath.cpp). `bounded` is false for
/// _makepath/_wmakepath, which pass _CRT_UNBOUNDED_BUFFER_SIZE. The drive
/// contributes its first character and ':'; a directory without a trailing
/// '/' or '\\' gets '\\'; a nonempty extension without '.' gets one.
u32 makepath(Marshaler& m, CrtMemoryManager& mm, usize width, bool bounded) {
    const Chars out{m, m.arg_ptr(0), width};
    const usize a = bounded ? 1 : 0;
    const usize count = bounded ? static_cast<usize>(m.arg(1)) : 0;
    if (out.base == 0 || (bounded && count == 0)) return fail(mm, kEINVAL);
    const Chars drive{m, m.arg_ptr(a + 1), width};
    const Chars dir{m, m.arg_ptr(a + 2), width};
    const Chars name{m, m.arg_ptr(a + 3), width};
    const Chars ext{m, m.arg_ptr(a + 4), width};
    usize n = 0;
    bool overflow = false;
    // The UCRT checks each character against the end of the buffer and the
    // terminator last; msvcrt.dll's older _wmakepath_s refuses a character
    // that would leave no room for the terminator, so a failed call leaves
    // one character less behind (measured: "c" "dir" into 5 -> "\0:di").
    const bool msvcrt = CrtDllFlavorScope::current() == CrtDllFlavor::Msvcrt;
    auto room = [&](usize need) {
        if (!bounded) return true;
        return msvcrt ? count - n > need : count - n >= need;
    };
    auto put = [&](u32 c) {
        if (!room(1)) {
            overflow = true;
            return false;
        }
        out.set(n++, c);
        return true;
    };
    auto put_all = [&](const Chars& src) {
        for (usize i = 0;; ++i) {
            const u32 c = src.get(i);
            if (c == 0) return true;
            if (!put(c)) return false;
        }
    };
    if (drive.base != 0 && drive.get(0) != 0) {
        if (!room(2)) overflow = true;
        else {
            out.set(n++, drive.get(0));
            out.set(n++, ':');
        }
    }
    if (!overflow && dir.base != 0 && dir.get(0) != 0) {
        usize len = 0;
        while (dir.get(len) != 0) ++len;
        if (put_all(dir)) {
            const u32 last = dir.get(len - 1);
            if (last != '/' && last != '\\') (void)put('\\');
        }
    }
    if (!overflow && name.base != 0) (void)put_all(name);
    if (!overflow && ext.base != 0) {
        const u32 first = ext.get(0);
        if (first != 0 && first != '.') (void)put('.');
        if (!overflow) (void)put_all(ext);
    }
    if (!overflow) (void)put(0);
    if (overflow) {
        out.set(0, 0);
        return fail(mm, kERANGE);
    }
    return 0;
}

/// ucrt/string/strlwr.cpp (and strupr, wcslwr, wcsupr) in the "C" locale,
/// the only one Vitreo's CRT has: only 'A'-'Z' / 'a'-'z' change.
u32 map_case(u32 c, bool lower) {
    if (lower && c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    if (!lower && c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

/// _strlwr_s / _strupr_s / _wcslwr_s / _wcsupr_s.
u32 case_s(Marshaler& m, CrtMemoryManager& mm, usize width, bool lower) {
    const Chars str{m, m.arg_ptr(0), width};
    const usize size = static_cast<usize>(m.arg(1));
    if (str.base == 0) return fail(mm, kEINVAL);
    usize len = 0;
    while (len < size && str.get(len) != 0) ++len;
    if (len >= size) {  // not NUL-terminated within the buffer
        str.set(0, 0);  // _RESET_STRING writes [0] even when size is 0
        return fail(mm, kEINVAL);
    }
    for (usize i = 0; i < len; ++i) str.set(i, map_case(str.get(i), lower));
    return 0;
}

/// _strlwr / _strupr / _wcslwr / _wcsupr: NULL is EINVAL (+handler) -> NULL.
u64 case_inplace(Marshaler& m, CrtMemoryManager& mm, usize width, bool lower) {
    const Chars str{m, m.arg_ptr(0), width};
    if (str.base == 0) {
        (void)fail(mm, kEINVAL);
        return 0;
    }
    for (usize i = 0;; ++i) {
        const u32 c = str.get(i);
        if (c == 0) break;
        const u32 mapped = map_case(c, lower);
        if (mapped != c) str.set(i, mapped);
    }
    return str.base;
}

} // namespace

void add_crt_secure_string_exports(DllExports& exports, CrtMemoryManager& mm) {
    struct Row {
        const char* name;
        u32 (*fn)(Marshaler&, CrtMemoryManager&, usize);
        usize width;
    };
    static constexpr Row kRows[] = {
        {"strcpy_s", tcscpy_s, 1},   {"wcscpy_s", tcscpy_s, 2},
        {"strcat_s", tcscat_s, 1},   {"wcscat_s", tcscat_s, 2},
        {"strncpy_s", tcsncpy_s, 1}, {"wcsncpy_s", tcsncpy_s, 2},
        {"strncat_s", tcsncat_s, 1}, {"wcsncat_s", tcsncat_s, 2},
    };
    for (const Row& row : kRows) {
        const Row r = row;
        exports.entries.push_back({r.name, 0, [r, &mm](Marshaler& m) {
            m.set_return(r.fn(m, mm, r.width));
        }, 0});
    }
    struct CaseRow {
        const char* inplace;
        const char* secure;
        usize width;
        bool lower;
    };
    static constexpr CaseRow kCase[] = {
        {"_strlwr", "_strlwr_s", 1, true},  {"_strupr", "_strupr_s", 1, false},
        {"_wcslwr", "_wcslwr_s", 2, true},  {"_wcsupr", "_wcsupr_s", 2, false},
    };
    for (const CaseRow& row : kCase) {
        const CaseRow c = row;
        exports.entries.push_back({c.inplace, 0, [c, &mm](Marshaler& m) {
            m.set_return(case_inplace(m, mm, c.width, c.lower));
        }, 0});
        exports.entries.push_back({c.secure, 0, [c, &mm](Marshaler& m) {
            m.set_return(case_s(m, mm, c.width, c.lower));
        }, 0});
    }
    // errno_t _makepath_s(char* path, size_t count, drive, dir, fname, ext)
    // void _makepath(char* path, drive, dir, fname, ext)  (+ wide forms)
    for (const usize width : {usize{1}, usize{2}}) {
        exports.entries.push_back({width == 1 ? "_makepath_s" : "_wmakepath_s", 0,
            [width, &mm](Marshaler& m) { m.set_return(makepath(m, mm, width, true)); }, 0});
        exports.entries.push_back({width == 1 ? "_makepath" : "_wmakepath", 0,
            [width, &mm](Marshaler& m) { (void)makepath(m, mm, width, false); }, 0});
    }
}

} // namespace vitreo::winapi
