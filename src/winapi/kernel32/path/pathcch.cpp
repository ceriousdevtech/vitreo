/// @file pathcch.cpp
/// @brief Guest marshalling for the `PathCch*` family.
///
/// The path algebra lives in pathcch_core.hpp; this file only moves UTF-16
/// code units and pointers across the guest boundary. Two things matter here:
///
///  * These are UNICODE-only APIs, so the strings are read as raw UTF-16 code
///    units rather than through the Marshaler's UTF-8 conversion helpers. A
///    round trip through UTF-8 would rewrite lone surrogates, and these
///    functions must return the caller's own characters untouched.
///
///  * The `PCWSTR*` out-parameters (PathCchSkipRoot, PathCchFindExtension,
///    PathIsUNCEx, the `*Ex` end pointers) receive a pointer *into the guest's
///    own buffer*, so they are computed as `guest_base + index * 2` and stored
///    at the guest's pointer width — never as a host pointer.

#include "pathcch.hpp"

#include "pathcch_core.hpp"
#include "winapi/core/marshaler.hpp"

#include <vector>

namespace vitreo::winapi {

namespace {

using pathcch::PathString;

enum class ReadStatus { Ok, Fault, Unterminated };

/// Read a NUL-terminated UTF-16 string from the guest, reading at most
/// `max_cch` code units (the terminator included).
[[nodiscard]] ReadStatus read_path(const Marshaler& m, GuestAddr addr,
                                   usize max_cch, PathString& out) {
    out.clear();
    for (usize i = 0; i < max_cch; ++i) {
        auto unit = m.read_u16(addr + static_cast<GuestAddr>(i * 2));
        if (unit.is_err()) return ReadStatus::Fault;
        if (unit.value() == 0) return ReadStatus::Ok;
        out.push_back(static_cast<char16_t>(unit.value()));
    }
    return ReadStatus::Unterminated;
}

/// Write a NUL-terminated UTF-16 string back to the guest.
[[nodiscard]] bool write_path(Marshaler& m, GuestAddr addr,
                              const PathString& value) {
    std::vector<Byte> bytes((value.size() + 1) * 2, Byte{0});
    for (usize i = 0; i < value.size(); ++i) {
        bytes[i * 2] = static_cast<Byte>(value[i] & 0xFF);
        bytes[i * 2 + 1] = static_cast<Byte>((value[i] >> 8) & 0xFF);
    }
    return m.memory().write_bytes(addr, bytes.data(), bytes.size()).is_ok();
}

/// Store a pointer that addresses character `index` of the guest string at
/// `base`. The width follows the guest's, so an x64 caller does not get a
/// truncated pointer with a stale high dword.
[[nodiscard]] bool write_char_pointer(Marshaler& m, GuestAddr out_addr,
                                      GuestAddr base, usize index) {
    if (out_addr == 0) return true; // documented as optional
    return m.write_ptr(out_addr,
                       static_cast<u64>(base) + static_cast<u64>(index) * 2)
        .is_ok();
}

[[nodiscard]] bool write_count(Marshaler& m, GuestAddr out_addr, usize value) {
    if (out_addr == 0) return true; // documented as optional
    return m.write_ptr(out_addr, static_cast<u64>(value)).is_ok();
}

/// Map a failed guest read onto the HRESULT the caller should see. A fault is
/// reported as E_INVALIDARG rather than silently succeeding: a bad buffer is a
/// bad argument, and an over-long unterminated string is exactly what
/// PATHCCH_MAX_CCH exists to reject.
[[nodiscard]] u32 read_failure_hr(ReadStatus status) {
    return status == ReadStatus::Unterminated ? pathcch::kFilenameTooLong
                                              : pathcch::kEInvalidArg;
}

// -----------------------------------------------------------------------------
// Shapes shared by several exports
// -----------------------------------------------------------------------------

/// PathCchCombineEx / PathCchCombine.
void impl_combine(Marshaler& m, bool has_flags) {
    const GuestAddr out_addr = m.arg_ptr(0);
    const usize cch_out = static_cast<usize>(m.arg(1));
    const GuestAddr in_addr = m.arg_ptr(2);
    const GuestAddr more_addr = m.arg_ptr(3);
    const u32 flags = has_flags ? m.arg32(4) : pathcch::kNone;

    if (out_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch_out != 0 && cch_out <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }

    PathString first;
    PathString second;
    if (in_addr != 0) {
        const auto status = read_path(m, in_addr, pathcch::kPathCchMaxCch, first);
        if (status != ReadStatus::Ok) {
            m.set_return(read_failure_hr(status));
            return;
        }
    }
    if (more_addr != 0) {
        const auto status =
            read_path(m, more_addr, pathcch::kPathCchMaxCch, second);
        if (status != ReadStatus::Ok) {
            m.set_return(read_failure_hr(status));
            return;
        }
    }

    PathString result;
    const u32 hr = pathcch::combine(result, cch_out,
                                    in_addr != 0 ? &first : nullptr,
                                    more_addr != 0 ? &second : nullptr, flags);
    // On failure the output buffer is emptied rather than left holding the
    // caller's previous contents, which would read as a valid combined path.
    if (!write_path(m, out_addr, hr == pathcch::kSOk ? result : PathString())) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(hr);
}

/// PathCchCanonicalizeEx / PathCchCanonicalize.
void impl_canonicalize(Marshaler& m, bool has_flags) {
    const GuestAddr out_addr = m.arg_ptr(0);
    const usize cch_out = static_cast<usize>(m.arg(1));
    const GuestAddr in_addr = m.arg_ptr(2);
    const u32 flags = has_flags ? m.arg32(3) : pathcch::kNone;

    if (out_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch_out != 0 && cch_out <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }

    PathString src;
    if (in_addr != 0) {
        const auto status = read_path(m, in_addr, pathcch::kPathCchMaxCch, src);
        if (status != ReadStatus::Ok) {
            m.set_return(read_failure_hr(status));
            return;
        }
    }

    PathString result;
    const u32 hr = pathcch::canonicalize(result, cch_out,
                                         in_addr != 0 ? &src : nullptr, flags);
    if (!write_path(m, out_addr, hr == pathcch::kSOk ? result : PathString())) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(hr);
}

/// PathCchAppendEx / PathCchAppend.
void impl_append(Marshaler& m, bool has_flags) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const usize cch = static_cast<usize>(m.arg(1));
    const GuestAddr more_addr = m.arg_ptr(2);
    const u32 flags = has_flags ? m.arg32(3) : pathcch::kNone;

    if (path_addr == 0 || more_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch != 0 && cch <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }

    PathString path;
    // A pszPath with no terminator inside cchPath is a bad argument, not a
    // too-long path: the caller's own size says the buffer ends there.
    if (read_path(m, path_addr, cch, path) != ReadStatus::Ok) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString more;
    if (const auto status = read_path(m, more_addr, pathcch::kPathCchMaxCch, more);
        status != ReadStatus::Ok) {
        m.set_return(read_failure_hr(status));
        return;
    }

    const u32 hr = pathcch::append(path, cch, &more, flags);
    if (hr == pathcch::kSOk && !write_path(m, path_addr, path)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(hr);
}

/// Shared body for the in-place functions that take only (pszPath, cchPath).
template <typename Fn>
void impl_in_place(Marshaler& m, Fn&& op) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const usize cch = static_cast<usize>(m.arg(1));
    if (path_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch != 0 && cch <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString path;
    if (read_path(m, path_addr, cch, path) != ReadStatus::Ok) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    const u32 hr = op(path, cch);
    if (hr == pathcch::kSOk && !write_path(m, path_addr, path)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(hr);
}

/// PathCchAddBackslashEx / PathCchRemoveBackslashEx (and their plain forms,
/// which pass 0 for both out-parameters).
void impl_backslash(Marshaler& m, bool add, bool has_out_params) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const usize cch = static_cast<usize>(m.arg(1));
    const GuestAddr end_addr = has_out_params ? m.arg_ptr(2) : 0;
    const GuestAddr remaining_addr = has_out_params ? m.arg_ptr(3) : 0;

    if (path_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch != 0 && cch <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString path;
    if (read_path(m, path_addr, cch, path) != ReadStatus::Ok) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }

    usize end_index = 0;
    usize remaining = 0;
    const u32 hr = add ? pathcch::add_backslash(path, cch, &end_index, &remaining)
                       : pathcch::remove_backslash(path, cch, &end_index,
                                                   &remaining);
    if (hr == pathcch::kSOk && !write_path(m, path_addr, path)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    // Learn describes both out-parameters as valid "when this function returns
    // successfully"; S_FALSE is a success, an error code is not.
    if (hr == pathcch::kSOk || hr == pathcch::kSFalse) {
        if (!write_char_pointer(m, end_addr, path_addr, end_index) ||
            !write_count(m, remaining_addr, remaining)) {
            m.set_return(pathcch::kEInvalidArg);
            return;
        }
    }
    m.set_return(hr);
}

/// PathCchAddExtension / PathCchRenameExtension.
void impl_extension(Marshaler& m, bool add) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const usize cch = static_cast<usize>(m.arg(1));
    const GuestAddr ext_addr = m.arg_ptr(2);

    if (path_addr == 0 || ext_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch != 0 && cch <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString path;
    if (read_path(m, path_addr, cch, path) != ReadStatus::Ok) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString ext;
    if (const auto status = read_path(m, ext_addr, pathcch::kPathCchMaxCch, ext);
        status != ReadStatus::Ok) {
        m.set_return(read_failure_hr(status));
        return;
    }

    const u32 hr = add ? pathcch::add_extension(path, cch, &ext)
                       : pathcch::rename_extension(path, cch, &ext);
    if (hr == pathcch::kSOk && !write_path(m, path_addr, path)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(hr);
}

void impl_skip_root(Marshaler& m) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const GuestAddr out_addr = m.arg_ptr(1);
    if (path_addr == 0 || out_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString path;
    if (const auto status = read_path(m, path_addr, pathcch::kPathCchMaxCch, path);
        status != ReadStatus::Ok) {
        m.set_return(read_failure_hr(status));
        return;
    }
    usize root_len = 0;
    const u32 hr = pathcch::skip_root(path, root_len);
    if (hr != pathcch::kSOk) {
        m.set_return(hr);
        return;
    }
    if (!write_char_pointer(m, out_addr, path_addr, root_len)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(pathcch::kSOk);
}

void impl_find_extension(Marshaler& m) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const usize cch = static_cast<usize>(m.arg(1));
    const GuestAddr out_addr = m.arg_ptr(2);
    if (path_addr == 0 || out_addr == 0) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    if (!(cch != 0 && cch <= pathcch::kPathCchMaxCch)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    PathString path;
    if (read_path(m, path_addr, cch, path) != ReadStatus::Ok) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    usize ext_index = 0;
    const u32 hr = pathcch::find_extension(path, cch, ext_index);
    if (hr != pathcch::kSOk) {
        m.set_return(hr);
        return;
    }
    if (!write_char_pointer(m, out_addr, path_addr, ext_index)) {
        m.set_return(pathcch::kEInvalidArg);
        return;
    }
    m.set_return(pathcch::kSOk);
}

void impl_is_root(Marshaler& m) {
    const GuestAddr path_addr = m.arg_ptr(0);
    if (path_addr == 0) {
        m.set_return(0); // documented: NULL is not a root
        return;
    }
    PathString path;
    if (read_path(m, path_addr, pathcch::kPathCchMaxCch, path) != ReadStatus::Ok) {
        m.set_return(0);
        return;
    }
    m.set_return(pathcch::is_root(path) ? 1u : 0u);
}

void impl_is_unc_ex(Marshaler& m) {
    const GuestAddr path_addr = m.arg_ptr(0);
    const GuestAddr out_addr = m.arg_ptr(1);
    if (path_addr == 0) {
        m.set_return(0);
        return;
    }
    PathString path;
    if (read_path(m, path_addr, pathcch::kPathCchMaxCch, path) != ReadStatus::Ok) {
        m.set_return(0);
        return;
    }
    usize server_index = 0;
    const bool unc = pathcch::is_unc(path, &server_index);
    if (unc && !write_char_pointer(m, out_addr, path_addr, server_index)) {
        m.set_return(0);
        return;
    }
    m.set_return(unc ? 1u : 0u);
}

} // namespace

DllExports make_pathcch_exports() {
    DllExports exports;
    exports.entries.reserve(20);

    // stdcall_bytes are the x86 figures: every parameter here is 4 bytes wide
    // in a 32-bit guest (size_t and every pointer included).
    exports.entries.push_back(ExportEntry{
        "PathCchCombineEx", 0,
        [](Marshaler& m) { impl_combine(m, /*has_flags=*/true); }, 20});
    exports.entries.push_back(ExportEntry{
        "PathCchCombine", 0,
        [](Marshaler& m) { impl_combine(m, /*has_flags=*/false); }, 16});
    exports.entries.push_back(ExportEntry{
        "PathCchCanonicalizeEx", 0,
        [](Marshaler& m) { impl_canonicalize(m, /*has_flags=*/true); }, 16});
    exports.entries.push_back(ExportEntry{
        "PathCchCanonicalize", 0,
        [](Marshaler& m) { impl_canonicalize(m, /*has_flags=*/false); }, 12});
    exports.entries.push_back(ExportEntry{
        "PathCchAppendEx", 0,
        [](Marshaler& m) { impl_append(m, /*has_flags=*/true); }, 16});
    exports.entries.push_back(ExportEntry{
        "PathCchAppend", 0,
        [](Marshaler& m) { impl_append(m, /*has_flags=*/false); }, 12});
    exports.entries.push_back(ExportEntry{
        "PathCchAddBackslashEx", 0,
        [](Marshaler& m) { impl_backslash(m, /*add=*/true, /*out=*/true); }, 16});
    exports.entries.push_back(ExportEntry{
        "PathCchAddBackslash", 0,
        [](Marshaler& m) { impl_backslash(m, /*add=*/true, /*out=*/false); }, 8});
    exports.entries.push_back(ExportEntry{
        "PathCchRemoveBackslashEx", 0,
        [](Marshaler& m) { impl_backslash(m, /*add=*/false, /*out=*/true); }, 16});
    exports.entries.push_back(ExportEntry{
        "PathCchRemoveBackslash", 0,
        [](Marshaler& m) { impl_backslash(m, /*add=*/false, /*out=*/false); }, 8});
    exports.entries.push_back(ExportEntry{
        "PathCchSkipRoot", 0, [](Marshaler& m) { impl_skip_root(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "PathCchFindExtension", 0,
        [](Marshaler& m) { impl_find_extension(m); }, 12});
    exports.entries.push_back(ExportEntry{
        "PathCchAddExtension", 0,
        [](Marshaler& m) { impl_extension(m, /*add=*/true); }, 12});
    exports.entries.push_back(ExportEntry{
        "PathCchRenameExtension", 0,
        [](Marshaler& m) { impl_extension(m, /*add=*/false); }, 12});
    exports.entries.push_back(ExportEntry{
        "PathCchRemoveExtension", 0,
        [](Marshaler& m) {
            impl_in_place(m, [](PathString& p, usize cch) {
                return pathcch::remove_extension(p, cch);
            });
        },
        8});
    exports.entries.push_back(ExportEntry{
        "PathCchRemoveFileSpec", 0,
        [](Marshaler& m) {
            impl_in_place(m, [](PathString& p, usize cch) {
                return pathcch::remove_file_spec(p, cch);
            });
        },
        8});
    exports.entries.push_back(ExportEntry{
        "PathCchStripPrefix", 0,
        [](Marshaler& m) {
            impl_in_place(m, [](PathString& p, usize cch) {
                return pathcch::strip_prefix(p, cch);
            });
        },
        8});
    exports.entries.push_back(ExportEntry{
        "PathCchStripToRoot", 0,
        [](Marshaler& m) {
            impl_in_place(m, [](PathString& p, usize cch) {
                return pathcch::strip_to_root(p, cch);
            });
        },
        8});
    exports.entries.push_back(ExportEntry{
        "PathCchIsRoot", 0, [](Marshaler& m) { impl_is_root(m); }, 4});
    exports.entries.push_back(ExportEntry{
        "PathIsUNCEx", 0, [](Marshaler& m) { impl_is_unc_ex(m); }, 8});

    return exports;
}

} // namespace vitreo::winapi
