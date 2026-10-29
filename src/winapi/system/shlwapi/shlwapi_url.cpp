/// @file shlwapi_url.cpp
/// @brief Guest entry points of shlwapi's URL family (A and W): argument and
///        buffer rules around the string model in shlwapi_url_core.cpp.
///
/// Buffer rules (Learn, and measured by tools/oracle_probe/shlwapi_url_probe.c):
///  * UrlCanonicalize, UrlEscape, UrlCreateFromPath, PathCreateFromUrl,
///    UrlApplyScheme, UrlGetPart, UrlUnescape: a NULL string, a NULL buffer, a
///    NULL count or a zero count is E_INVALIDARG with the count untouched. A
///    result that fits is written with its terminator and the count becomes
///    its length; otherwise E_POINTER, nothing written, and the count becomes
///    the length plus one.
///  * UrlCombine: no E_INVALIDARG for the buffer -- a zero count or a short
///    buffer is E_POINTER with the needed size, and UrlCombineW with a NULL
///    buffer returns S_OK with the needed size (UrlCombineA: E_POINTER).
///  * The A forms convert their strings with CP_ACP; converting an empty
///    string leaves ERROR_INVALID_PARAMETER as the last error, and
///    UrlCanonicalizeA always leaves it (measured).

#include "winapi/system/shlwapi/shlwapi_url.hpp"

#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/locale_string.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/system/shlwapi/shlwapi_url_core.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace shlwapi_url;

constexpr usize kReadLimit = 1u << 20;
constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorCannotCopy = 266;          // GetAcceptLanguages' short buffer
constexpr u32 kHrCannotCopy = 0x8007010A;      // HRESULT_FROM_WIN32(ERROR_CANNOT_COPY)
constexpr u32 kHrFileNotFound = 0x80070002;    // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
constexpr u64 kHkeyCurrentUser = 0x80000001;
constexpr u64 kHkeyLocalMachine = 0x80000002;
constexpr u32 kRrfRtRegSz = 0x00000002;
constexpr u32 kRegSz = 1;

void log_unimplemented_once(std::atomic<bool>& flag, const char* what) {
    if (!flag.exchange(true)) std::fprintf(stderr, "[vitreo] UNIMPLEMENTED shlwapi!%s\n", what);
}

U16 read_w(Marshaler& m, GuestAddr at) {
    U16 s;
    for (usize i = 0; i < kReadLimit; ++i) {
        const u16 c = m.read_u16(at + i * 2).value_or(0);
        if (c == 0) break;
        s.push_back(static_cast<char16_t>(c));
    }
    return s;
}
std::string read_a(Marshaler& m, GuestAddr at) {
    std::string s;
    for (usize i = 0; i < kReadLimit; ++i) {
        const u8 c = m.read_u8(at + i).value_or(0);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}
/// An ANSI string as code units of the same value (for rules that only look
/// at ASCII structure and report byte offsets).
U16 bytes_as_units(const std::string& s) {
    U16 out;
    for (const unsigned char c : s) out.push_back(c);
    return out;
}

/// The string argument at @p index: W text, or the A bytes converted with
/// CP_ACP (an empty A string leaves ERROR_INVALID_PARAMETER behind).
U16 read_arg(Marshaler& m, GuestAddr at, bool wide) {
    if (wide) return read_w(m, at);
    const std::string a = read_a(m, at);
    if (a.empty()) set_last_error(kErrorInvalidParameter);
    return from_ansi(a);
}

/// Output text in the caller's encoding: UTF-16 units, or CP_ACP bytes (up
/// to an embedded NUL).
struct Encoded {
    std::vector<u16> units;
};
Encoded encode(const U16& text, bool wide) {
    Encoded e;
    if (wide) {
        e.units.assign(text.begin(), text.end());
    } else {
        for (const unsigned char c : to_ansi(text)) e.units.push_back(c);
    }
    return e;
}

bool write_units(Marshaler& m, GuestAddr at, const std::vector<u16>& units, bool wide, bool terminate = true) {
    for (usize i = 0; i < units.size(); ++i) {
        const bool ok = wide ? m.write_u16(at + i * 2, units[i]).is_ok()
                             : m.write_u8(at + i, static_cast<u8>(units[i])).is_ok();
        if (!ok) return false;
    }
    if (!terminate) return true;
    return wide ? m.write_u16(at + units.size() * 2, 0).is_ok() : m.write_u8(at + units.size(), 0).is_ok();
}

/// The common "buffer + count" output. @p r is the computed result.
u32 deliver(Marshaler& m, const Result& r, GuestAddr out, GuestAddr pcch, bool wide) {
    if (r.hr != kSOk && r.hr != kSFalse) return r.hr;
    const Encoded e = encode(r.text, wide);
    const u32 cch = m.read_u32(pcch).value_or(0);
    const u32 len = static_cast<u32>(e.units.size());
    if (cch > len) {
        if (!write_units(m, out, e.units, wide)) return kEPointer;
        (void)m.write_u32(pcch, len);
        return r.hr;
    }
    (void)m.write_u32(pcch, len + 1);
    return kEPointer;
}

bool buffer_args_valid(Marshaler& m, GuestAddr in, GuestAddr out, GuestAddr pcch) {
    if (in == 0 || out == 0 || pcch == 0) return false;
    return m.read_u32(pcch).value_or(0) != 0;
}

// ---------------------------------------------------------------------------

/// HRESULT UrlCanonicalize{A,W}(PCTSTR, PTSTR, DWORD*, DWORD).
void url_canonicalize(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    if (!buffer_args_valid(m, in, out, pcch)) { m.set_return(kEInvalidArg); return; }
    const U16 url = read_arg(m, in, wide);
    Result r = canonicalize(url, flags);
    if (!wide) {
        set_last_error(kErrorInvalidParameter);
        r.hr = ansi_hr(r.hr, r.text, flags);
    }
    m.set_return(deliver(m, r, out, pcch, wide));
}

/// HRESULT UrlEscape{A,W}(PCTSTR, PTSTR, DWORD*, DWORD). UrlEscapeA rejects
/// URL_ESCAPE_AS_UTF8 and URL_ESCAPE_ASCII_URI_COMPONENT with E_NOTIMPL
/// (measured; Learn documents both for UrlEscapeW).
void url_escape(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    if (!buffer_args_valid(m, in, out, pcch)) { m.set_return(kEInvalidArg); return; }
    if (!wide && (flags & (URL_ESCAPE_AS_UTF8 | URL_ESCAPE_ASCII_URI_COMPONENT))) { m.set_return(kENotImpl); return; }
    const U16 url = read_arg(m, in, wide);
    Result r = escape(url, flags);
    if (!wide) r.hr = ansi_hr(r.hr, r.text, flags);
    m.set_return(deliver(m, r, out, pcch, wide));
}

/// HRESULT UrlUnescape{A,W}(PTSTR, PTSTR, DWORD*, DWORD).
void url_unescape(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    const bool in_place = flags & URL_UNESCAPE_INPLACE;
    if (in == 0) { m.set_return(kEInvalidArg); return; }
    if (!in_place && (out == 0 || pcch == 0 || m.read_u32(pcch).value_or(0) == 0)) {
        m.set_return(kEInvalidArg);
        return;
    }
    std::vector<u16> units;
    if (wide) {
        const auto r = unescape(read_w(m, in), flags);
        if (!r) { m.set_return(kEInvalidArg); return; }
        units.assign(r->begin(), r->end());
    } else {
        // UrlUnescapeA: URL_UNESCAPE_AS_UTF8 is E_INVALIDARG except in place,
        // where it is ignored; in place, an escaped NUL is stored (ending the
        // string) and reported as E_INVALIDARG (measured).
        const std::string text = read_a(m, in);
        const auto r = unescape_ansi(text, in_place ? (flags & ~URL_ESCAPE_AS_UTF8) : flags);
        if (!r) { m.set_return(kEInvalidArg); return; }
        for (const unsigned char c : *r) units.push_back(c);
        if (in_place && text.find("%00") != std::string::npos) {
            (void)write_units(m, in, units, false);
            m.set_return(kEInvalidArg);
            return;
        }
    }
    if (in_place) {
        (void)write_units(m, in, units, wide);
        m.set_return(kSOk);
        return;
    }
    const u32 cch = m.read_u32(pcch).value_or(0);
    const u32 len = static_cast<u32>(units.size());
    if (cch > len) {
        (void)write_units(m, out, units, wide);
        (void)m.write_u32(pcch, len);
        m.set_return(kSOk);
        return;
    }
    (void)m.write_u32(pcch, len + 1);
    m.set_return(kEPointer);
}

/// HRESULT UrlCombine{A,W}(PCTSTR base, PCTSTR rel, PTSTR, DWORD*, DWORD).
void url_combine(Marshaler& m, bool wide) {
    const GuestAddr base = m.arg_ptr(0), rel = m.arg_ptr(1), out = m.arg_ptr(2), pcch = m.arg_ptr(3);
    const u32 flags = m.arg32(4);
    if (base == 0 || rel == 0 || pcch == 0) { m.set_return(kEInvalidArg); return; }
    const U16 b = read_arg(m, base, wide);
    const U16 r16 = read_arg(m, rel, wide);
    Result r = combine(b, r16, flags);
    if (!wide) r.hr = ansi_hr(r.hr, r.text, flags);
    if (r.hr != kSOk && r.hr != kSFalse) { m.set_return(r.hr); return; }
    const Encoded e = encode(r.text, wide);
    const u32 cch = m.read_u32(pcch).value_or(0);
    const u32 len = static_cast<u32>(e.units.size());
    if (out == 0) {
        (void)m.write_u32(pcch, len + 1);
        m.set_return(wide ? kSOk : kEPointer);
        return;
    }
    if (cch > len) {
        (void)write_units(m, out, e.units, wide);
        (void)m.write_u32(pcch, len);
        m.set_return(r.hr);
        return;
    }
    (void)m.write_u32(pcch, len + 1);
    m.set_return(kEPointer);
}

/// HRESULT UrlGetPart{A,W}(PCTSTR, PTSTR, DWORD*, DWORD dwPart, DWORD dwFlags).
void url_get_part(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 part = m.arg32(3), flags = m.arg32(4);
    if (!buffer_args_valid(m, in, out, pcch)) { m.set_return(kEInvalidArg); return; }
    // dwPart 0 is refused before the string is read, an unknown part after
    // (measured from the A form's last error).
    if (part == 0) { m.set_return(kEInvalidArg); return; }
    const U16 url = read_arg(m, in, wide);
    Result r = get_part(url, part, flags);
    if (!wide) r.hr = ansi_hr(r.hr, r.text, 0);
    m.set_return(deliver(m, r, out, pcch, wide));
}

/// HRESULT UrlCreateFromPath{A,W}(PCTSTR path, PTSTR url, DWORD*, DWORD).
void url_create_from_path(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    if (!buffer_args_valid(m, in, out, pcch)) { m.set_return(kEInvalidArg); return; }
    m.set_return(deliver(m, create_from_path(read_arg(m, in, wide), flags), out, pcch, wide));
}

/// HRESULT PathCreateFromUrl{A,W}(PCTSTR url, PTSTR path, DWORD*, DWORD).
void path_create_from_url(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    if (!buffer_args_valid(m, in, out, pcch)) { m.set_return(kEInvalidArg); return; }
    m.set_return(deliver(m, path_from_url(read_arg(m, in, wide), flags), out, pcch, wide));
}

/// HRESULT PathCreateFromUrlAlloc(PCWSTR, PWSTR* ppszOut, DWORD): the path in
/// a LocalAlloc block; *ppszOut is NULL on failure.
void path_create_from_url_alloc(Marshaler& m, GlobalLocalManager* local_heap) {
    const GuestAddr in = m.arg_ptr(0), pp = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    if (pp == 0) { m.set_return(kEInvalidArg); return; }
    (void)m.write_ptr(pp, 0);
    if (in == 0) { m.set_return(kEInvalidArg); return; }
    const Result r = path_from_url(read_w(m, in), flags);
    if (r.hr != kSOk && r.hr != kSFalse) { m.set_return(r.hr); return; }
    if (local_heap == nullptr) { m.set_return(0x8007000E); return; }  // E_OUTOFMEMORY
    local_heap->set_guest_64bit(m.cpu_is_64bit());
    const GuestAddr block = static_cast<GuestAddr>(local_heap->global_alloc(0 /*LMEM_FIXED*/, (r.text.size() + 1) * 2, true));
    if (block == 0) { m.set_return(0x8007000E); return; }
    (void)write_units(m, block, std::vector<u16>(r.text.begin(), r.text.end()), true);
    (void)m.write_ptr(pp, block);
    m.set_return(r.hr);
}

/// HRESULT UrlApplyScheme{A,W}(PCTSTR, PTSTR, DWORD*, DWORD). S_FALSE (no
/// scheme applied) leaves the buffer and count alone.
void url_apply_scheme(Marshaler& m, bool wide, RegistryThunks* registry) {
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1), pcch = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    if (!buffer_args_valid(m, in, out, pcch)) { m.set_return(kEInvalidArg); return; }
    const U16 url = read_arg(m, in, wide);
    ApplySchemeRegistry reg;
    if (registry != nullptr) {
        reg.prefix = [registry](const U16& text) -> std::optional<U16> {
            u64 key = 0;
            if (registry->reg_open_key_ex_w(kHkeyLocalMachine,
                                            L"Software\\Microsoft\\Windows\\CurrentVersion\\URL\\Prefixes", 0,
                                            0x20019 /*KEY_READ*/, &key) != 0)
                return std::nullopt;
            std::optional<U16> found;
            for (u32 i = 0; !found; ++i) {
                std::wstring name;
                u32 type = 0;
                std::vector<u8> data(4096);
                u32 size = static_cast<u32>(data.size());
                if (registry->reg_enum_value_w(key, i, name, &type, data.data(), &size) != 0) break;
                if (type != kRegSz || name.empty() || name.size() >= text.size()) continue;
                bool match = true;
                for (usize k = 0; k < name.size() && match; ++k) {
                    auto low = [](u32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; };
                    match = low(static_cast<u32>(name[k])) == low(text[k]);
                }
                if (!match) continue;
                U16 value;
                for (u32 k = 0; k + 1 < size; k += 2) {
                    const char16_t c = static_cast<char16_t>(data[k] | (data[k + 1] << 8));
                    if (c == 0) break;
                    value.push_back(c);
                }
                found = value;
            }
            (void)registry->reg_close_key(key);
            return found;
        };
        reg.default_prefix = [registry]() -> std::optional<U16> {
            std::vector<u8> data(4096);
            u32 size = static_cast<u32>(data.size());
            u32 type = 0;
            if (registry->reg_get_value_w(kHkeyLocalMachine,
                                          L"Software\\Microsoft\\Windows\\CurrentVersion\\URL\\DefaultPrefix", L"",
                                          kRrfRtRegSz, &type, data.data(), &size) != 0)
                return std::nullopt;
            U16 value;
            for (u32 k = 0; k + 1 < size; k += 2) {
                const char16_t c = static_cast<char16_t>(data[k] | (data[k + 1] << 8));
                if (c == 0) break;
                value.push_back(c);
            }
            return value;
        };
    }
    const Result r = apply_scheme(url, flags, reg);
    if (r.hr == kSFalse) { m.set_return(kSFalse); return; }
    m.set_return(deliver(m, r, out, pcch, wide));
}

/// HRESULT UrlFixupW(PCWSTR, PWSTR, DWORD cchMax): the corrected URL copied
/// like lstrcpyn (cut to cchMax - 1 units; nothing for cchMax 0); S_FALSE
/// writes nothing.
void url_fixup(Marshaler& m) {
    static std::atomic<bool> logged{false};
    const GuestAddr in = m.arg_ptr(0), out = m.arg_ptr(1);
    const u32 cch = m.arg32(2);
    if (in == 0 || out == 0) { m.set_return(kEInvalidArg); return; }
    const auto r = fixup(read_w(m, in));
    if (!r) {
        log_unimplemented_once(logged, "UrlFixupW: correcting a misspelled scheme");
        m.set_return(kSFalse);
        return;
    }
    if (r->hr != kSOk) { m.set_return(r->hr); return; }
    if (cch > 0) {
        const usize n = std::min<usize>(r->text.size(), cch - 1);
        (void)write_units(m, out, std::vector<u16>(r->text.begin(), r->text.begin() + static_cast<std::ptrdiff_t>(n)), true);
    }
    m.set_return(kSOk);
}

/// BOOL UrlIs{A,W}(PCTSTR, URLIS).
void url_is_thunk(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0);
    if (in == 0) { m.set_return(0); return; }
    const U16 url = wide ? read_w(m, in) : bytes_as_units(read_a(m, in));
    m.set_return(url_is(url, m.arg32(1)));
}

/// HRESULT ParseURL{A,W}(LPCTSTR, PARSEDURL*): cbSize must be the
/// structure's size (24 on x86, 40 on x64), else E_INVALIDARG; an invalid
/// URL is URL_E_INVALID_SYNTAX and leaves the structure alone.
void parse_url_thunk(Marshaler& m, bool wide) {
    const GuestAddr in = m.arg_ptr(0), pu = m.arg_ptr(1);
    const bool x64 = m.cpu_is_64bit();
    const u32 size = x64 ? 40 : 24;
    if (in == 0 || pu == 0 || m.read_u32(pu).value_or(0) != size) { m.set_return(kEInvalidArg); return; }
    const U16 url = wide ? read_w(m, in) : bytes_as_units(read_a(m, in));
    const auto p = parse_url(url);
    if (!p) { m.set_return(kUrlEInvalidSyntax); return; }
    const usize unit = wide ? 2 : 1;
    const u32 o_proto = x64 ? 8 : 4, o_cchp = x64 ? 16 : 8, o_suf = x64 ? 24 : 12, o_cchs = x64 ? 32 : 16,
              o_scheme = x64 ? 36 : 20;
    (void)m.write_ptr(pu + o_proto, in);
    (void)m.write_u32(pu + o_cchp, static_cast<u32>(p->protocol_len));
    (void)m.write_ptr(pu + o_suf, in + p->suffix_start * unit);
    (void)m.write_u32(pu + o_cchs, static_cast<u32>(p->suffix_len));
    (void)m.write_u32(pu + o_scheme, p->scheme);
    m.set_return(kSOk);
}

/// int UrlCompare{A,W}(PCTSTR, PCTSTR, BOOL fIgnoreSlash): both URLs
/// unescaped, one trailing '/' dropped when fIgnoreSlash, then compared like
/// lstrcmp (CompareString, user locale, case-sensitive).
void url_compare(Marshaler& m, bool wide) {
    const GuestAddr a = m.arg_ptr(0), b = m.arg_ptr(1);
    // A NULL operand compares below anything, itself included (measured).
    if (a == 0 || b == 0) { m.set_return(static_cast<u32>(-1)); return; }
    const U16 x = a ? (wide ? read_w(m, a) : from_ansi(read_a(m, a))) : U16();
    const U16 y = b ? (wide ? read_w(m, b) : from_ansi(read_a(m, b))) : U16();
    const auto [p, q] = compare_operands(x, y, m.arg32(2) != 0);
    const std::vector<u16> pu(p.begin(), p.end()), qu(q.begin(), q.end());
    const i32 r = LocaleStringManager::compare_units(pu, qu, 0);
    m.set_return(static_cast<u32>(r - 2));
}

/// HRESULT UrlHash{A,W}(PCTSTR, BYTE*, DWORD) / HashData(BYTE*, DWORD, BYTE*,
/// DWORD). The argument rules are measured (a NULL pointer is E_INVALIDARG; a
/// zero-length hash is S_OK and writes nothing). Windows' hash function
/// itself is not documented and is not modelled: those calls fail with
/// E_NOTIMPL.
void url_hash(Marshaler& m, bool wide, bool hash_data) {
    static std::atomic<bool> logged{false};
    (void)wide;
    const GuestAddr data = m.arg_ptr(0);
    const GuestAddr hash = hash_data ? m.arg_ptr(2) : m.arg_ptr(1);
    const u32 cb = hash_data ? m.arg32(3) : m.arg32(2);
    if (data == 0 || hash == 0) { m.set_return(kEInvalidArg); return; }
    if (cb == 0) { m.set_return(kSOk); return; }
    log_unimplemented_once(logged, hash_data ? "HashData: the hash function" : "UrlHash: the hash function");
    m.set_return(kENotImpl);
}

/// HRESULT GetAcceptLanguages{A,W}(PTSTR, DWORD*): HKCU\\Software\\Microsoft\\
/// Internet Explorer\\International\\AcceptLanguage, else the user default
/// locale name (Vitreo's user locale is en-US, see GetUserDefaultLocaleName).
/// A buffer that is too short gets HRESULT_FROM_WIN32(ERROR_CANNOT_COPY),
/// *pcch = 0 and the last error; GetAcceptLanguagesW still stores the
/// cut string. A NULL buffer is HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND).
void get_accept_languages(Marshaler& m, bool wide, RegistryThunks* registry) {
    const GuestAddr out = m.arg_ptr(0), pcch = m.arg_ptr(1);
    if (pcch == 0) { m.set_return(kEInvalidArg); return; }
    if (out == 0) { m.set_return(kHrFileNotFound); return; }
    U16 langs = u"en-US";
    if (registry != nullptr) {
        std::vector<u8> data(4096);
        u32 size = static_cast<u32>(data.size());
        u32 type = 0;
        if (registry->reg_get_value_w(kHkeyCurrentUser, L"Software\\Microsoft\\Internet Explorer\\International",
                                      L"AcceptLanguage", kRrfRtRegSz, &type, data.data(), &size) == 0) {
            langs.clear();
            for (u32 k = 0; k + 1 < size; k += 2) {
                const char16_t c = static_cast<char16_t>(data[k] | (data[k + 1] << 8));
                if (c == 0) break;
                langs.push_back(c);
            }
        }
    }
    const Encoded e = encode(langs, wide);
    const u32 cch = m.read_u32(pcch).value_or(0);
    const u32 len = static_cast<u32>(e.units.size());
    if (cch > len) {
        (void)write_units(m, out, e.units, wide);
        // GetAcceptLanguagesA also terminates the buffer's last unit (measured).
        if (!wide) (void)m.write_u8(out + cch - 1, 0);
        (void)m.write_u32(pcch, len);
        m.set_return(kSOk);
        return;
    }
    if (wide && cch > 0) {
        std::vector<u16> cut(e.units.begin(), e.units.begin() + (cch - 1));
        (void)write_units(m, out, cut, true);
    }
    (void)m.write_u32(pcch, 0);
    set_last_error(kErrorCannotCopy);
    m.set_return(kHrCannotCopy);
}

} // namespace

void add_shlwapi_url_exports(DllExports& exports, RegistryThunks* registry, GlobalLocalManager* local_heap) {
    auto add = [&](const std::string& name, std::function<void(Marshaler&)> fn, u16 bytes) {
        exports.entries.push_back(ExportEntry{name, 0, std::move(fn), bytes});
    };
    for (const bool w : {true, false}) {
        const std::string s = w ? "W" : "A";
        add("UrlCanonicalize" + s, [w](Marshaler& m) { url_canonicalize(m, w); }, 16);
        add("UrlEscape" + s, [w](Marshaler& m) { url_escape(m, w); }, 16);
        add("UrlUnescape" + s, [w](Marshaler& m) { url_unescape(m, w); }, 16);
        add("UrlCombine" + s, [w](Marshaler& m) { url_combine(m, w); }, 20);
        add("UrlGetPart" + s, [w](Marshaler& m) { url_get_part(m, w); }, 20);
        add("UrlCreateFromPath" + s, [w](Marshaler& m) { url_create_from_path(m, w); }, 16);
        add("PathCreateFromUrl" + s, [w](Marshaler& m) { path_create_from_url(m, w); }, 16);
        add("UrlApplyScheme" + s, [w, registry](Marshaler& m) { url_apply_scheme(m, w, registry); }, 16);
        add("UrlIs" + s, [w](Marshaler& m) { url_is_thunk(m, w); }, 8);
        add("UrlIsOpaque" + s, [w](Marshaler& m) {
            const GuestAddr in = m.arg_ptr(0);
            m.set_return(in && is_opaque(w ? read_w(m, in) : bytes_as_units(read_a(m, in))) ? 1u : 0u);
        }, 4);
        add("UrlIsNoHistory" + s, [w](Marshaler& m) {
            const GuestAddr in = m.arg_ptr(0);
            m.set_return(in ? is_no_history(w ? read_w(m, in) : bytes_as_units(read_a(m, in))) : 0u);
        }, 4);
        add("PathIsURL" + s, [w](Marshaler& m) {
            const GuestAddr in = m.arg_ptr(0);
            m.set_return(in && path_is_url(w ? read_w(m, in) : bytes_as_units(read_a(m, in))) ? 1u : 0u);
        }, 4);
        add("UrlGetLocation" + s, [w](Marshaler& m) {
            const GuestAddr in = m.arg_ptr(0);
            if (in == 0) { m.set_return(0); return; }
            const auto at = get_location(w ? read_w(m, in) : bytes_as_units(read_a(m, in)));
            m.set_return(at ? static_cast<u64>(in + *at * (w ? 2 : 1)) : 0u);
        }, 4);
        add("UrlCompare" + s, [w](Marshaler& m) { url_compare(m, w); }, 12);
        add("ParseURL" + s, [w](Marshaler& m) { parse_url_thunk(m, w); }, 8);
        add("UrlHash" + s, [w](Marshaler& m) { url_hash(m, w, false); }, 12);
        add("GetAcceptLanguages" + s, [w, registry](Marshaler& m) { get_accept_languages(m, w, registry); }, 8);
    }
    add("PathCreateFromUrlAlloc", [local_heap](Marshaler& m) { path_create_from_url_alloc(m, local_heap); }, 12);
    add("UrlFixupW", [](Marshaler& m) { url_fixup(m); }, 12);
    add("HashData", [](Marshaler& m) { url_hash(m, true, true); }, 16);
}

} // namespace vitreo::winapi
