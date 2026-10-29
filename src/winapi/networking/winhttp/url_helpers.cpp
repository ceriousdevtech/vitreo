// SPDX-License-Identifier: LGPL-2.1-only
/// @file url_helpers.cpp
/// @brief WinHttpCrackUrl / WinHttpCreateUrl and the shared URL_COMPONENTS
///        codec (also used by InternetCrackUrlA/W).
///
/// The codec follows the measured contract in
/// tools/oracle_probe/http_crackurl_probe*.expected (Windows 11, x86 + x64);
/// the rules below cite the probe cases that pin them.

#include "winhttp.hpp"
#include "url_components.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace vitreo::winapi {

#include "http_auth_constants.inc"

namespace urlcrack {
namespace {

using Units = std::vector<u16>;

enum Comp : int { SCHEME = 0, HOST, USER, PASS, PATH, EXTRA, COUNT };

constexpr u32 ERROR_SUCCESS_VALUE = 0;
constexpr u32 ERROR_NO_UNICODE_TRANSLATION_VALUE = 1113;
constexpr u32 MB_ERR_INVALID_CHARS_FLAG = 0x8;

struct Span {
    bool present = false;
    usize off = 0;
    usize len = 0;
};

struct Parsed {
    i32 nscheme = 0;
    u16 port = 0;
    Span c[COUNT];
    Span port_text;   ///< the digits after ':' when the URL states a port
    bool file = false;
};

Span span(usize off, usize len) { return Span{true, off, len}; }

u16 lower(u16 c) { return (c >= 'A' && c <= 'Z') ? static_cast<u16>(c + 32) : c; }

bool equals_ci(const Units& s, usize off, usize len, const char* name) {
    usize i = 0;
    for (; name[i] != 0; ++i) {
        if (i >= len || lower(s[off + i]) != static_cast<u8>(name[i])) return false;
    }
    return i == len;
}

bool is_hex(u16 c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
u8 hex_value(u16 c) {
    if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
    return static_cast<u8>(lower(c) - 'a' + 10);
}

usize find(const Units& s, usize from, usize to, u16 ch) {
    for (usize i = from; i < to; ++i) if (s[i] == ch) return i;
    return to;
}

// Authority: [user[:pass]@]host[:port], ending at '/', '?' or '#'.
// WinHTTP: an empty user or password is absent; any number of ':' allowed in
// the password ("us:er:pw" -> user "us", password "er:pw").
// WinINet: more than one ':' in userinfo is INVALID_URL; "@host" yields a
// present empty user and "u:@h" a present empty password.
u32 parse_authority(const Units& s, usize start, usize n, bool inet,
                    bool reject_userpwd, u16 default_port, Parsed& p,
                    usize& authority_end) {
    usize ae = start;
    while (ae < n && s[ae] != '/' && s[ae] != '?' && s[ae] != '#') ++ae;
    authority_end = ae;
    usize hs = start;
    const usize at = find(s, start, ae, '@');
    if (at < ae) {
        if (reject_userpwd) return http_contract::ERROR_WINHTTP_INVALID_URL;
        const usize colon = find(s, start, at, ':');
        if (inet) {
            if (colon < at && find(s, colon + 1, at, ':') < at)
                return http_contract::ERROR_INTERNET_INVALID_URL;
            if (colon < at) {
                if (colon > start) p.c[USER] = span(start, colon - start);
                p.c[PASS] = span(colon + 1, at - colon - 1);
            } else {
                p.c[USER] = span(start, at - start);
            }
        } else {
            if (colon < at) {
                if (colon > start) p.c[USER] = span(start, colon - start);
                if (at > colon + 1) p.c[PASS] = span(colon + 1, at - colon - 1);
            } else if (at > start) {
                p.c[USER] = span(start, at - start);
            }
        }
        hs = at + 1;
    }
    usize he = hs;
    if (hs < ae && s[hs] == '[') {
        const usize close = find(s, hs, ae, ']');
        if (close >= ae) return http_contract::ERROR_WINHTTP_INVALID_URL;
        he = close + 1;
        if (he < ae && s[he] != ':') return http_contract::ERROR_WINHTTP_INVALID_URL;
    } else {
        while (he < ae && s[he] != ':') ++he;
    }
    if (he == hs) return http_contract::ERROR_WINHTTP_INVALID_URL;
    p.c[HOST] = span(hs, he - hs);
    p.port = default_port;
    if (he < ae) {
        u32 value = 0;
        for (usize i = he + 1; i < ae; ++i) {
            if (s[i] < '0' || s[i] > '9') return http_contract::ERROR_WINHTTP_INVALID_URL;
            value = value * 10 + (s[i] - '0');
            if (value > 0xFFFF) return http_contract::ERROR_WINHTTP_INVALID_URL;
        }
        if (ae > he + 1) {
            p.port = static_cast<u16>(value);
            p.port_text = span(he + 1, ae - he - 1);
        }
    }
    return 0;
}

void split_path(const Units& s, usize from, usize n, bool extra_always, Parsed& p) {
    usize q = from;
    while (q < n && s[q] != '?' && s[q] != '#') ++q;
    p.c[PATH] = span(from, q - from);
    if (extra_always || q < n) p.c[EXTRA] = span(q, n - q);
}

// WinHTTP: http, https, ftp and gopher are recognised (gopher reports
// INTERNET_SCHEME -1 with port 0, ftp scheme 3 with port 0: probe cases 3/36);
// every other scheme or a missing ':' is UNRECOGNIZED_SCHEME; a recognised
// scheme without "//" or with an empty host is INVALID_URL.
u32 parse_winhttp(const Units& s, usize n, bool reject_userpwd, Parsed& p) {
    const usize colon = find(s, 0, n, ':');
    if (colon >= n) return http_contract::ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    struct Known { const char* name; i32 scheme; u16 port; };
    static constexpr Known known[] = {
        {"http", static_cast<i32>(http_contract::INTERNET_SCHEME_HTTP),
         static_cast<u16>(http_contract::INTERNET_DEFAULT_HTTP_PORT)},
        {"https", static_cast<i32>(http_contract::INTERNET_SCHEME_HTTPS),
         static_cast<u16>(http_contract::INTERNET_DEFAULT_HTTPS_PORT)},
        {"ftp", static_cast<i32>(http_contract::INTERNET_SCHEME_FTP), 0},
        {"gopher", -1, 0},
    };
    const Known* scheme = nullptr;
    for (const auto& k : known)
        if (equals_ci(s, 0, colon, k.name)) scheme = &k;
    if (!scheme) return http_contract::ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    p.c[SCHEME] = span(0, colon);
    p.nscheme = scheme->scheme;
    if (colon + 2 >= n || s[colon + 1] != '/' || s[colon + 2] != '/')
        return http_contract::ERROR_WINHTTP_INVALID_URL;
    usize ae = 0;
    if (const u32 error = parse_authority(s, colon + 3, n, false, reject_userpwd,
                                          scheme->port, p, ae))
        return error;
    split_path(s, ae, n, true, p);
    return 0;
}

// WinINet recognises the INTERNET_SCHEME enum names (socks fails as
// UNRECOGNIZED_SCHEME); any other name before ':' is scheme -1 and parsed
// generically. Default ports: http 80, https 443, ftp 21, gopher 0 (probe).
u32 parse_wininet(const Units& s, usize n, bool escape, Parsed& p) {
    const usize colon = find(s, 0, n, ':');
    if (colon >= n || colon == 0) return http_contract::ERROR_INTERNET_UNRECOGNIZED_SCHEME;
    struct Known { const char* name; i32 scheme; u16 port; };
    static constexpr Known known[] = {
        {"ftp", http_contract::INET_SCHEME_FTP,
         static_cast<u16>(http_contract::INTERNET_DEFAULT_FTP_PORT)},
        {"gopher", http_contract::INET_SCHEME_GOPHER, 0},
        {"http", http_contract::INET_SCHEME_HTTP,
         static_cast<u16>(http_contract::INTERNET_DEFAULT_HTTP_PORT)},
        {"https", http_contract::INET_SCHEME_HTTPS,
         static_cast<u16>(http_contract::INTERNET_DEFAULT_HTTPS_PORT)},
        {"file", http_contract::INET_SCHEME_FILE, 0},
        {"news", http_contract::INET_SCHEME_NEWS, 0},
        {"mailto", http_contract::INET_SCHEME_MAILTO, 0},
        {"socks", http_contract::INET_SCHEME_SOCKS, 0},
        {"javascript", http_contract::INET_SCHEME_JAVASCRIPT, 0},
        {"vbscript", http_contract::INET_SCHEME_VBSCRIPT, 0},
        {"res", http_contract::INET_SCHEME_RES, 0},
    };
    i32 scheme = http_contract::INET_SCHEME_UNKNOWN;
    u16 port = 0;
    for (const auto& k : known)
        if (equals_ci(s, 0, colon, k.name)) { scheme = k.scheme; port = k.port; }
    p.c[SCHEME] = span(0, colon);
    p.nscheme = scheme;
    const usize rest = colon + 1;
    const bool slashes = rest + 1 < n && s[rest] == '/' && s[rest + 1] == '/';
    usize ae = 0;
    switch (scheme) {
    case http_contract::INET_SCHEME_HTTP:
    case http_contract::INET_SCHEME_HTTPS:
    case http_contract::INET_SCHEME_FTP:
    case http_contract::INET_SCHEME_GOPHER:
        if (!slashes || rest + 2 >= n) return http_contract::ERROR_INTERNET_UNRECOGNIZED_SCHEME;
        if (const u32 error = parse_authority(s, rest + 2, n, true, false, port, p, ae))
            return error;
        split_path(s, ae, n, false, p);
        break;
    case http_contract::INET_SCHEME_SOCKS:
        return http_contract::ERROR_INTERNET_UNRECOGNIZED_SCHEME;
    case http_contract::INET_SCHEME_FILE:
        if (!slashes) return http_contract::ERROR_INTERNET_UNRECOGNIZED_SCHEME;
        p.file = true;
        split_path(s, rest + 2, n, false, p);
        break;
    case http_contract::INET_SCHEME_JAVASCRIPT:
    case http_contract::INET_SCHEME_VBSCRIPT:
        if (slashes) return http_contract::ERROR_INTERNET_UNRECOGNIZED_SCHEME;
        p.c[HOST] = span(rest, n - rest);
        p.c[PATH] = span(n, 0);
        break;
    case http_contract::INET_SCHEME_MAILTO: {
        const usize at = find(s, rest, n, '@');
        if (at < n) {
            p.c[USER] = span(rest, at - rest);
            p.c[HOST] = span(at + 1, n - at - 1);
        } else {
            p.c[HOST] = span(rest, n - rest);
        }
        p.c[PATH] = span(n, 0);
        break;
    }
    default:
        if (slashes) {
            if (const u32 error = parse_authority(s, rest + 2, n, true, false, port, p, ae))
                return error;
            split_path(s, ae, n, false, p);
        } else {
            split_path(s, rest, n, false, p);
        }
        break;
    }
    if (escape) {
        // ICU_ESCAPE validates every escape in the path and extra info.
        for (int c : {PATH, EXTRA}) {
            if (!p.c[c].present) continue;
            const usize end = p.c[c].off + p.c[c].len;
            for (usize i = p.c[c].off; i < end; ++i) {
                if (s[i] == '%' && (i + 2 >= end || !is_hex(s[i + 1]) || !is_hex(s[i + 2])))
                    return http_contract::ERROR_INTERNET_INVALID_URL;
            }
        }
    }
    return 0;
}

// UTF-8 encoding of UTF-16 units; a paired surrogate becomes one code point.
void append_utf8(const Units& in, usize& i, std::vector<u8>& out) {
    const u16 c = in[i];
    if (c < 0x80) {
        out.push_back(static_cast<u8>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<u8>(0xC0 | (c >> 6)));
        out.push_back(static_cast<u8>(0x80 | (c & 0x3F)));
    } else if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.size() &&
               in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
        const u32 cp = 0x10000 + ((static_cast<u32>(c) - 0xD800) << 10) + (in[i + 1] - 0xDC00u);
        out.push_back(static_cast<u8>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<u8>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<u8>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<u8>(0x80 | (cp & 0x3F)));
        ++i;
    } else {
        out.push_back(static_cast<u8>(0xE0 | (c >> 12)));
        out.push_back(static_cast<u8>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<u8>(0x80 | (c & 0x3F)));
    }
}

bool valid_escape_at(const Units& in, usize i) {
    return in[i] == '%' && i + 2 < in.size() && is_hex(in[i + 1]) && is_hex(in[i + 2]);
}

// Percent-decode to bytes; other units contribute their UTF-8 encoding.
std::vector<u8> percent_bytes(const Units& in) {
    std::vector<u8> out;
    for (usize i = 0; i < in.size(); ++i) {
        if (valid_escape_at(in, i)) {
            out.push_back(static_cast<u8>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2])));
            i += 2;
        } else {
            append_utf8(in, i, out);
        }
    }
    return out;
}

// Bytes -> UTF-16 through the measured MultiByteToWideChar decoder.
bool bytes_to_units(u32 code_page, u32 flags, const std::vector<u8>& bytes, Units& out) {
    out.clear();
    if (bytes.empty()) return true;
    out.resize(bytes.size() * 2 + 2);
    const auto r = codepage::multi_byte_to_wide(code_page, flags, bytes.data(), bytes.size(),
                                                out.data(), out.size());
    if (!r.ok()) { out.clear(); return false; }
    out.resize(r.count);
    return true;
}

// WinHTTP ICU_DECODE: %XX -> bytes, then UTF-8 with U+FFFD replacement. An
// invalid sequence fails the call with ERROR_INVALID_PARAMETER after the
// replaced text was stored (probe H-dec-utf8).
bool winhttp_decode(const Units& in, Units& out) {
    const std::vector<u8> bytes = percent_bytes(in);
    (void)bytes_to_units(codepage::kCpUtf8, 0, bytes, out);
    Units strict;
    return bytes_to_units(codepage::kCpUtf8, MB_ERR_INVALID_CHARS_FLAG, bytes, strict);
}

// WinHTTP ICU_ESCAPE: UTF-8 encode (an unpaired surrogate fails with
// ERROR_NO_UNICODE_TRANSLATION) and escape control characters, space,
// " # % < > [ \ ] ^ ` { | } ~ DEL and every non-ASCII byte, upper-case hex
// (per-character scan of Windows 11 winhttp.dll).
bool winhttp_escape(const Units& in, Units& out) {
    out.clear();
    std::vector<u8> bytes;
    for (usize i = 0; i < in.size(); ++i) {
        const u16 c = in[i];
        if (c >= 0xD800 && c < 0xE000 &&
            (c >= 0xDC00 || i + 1 >= in.size() || in[i + 1] < 0xDC00 || in[i + 1] >= 0xE000))
            return false;
        append_utf8(in, i, bytes);
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (u8 b : bytes) {
        const bool escape = b <= 0x20 || b >= 0x7F || b == '"' || b == '#' || b == '%' ||
                            b == '<' || b == '>' || b == '[' || b == '\\' || b == ']' ||
                            b == '^' || b == '`' || b == '{' || b == '|' || b == '}' || b == '~';
        if (escape) {
            out.push_back('%');
            out.push_back(static_cast<u16>(kHex[b >> 4]));
            out.push_back(static_cast<u16>(kHex[b & 15]));
        } else {
            out.push_back(b);
        }
    }
    return true;
}

// WinINet decode (ICU_DECODE, and ICU_ESCAPE for path/extra): %XX -> byte.
// ANSI keeps the bytes; Unicode converts them as UTF-8 and, when that fails,
// as the ANSI code page with ERROR_NO_UNICODE_TRANSLATION left as the last
// error of the successful call (probe W-dec-utf8).
Units wininet_decode(const Units& in, bool wide, bool& utf8_fallback) {
    std::vector<u8> bytes;
    if (wide) {
        bytes = percent_bytes(in);
        Units out;
        if (bytes_to_units(codepage::kCpUtf8, MB_ERR_INVALID_CHARS_FLAG, bytes, out)) return out;
        utf8_fallback = true;
        (void)bytes_to_units(codepage::kCpAcp, 0, bytes, out);
        return out;
    }
    Units out;
    for (usize i = 0; i < in.size(); ++i) {
        if (valid_escape_at(in, i)) {
            out.push_back(static_cast<u16>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2])));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// WinINet copies a file: URL path as a DOS path: escapes decoded, a
// "localhost/" authority dropped, "/X:" or "/X|" drive paths lose the leading
// slash ('|' -> ':'), a host-first path becomes UNC, and '/' becomes '\'.
Units file_url_to_dos(const Units& raw, bool wide, bool& utf8_fallback) {
    Units d = wininet_decode(raw, wide, utf8_fallback);
    if (d.size() >= 10 && equals_ci(d, 0, 9, "localhost") && d[9] == '/')
        d.erase(d.begin(), d.begin() + 9);
    auto is_alpha = [](u16 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!d.empty() && d[0] == '/') {
        if (d.size() >= 3 && is_alpha(d[1]) && (d[2] == ':' || d[2] == '|'))
            d.erase(d.begin());
    } else {
        d.insert(d.begin(), {'/', '/'});
    }
    if (d.size() >= 2 && is_alpha(d[0]) && d[1] == '|') d[1] = ':';
    for (auto& c : d) if (c == '/') c = '\\';
    return d;
}

struct Guest {
    Marshaler& m;
    bool is64;
    bool wide;
    GuestAddr read_ptr(GuestAddr a) const {
        return is64 ? m.read_u64(a).value_or(0) : m.read_u32(a).value_or(0);
    }
    void write_ptr(GuestAddr a, GuestAddr v) const {
        if (is64) (void)m.write_u64(a, v); else (void)m.write_u32(a, static_cast<u32>(v));
    }
    void write_units(GuestAddr dst, const Units& u, usize count) const {
        for (usize i = 0; i < count; ++i) {
            const u16 v = i < u.size() ? u[i] : 0;
            if (wide) (void)m.write_u16(dst + i * 2, v);
            else (void)m.write_u8(dst + i, static_cast<u8>(v));
        }
    }
    void write_string(GuestAddr dst, const Units& u) const { write_units(dst, u, u.size() + 1); }
};

} // namespace

Layout layout(bool is64) noexcept {
    // Field kinds in header order: D = DWORD/INTERNET_SCHEME (4 bytes),
    // P = LPxSTR, W = INTERNET_PORT (WORD).
    static constexpr char kFields[] = "DPDDPDWPDPDPDPD";
    const u32 pointer = is64 ? 8u : 4u;
    u32 offsets[15] = {};
    u32 offset = 0;
    u32 max_align = 4;
    for (int i = 0; i < 15; ++i) {
        const u32 size = kFields[i] == 'P' ? pointer : kFields[i] == 'W' ? 2u : 4u;
        offset = (offset + size - 1) / size * size;
        offsets[i] = offset;
        offset += size;
        max_align = std::max(max_align, size);
    }
    Layout l;
    l.size = (offset + max_align - 1) / max_align * max_align;
    const int ptr_fields[6] = {1, 4, 7, 9, 11, 13};
    const int len_fields[6] = {2, 5, 8, 10, 12, 14};
    for (int c = 0; c < 6; ++c) {
        l.ptr[c] = offsets[ptr_fields[c]];
        l.len[c] = offsets[len_fields[c]];
    }
    l.nscheme = offsets[3];
    l.port = offsets[6];
    return l;
}

u32 crack(Marshaler& m, Api api, GuestAddr url, u32 url_length, u32 flags,
          GuestAddr components, const ScratchAlloc& scratch) {
    const bool is64 = m.cpu_is_64bit();
    const bool wide = api != Api::WinInetA;
    const bool winhttp = api == Api::WinHttp;
    const Guest g{m, is64, wide};
    const Layout L = layout(is64);
    const u32 unit = wide ? 2u : 1u;
    auto fail = [](u32 error) { set_last_error(error); return 0u; };

    const u32 valid_flags = http_contract::ICU_DECODE | http_contract::ICU_ESCAPE |
                            (winhttp ? http_contract::ICU_REJECT_USERPWD : 0u);
    if ((flags & ~valid_flags) != 0 || url == 0 || components == 0)
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (api != Api::WinInetW && m.read_u32(components).value_or(0) != L.size)
        return fail(error_codes::ERROR_INVALID_PARAMETER);

    GuestAddr ptr[COUNT];
    u32 len[COUNT];
    bool any_buffer = false;
    for (int c = 0; c < COUNT; ++c) {
        ptr[c] = g.read_ptr(components + L.ptr[c]);
        len[c] = m.read_u32(components + L.len[c]).value_or(0);
        any_buffer = any_buffer || ptr[c] != 0;
    }
    const bool transform = (flags & (http_contract::ICU_DECODE | http_contract::ICU_ESCAPE)) != 0;
    if (winhttp) {
        // A buffer with no size, or (with DECODE/ESCAPE) any component
        // requested in place, is ERROR_INVALID_PARAMETER.
        for (int c = 0; c < COUNT; ++c) {
            if (ptr[c] != 0 && len[c] == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
            if (transform && ptr[c] == 0 && len[c] != 0)
                return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
    } else if (transform && !any_buffer) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }

    // Effective length: dwUrlLength characters, stopping at a terminator.
    Units s;
    const usize limit = url_length != 0 ? url_length : 1u << 20;
    for (usize i = 0; i < limit; ++i) {
        const u16 v = wide ? m.read_u16(url + i * 2).value_or(0)
                           : m.read_u8(url + i).value_or(0);
        if (v == 0) break;
        s.push_back(v);
    }
    const usize n = s.size();

    Parsed p;
    const u32 parse_error = winhttp
        ? parse_winhttp(s, n, (flags & http_contract::ICU_REJECT_USERPWD) != 0, p)
        : parse_wininet(s, n, (flags & http_contract::ICU_ESCAPE) != 0, p);
    if (parse_error != 0) {
        if (api == Api::WinInetA) {
            for (int c = 0; c < COUNT; ++c)
                if (ptr[c] != 0 && len[c] != 0) (void)m.write_u8(ptr[c], 0);
        }
        return fail(parse_error);
    }

    // With lpszExtraInfo not requested, the path carries query and fragment.
    if (ptr[EXTRA] == 0 && len[EXTRA] == 0 && p.c[PATH].present)
        p.c[PATH].len = n - p.c[PATH].off;

    // WinINet ANSI decodes "%25" inside a bracketed host in the caller's own
    // string (probe A-http-ipv6-noport).
    if (api == Api::WinInetA && p.c[HOST].present && p.c[HOST].len > 0 &&
        s[p.c[HOST].off] == '[') {
        const Units host(s.begin() + static_cast<std::ptrdiff_t>(p.c[HOST].off),
                         s.begin() + static_cast<std::ptrdiff_t>(p.c[HOST].off + p.c[HOST].len));
        Units fixed;
        for (usize i = 0; i < host.size(); ++i) {
            if (host[i] == '%' && i + 2 < host.size() && host[i + 1] == '2' && host[i + 2] == '5') {
                fixed.push_back('%');
                i += 2;
            } else {
                fixed.push_back(host[i]);
            }
        }
        if (fixed.size() != host.size()) {
            g.write_units(url + p.c[HOST].off, fixed, fixed.size());
            std::copy(fixed.begin(), fixed.end(),
                      s.begin() + static_cast<std::ptrdiff_t>(p.c[HOST].off));
            p.c[HOST].len = fixed.size();
        }
    }

    auto component = [&](int c) {
        const Span& sp = p.c[c];
        return sp.present ? Units(s.begin() + static_cast<std::ptrdiff_t>(sp.off),
                                  s.begin() + static_cast<std::ptrdiff_t>(sp.off + sp.len))
                          : Units{};
    };
    auto write_scheme_port = [&] {
        (void)m.write_u32(components + L.nscheme, static_cast<u32>(p.nscheme));
        (void)m.write_u16(components + L.port, p.port);
    };

    if (winhttp) {
        u32 pending = 0;
        for (int c = 0; c < COUNT; ++c) {
            if (ptr[c] == 0 && len[c] == 0) continue;
            const Span& sp = p.c[c];
            if (ptr[c] == 0) {
                g.write_ptr(components + L.ptr[c], sp.present ? url + sp.off * unit : 0);
                (void)m.write_u32(components + L.len[c], sp.present ? static_cast<u32>(sp.len) : 0u);
                continue;
            }
            const Units raw = component(c);
            const bool escapable = (c == PATH || c == EXTRA) &&
                                   (flags & http_contract::ICU_ESCAPE) != 0;
            if (raw.size() + 1 > len[c]) {
                // A plain component reports its exact size; an escapable one
                // the worst case of 18 units per character (probe esc_small).
                g.write_units(ptr[c], {}, 1);
                (void)m.write_u32(components + L.len[c],
                                  static_cast<u32>(raw.size() * (escapable ? 18u : 1u) + 1u));
                pending = error_codes::ERROR_INSUFFICIENT_BUFFER;
                continue;
            }
            g.write_string(ptr[c], raw);
            Units value = raw;
            if ((flags & http_contract::ICU_DECODE) != 0 && c != SCHEME) {
                Units decoded;
                if (!winhttp_decode(raw, decoded)) {
                    g.write_units(ptr[c], decoded, decoded.size());
                    return fail(error_codes::ERROR_INVALID_PARAMETER);
                }
                value = std::move(decoded);
            }
            if (escapable) {
                Units escaped;
                if (!winhttp_escape(value, escaped))
                    return fail(ERROR_NO_UNICODE_TRANSLATION_VALUE);
                if (escaped.size() + 1 > len[c]) {
                    g.write_units(ptr[c], {}, 1);
                    (void)m.write_u32(components + L.len[c], static_cast<u32>(escaped.size() + 1));
                    pending = error_codes::ERROR_INSUFFICIENT_BUFFER;
                    continue;
                }
                value = std::move(escaped);
            }
            if (value != raw) g.write_string(ptr[c], value);
            (void)m.write_u32(components + L.len[c], static_cast<u32>(value.size()));
        }
        write_scheme_port();
        if (pending != 0) return fail(pending);
        set_last_error(ERROR_SUCCESS_VALUE);
        return 1u;
    }

    // WinINet.
    write_scheme_port();
    const bool ansi = api == Api::WinInetA;
    bool utf8_fallback = false;
    u32 pending = 0;
    for (int c = 0; c < COUNT; ++c) {
        if (ptr[c] == 0 && len[c] == 0) continue;
        const Span& sp = p.c[c];
        const bool decode = c != SCHEME &&
            ((flags & http_contract::ICU_DECODE) != 0 ||
             ((c == PATH || c == EXTRA) && (flags & http_contract::ICU_ESCAPE) != 0));
        if (ptr[c] == 0) {
            if (!sp.present) {
                g.write_ptr(components + L.ptr[c], 0);
                (void)m.write_u32(components + L.len[c], 0);
                continue;
            }
            GuestAddr where = url + sp.off * unit;
            if (ansi && decode && scratch) {
                // ANSI hands back a decoded private copy, not the caller's
                // string, with the undecoded length (probe A-dec-mixed).
                bool unused = false;
                const Units decoded = wininet_decode(component(c), false, unused);
                if (const GuestAddr copy = scratch(decoded.size() + 1)) {
                    g.write_string(copy, decoded);
                    where = copy;
                }
            }
            g.write_ptr(components + L.ptr[c], where);
            (void)m.write_u32(components + L.len[c], static_cast<u32>(sp.len));
            continue;
        }
        const Units raw = component(c);
        const bool file_path = p.file && c == PATH;
        Units value;
        u32 reported = 0;
        bool fits = false;
        if (file_path) {
            value = file_url_to_dos(raw, wide, utf8_fallback);
            reported = static_cast<u32>(value.size());
            fits = value.size() + 1 <= len[c];
        } else {
            value = decode ? wininet_decode(raw, wide, utf8_fallback) : raw;
            // Path and extra report the undecoded length unless ICU_ESCAPE.
            reported = static_cast<u32>((c == PATH || c == EXTRA) &&
                                        (flags & http_contract::ICU_ESCAPE) == 0
                                        ? raw.size() : value.size());
            fits = raw.size() + 1 <= len[c];
        }
        if (len[c] == 0) {
            // A sized-zero buffer: ANSI reports the size it needs, Unicode
            // the component length, and neither writes (probe path0).
            (void)m.write_u32(components + L.len[c],
                              ansi ? static_cast<u32>(raw.size() + 1) : reported);
            if (ansi) pending = error_codes::ERROR_INSUFFICIENT_BUFFER;
            continue;
        }
        if (!fits) {
            if (ansi) {
                (void)m.write_u8(ptr[c], 0);
                if (!file_path)
                    (void)m.write_u32(components + L.len[c], static_cast<u32>(raw.size() + 1));
                pending = error_codes::ERROR_INSUFFICIENT_BUFFER;
                continue;
            }
            // Unicode stores what fits of the result and stops.
            g.write_units(ptr[c], value, std::min<usize>(len[c], value.size() + 1));
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        g.write_string(ptr[c], value);
        (void)m.write_u32(components + L.len[c], reported);
    }
    if (pending != 0) return fail(pending);
    set_last_error(utf8_fallback ? ERROR_NO_UNICODE_TRANSLATION_VALUE : ERROR_SUCCESS_VALUE);
    return 1u;
}

u32 proxy_script_arguments(const std::u16string& url, std::string& script_url,
                           std::string& host) {
    Units s(url.begin(), url.end());
    Parsed p;
    if (const u32 error = parse_winhttp(s, s.size(), false, p)) return error;
    auto narrow = [&](usize off, usize len, bool lower_case) {
        std::string out;
        std::vector<u8> bytes;
        Units part(s.begin() + static_cast<std::ptrdiff_t>(off),
                   s.begin() + static_cast<std::ptrdiff_t>(off + len));
        for (usize i = 0; i < part.size(); ++i) append_utf8(part, i, bytes);
        for (u8 b : bytes) out.push_back(static_cast<char>(lower_case ? lower(b) : b));
        return out;
    };
    // Measured (http_winhttp_handles_probe, urlhex.pac): the scheme is
    // lower-cased, credentials are dropped, the host keeps its case, a stated
    // port is kept, https URLs are reduced to "/" and other URLs keep path,
    // query and fragment ("/" when the path is empty). The host argument is
    // the host name as written (case kept: probe url.pac / url2.pac).
    const std::string scheme = narrow(p.c[SCHEME].off, p.c[SCHEME].len, true);
    script_url = scheme + "://" + narrow(p.c[HOST].off, p.c[HOST].len, false);
    if (p.port_text.present) script_url += ":" + narrow(p.port_text.off, p.port_text.len, false);
    if (scheme == "https") {
        script_url += "/";
    } else {
        const std::string path = narrow(p.c[PATH].off, p.c[PATH].len, false);
        script_url += path.empty() ? "/" : path;
        script_url += narrow(p.c[EXTRA].off, p.c[EXTRA].len, false);
    }
    host = narrow(p.c[HOST].off, p.c[HOST].len, false);
    return 0;
}

} // namespace urlcrack

// =============================================================================
// WinHTTP URL helpers
// =============================================================================

void WinHttpManager::win_http_crack_url(Marshaler& m) {
    // BOOL WinHttpCrackUrl(LPCWSTR pwszUrl, DWORD dwUrlLength, DWORD dwFlags,
    //                      LPURL_COMPONENTS lpUrlComponents)
    m.set_return(urlcrack::crack(m, urlcrack::Api::WinHttp, m.arg_ptr(0), m.arg32(1),
                                 m.arg32(2), m.arg_ptr(3)));
}

void WinHttpManager::win_http_create_url(Marshaler& m) {
    // BOOL WinHttpCreateUrl(LPURL_COMPONENTS lpUrlComponents, DWORD dwFlags,
    //                       LPWSTR pwszUrl, LPDWORD pdwUrlLength)
    const GuestAddr comp_addr = m.arg_ptr(0);
    const GuestAddr url_buf = m.arg_ptr(2);
    const GuestAddr url_len_ptr = m.arg_ptr(3);
    if (!comp_addr || !url_len_ptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const bool is64 = m.cpu_is_64bit();
    const urlcrack::Layout L = urlcrack::layout(is64);
    auto read_comp_wide = [&](int c) -> std::string {
        const GuestAddr str_ptr = is64 ? m.read_u64(comp_addr + L.ptr[c]).value_or(0)
                                       : m.read_u32(comp_addr + L.ptr[c]).value_or(0);
        const u32 str_len = m.read_u32(comp_addr + L.len[c]).value_or(0);
        if (!str_ptr) return "";
        return str_len > 0 ? m.read_wide_string(str_ptr, str_len).value_or("")
                           : m.read_wide_string(str_ptr).value_or("");
    };

    std::string scheme = read_comp_wide(0);
    const u32 nScheme = m.read_u32(comp_addr + L.nscheme).value_or(0);
    const std::string host = read_comp_wide(1);
    const u16 port = m.read_u16(comp_addr + L.port).value_or(0);
    const std::string username = read_comp_wide(2);
    const std::string password = read_comp_wide(3);
    const std::string path = read_comp_wide(4);
    const std::string extra = read_comp_wide(5);

    if (scheme.empty()) scheme = nScheme == http_contract::INTERNET_SCHEME_HTTPS ? "https" : "http";
    std::string url = scheme + "://";
    if (!username.empty()) {
        url += username;
        if (!password.empty()) url += ":" + password;
        url += "@";
    }
    url += host;
    const bool default_port =
        (nScheme == http_contract::INTERNET_SCHEME_HTTP && port == http_contract::INTERNET_DEFAULT_HTTP_PORT) ||
        (nScheme == http_contract::INTERNET_SCHEME_HTTPS && port == http_contract::INTERNET_DEFAULT_HTTPS_PORT) ||
        port == 0;
    if (!default_port) url += ":" + std::to_string(port);
    if (path.empty() || path[0] != '/') url += "/";
    url += path;
    url += extra;

    const u32 required_wchars = static_cast<u32>(url.size() + 1);
    if (!url_buf || m.read_u32(url_len_ptr).value_or(0) < required_wchars) {
        (void)m.write_u32(url_len_ptr, required_wchars);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    (void)m.write_wide_string(url_buf, url);
    (void)m.write_u32(url_len_ptr, static_cast<u32>(url.size()));
    m.set_return(1);
}

} // namespace vitreo::winapi
