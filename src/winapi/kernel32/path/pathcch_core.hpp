#pragma once

/// @file pathcch_core.hpp
/// @brief Pure UTF-16 path algebra behind the KernelBase `PathCch*` family.
///
/// The exports live in `api-ms-win-core-path-l1-1-0.dll`, which real Windows
/// forwards to KernelBase.dll. Everything here is host-side string algebra on
/// `char16_t` so it can be unit tested without guest memory; `pathcch.cpp`
/// owns the guest-pointer marshalling.
///
/// Contract sources (Microsoft Learn, pathcch.h reference):
///   - nf-pathcch-pathcchcombineex, -pathcchcombine
///   - nf-pathcch-pathcchappendex, -pathcchappend
///   - nf-pathcch-pathcchcanonicalizeex, -pathcchcanonicalize
///   - nf-pathcch-pathcchskiproot, -pathcchisroot, -pathisuncex
///   - nf-pathcch-pathcchaddbackslashex, -pathcchremovebackslashex
///   - nf-pathcch-pathcchfindextension, -pathcchaddextension,
///     -pathcchrenameextension, -pathcchremoveextension
///   - nf-pathcch-pathcchremovefilespec, -pathcchstripprefix,
///     -pathcchstriptoroot
///
/// Two deliberate contract decisions, both recorded because the published
/// documentation is self-contradictory:
///
///  1. Buffer-too-small is reported as STRSAFE_E_INSUFFICIENT_BUFFER for the
///     whole family. The PathCchAddExtension page instead names
///     PATHCCH_E_FILENAME_TOO_LONG for "the buffer is too small"; every other
///     page reserves that code for a path that exceeds a *length limit*
///     (MAX_PATH or PATHCCH_MAX_CCH), which is a different condition. A single
///     consistent split is the only way a caller can tell "grow the buffer"
///     from "this path can never be represented".
///
///  2. `PathCchIsRoot` follows the truth table on its own Learn page verbatim
///     (so "\path1" and "\\server" are roots), even though PathCchStripToRoot's
///     page implies root("\path1") == "\". The IsRoot table is specific,
///     complete and self-consistent; that page's closing prose contradicts its
///     own table (it claims "" and "X:" are roots, the table says otherwise),
///     so the table is the better evidence.
///
/// Separator handling: the PathCch family treats only `\` as a separator.
/// Learn states outright that canonicalization "does not convert forward
/// slashes (/) into back slashes (\)", so `/` is an ordinary name character
/// here — unlike the shlwapi `Path*` functions.

#include <vitreo/common.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::pathcch {

using PathString = std::u16string;
using PathView = std::u16string_view;

// -----------------------------------------------------------------------------
// Contract constants
// -----------------------------------------------------------------------------

/// PATHCCH_MAX_CCH: the largest buffer these APIs will accept, in characters.
constexpr usize kPathCchMaxCch = 0x8000;

/// MAX_PATH: the cap that applies unless PATHCCH_ALLOW_LONG_PATHS is passed.
constexpr usize kMaxPath = 260;

// HRESULTs. Spelled out rather than pulled from a Windows header so the exact
// value a guest sees is visible at the point of decision.
constexpr u32 kSOk = 0x00000000u;                      ///< S_OK
constexpr u32 kSFalse = 0x00000001u;                   ///< S_FALSE
constexpr u32 kEInvalidArg = 0x80070057u;              ///< E_INVALIDARG
constexpr u32 kInsufficientBuffer = 0x8007007Au;       ///< STRSAFE_E_INSUFFICIENT_BUFFER
constexpr u32 kFilenameTooLong = 0x800700CEu;          ///< PATHCCH_E_FILENAME_TOO_LONG

// PATHCCH_OPTIONS
constexpr u32 kNone = 0x00000000u;
constexpr u32 kAllowLongPaths = 0x00000001u;
constexpr u32 kForceEnableLongNameProcess = 0x00000002u;
constexpr u32 kForceDisableLongNameProcess = 0x00000004u;
constexpr u32 kDoNotNormalizeSegments = 0x00000008u;
constexpr u32 kEnsureIsExtendedLengthPath = 0x00000010u;
constexpr u32 kEnsureTrailingSlash = 0x00000020u;

constexpr u32 kAllOptions =
    kAllowLongPaths | kForceEnableLongNameProcess | kForceDisableLongNameProcess |
    kDoNotNormalizeSegments | kEnsureIsExtendedLengthPath | kEnsureTrailingSlash;

constexpr char16_t kSep = u'\\';

// -----------------------------------------------------------------------------
// Small predicates
// -----------------------------------------------------------------------------

[[nodiscard]] inline bool is_sep(char16_t c) noexcept { return c == kSep; }

[[nodiscard]] inline bool is_drive_letter(char16_t c) noexcept {
    return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
}

/// "X:" — a drive specification, with or without a following separator.
[[nodiscard]] inline bool has_drive_spec(PathView p) noexcept {
    return p.size() >= 2 && is_drive_letter(p[0]) && p[1] == u':';
}

/// "\\?\" — the extended-length (DOS device) prefix.
[[nodiscard]] inline bool has_extended_prefix(PathView p) noexcept {
    return p.size() >= 4 && p[0] == kSep && p[1] == kSep && p[2] == u'?' &&
           p[3] == kSep;
}

/// "\\?\UNC\" — the extended-length UNC prefix. "UNC" is matched
/// case-insensitively, as Windows does for the device namespace.
[[nodiscard]] inline bool has_extended_unc_prefix(PathView p) noexcept {
    if (!has_extended_prefix(p) || p.size() < 8) return false;
    const auto up = [](char16_t c) {
        return (c >= u'a' && c <= u'z') ? static_cast<char16_t>(c - u'a' + u'A')
                                        : c;
    };
    return up(p[4]) == u'U' && up(p[5]) == u'N' && up(p[6]) == u'C' &&
           p[7] == kSep;
}

/// "\\" with something other than the device namespace behind it.
[[nodiscard]] inline bool has_plain_unc_prefix(PathView p) noexcept {
    if (p.size() < 2 || p[0] != kSep || p[1] != kSep) return false;
    // "\\?\..." is the device namespace and "\\.\..." is a device path;
    // neither is a UNC server/share.
    return !(p.size() >= 3 && (p[2] == u'?' || p[2] == u'.'));
}

// -----------------------------------------------------------------------------
// Roots
// -----------------------------------------------------------------------------

namespace detail {

/// End of a UNC root measured from the first character of the server name,
/// *including* the separator that follows the share (so it addresses the first
/// character of the remainder — what PathCchSkipRoot reports).
[[nodiscard]] inline usize unc_root_end(PathView path, usize start) noexcept {
    const usize server_sep = path.find(kSep, start);
    if (server_sep == PathView::npos) return path.size();
    const usize share_sep = path.find(kSep, server_sep + 1);
    if (share_sep == PathView::npos) return path.size();
    return share_sep + 1;
}

/// End of a UNC root *excluding* the separator that follows the share — the
/// form PathCchStripToRoot leaves behind ("\\server\share", no trailing "\").
[[nodiscard]] inline usize unc_root_trim(PathView path, usize start) noexcept {
    const usize server_sep = path.find(kSep, start);
    if (server_sep == PathView::npos) return path.size();
    const usize share_sep = path.find(kSep, server_sep + 1);
    if (share_sep == PathView::npos) return path.size();
    return share_sep;
}

} // namespace detail

/// PathCchSkipRoot: length in characters of the root prefix, i.e. the index of
/// the first character that follows the drive letter or UNC server/share
/// elements. Returns E_INVALIDARG when the path has no recognisable root.
[[nodiscard]] inline u32 skip_root(PathView path, usize& root_len) noexcept {
    root_len = 0;
    if (path.empty()) return kEInvalidArg;

    if (has_extended_prefix(path)) {
        if (has_extended_unc_prefix(path)) {
            root_len = detail::unc_root_end(path, 8);
            return kSOk;
        }
        if (path.size() >= 6 && is_drive_letter(path[4]) && path[5] == u':') {
            root_len = (path.size() >= 7 && path[6] == kSep) ? 7 : 6;
            return kSOk;
        }
        // Volume GUID or other device name: "\\?\Volume{...}\".
        const usize sep = path.find(kSep, 4);
        root_len = (sep == PathView::npos) ? path.size() : sep + 1;
        return kSOk;
    }
    if (path.size() >= 2 && path[0] == kSep && path[1] == kSep) {
        root_len = detail::unc_root_end(path, 2);
        return kSOk;
    }
    if (path[0] == kSep) {
        root_len = 1;
        return kSOk;
    }
    if (has_drive_spec(path)) {
        // "C:\" is an absolute root; "C:" alone is the drive-relative root.
        root_len = (path.size() >= 3 && path[2] == kSep) ? 3 : 2;
        return kSOk;
    }
    return kEInvalidArg;
}

/// Length of the root in the form PathCchStripToRoot / PathCchRemoveFileSpec
/// leave behind: drive and device roots keep their trailing separator, UNC
/// roots do not. 0 when the path has no root at all.
[[nodiscard]] inline usize canonical_root_len(PathView path) noexcept {
    if (path.empty()) return 0;
    if (has_extended_prefix(path)) {
        if (has_extended_unc_prefix(path)) return detail::unc_root_trim(path, 8);
        const usize sep = path.find(kSep, 4);
        return (sep == PathView::npos) ? path.size() : sep + 1;
    }
    if (path.size() >= 2 && path[0] == kSep && path[1] == kSep) {
        return detail::unc_root_trim(path, 2);
    }
    if (path[0] == kSep) return 1;
    if (has_drive_spec(path)) {
        return (path.size() >= 3 && path[2] == kSep) ? 3 : 2;
    }
    return 0;
}

/// PathCchIsRoot. Implements the truth table published on the Learn page (see
/// the decision note at the top of this file).
[[nodiscard]] inline bool is_root(PathView path, bool is_null = false) noexcept {
    if (is_null || path.empty()) return false;

    // A UNC-style tail is a root when it holds at most a server and a share and
    // carries no trailing separator: "", "srv", "srv\shr" are roots; anything
    // with a trailing separator or a third element is not.
    const auto unc_tail_is_root = [](PathView tail) {
        const usize first = tail.find(kSep);
        if (first == PathView::npos) return true;             // 0 or 1 element
        if (first + 1 == tail.size()) return false;           // trailing "\"
        return tail.find(kSep, first + 1) == PathView::npos;  // exactly 2
    };

    if (has_extended_prefix(path)) {
        if (has_extended_unc_prefix(path)) return unc_tail_is_root(path.substr(8));
        // "\\?\c:\" and "\\?\Volume{guid}\" are roots; the same names without
        // the trailing separator, or with anything after it, are not.
        const usize sep = path.find(kSep, 4);
        return sep != PathView::npos && sep + 1 == path.size() && sep > 4;
    }
    if (path.size() >= 2 && path[0] == kSep && path[1] == kSep) {
        return unc_tail_is_root(path.substr(2));
    }
    if (path[0] == kSep) {
        // "\" and "\path1" are roots; "\path1\" and deeper are not.
        return path.find(kSep, 1) == PathView::npos;
    }
    // "c:\" is a root, "c:" and "c:\path1" are not.
    return has_drive_spec(path) && path.size() == 3 && path[2] == kSep;
}

/// PathIsUNCEx. `server_index` receives the offset of the server name.
[[nodiscard]] inline bool is_unc(PathView path, usize* server_index = nullptr,
                                 bool is_null = false) noexcept {
    if (is_null || path.empty()) return false;
    if (has_extended_unc_prefix(path)) {
        if (server_index) *server_index = 8;
        return true;
    }
    if (has_plain_unc_prefix(path)) {
        if (server_index) *server_index = 2;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Shared validation
// -----------------------------------------------------------------------------

namespace detail {

/// cchPath of 0 or greater than PATHCCH_MAX_CCH is E_INVALIDARG on every
/// function in the family.
[[nodiscard]] inline bool cch_in_range(usize cch) noexcept {
    return cch != 0 && cch <= kPathCchMaxCch;
}

/// Reject flag combinations the documentation forbids outright rather than
/// silently doing something else with them.
[[nodiscard]] inline u32 validate_flags(u32 flags) noexcept {
    if ((flags & ~kAllOptions) != 0) return kEInvalidArg;
    const bool force_enable = (flags & kForceEnableLongNameProcess) != 0;
    const bool force_disable = (flags & kForceDisableLongNameProcess) != 0;
    if (force_enable && force_disable) return kEInvalidArg;
    if ((force_enable || force_disable) && (flags & kAllowLongPaths) == 0) {
        return kEInvalidArg;
    }
    return kSOk;
}

/// Longest result these flags permit, in characters including the terminator.
[[nodiscard]] inline usize effective_max_cch(u32 flags, usize cch_out) noexcept {
    if (flags & kEnsureIsExtendedLengthPath) return kPathCchMaxCch;
    // Learn: "cchPathOut must be greater than MAX_PATH. If it is not, this
    // flag is ignored."
    if ((flags & kAllowLongPaths) && cch_out > kMaxPath) return kPathCchMaxCch;
    return kMaxPath;
}

/// Common tail for a function that produces `result` into a caller buffer of
/// `cch_out` characters under `limit_cch`.
[[nodiscard]] inline u32 check_fit(usize result_len, usize cch_out,
                                   usize limit_cch) noexcept {
    // A path that no buffer of a legal size could hold is reported as
    // too-long; only a merely undersized caller buffer is INSUFFICIENT_BUFFER.
    if (result_len + 1 > limit_cch) return kFilenameTooLong;
    if (result_len + 1 > cch_out) return kInsufficientBuffer;
    return kSOk;
}

} // namespace detail

// -----------------------------------------------------------------------------
// Canonicalization
// -----------------------------------------------------------------------------

/// PathCchCanonicalizeEx. `in == nullptr` models a NULL pszPathIn.
///
/// On failure `out` is cleared: a stale buffer that still holds the caller's
/// input is indistinguishable from success to a caller that forgets to check
/// the HRESULT, which is the exact failure mode this family exists to prevent.
[[nodiscard]] inline u32 canonicalize(PathString& out, usize cch_out,
                                      const PathString* in, u32 flags) {
    // Copied, not referenced: a caller is allowed to pass the same buffer as
    // input and output, exactly as PathCchCombineEx documents.
    const PathString src = in ? *in : PathString();
    out.clear();
    if (const u32 hr = detail::validate_flags(flags); hr != kSOk) return hr;
    if (!detail::cch_in_range(cch_out)) return kEInvalidArg;
    if (src.size() + 1 > kPathCchMaxCch) return kFilenameTooLong;

    usize root_len = 0;
    (void)skip_root(src, root_len); // no root simply means root_len stays 0
    const bool rooted = root_len > 0;

    std::vector<PathView> stack;
    usize i = root_len;
    while (i < src.size()) {
        const usize next = src.find(kSep, i);
        const usize end = (next == PathString::npos) ? src.size() : next;
        const PathView segment = PathView(src).substr(i, end - i);
        if (segment.empty() || segment == u".") {
            // Repeated separators collapse; "." names the current directory.
        } else if (segment == u"..") {
            if (!stack.empty()) {
                stack.pop_back();
            } else if (!rooted) {
                // Below a relative path there is nothing to remove, so the
                // ".." has to survive. Above a root it is simply discarded:
                // Learn says the root segment cannot be removed.
                stack.push_back(segment);
            }
        } else {
            stack.push_back(segment);
        }
        i = (next == PathString::npos) ? src.size() : next + 1;
    }

    PathString result(src.substr(0, root_len));
    for (usize k = 0; k < stack.size(); ++k) {
        // No separator before the first segment: the root already carries one
        // when it has one ("C:\"), and must not gain one when it does not
        // ("C:" is drive-relative, so "C:" + "a" is "C:a", not "C:\a").
        if (k > 0) result.push_back(kSep);
        result.append(stack[k]);
    }
    if (!src.empty() && src.back() == kSep && !result.empty() &&
        result.back() != kSep) {
        result.push_back(kSep);
    }

    // Learn: "All trailing periods are removed from the path, except when
    // preceded by the '*' wild card character. In that case, a single period
    // is retained." Segments made only of periods are navigation elements or
    // degenerate names and are left alone rather than erased.
    const bool normalize_segments =
        (flags & (kDoNotNormalizeSegments | kEnsureIsExtendedLengthPath)) == 0;
    if (normalize_segments && !result.empty() && result.back() == u'.') {
        const usize seg_start = [&] {
            const usize sep = result.rfind(kSep);
            return sep == PathString::npos ? usize{0} : sep + 1;
        }();
        const bool all_periods =
            result.find_first_not_of(u'.', seg_start) == PathString::npos;
        if (!all_periods && seg_start >= root_len) {
            usize end = result.size();
            while (end > seg_start && result[end - 1] == u'.') --end;
            if (end > seg_start && result[end - 1] == u'*') ++end;
            result.resize(end);
        }
    }

    // Learn: an input that empties out yields a single backslash, and a bare
    // root drive gains one.
    if (result.empty()) result.push_back(kSep);
    if (result.size() == 2 && has_drive_spec(result)) result.push_back(kSep);

    if ((flags & kEnsureTrailingSlash) && result.back() != kSep) {
        result.push_back(kSep);
    }
    if (flags & kEnsureIsExtendedLengthPath) {
        if (!has_extended_prefix(result)) {
            if (has_plain_unc_prefix(result)) {
                result = PathString(u"\\\\?\\UNC\\") + result.substr(2);
            } else {
                result = PathString(u"\\\\?\\") + result;
            }
        }
    }

    const usize limit = detail::effective_max_cch(flags, cch_out);
    if (const u32 hr = detail::check_fit(result.size(), cch_out, limit);
        hr != kSOk) {
        return hr;
    }
    out = std::move(result);
    return kSOk;
}

// -----------------------------------------------------------------------------
// Combine / append
// -----------------------------------------------------------------------------

/// PathCchCombineEx. `in`/`more` of nullptr model NULL arguments.
[[nodiscard]] inline u32 combine(PathString& out, usize cch_out,
                                 const PathString* in, const PathString* more,
                                 u32 flags) {
    // Copied for the same aliasing reason as canonicalize().
    const PathString first = in ? *in : PathString();
    const PathString second = more ? *more : PathString();
    out.clear();
    if (const u32 hr = detail::validate_flags(flags); hr != kSOk) return hr;
    if (!detail::cch_in_range(cch_out)) return kEInvalidArg;
    // Learn: "The size of one or both of the original paths exceeded
    // PATHCCH_MAX_CCH."
    if (first.size() + 1 > kPathCchMaxCch || second.size() + 1 > kPathCchMaxCch) {
        return kFilenameTooLong;
    }

    PathString combined;
    usize ignored = 0;
    if (first.empty() && second.empty()) {
        // Learn: both NULL or empty yields a single backslash.
        combined.push_back(kSep);
    } else if (second.empty()) {
        combined = first;
    } else if (second[0] == kSep && !(second.size() >= 2 && second[1] == kSep)) {
        // Learn: "If this path begins with a single backslash, it is combined
        // with only the root of the path pointed to by pszPathIn."
        usize root_len = 0;
        if (skip_root(first, root_len) == kSOk && root_len > 0) {
            PathString root = first.substr(0, root_len);
            while (!root.empty() && root.back() == kSep) root.pop_back();
            combined = root + second;
        } else {
            combined = second;
        }
    } else if (skip_root(second, ignored) == kSOk) {
        // Learn: "If this path is fully qualified, it is copied directly to
        // the output buffer without being combined with the other path."
        combined = second;
    } else if (first.empty()) {
        combined = second;
    } else {
        combined = first;
        if (combined.back() != kSep) combined.push_back(kSep);
        combined += second;
    }

    // Learn: PathCchCombineEx "also canonicalizes any relative path elements".
    return canonicalize(out, cch_out, &combined, flags);
}

/// PathCchAppendEx: PathCchCombineEx with pszPath as both source and
/// destination. `more == nullptr` models a NULL pszMore, which the documented
/// error table calls out as E_INVALIDARG.
[[nodiscard]] inline u32 append(PathString& path, usize cch, const PathString* more,
                                u32 flags) {
    if (const u32 hr = detail::validate_flags(flags); hr != kSOk) return hr;
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (!more) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    PathString result;
    const u32 hr = combine(result, cch, &path, more, flags);
    if (hr != kSOk) return hr;
    path = std::move(result);
    return kSOk;
}

// -----------------------------------------------------------------------------
// Backslash management
// -----------------------------------------------------------------------------

/// PathCchAddBackslashEx. `end_index` receives the offset of the terminating
/// null; `remaining` the unused character count including that null.
[[nodiscard]] inline u32 add_backslash(PathString& path, usize cch,
                                       usize* end_index, usize* remaining) {
    if (end_index) *end_index = 0;
    if (remaining) *remaining = 0;
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    if (!path.empty() && path.back() == kSep) {
        if (end_index) *end_index = path.size();
        if (remaining) *remaining = cch - path.size();
        return kSFalse;
    }
    if (path.size() + 2 > cch) return kInsufficientBuffer;
    path.push_back(kSep);
    if (end_index) *end_index = path.size();
    if (remaining) *remaining = cch - path.size();
    return kSOk;
}

/// PathCchRemoveBackslashEx.
///
/// Learn: on a root path the backslash stays, `ppszEnd` points *at* the
/// backslash and the remaining count includes it.
[[nodiscard]] inline u32 remove_backslash(PathString& path, usize cch,
                                          usize* end_index, usize* remaining) {
    if (end_index) *end_index = 0;
    if (remaining) *remaining = 0;
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    if (path.empty() || path.back() != kSep) {
        if (end_index) *end_index = path.size();
        if (remaining) *remaining = cch - path.size();
        return kSFalse;
    }
    if (is_root(path)) {
        if (end_index) *end_index = path.size() - 1;
        if (remaining) *remaining = cch - (path.size() - 1);
        return kSFalse;
    }
    path.pop_back();
    if (end_index) *end_index = path.size();
    if (remaining) *remaining = cch - path.size();
    return kSOk;
}

// -----------------------------------------------------------------------------
// Extensions
// -----------------------------------------------------------------------------

namespace detail {

/// First character of the last path element (after any separator or drive
/// colon).
[[nodiscard]] inline usize last_element_start(PathView path) noexcept {
    usize start = 0;
    for (usize i = 0; i < path.size(); ++i) {
        if (path[i] == kSep || path[i] == u':') start = i + 1;
    }
    return start;
}

} // namespace detail

/// PathCchFindExtension. `ext_index` receives the offset of the '.' that
/// begins the extension, or the offset of the terminating null when the path
/// has no extension.
[[nodiscard]] inline u32 find_extension(PathView path, usize cch,
                                        usize& ext_index) noexcept {
    ext_index = 0;
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    const usize start = detail::last_element_start(path);
    const usize dot = path.rfind(u'.');
    ext_index = (dot == PathView::npos || dot < start) ? path.size() : dot;
    return kSOk;
}

namespace detail {

/// An extension may not carry path syntax or wildcards; Learn calls that
/// "illegal characters or otherwise not being a valid extension".
[[nodiscard]] inline bool extension_is_legal(PathView ext) noexcept {
    for (const char16_t c : ext) {
        switch (c) {
            case u'\\': case u'/': case u':': case u'*': case u'?':
            case u'"': case u'<': case u'>': case u'|':
                return false;
            default:
                break;
        }
    }
    // A period may only lead; ".a.b" is not one extension.
    return ext.find(u'.', 1) == PathView::npos;
}

/// ".ext" from either "ext" or ".ext"; empty stays empty.
[[nodiscard]] inline PathString normalize_extension(PathView ext) {
    PathString out;
    if (ext.empty()) return out;
    if (ext[0] != u'.') out.push_back(u'.');
    out.append(ext);
    return out;
}

} // namespace detail

/// PathCchAddExtension. `ext == nullptr` models a NULL pszExt.
[[nodiscard]] inline u32 add_extension(PathString& path, usize cch,
                                       const PathString* ext) {
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (!ext) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;
    if (!detail::extension_is_legal(*ext)) return kEInvalidArg;

    usize ext_index = 0;
    if (const u32 hr = find_extension(path, cch, ext_index); hr != kSOk) return hr;
    // Learn: an existing extension is left alone and reported with S_FALSE.
    if (ext_index != path.size()) return kSFalse;

    const PathString suffix = detail::normalize_extension(*ext);
    // Learn: an empty extension (including a bare ".") returns the original
    // string unaltered, with S_OK.
    if (suffix.size() <= 1) return kSOk;
    if (path.size() + suffix.size() + 1 > cch) return kInsufficientBuffer;
    path += suffix;
    return kSOk;
}

/// PathCchRenameExtension. An empty pszExt removes any existing extension.
[[nodiscard]] inline u32 rename_extension(PathString& path, usize cch,
                                          const PathString* ext) {
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (!ext) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;
    if (!detail::extension_is_legal(*ext)) return kEInvalidArg;

    usize ext_index = 0;
    if (const u32 hr = find_extension(path, cch, ext_index); hr != kSOk) return hr;

    const PathString suffix = detail::normalize_extension(*ext);
    if (ext_index + suffix.size() + 1 > cch) return kInsufficientBuffer;
    path.resize(ext_index);
    path += suffix;
    return kSOk;
}

/// PathCchRemoveExtension. S_FALSE when there was no extension to remove.
[[nodiscard]] inline u32 remove_extension(PathString& path, usize cch) {
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    usize ext_index = 0;
    if (const u32 hr = find_extension(path, cch, ext_index); hr != kSOk) return hr;
    if (ext_index == path.size()) return kSFalse;
    path.resize(ext_index);
    return kSOk;
}

// -----------------------------------------------------------------------------
// Trimming
// -----------------------------------------------------------------------------

/// PathCchRemoveFileSpec. S_FALSE when there was nothing to remove.
[[nodiscard]] inline u32 remove_file_spec(PathString& path, usize cch) {
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    const usize root_len = canonical_root_len(path);
    if (path.size() <= root_len) return kSFalse;

    // Learn: "If a path string ends with a trailing backslash, only that
    // backslash is removed."
    if (path.back() == kSep) {
        path.pop_back();
        return kSOk;
    }
    const usize sep = path.rfind(kSep);
    const usize cut =
        (sep == PathString::npos || sep < root_len) ? root_len : sep;
    if (cut == path.size()) return kSFalse;
    path.resize(cut);
    return kSOk;
}

/// PathCchStripToRoot. S_OK if truncated, S_FALSE if already just a root.
[[nodiscard]] inline u32 strip_to_root(PathString& path, usize cch) {
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    const usize root_len = canonical_root_len(path);
    if (root_len == 0) return kEInvalidArg; // no root to strip to
    if (path.size() == root_len) return kSFalse;
    path.resize(root_len);
    return kSOk;
}

/// PathCchStripPrefix. Removes "\\?\" — and rewrites "\\?\UNC\" back to "\\"
/// — only when the remainder is itself a valid Win32 path, so the result is
/// never a path shape the caller cannot use. S_FALSE when nothing was removed.
[[nodiscard]] inline u32 strip_prefix(PathString& path, usize cch) {
    if (!detail::cch_in_range(cch)) return kEInvalidArg;
    if (path.size() + 1 > cch) return kEInvalidArg;

    if (has_extended_unc_prefix(path)) {
        path = PathString(u"\\\\") + path.substr(8);
        return kSOk;
    }
    if (has_extended_prefix(path) && path.size() >= 6 &&
        is_drive_letter(path[4]) && path[5] == u':') {
        path = path.substr(4);
        return kSOk;
    }
    return kSFalse;
}

} // namespace vitreo::winapi::pathcch
