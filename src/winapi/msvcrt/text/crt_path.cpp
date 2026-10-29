// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_path.cpp
/// @brief _splitpath / _wsplitpath / _splitpath_s / _wsplitpath_s and _fullpath.
///
/// Split out of crt_string_registration.cpp. The path decomposition the four
/// split exports share lives in this file's anonymous namespace.

#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"
#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

// ---------------------------------------------------------------------------
// _splitpath family (Learn _splitpath/_wsplitpath, _splitpath_s/_wsplitpath_s)
//
// One decomposition shared by the four exports, on raw code units: bytes for
// the narrow forms, UTF-16 units for the wide forms. Every delimiter it looks
// for ('\\', '/', ':', '.') is ASCII; converting through UTF-8 would change the
// unit counts the caller's buffers are sized in. Learn says the narrow forms
// recognise multibyte sequences of the current multibyte code page; Vitreo's
// CRT multibyte state has no lead bytes (_ismbblead is 0 for every byte), so
// no byte is a lead byte here either -- the same answer the rest of the CRT
// gives.
//
//   drive = the first two units when the second is ':'  ("C:")
//   dir   = everything after the drive up to and INCLUDING the last '\' or '/'
//   ext   = from the LAST '.' in the remaining leaf to the end (keeps the dot)
//   fname = the leaf before that dot (the whole leaf when there is no dot)
// A component the path does not contain is the empty string.
// ---------------------------------------------------------------------------

/// Component boundaries: drive = [0, drive_end), dir = [drive_end, dir_end),
/// fname = [dir_end, fname_end), ext = [fname_end, size).
struct SplitPathBounds {
    usize drive_end = 0;
    usize dir_end = 0;
    usize fname_end = 0;
};

template <typename Unit>
[[nodiscard]] SplitPathBounds split_path_bounds(
    std::basic_string_view<Unit> path) {
    SplitPathBounds b;
    if (path.size() >= 2 && path[1] == static_cast<Unit>(':')) b.drive_end = 2;
    b.dir_end = b.drive_end;
    for (usize k = path.size(); k > b.drive_end; --k) {
        const Unit c = path[k - 1];
        if (c == static_cast<Unit>('\\') || c == static_cast<Unit>('/')) {
            b.dir_end = k;
            break;
        }
    }
    b.fname_end = path.size();
    for (usize k = path.size(); k > b.dir_end; --k) {
        if (path[k - 1] == static_cast<Unit>('.')) {
            b.fname_end = k - 1;
            break;
        }
    }
    return b;
}

/// Read a NUL-terminated guest string as raw code units (u8 or u16). A read
/// fault ends the string, as the other CRT string readers here do.
template <typename Unit>
[[nodiscard]] std::vector<Unit> read_guest_units(Marshaler& m, GuestAddr p) {
    std::vector<Unit> units;
    for (usize i = 0;; ++i) {
        const GuestAddr a = p + static_cast<GuestAddr>(i * sizeof(Unit));
        Unit unit = 0;
        if constexpr (sizeof(Unit) == 1) {
            auto r = m.read_u8(a);
            if (r.is_err()) break;
            unit = r.value();
        } else {
            auto r = m.read_u16(a);
            if (r.is_err()) break;
            unit = r.value();
        }
        if (unit == 0) break;
        units.push_back(unit);
    }
    return units;
}

/// Write @p v plus a terminator at @p dst (code units).
template <typename Unit>
void write_guest_units(Marshaler& m, GuestAddr dst,
                       std::basic_string_view<Unit> v) {
    if (!v.empty()) {
        (void)m.memory().write_bytes(
            dst, reinterpret_cast<const Byte*>(v.data()), v.size() * sizeof(Unit));
    }
    const GuestAddr end = dst + static_cast<GuestAddr>(v.size() * sizeof(Unit));
    if constexpr (sizeof(Unit) == 1) (void)m.write_u8(end, 0);
    else (void)m.write_u16(end, 0);
}

/// The four components of @p path, in drive/dir/fname/ext order.
template <typename Unit>
[[nodiscard]] std::array<std::basic_string_view<Unit>, 4> split_path_parts(
    std::basic_string_view<Unit> path) {
    const SplitPathBounds b = split_path_bounds(path);
    return {path.substr(0, b.drive_end),
            path.substr(b.drive_end, b.dir_end - b.drive_end),
            path.substr(b.dir_end, b.fname_end - b.dir_end),
            path.substr(b.fname_end)};
}

/// void _splitpath / _wsplitpath (path, drive, dir, fname, ext).
/// No size arguments: each non-NULL output receives its component.
template <typename Unit>
void crt_splitpath(Marshaler& m) {
    const GuestAddr path_p = m.arg_ptr(0);
    const std::vector<Unit> path =
        path_p != 0 ? read_guest_units<Unit>(m, path_p) : std::vector<Unit>{};
    const auto parts = split_path_parts(
        std::basic_string_view<Unit>(path.data(), path.size()));
    for (usize k = 0; k < 4; ++k) {
        const GuestAddr dst = m.arg_ptr(1 + k);
        if (dst != 0) write_guest_units(m, dst, parts[k]);
    }
}

/// errno_t _splitpath_s / _wsplitpath_s (path, drive, driveNumberOfElements,
/// dir, dirNumberOfElements, fname, nameNumberOfElements, ext,
/// extNumberOfElements). Sizes are size_t element counts (4 bytes on x86,
/// 8 on x64 -- arg() reads the native slot width).
///
/// Learn error table: path NULL, or any (buffer NULL, size != 0) or
/// (buffer non-NULL, size == 0) pair -> invalid parameter handler, then
/// errno = EINVAL and return EINVAL. Any buffer too short for its component
/// plus terminator -> every buffer cleared to "", errno = ERANGE, return
/// ERANGE (no handler). On EINVAL every usable buffer (non-NULL, size > 0) is
/// cleared too, so no failure leaves a partial result behind.
template <typename Unit>
void crt_splitpath_s(Marshaler& m, CrtMemoryManager& mem_mgr) {
    constexpr u32 EINVAL_VALUE = 22;
    constexpr u32 ERANGE_VALUE = 34;
    struct Out {
        GuestAddr buffer;
        usize count;
    };
    const GuestAddr path_p = m.arg_ptr(0);
    std::array<Out, 4> outs{};
    for (usize k = 0; k < 4; ++k) {
        outs[k] = {m.arg_ptr(1 + 2 * k), static_cast<usize>(m.arg(2 + 2 * k))};
    }
    const auto clear_all = [&m, &outs] {
        for (const Out& o : outs) {
            if (o.buffer != 0 && o.count > 0) {
                write_guest_units(m, o.buffer, std::basic_string_view<Unit>{});
            }
        }
    };

    bool invalid = path_p == 0;
    for (const Out& o : outs) {
        if ((o.buffer == 0) != (o.count == 0)) invalid = true;
    }
    if (invalid) {
        clear_all();
        mem_mgr.set_errno(EINVAL_VALUE);
        if (!mem_mgr.invalid_parameter()) return;  // fast-failed: no handler
        m.set_return(EINVAL_VALUE);
        return;
    }

    const std::vector<Unit> path = read_guest_units<Unit>(m, path_p);
    const auto parts = split_path_parts(
        std::basic_string_view<Unit>(path.data(), path.size()));
    for (usize k = 0; k < 4; ++k) {
        if (outs[k].buffer != 0 && parts[k].size() + 1 > outs[k].count) {
            clear_all();
            mem_mgr.set_errno(ERANGE_VALUE);
            m.set_return(ERANGE_VALUE);
            return;
        }
    }
    for (usize k = 0; k < 4; ++k) {
        if (outs[k].buffer != 0) write_guest_units(m, outs[k].buffer, parts[k]);
    }
    m.set_return(0);
}

} // namespace

void add_crt_path_exports(DllExports& exports, CrtMemoryManager& mem_mgr,
                          capsule::Vfs& vfs) {
    // void _splitpath(const char* path, char* drive, char* dir,
    //                 char* fname, char* ext)
    //
    // Neither _splitpath nor _makepath was registered, so both reached the
    // missing-export stub: the caller's buffers came back untouched (empty
    // strings) while the call reported success. Semantics verified against
    // Windows:
    //   "C:\Program Files\Game\bin\app.exe" -> "C:" "\Program Files\Game\bin\" "app" ".exe"
    //   "relative\x.tar.gz"                  -> ""   "relative\"                 "x.tar" ".gz"
    //   "noext"                              -> ""   ""                          "noext" ""
    // Note the extension is the LAST dot, the directory keeps its trailing
    // separator, and the drive keeps its colon.
    // The decomposition lives in split_path_bounds (shared with the _s forms).
    exports.entries.push_back(
        {"_splitpath", 0, [](Marshaler& m) { crt_splitpath<u8>(m); }});

    // void _wsplitpath(const wchar_t* path, wchar_t* drive, wchar_t* dir,
    //                  wchar_t* fname, wchar_t* ext)
    //
    // Same decomposition as _splitpath, done on UTF-16 code units rather than
    // on a UTF-8 conversion: every separator it looks for ('\\', '/', ':',
    // '.') is ASCII, but the surrounding text need not be, and a round trip
    // through UTF-8 would change the unit counts the caller's buffers are
    // sized in.
    exports.entries.push_back(
        {"_wsplitpath", 0, [](Marshaler& m) { crt_splitpath<u16>(m); }});

    // errno_t _splitpath_s(const char* path, char* drive, size_t driveN,
    //                      char* dir, size_t dirN, char* fname, size_t nameN,
    //                      char* ext, size_t extN)
    // errno_t _wsplitpath_s(<same, wchar_t>)
    // Learn _splitpath_s/_wsplitpath_s; the validation and failure-output
    // rules are documented on crt_splitpath_s.
    exports.entries.push_back(
        {"_splitpath_s", 0, [&mem_mgr](Marshaler& m) {
            crt_splitpath_s<u8>(m, mem_mgr);
        }});
    exports.entries.push_back(
        {"_wsplitpath_s", 0, [&mem_mgr](Marshaler& m) {
            crt_splitpath_s<u16>(m, mem_mgr);
        }});


    // char* _fullpath(char* abs_path, const char* relative_path, size_t max_length)
    exports.entries.push_back(
        {"_fullpath", 0, [&mem_mgr, &vfs](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            constexpr u32 ERANGE_VALUE = 34;

            GuestAddr destination = m.arg_ptr(0);
            GuestAddr source = m.arg_ptr(1);
            usize max_length = static_cast<usize>(m.arg(2));
            if (source == 0 || max_length == 0) {
                (void)m.memory().write_u32(mem_mgr.errno_addr(), EINVAL_VALUE);
                m.set_return(0);
                return;
            }

            auto input = m.read_ansi_string(source, MAX_BULK_SIZE);
            if (input.is_err()) {
                (void)m.memory().write_u32(mem_mgr.errno_addr(), EINVAL_VALUE);
                m.set_return(0);
                return;
            }
            auto full = CrtStringManager::resolve_fullpath(input.value(), vfs.cwd());
            if (!full || full->size() + 1 > max_length) {
                (void)m.memory().write_u32(
                    mem_mgr.errno_addr(), full ? ERANGE_VALUE : EINVAL_VALUE);
                m.set_return(0);
                return;
            }

            if (destination == 0) {
                destination = mem_mgr.crt_malloc(max_length);
                if (destination == 0) {
                    (void)m.memory().write_u32(mem_mgr.errno_addr(), 12);
                    m.set_return(0);
                    return;
                }
            }
            for (usize i = 0; i < full->size(); ++i) {
                (void)m.write_u8(destination + i, static_cast<u8>((*full)[i]));
            }
            (void)m.write_u8(destination + full->size(), 0);
            const std::string cwd_snapshot = vfs.cwd();
            VITREO_TRACE("[msvcrt] _fullpath('%s', cwd='%s') -> '%s'\n",
                         input.value().c_str(), cwd_snapshot.c_str(),
                         full->c_str());
            m.set_return(destination);
        }});
}

} // namespace vitreo::winapi
