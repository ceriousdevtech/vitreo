/// @file shlwapi_url_core.cpp
/// @brief shlwapi's URL family as host functions on UTF-16 strings.
///
/// Contract sources: the Learn pages for UrlCanonicalize, UrlEscape,
/// UrlUnescape, UrlCombine, UrlGetPart, UrlIs, UrlIsOpaque, UrlIsNoHistory,
/// UrlGetLocation, UrlCompare, UrlCreateFromPath, PathCreateFromUrl(Alloc),
/// UrlApplyScheme, UrlFixupW, ParseURL and PathIsURL; the URL_* constants
/// and URL_SCHEME_* values of the Windows SDK's shlwapi.h. Learn describes
/// the flags but not the canonical forms; every rule below that it leaves
/// open was measured on Windows 11 by tools/oracle_probe/shlwapi_url_probe.c
/// (tests/oracle_data/shlwapi_url_probe{,.x86}.expected).

#include "winapi/system/shlwapi/shlwapi_url_core.hpp"

#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace vitreo::winapi::shlwapi_url {
namespace {

// ---------------------------------------------------------------------------
// Characters and schemes

bool is_alpha(char16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char16_t c) { return c >= '0' && c <= '9'; }
bool is_hex(char16_t c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hex_value(char16_t c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}
char16_t lower(char16_t c) { return (c >= 'A' && c <= 'Z') ? static_cast<char16_t>(c + 32) : c; }
bool is_scheme_char(char16_t c) { return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.'; }
bool is_slash(char16_t c) { return c == '/' || c == '\\'; }

U16 lower_ascii(U16 s) {
    for (auto& c : s) c = lower(c);
    return s;
}
bool iequals(const U16& a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (lower(a[i]) != static_cast<char16_t>(b[i])) return false;
    return true;
}
U16 u(std::string_view s) { return U16(s.begin(), s.end()); }

// URL_SCHEME_* (shlwapi.h) by name. 22 (MSSHELLDEVICE) and 23 (WILDCARD)
// have no name ParseURL recognises (measured: "ms-shell-device:" and "*:"
// are not them).
enum : u32 {
    kUnknown = 0, kFtp = 1, kHttp = 2, kGopher = 3, kMailto = 4, kNews = 5, kNntp = 6, kTelnet = 7,
    kWais = 8, kFile = 9, kMk = 10, kHttps = 11, kShell = 12, kSnews = 13, kLocal = 14,
    kJavascript = 15, kVbscript = 16, kAbout = 17, kRes = 18, kMsShellRooted = 19,
    kMsShellIdlist = 20, kMsHelp = 21, kSearchMs = 24, kSearch = 25, kKnownFolder = 26,
};
struct SchemeName { std::string_view name; u32 id; };
constexpr std::array<SchemeName, 24> kSchemes{{
    {"ftp", kFtp}, {"http", kHttp}, {"gopher", kGopher}, {"mailto", kMailto}, {"news", kNews},
    {"nntp", kNntp}, {"telnet", kTelnet}, {"wais", kWais}, {"file", kFile}, {"mk", kMk},
    {"https", kHttps}, {"shell", kShell}, {"snews", kSnews}, {"local", kLocal},
    {"javascript", kJavascript}, {"vbscript", kVbscript}, {"about", kAbout}, {"res", kRes},
    {"ms-shell-rooted", kMsShellRooted}, {"ms-shell-idlist", kMsShellIdlist}, {"hcp", kMsHelp},
    {"search-ms", kSearchMs}, {"search", kSearch}, {"knownfolder", kKnownFolder},
}};

u32 scheme_id(const U16& name) {
    for (const auto& s : kSchemes)
        if (iequals(name, s.name)) return s.id;
    return kUnknown;
}
// UrlIsOpaque (measured): mailto, shell, javascript, vbscript, about, knownfolder.
bool opaque_id(u32 id) {
    return id == kMailto || id == kShell || id == kJavascript || id == kVbscript || id == kAbout ||
           id == kKnownFolder;
}
// UrlIsNoHistory (measured; the BOOL is the scheme's flag bit, 4).
bool no_history_id(u32 id) {
    return id == kMk || id == kJavascript || id == kVbscript || id == kAbout || id == kRes;
}
// Schemes whose "//" part UrlGetPart splits into user, password, host, port.
bool server_id(u32 id) {
    return id == kFtp || id == kHttp || id == kGopher || id == kNews || id == kNntp || id == kTelnet ||
           id == kHttps || id == kSnews;
}

/// Length of the scheme (the characters before ':'), or 0 when @p s does not
/// start with one: two or more of [A-Za-z0-9+-.] followed by ':' (measured:
/// "1http:" and "ht-tp+1.x:" are schemes; "h:" and "*:" are not).
usize scheme_length(const U16& s) {
    usize i = 0;
    while (i < s.size() && is_scheme_char(s[i])) ++i;
    if (i < 2 || i >= s.size() || s[i] != ':') return 0;
    return i;
}

bool is_drive_path(const U16& s) {
    return s.size() >= 2 && is_alpha(s[0]) && (s[1] == ':' || s[1] == '|');
}
bool is_unc_path(const U16& s) { return s.size() >= 2 && s[0] == '\\' && s[1] == '\\'; }

// ---------------------------------------------------------------------------
// Escaping primitives

void append_escaped(U16& out, u8 byte) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    out.push_back('%');
    out.push_back(static_cast<char16_t>(kHex[byte >> 4]));
    out.push_back(static_cast<char16_t>(kHex[byte & 15]));
}

/// Escapes one UTF-16 code unit (or a surrogate pair when @p utf8): the code
/// unit's low byte for 0x01..0xFF, UTF-8 bytes when @p utf8. Returns the
/// units consumed.
usize append_escaped_char(U16& out, const U16& s, usize i, bool utf8) {
    const char16_t c = s[i];
    if (!utf8 || c < 0x80) {
        append_escaped(out, static_cast<u8>(c));
        return 1;
    }
    u32 cp = c;
    usize used = 1;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        cp = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
        used = 2;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
        cp = 0xFFFD;
    }
    u8 b[4];
    int n = 0;
    if (cp < 0x800) { b[0] = static_cast<u8>(0xC0 | (cp >> 6)); b[1] = static_cast<u8>(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = static_cast<u8>(0xE0 | (cp >> 12)); b[1] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
        b[2] = static_cast<u8>(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = static_cast<u8>(0xF0 | (cp >> 18)); b[1] = static_cast<u8>(0x80 | ((cp >> 12) & 0x3F));
        b[2] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F)); b[3] = static_cast<u8>(0x80 | (cp & 0x3F)); n = 4;
    }
    for (int k = 0; k < n; ++k) append_escaped(out, b[k]);
    return used;
}

/// The characters UrlEscape / URL_ESCAPE_UNSAFE convert (measured): the
/// controls, space, '"', '<', '>', '{', '}', '|', '^', '`', '[', ']', '&',
/// DEL, and 0x80..0xFF; '\\' too when it is not a separator.
bool is_unsafe(char16_t c) {
    if (c < 0x21 || c == 0x7F) return true;
    if (c >= 0x80 && c <= 0xFF) return true;
    switch (c) {
        case '"': case '<': case '>': case '{': case '}': case '|': case '^': case '`':
        case '[': case ']': case '&':
            return true;
        default:
            return false;
    }
}
/// RFC 3986 unreserved (URL_ESCAPE_ASCII_URI_COMPONENT keeps only these).
bool is_unreserved(char16_t c) {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// ---------------------------------------------------------------------------
// Unescaping

/// Decodes %XX escapes. @p utf8 decodes each run of escaped bytes as UTF-8
/// (with MultiByteToWideChar's replacement rules); otherwise each byte is the
/// code unit of the same value. Stops before the first '?' or '#' copied
/// verbatim when @p stop_at_extra.
U16 decode(const U16& s, bool utf8, bool stop_at_extra) {
    U16 out;
    usize i = 0;
    while (i < s.size()) {
        const char16_t c = s[i];
        if (stop_at_extra && (c == '?' || c == '#')) {
            out.append(s, i, U16::npos);
            break;
        }
        if (c == '%' && i + 2 < s.size() + 0 && i + 2 <= s.size() - 1 && is_hex(s[i + 1]) && is_hex(s[i + 2])) {
            if (!utf8) {
                out.push_back(static_cast<char16_t>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
                i += 3;
                continue;
            }
            std::vector<u8> bytes;
            while (i + 2 < s.size() && s[i] == '%' && is_hex(s[i + 1]) && is_hex(s[i + 2])) {
                bytes.push_back(static_cast<u8>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
                i += 3;
            }
            std::vector<u16> wide(bytes.size() * 2 + 2);
            const auto r = codepage::multi_byte_to_wide(65001, 0, bytes.data(), bytes.size(), wide.data(), wide.size());
            for (u32 k = 0; k < r.count; ++k) out.push_back(static_cast<char16_t>(wide[k]));
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

/// UrlCanonicalize's URL_UNESCAPE: like decode() except that an escape cut
/// off by the end of the string is dropped ("a%2" -> "a", "%" -> "") and a
/// '%' not followed by two hex digits is copied with the next two characters
/// ("%%41" stays "%%41") (measured).
U16 decode_canon(const U16& s) {
    U16 out;
    usize i = 0;
    while (i < s.size()) {
        if (s[i] != '%') { out.push_back(s[i++]); continue; }
        if (i + 2 >= s.size()) break;
        if (is_hex(s[i + 1]) && is_hex(s[i + 2])) {
            out.push_back(static_cast<char16_t>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
        } else {
            out.append(s, i, 3);
        }
        i += 3;
    }
    return out;
}

bool has_escaped_nul(const U16& s) {
    for (usize i = 0; i + 2 < s.size(); ++i)
        if (s[i] == '%' && s[i + 1] == '0' && s[i + 2] == '0') return true;
    return false;
}

// ---------------------------------------------------------------------------
// Splitting

/// Where the path of a hierarchical suffix ends: the first '?' or '#'.
usize extra_start(const U16& s, usize from) {
    for (usize i = from; i < s.size(); ++i)
        if (s[i] == '?' || s[i] == '#') return i;
    return s.size();
}

/// Removes '.' and '..' segments from @p path (which starts after the
/// authority, typically with '/'). Measured rules: nothing is simplified when
/// the first segment is "." or ".."; a "." segment disappears; ".." removes
/// the segment before it and disappears itself, also at the root; a final "."
/// or ".." leaves the trailing '/'. @p root_segments segments at the start
/// (the drive of a file URL) are never removed.
U16 simplify(const U16& path, usize keep_prefix, bool keep_excess = false, bool force = false) {
    // Segments after the first '/' of the kept prefix.
    const U16 prefix = path.substr(0, keep_prefix);
    U16 rest = path.substr(keep_prefix);
    const bool leading_slash = !rest.empty() && rest[0] == '/';
    if (leading_slash) rest.erase(0, 1);
    std::vector<U16> segs;
    {
        usize start = 0;
        for (usize i = 0; i <= rest.size(); ++i) {
            if (i == rest.size() || rest[i] == '/') {
                segs.push_back(rest.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    if (!force && !segs.empty() && (segs[0] == u(".") || segs[0] == u(".."))) return path;
    std::vector<U16> out;
    bool trailing = false;
    for (usize i = 0; i < segs.size(); ++i) {
        const bool last = i + 1 == segs.size();
        if (segs[i] == u(".")) {
            trailing = last;
            continue;
        }
        if (segs[i] == u("..")) {
            if (!out.empty() && out.back() != u("..")) out.pop_back();
            else if (keep_excess) { out.push_back(segs[i]); trailing = last; continue; }
            trailing = last;
            continue;
        }
        out.push_back(segs[i]);
        trailing = false;
    }
    U16 r = prefix;
    if (leading_slash) r.push_back('/');
    for (usize i = 0; i < out.size(); ++i) {
        if (i) r.push_back('/');
        r += out[i];
    }
    if (trailing && (!out.empty() || !leading_slash)) r.push_back('/');
    return r;
}

// ---------------------------------------------------------------------------
// The canonical form

struct Canon {
    u32 flags = 0;
    bool escape_mode = false;  // UrlEscape rather than UrlCanonicalize
};

/// Converts a DOS path to a file URL ("file:///C:/dir/x", "file://server/x").
U16 dos_path_to_url(const U16& path, u32 flags, bool escape_unsafe) {
    U16 body;
    for (const char16_t c : path) body.push_back(c == '\\' ? u'/' : c);
    U16 out = u("file:");
    if (is_unc_path(path)) {
        out += u("//");
        body = body.substr(2);
    } else if (is_drive_path(path)) {
        out += u("///");
    } else if (!path.empty() && path[0] == '\\') {
        out += u("//");
    }
    U16 esc;
    for (usize i = 0; i < body.size(); ++i) {
        const char16_t c = body[i];
        const bool utf8 = flags & URL_ESCAPE_AS_UTF8;
        if (utf8 && c >= 0x80) { i += append_escaped_char(esc, body, i, true) - 1; continue; }
        if (escape_unsafe && (is_unsafe(c) || c == '#' || c == '%' || c == '?')) {
            append_escaped(esc, static_cast<u8>(c));
            continue;
        }
        esc.push_back(c);
    }
    return out + esc;
}

U16 strip_whitespace(const U16& in) {
    usize b = 0, e = in.size();
    while (b < e && in[b] <= ' ') ++b;
    while (e > b && in[e - 1] <= ' ') --e;
    U16 out;
    for (usize i = b; i < e; ++i)
        if (in[i] != '\t' && in[i] != '\r' && in[i] != '\n') out.push_back(in[i]);
    return out;
}

/// The default port of a server scheme ("https://host:443" loses ":443").
std::string_view default_port(u32 id) {
    switch (id) {
        case kHttp: return "80";
        case kHttps: return "443";
        case kFtp: return "21";
        case kGopher: return "70";
        default: return {};
    }
}

/// How the part after "scheme:" is laid out.
enum class Shape {
    Relative,      // no scheme
    Hierarchical,  // scheme://authority/path?query#fragment
    File,          // file: (no fragment: '#' belongs to the path)
    Opaque,        // a known opaque scheme (UrlIsOpaque)
    Plain,         // a known non-opaque scheme without "//" (news:, mk:, search-ms:)
    Unknown,       // an unknown scheme without "//"
};

/// The escape step of UrlCanonicalize / UrlCombine over the canonical string
/// @p s whose escapable region starts at @p from (after "scheme:").
U16 escape_step(const U16& s, usize from, Shape shape, u32 flags, bool unescaped, bool escape_percent) {
    const bool unsafe = flags & URL_ESCAPE_UNSAFE;
    const bool spaces = flags & URL_ESCAPE_SPACES_ONLY;
    U16 out = s.substr(0, from);
    if (spaces) {
        if (shape == Shape::Opaque && !unescaped) {
            // Learn: URL_ESCAPE_SPACES_ONLY "does not apply to opaque URLs".
            for (usize i = from; i < s.size() && s[i]; ++i) out.push_back(s[i]);
            return out;
        }
        bool extra = false;
        for (usize i = from; i < s.size() && s[i]; ++i) {
            const char16_t c = s[i];
            if (c == '?' || (c == '#' && shape != Shape::File)) extra = true;
            if (c == ' ' && !(extra && (flags & URL_DONT_ESCAPE_EXTRA_INFO))) append_escaped(out, ' ');
            else out.push_back(c);
        }
        return out;
    }
    if (!unsafe) {
        for (usize i = from; i < s.size() && s[i]; ++i) out.push_back(s[i]);
        return out;
    }
    auto escapes = [&](char16_t c, bool body) {
        if (c == '%') return escape_percent;
        if (is_unsafe(c) || c == '\\') return true;
        if (!body) return false;
        if (c == '?' || c == '#') return true;
        if (c == '/') return shape == Shape::Unknown || unescaped;
        return false;
    };
    if (shape == Shape::Opaque || shape == Shape::Plain) {
        for (usize i = from; i < s.size() && s[i]; ++i) {
            if (escapes(s[i], true)) append_escaped(out, static_cast<u8>(s[i]));
            else out.push_back(s[i]);
        }
        return out;
    }
    // Hierarchical, file and unknown-scheme URLs: the path is escaped, the
    // query and fragment are not. An unknown scheme's body escapes '/'
    // (except a leading one); a file URL has no fragment, its '#' is escaped
    // wherever it is (measured).
    usize end = from;
    while (end < s.size() && s[end] && s[end] != '?' && (shape == Shape::File || s[end] != '#')) ++end;
    // A backslash inside an unknown scheme's authority is not escaped
    // (measured: "myproto://h\\a/" stays, "ftp://h%5Ca/" does not).
    usize auth_end = from;
    const usize ssl = scheme_length(s);
    if (shape == Shape::Hierarchical && ssl && scheme_id(s.substr(0, ssl)) == kUnknown && from + 1 < s.size() &&
        s[from] == '/' && s[from + 1] == '/') {
        auth_end = from + 2;
        while (auth_end < end && s[auth_end] != '/') ++auth_end;
    }
    for (usize i = from; i < end; ++i) {
        const char16_t c = s[i];
        if (c == '\\' && i < auth_end) { out.push_back(c); continue; }
        if (shape == Shape::File && c == '#') { append_escaped(out, '#'); continue; }
        if (shape == Shape::Unknown && c == '/' && i != from) { append_escaped(out, '/'); continue; }
        if (escapes(c, false)) append_escaped(out, static_cast<u8>(c));
        else out.push_back(c);
    }
    for (usize i = end; i < s.size() && s[i]; ++i) {
        if (shape == Shape::File && s[i] == '#') append_escaped(out, '#');
        else out.push_back(s[i]);
    }
    return out;
}

/// The "file://C:\\dir\\x" form URL_FILE_USE_PATHURL / URL_WININET_COMPATIBILITY
/// give a canonical file URL: its path with backslashes; PATHURL also
/// unescapes it and drops the query.
U16 file_url_to_pathurl(const U16& url, bool pathurl, bool keep_server_root = false) {
    // url is canonical: file:///X:/..., file://server/..., file://, file:x
    U16 rest = url.substr(5);
    if (rest.size() < 2 || rest[0] != '/' || rest[1] != '/') {
        if (!pathurl) return url;
        return u("file://") + rest;
    }
    U16 body;
    if (rest.size() >= 3 && rest[2] == '/') {
        body = rest.substr(3);
        // "file:///b" (no drive) is the rooted path "\\b".
        if (!(body.size() >= 2 && is_alpha(body[0]) && (body[1] == ':' || body[1] == '|')) && !body.empty())
            body.insert(body.begin(), u'/');
    } else {
        U16 server = rest.substr(2);
        const usize e = server.find('/');
        if (iequals(server.substr(0, e), "localhost")) body = e == U16::npos ? U16() : server.substr(e + 1);
        else {
            body = u("\\\\") + server;
            // PATHURL drops the root '/' of a bare server ("file://\\\\server").
            if (pathurl && !keep_server_root && e != U16::npos && e + 1 == server.size()) body.pop_back();
        }
    }
    if (pathurl) {
        const usize q = body.find('?');
        if (q != U16::npos) body.resize(q);
    }
    for (auto& c : body) if (c == '/') c = '\\';
    if (pathurl) body = decode(body, false, false);
    if (body.empty() && !pathurl) body = u("\\");
    return u("file://") + body;
}

/// UrlCanonicalize (W). @p force simplifies dot segments even at the start
/// of the path (UrlCombine's merged paths).
Result canonical(const U16& input, u32 flags, bool force = false) {
    U16 s = strip_whitespace(input);
    const usize sl = scheme_length(s);
    const U16 scheme = lower_ascii(s.substr(0, sl));
    const u32 id = sl ? scheme_id(scheme) : kUnknown;
    const bool known = sl != 0 && id != kUnknown;
    const bool simplify_paths =
        !(flags & URL_DONT_SIMPLIFY) && !((flags & URL_PLUGGABLE_PROTOCOL) && !known);
    bool escape_percent = flags & URL_ESCAPE_PERCENT;
    const bool pathurl = flags & (URL_FILE_USE_PATHURL | URL_WININET_COMPATIBILITY);

    Shape shape;
    U16 out;
    usize region = 0;  // where the escapable part starts
    if (sl == 0) {
        const bool dos = pathurl ? ((s.size() >= 2 && is_alpha(s[0]) && s[1] == ':') || is_unc_path(s))
                                 : (is_drive_path(s) || is_unc_path(s));
        if (dos) {
            if (pathurl && !is_unc_path(s) && !(s.size() >= 3 && is_slash(s[2]))) {
                // A drive-relative path ("C:x/../y") is not simplified.
                U16 p = s;
                for (auto& c : p) if (c == '/') c = '\\';
                return {kSOk, u("file://") + p};
            }
            U16 url = dos_path_to_url(s, 0, true);
            escape_percent = false;
            Result r = canonical(url, (flags & ~(URL_FILE_USE_PATHURL | URL_WININET_COMPATIBILITY | URL_ESCAPE_PERCENT)));
            if (pathurl) r.text = file_url_to_pathurl(r.text, true);
            return r;
        }
        shape = Shape::Relative;
        const usize ex = extra_start(s, 0);
        U16 path = s.substr(0, ex);
        if (simplify_paths) path = simplify(path, 0, force, force);
        out = path + s.substr(ex);
    } else {
        U16 rest = s.substr(sl + 1);
        out = scheme + u(":");
        region = out.size();
        if (id == kFile) {
            shape = Shape::File;
            for (auto& c : rest) if (c == '\\') c = '/';
            usize slashes = 0;
            while (slashes < rest.size() && rest[slashes] == '/') ++slashes;
            U16 body = rest.substr(slashes);
            const bool drive = body.size() >= 2 && is_alpha(body[0]) && (body[1] == ':' || body[1] == '|');
            const usize q = body.find('?');
            U16 path = body.substr(0, q);
            const U16 query = q == U16::npos ? U16() : body.substr(q);
            if (drive) {
                // Under URL_FILE_USE_PATHURL / URL_WININET_COMPATIBILITY a
                // ".." above the drive is kept ("file://C:\\..\\x").
                // The path forms (URL_FILE_USE_PATHURL / WININET) simplify
                // like a DOS path: a leading "." goes, a ".." above the drive
                // stays ("file://C:\\..\\x").
                if (simplify_paths) path = simplify(u("/") + path, 3, pathurl && !force, force || pathurl).substr(1);
                out += u("///") + path + query;
            } else if (slashes == 0) {
                const usize ex0 = body.find('?');
                U16 p0 = body.substr(0, ex0);
                if (simplify_paths) p0 = simplify(p0, 0, false, true);
                out += p0 + (ex0 == U16::npos ? U16() : body.substr(ex0));
            } else if (slashes == 1 || slashes == 3) {
                if (slashes == 3 && body.empty()) { out += u("///"); }
                else {
                    if (simplify_paths) path = simplify(u("/") + path, 0, false, force).substr(1);
                    out += u("///") + path + query;
                }
            } else {
                // file://server/share/... (also file:////server and file:///\\server)
                const usize e = path.find('/');
                const U16 host = path.substr(0, e);
                U16 p = e == U16::npos ? U16(u"/") : path.substr(e);
                if (simplify_paths) p = simplify(p, 0, pathurl && !force, force || pathurl);
                out += u("//") + host + p + query;
            }
        } else if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/' && (flags & URL_PLUGGABLE_PROTOCOL) && !known) {
            // A pluggable protocol's URL is left as written.
            shape = Shape::Hierarchical;
            out += rest;
        } else if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
            shape = Shape::Hierarchical;
            const bool backslash = id == kHttp || id == kHttps;
            usize e = 2;
            while (e < rest.size() && rest[e] != '/' && !(backslash && rest[e] == '\\') && rest[e] != '?' &&
                   rest[e] != '#')
                ++e;
            U16 auth = rest.substr(2, e - 2);
            U16 tail = rest.substr(e);
            if (known) auth = lower_ascii(auth);
            const auto dp = default_port(id);
            if (!dp.empty()) {
                const usize at = auth.find('@');
                const usize colon = auth.find(':', at == U16::npos ? 0 : at + 1);
                if (colon != U16::npos && auth.substr(colon + 1) == u(dp)) auth.resize(colon);
            }
            const usize ex = extra_start(tail, 0);
            U16 path = tail.substr(0, ex);
            if (backslash) for (auto& c : path) if (c == '\\') c = '/';
            if (path.empty() || path[0] != '/') path.insert(path.begin(), u'/');
            if (simplify_paths) path = simplify(path, 0, false, force);
            out += u("//") + auth + path + tail.substr(ex);
        } else if (!known) {
            shape = Shape::Unknown;
            out += rest;
        } else if (opaque_id(id)) {
            shape = Shape::Opaque;
            out += rest;
        } else {
            // A known non-opaque scheme without "//" (news:, local:, mk:,
            // "http:host/x"): its body is simplified like a path.
            shape = Shape::Plain;
            const usize ex = extra_start(rest, 0);
            U16 path = rest.substr(0, ex);
            if (simplify_paths) path = simplify(path, 0, false, true);
            out += path + rest.substr(ex);
        }
    }

    const bool unescaped = flags & URL_UNESCAPE;
    if (flags & (URL_ESCAPE_UNSAFE | URL_ESCAPE_SPACES_ONLY)) {
        if ((flags & URL_ESCAPE_SPACES_ONLY) && out.empty()) return {kSFalse, {}};
        if (unescaped) {
            // Learn: with URL_UNESCAPE and an escape flag the URL is first
            // unescaped and then escaped -- by UrlEscape (measured: the
            // result is UrlEscape's, e.g. "http://host/.." gains its '/').
            U16 plain = decode_canon(out);
            const usize nul = plain.find(u'\0');
            if (nul != U16::npos) plain.resize(nul);
            u32 eflags = flags & (URL_ESCAPE_SPACES_ONLY | URL_DONT_ESCAPE_EXTRA_INFO | URL_ESCAPE_PERCENT);
            out = escape(plain, eflags).text;
        } else {
            out = escape_step(out, region, shape, flags, false, escape_percent);
        }
    } else if (unescaped) {
        out = decode_canon(out);
    }
    if (pathurl && shape == Shape::File) out = file_url_to_pathurl(out, (flags & URL_FILE_USE_PATHURL) != 0, force);
    return {kSOk, out};
}

} // namespace

// ---------------------------------------------------------------------------

std::optional<Parsed> parse_url(const U16& url) {
    const usize sl = scheme_length(url);
    if (sl == 0) return std::nullopt;
    Parsed p;
    p.protocol_len = sl;
    p.scheme = scheme_id(url.substr(0, sl));
    usize start = sl + 1;
    if (p.scheme == kFile && url.size() >= start + 2 && url[start] == '/' && url[start + 1] == '/') {
        start += 2;
        if (start < url.size() && url[start] == '/') ++start;
    }
    p.suffix_start = start;
    p.suffix_len = url.size() - start;
    return p;
}

bool path_is_url(const U16& url) { return scheme_length(url) != 0; }

bool is_opaque(const U16& url) {
    const usize sl = scheme_length(url);
    return sl != 0 && opaque_id(scheme_id(url.substr(0, sl)));
}

u32 is_no_history(const U16& url) {
    const usize sl = scheme_length(url);
    return sl != 0 && no_history_id(scheme_id(url.substr(0, sl))) ? 4u : 0u;
}

u32 url_is(const U16& url, u32 which) {
    const usize sl = scheme_length(url);
    switch (which) {
        case 0: return sl != 0;                    // URLIS_URL
        case 1: return is_opaque(url);             // URLIS_OPAQUE
        case 2: return is_no_history(url);         // URLIS_NOHISTORY
        case 3: return sl != 0 && scheme_id(url.substr(0, sl)) == kFile;  // URLIS_FILEURL
        case 4: return 0;                          // URLIS_APPLIABLE
        case 5: {                                  // URLIS_DIRECTORY
            if (sl == 0) return 0;
            const U16 rest = url.substr(sl + 1);
            const U16 path = rest.substr(0, extra_start(rest, 0));
            if (path.empty()) return 1;
            // Only a rooted path can name a directory ("news:x/" is not one).
            if (!is_slash(path[0])) return 0;
            if (is_slash(path.back())) return 1;
            auto ends = [&](std::string_view t) {
                return path.size() >= t.size() && path.compare(path.size() - t.size(), t.size(), u(t)) == 0;
            };
            if (ends("/..") || ends("/.")) return 1;
            if (path.size() >= 2 && path[0] == '/' && path[1] == '/' &&
                path.find_first_of(u("/\\"), 2) == U16::npos)
                return 1;
            return 0;
        }
        case 6: {                                  // URLIS_HASQUERY
            // A non-empty query (measured: "http://h/x?" has none).
            if (sl == 0 || is_opaque(url)) return 0;
            const usize q = url.find('?');
            return q != U16::npos && q + 1 < url.size() && url[q + 1] != '#';
        }
        default: return 0;
    }
}

std::optional<usize> get_location(const U16& url) {
    const usize sl = scheme_length(url);
    if (sl == 0) return std::nullopt;
    const u32 id = scheme_id(url.substr(0, sl));
    if (id == kFile || opaque_id(id)) return std::nullopt;
    const usize h = url.find('#');
    if (h == U16::npos) return std::nullopt;
    return h;
}

std::optional<U16> unescape(const U16& url, u32 flags) {
    if (has_escaped_nul(url)) return std::nullopt;
    return decode(url, (flags & URL_ESCAPE_AS_UTF8) != 0, (flags & URL_DONT_ESCAPE_EXTRA_INFO) != 0);
}

std::optional<std::string> unescape_ansi(const std::string& url, u32 flags) {
    // UrlUnescapeA decodes bytes in place of characters (measured: %80..%9F
    // come back as those bytes), rejects URL_UNESCAPE_AS_UTF8 with
    // E_INVALIDARG, and an escaped NUL simply ends the string.
    if (flags & URL_ESCAPE_AS_UTF8) return std::nullopt;
    const U16 wide(url.begin(), url.end());
    U16 bytes;
    for (const unsigned char c : url) bytes.push_back(c);
    const U16 r = decode(bytes, false, (flags & URL_DONT_ESCAPE_EXTRA_INFO) != 0);
    std::string out;
    for (const char16_t c : r) {
        if (c == 0) break;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

Result get_part(const U16& url, u32 part, u32 flags) {
    if (part == 0) return {kEInvalidArg, {}};
    if (part > 6) return {kEUnexpected, {}};
    const usize sl = scheme_length(url);
    const U16 scheme = sl ? lower_ascii(url.substr(0, sl)) : U16();
    const u32 id = sl ? scheme_id(scheme) : kUnknown;
    if (part == 1) return {kSOk, scheme};
    if (part == 6) {
        if (sl && opaque_id(id)) return {kSOk, {}};
        const usize q = url.find('?');
        if (q == U16::npos) return {kSOk, {}};
        usize e = url.find('#', q + 1);
        if (e == U16::npos) e = url.size();
        return {kSOk, url.substr(q + 1, e - q - 1)};
    }
    if (sl == 0) return {kEFail, {}};
    const U16 rest = url.substr(sl + 1);
    const bool keep = flags & URL_PARTFLAG_KEEPSCHEME;
    if (id == kFile) {
        if (part != 2) return {kEFail, {}};
        U16 p = rest;
        for (auto& c : p) if (c == '\\') c = '/';
        usize slashes = 0;
        while (slashes < p.size() && p[slashes] == '/') ++slashes;
        if (slashes == 2 || slashes >= 4) {
            U16 body = p.substr(slashes);
            const usize e = body.find('/');
            U16 host = body.substr(0, e);
            if (host.size() >= 2 && is_alpha(host[0]) && (host[1] == ':' || host[1] == '|')) return {kSOk, {}};
            if (iequals(host, "localhost")) return {kSOk, {}};
            return {kSOk, host};
        }
        if (slashes == 3 && p.size() > 4 && p[3] == '/') {
            // file:///\\server -> after normalisation "////server".
        }
        return {kSOk, {}};
    }
    // "http:\\\\h\\x": http and https accept backslashes for the slashes.
    const bool backslash = id == kHttp || id == kHttps;
    auto sep = [&](char16_t c) { return c == '/' || (backslash && c == '\\'); };
    if (!server_id(id) || rest.size() < 2 || !sep(rest[0]) || !sep(rest[1])) return {kEFail, {}};
    usize e = 2;
    while (e < rest.size() && !sep(rest[e]) && rest[e] != '?' && rest[e] != '#') ++e;
    const U16 auth = rest.substr(2, e - 2);
    U16 userinfo, hostport;
    const usize at = auth.find('@');
    bool has_user = false;
    if (at != U16::npos) {
        has_user = true;
        userinfo = auth.substr(0, at);
        hostport = auth.substr(at + 1);
    } else {
        hostport = auth;
    }
    // The port separator is the first ':' after a bracketed IPv6 literal.
    usize port_colon = U16::npos;
    {
        usize from = 0;
        if (!hostport.empty() && hostport[0] == '[') {
            const usize close = hostport.find(']');
            from = close == U16::npos ? hostport.size() : close + 1;
        }
        port_colon = hostport.find(':', from);
    }
    std::optional<U16> value;
    switch (part) {
        case 2:
            value = hostport.substr(0, port_colon);
            break;
        case 3:
            if (has_user) value = userinfo.substr(0, userinfo.find(':'));
            break;
        case 4:
            if (has_user) {
                const usize c = userinfo.find(':');
                if (c != U16::npos) value = userinfo.substr(c + 1);
            }
            break;
        case 5:
            if (port_colon != U16::npos) value = hostport.substr(port_colon + 1);
            break;
        default: break;
    }
    if (!value) return {kEInvalidArg, {}};
    if (keep) return {kSOk, scheme + u(":") + *value};
    return {kSOk, *value};
}

Result canonicalize(const U16& url, u32 flags) { return canonical(url, flags); }

namespace {

/// UrlEscape's per-character rule inside its escapable region.
struct EscapeRule {
    bool percent = false;    // URL_ESCAPE_PERCENT (implied by ASCII_URI_COMPONENT)
    bool utf8 = false;       // URL_ESCAPE_AS_UTF8
    bool ascii_uri = false;  // URL_ESCAPE_ASCII_URI_COMPONENT
    bool segment = false;    // '/', '?', '#' are data
};

void escape_into(U16& out, const U16& s, usize begin, usize end, const EscapeRule& rule, bool hash_is_data = false) {
    for (usize i = begin; i < end; ++i) {
        const char16_t c = s[i];
        if (c >= 0x80 && rule.utf8) {
            i += append_escaped_char(out, s, i, true) - 1;
            continue;
        }
        bool esc;
        if (c == '%') esc = rule.percent || rule.ascii_uri;
        else if (rule.ascii_uri && c < 0x80) esc = !is_unreserved(c) && !(c == '/' && !rule.segment);
        else if (c == '/' || c == '?') esc = rule.segment;
        else if (c == '#') esc = rule.segment || hash_is_data;
        else esc = is_unsafe(c) || c == '\\';
        if (esc && c > 0xFF) esc = false;  // UTF-16 above Latin-1 is kept unless AS_UTF8
        if (esc) append_escaped(out, static_cast<u8>(c));
        else out.push_back(c);
    }
}

/// Emits a hierarchical tail after its path: the query ('?' to the end or
/// to a later '#') before the fragment ('#' up to a later '?') (measured:
/// "a b#c d?e f" -> "a%20b?e f#c d").
void append_extra(U16& out, const U16& s, usize from) {
    if (from >= s.size()) return;
    if (s[from] == '#') {
        const usize q = s.find('?', from);
        if (q != U16::npos) {
            out.append(s, q, U16::npos);
            out.append(s, from, q - from);
            return;
        }
    }
    out.append(s, from, U16::npos);
}

/// A path whose last segment is "." or ".." gains a '/' in UrlEscape.
void close_dot_segment(U16& path) {
    const usize slash = path.rfind('/');
    const U16 last = slash == U16::npos ? path : path.substr(slash + 1);
    if ((last == u(".") || last == u("..")) && slash != U16::npos) path.push_back('/');
}

/// UrlEscape without SEGMENT_ONLY / SPACES_ONLY: the URL is laid out like
/// UrlCanonicalize's (scheme lowercased, file: URLs normalised, "http" root
/// '/' added) without simplification or host changes, then its path (or the
/// whole body of a URL without "//") is escaped.
U16 escape_url(const U16& url, const EscapeRule& rule) {
    const usize sl = scheme_length(url);
    U16 out;
    if (sl == 0) {
        U16 s = url;
        if (!s.empty() && s[0] == '\\') s[0] = '/';
        const usize ex = extra_start(s, 0);
        escape_into(out, s, 0, ex, rule);
        append_extra(out, s, ex);
        return out;
    }
    const U16 scheme = lower_ascii(url.substr(0, sl));
    const u32 id = scheme_id(scheme);
    U16 rest = url.substr(sl + 1);
    out = scheme + u(":");
    if (id == kFile) {
        for (auto& c : rest) if (c == '\\') c = '/';
        usize slashes = 0;
        while (slashes < rest.size() && rest[slashes] == '/') ++slashes;
        U16 body = rest.substr(slashes);
        const bool drive = body.size() >= 2 && is_alpha(body[0]) && (body[1] == ':' || body[1] == '|');
        U16 path;
        if (drive || slashes == 1 || slashes == 3) {
            out += u("///");
            path = body;
        } else if (slashes == 0) {
            path = body;
        } else {
            const usize e = body.find('/');
            const U16 host = body.substr(0, e);
            U16 p = e == U16::npos ? U16(u"/") : body.substr(e);
            if (iequals(host, "localhost")) {
                out += u("//");
                path = p;
            } else {
                out += u("//") + host;
                path = p;
            }
        }
        const usize q = path.find('?');
        U16 head = path.substr(0, q);
        close_dot_segment(head);
        escape_into(out, head, 0, head.size(), rule, true);
        if (q != U16::npos) out.append(path, q, U16::npos);
        return out;
    }
    if (rest.size() >= 2 && is_slash(rest[0]) && is_slash(rest[1]) && !(rest[0] == '\\' && id != kHttp && id != kHttps)) {
        const bool backslash = id == kHttp || id == kHttps || id == kFtp;
        if (backslash) {
            const usize ex = extra_start(rest, 0);
            for (usize i = 0; i < ex; ++i) if (rest[i] == '\\') rest[i] = '/';
        }
        usize e = 2;
        while (e < rest.size() && rest[e] != '/' && rest[e] != '?' && rest[e] != '#') ++e;
        out += rest.substr(0, e);
        U16 tail = rest.substr(e);
        const usize ex = extra_start(tail, 0);
        U16 path = tail.substr(0, ex);
        if (path.empty() || path[0] != '/') path.insert(path.begin(), u'/');
        close_dot_segment(path);
        escape_into(out, path, 0, path.size(), rule);
        append_extra(out, tail, ex);
        return out;
    }
    if (id == kMk) {
        // mk: (the moniker scheme) is left as it is (measured).
        return out + rest;
    }
    if (!rest.empty() && rest[0] == '/') {
        // "http:/host/x": a rooted path without an authority.
        const usize ex = extra_start(rest, 0);
        escape_into(out, rest, 0, ex, rule);
        append_extra(out, rest, ex);
        return out;
    }
    EscapeRule seg = rule;
    seg.segment = true;
    escape_into(out, rest, 0, rest.size(), seg);
    return out;
}

} // namespace

Result escape(const U16& url, u32 flags) {
    U16 out;
    EscapeRule rule;
    rule.percent = flags & URL_ESCAPE_PERCENT;
    rule.utf8 = flags & URL_ESCAPE_AS_UTF8;
    rule.ascii_uri = flags & URL_ESCAPE_ASCII_URI_COMPONENT;
    if (flags & URL_ESCAPE_SEGMENT_ONLY) {
        rule.segment = true;
        escape_into(out, url, 0, url.size(), rule);
        return {kSOk, out};
    }
    if (flags & URL_ESCAPE_SPACES_ONLY) {
        const bool stop = flags & URL_DONT_ESCAPE_EXTRA_INFO;
        bool extra = false;
        for (const char16_t c : url) {
            if (c == '?' || c == '#') extra = true;
            if (c == ' ' && !(stop && extra)) append_escaped(out, ' ');
            else out.push_back(c);
        }
        return {kSOk, out};
    }
    return {kSOk, escape_url(url, rule)};
}

namespace {
/// URL_DONT_SIMPLIFY in UrlCombine still closes a final "." or ".."
/// segment with '/' (measured: "a/.." -> "a/../", "." -> "./").
U16 close_dot_path(const U16& path) {
    const usize slash = path.rfind('/');
    const U16 last = slash == U16::npos ? path : path.substr(slash + 1);
    return (last == u(".") || last == u("..")) ? path + u("/") : path;
}
U16 close_dot_url(const U16& url, usize path_start) {
    const usize ex = extra_start(url, path_start);
    return url.substr(0, path_start) + close_dot_path(url.substr(path_start, ex - path_start)) + url.substr(ex);
}
} // namespace

Result combine(const U16& base, const U16& relative, u32 flags) {
    U16 rel = strip_whitespace(relative);
    const usize rsl = scheme_length(rel);
    const usize bsl = scheme_length(base);
    // A relative URL naming the base's scheme without "//" is relative to the
    // base as written ("http:g" against "http://host" -> "http://hostg").
    if (rsl != 0) {
        const U16 rest = rel.substr(rsl + 1);
        const bool same = bsl != 0 && lower_ascii(rel.substr(0, rsl)) == lower_ascii(base.substr(0, bsl)) &&
                          !(rest.size() >= 2 && rest[0] == '/' && rest[1] == '/');
        const u32 bid = bsl ? scheme_id(base.substr(0, bsl)) : kUnknown;
        if (!same || opaque_id(bid) || bid == kUnknown || bid == kFile) return canonical(rel, flags);
        U16 raw = strip_whitespace(base);
        const usize ex = extra_start(raw, bsl + 1);
        usize last = raw.rfind('/', ex == 0 ? 0 : ex - 1);
        const usize auth_end = raw.size() > bsl + 3 && raw[bsl + 1] == '/' && raw[bsl + 2] == '/'
                                   ? raw.find('/', bsl + 3) : bsl;
        U16 joined;
        if (auth_end == U16::npos || last == U16::npos || last < auth_end) joined = raw.substr(0, ex) + rest;
        else joined = raw.substr(0, last + 1) + rest;
        if (!(raw.size() > bsl + 3 && raw[bsl + 1] == '/' && raw[bsl + 2] == '/' && raw.find('/', bsl + 3) == U16::npos))
            return canonical(joined, flags, true);
        return {kSOk, joined};
    }
    if (is_drive_path(rel) || is_unc_path(rel)) return canonical(rel, flags);

    const u32 base_flags = flags & ~(URL_ESCAPE_UNSAFE | URL_ESCAPE_SPACES_ONLY | URL_ESCAPE_PERCENT |
                                     URL_UNESCAPE | URL_FILE_USE_PATHURL | URL_WININET_COMPATIBILITY);
    const U16 b = canonical(base, base_flags).text;
    const usize sl = scheme_length(b);
    const u32 id = sl ? scheme_id(b.substr(0, sl)) : kUnknown;

    if (sl != 0 && opaque_id(id)) return canonical(b + rel, flags);
    // A leading backslash is the root (measured for every non-opaque base).
    if (!rel.empty() && rel[0] == '\\') rel[0] = '/';
    const bool hier = sl != 0 && b.size() >= sl + 3 && b[sl + 1] == '/' && b[sl + 2] == '/';
    const bool pluggable_unknown = (flags & URL_PLUGGABLE_PROTOCOL) && id == kUnknown;
    if (sl != 0 && !hier && id != kFile && id == kUnknown) {
        // An unknown or authority-less scheme: the relative URL replaces the
        // body unless it is only a query or fragment. Under
        // URL_PLUGGABLE_PROTOCOL it replaces it verbatim (even when empty).
        if (!rel.empty() && (rel[0] == '?' || rel[0] == '#'))
            return canonical(b.substr(0, extra_start(b, sl + 1)) + rel, flags);
        if (pluggable_unknown) return canonical(b.substr(0, sl + 1) + relative, flags);
        if (rel.empty()) return canonical(b, flags);
        const usize ex = extra_start(rel, 0);
        U16 path = rel.substr(0, ex);
        path = (flags & URL_DONT_SIMPLIFY) ? close_dot_path(path) : simplify(path, 0, true, true);
        return canonical(b.substr(0, sl + 1) + path + rel.substr(ex), flags);
    }
    if (sl == 0) {
        // No base scheme: a rooted base is merged with; otherwise only a query
        // or fragment keeps the base (measured: "www.x.com/a/b" + "g" -> "g").
        // URL_PLUGGABLE_PROTOCOL turns the relative URL's backslashes into
        // slashes and appends a query verbatim.
        if (flags & URL_PLUGGABLE_PROTOCOL) for (auto& c : rel) if (c == '\\') c = '/';
        if ((flags & URL_PLUGGABLE_PROTOCOL) && !rel.empty() && rel[0] == '?') return canonical(b + rel, flags, true);
        const bool rooted = !b.empty() && b[0] == '/';
        if (!rooted && !(!rel.empty() && (rel[0] == '?' || rel[0] == '#'))) {
            U16 r = rel;
            if (flags & URL_DONT_SIMPLIFY) r = close_dot_url(r, 0);
            return canonical(r, flags, true);
        }
        const usize ex = extra_start(b, 0);
        U16 joined;
        if (rel.empty()) {
            const usize last = b.rfind('/', ex == 0 ? 0 : ex - 1);
            joined = last == U16::npos || ex == 0 ? U16() : b.substr(0, last + 1);
        } else if (rel[0] == '?' || rel[0] == '#' || rel[0] == '/') {
            joined = rel[0] == '/' ? rel : b.substr(0, rel[0] == '#' ? b.find('#') : ex) + rel;
        } else {
            const usize last = ex == 0 ? U16::npos : b.rfind('/', ex - 1);
            joined = (last == U16::npos ? U16() : b.substr(0, last + 1)) + rel;
        }
        if (flags & URL_DONT_SIMPLIFY) joined = close_dot_url(joined, 0);
        return canonical(joined, flags, true);
    }
    // Hierarchical: scheme://authority (or file:///X:) + path. A known scheme
    // without "//" (news:, mk:) merges with its body as the path.
    usize path_start;
    if (!hier && id != kFile) {
        path_start = sl + 1;
    } else if (id == kFile && b.size() >= sl + 4 && b[sl + 3] == '/') {
        // file:///C:/...: the drive is part of the root.
        path_start = sl + 4;
        if (b.size() >= path_start + 2 && is_alpha(b[path_start]) && (b[path_start + 1] == ':' || b[path_start + 1] == '|'))
            path_start += 2;
    } else {
        path_start = b.find('/', sl + 3);
        if (path_start == U16::npos) path_start = b.size();
    }
    const usize ex = id == kFile ? std::min(b.find('?', path_start), b.size()) : extra_start(b, path_start);
    const U16 prefix = b.substr(0, path_start);
    U16 joined;
    if (rel.empty()) {
        const usize last = b.rfind('/', ex == 0 ? 0 : ex - 1);
        joined = (last == U16::npos || last < path_start) ? prefix + u("/") : b.substr(0, last + 1);
    } else if (rel[0] == '#') {
        // A fragment on a file URL is dropped by the path forms.
        if (id == kFile && (flags & URL_FILE_USE_PATHURL)) return canonical(b, flags, true);
        const usize h = id == kFile ? U16::npos : b.find('#', path_start);
        joined = b.substr(0, h) + rel;
    } else if (rel[0] == '?') {
        joined = b.substr(0, ex) + rel;
    } else if (rel.size() >= 2 && rel[0] == '/' && rel[1] == '/') {
        joined = b.substr(0, sl + 1) + rel;
    } else if (rel[0] == '/') {
        // URL_PLUGGABLE_PROTOCOL keeps the base's root slash ("//g").
        joined = prefix + (pluggable_unknown ? u("/") : U16()) + rel;
    } else {
        const usize last = b.rfind('/', ex == 0 ? 0 : ex - 1);
        joined = ((last == U16::npos || last < path_start) ? prefix + u("/") : b.substr(0, last + 1)) + rel;
    }
    if ((flags & URL_DONT_SIMPLIFY) && id == kUnknown && !(rel.size() >= 2 && rel[0] == '/' && rel[1] == '/'))
        joined = close_dot_url(joined, path_start);
    return canonical(joined, flags, true);
}

std::pair<U16, U16> compare_operands(const U16& a, const U16& b, bool ignore_slash) {
    U16 x = decode(a, false, false), y = decode(b, false, false);
    if (ignore_slash) {
        if (!x.empty() && x.back() == '/') x.pop_back();
        if (!y.empty() && y.back() == '/') y.pop_back();
    }
    return {x, y};
}

Result create_from_path(const U16& path, u32 flags) {
    if (scheme_length(path) != 0) return {kSFalse, path};
    if (flags & URL_FILE_USE_PATHURL) {
        // "file://" + the path with backslashes, unescaped (measured:
        // "C:\\%41" -> "file://C:\\A"). A "\\\\?\\" path keeps the prefix
        // and the rest with forward slashes ("file://\\\\?C:/x").
        if (path.size() >= 4 && path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\') {
            U16 r = path.substr(4);
            for (auto& c : r) if (c == '\\') c = '/';
            return {kSOk, u("file://\\\\?") + r};
        }
        U16 p = path;
        for (auto& c : p) if (c == '/') c = '\\';
        return {kSOk, u("file://") + decode(p, false, false)};
    }
    if (flags & URL_WININET_COMPATIBILITY) {
        // "file://" + the escaped path with backslashes; a relative path
        // without a root gets "file:" only.
        U16 p = path;
        for (auto& c : p) if (c == '/') c = '\\';
        U16 out = (is_drive_path(p) || (!p.empty() && p[0] == '\\')) ? u("file://") : u("file:");
        EscapeRule rule;
        rule.utf8 = flags & URL_ESCAPE_AS_UTF8;
        for (usize i = 0; i < p.size(); ++i) {
            const char16_t c = p[i];
            if (c == '\\') { out.push_back(c); continue; }
            if (c >= 0x80 && rule.utf8) { i += append_escaped_char(out, p, i, true) - 1; continue; }
            if ((is_unsafe(c) && c <= 0xFF) || c == '#' || c == '%' || c == '?') append_escaped(out, static_cast<u8>(c));
            else out.push_back(c);
        }
        // A bare server gets its root ("file://\\\\server\\").
        if (is_unc_path(p) && p.find('\\', 2) == U16::npos) out.push_back('\\');
        return {kSOk, out};
    }
    U16 r = dos_path_to_url(path, flags, true);
    if (is_unc_path(path) && path.find('\\', 2) == U16::npos && path.find('/', 2) == U16::npos) r.push_back('/');
    return {kSOk, r};
}

Result path_from_url(const U16& url, u32 flags) {
    const usize sl = scheme_length(url);
    if (sl == 0 || scheme_id(url.substr(0, sl)) != kFile) return {kEInvalidArg, {}};
    U16 p = url.substr(sl + 1);
    const usize q = p.find('?');
    if (q != U16::npos) p.resize(q);
    for (auto& c : p) if (c == '/') c = '\\';
    usize slashes = 0;
    while (slashes < p.size() && p[slashes] == '\\') ++slashes;
    U16 body = p.substr(slashes);
    U16 out;
    const bool drive = body.size() >= 2 && is_alpha(body[0]) && (body[1] == ':' || body[1] == '|');
    if (drive) {
        out = body;
        if (out[1] == '|') out[1] = ':';
    } else if (slashes >= 2) {
        U16 host = body.substr(0, body.find('\\'));
        if (iequals(host, "localhost")) {
            U16 tail = body.size() > host.size() ? body.substr(host.size() + 1) : U16();
            out = tail;
            if (out.size() >= 2 && out[1] == '|') out[1] = ':';
        } else if (!body.empty()) {
            out = u("\\\\") + body;
        }
    } else {
        out = body;
    }
    if (!(flags & URL_DONT_UNESCAPE)) out = decode(out, (flags & URL_ESCAPE_AS_UTF8) != 0, false);
    return {kSOk, out};
}

Result apply_scheme(const U16& url, u32 flags, const ApplySchemeRegistry& registry) {
    const bool force = flags & URL_APPLY_FORCEAPPLY;
    if (!force && scheme_length(url) != 0) return {kSFalse, {}};
    if ((flags & URL_APPLY_GUESSSCHEME) && registry.prefix) {
        // A Prefixes value name that starts the string, with something after
        // it (measured: "wwwfoo.com" and "ftp\\x" match, "www" does not).
        if (auto pre = registry.prefix(url)) return {kSOk, *pre + url};
    }
    if (flags & URL_APPLY_GUESSFILE) {
        // A drive ("X:") or UNC path becomes a file URL ("c|\\x" does not).
        if ((url.size() >= 2 && is_alpha(url[0]) && url[1] == ':') || is_unc_path(url)) return canonical(url, 0);
    }
    if ((flags & URL_APPLY_DEFAULT) || (flags & ~URL_APPLY_FORCEAPPLY) == 0) {
        if (force && !(flags & URL_APPLY_DEFAULT)) return {kSFalse, {}};
        if (registry.default_prefix) {
            if (auto pre = registry.default_prefix()) return {kSOk, *pre + url};
        }
    }
    return {kSFalse, {}};
}

std::optional<Result> fixup(const U16& url) {
    // The scheme: scheme characters up to ':' or ';' (measured: "http//x" and
    // "http.//x" have none and are left alone).
    usize i = 0;
    while (i < url.size() && is_scheme_char(url[i])) ++i;
    if (i < 2 || i >= url.size() || (url[i] != ':' && url[i] != ';')) return Result{kSFalse, {}};
    const U16 scheme = lower_ascii(url.substr(0, i));
    const u32 id = scheme_id(scheme);
    if (id == kUnknown) {
        // Windows also corrects a misspelled scheme ("htp:" -> "hcp:",
        // "fiel:" -> "file:"); that matching is not modelled.
        return std::nullopt;
    }
    if (id == kHttp || id == kHttps || id == kFtp) {
        // These need "//": the separator run ([:;/\\]+) becomes "://" unless
        // it already holds three or more slashes (measured: "http:x",
        // "http:/x", "http;//x", "http::/x", "http:\\\\x" -> "http://x";
        // "http:///x" stays).
        usize e = i;
        usize slashes = 0;
        while (e < url.size() && (url[e] == ':' || url[e] == ';' || url[e] == '/' || url[e] == '\\')) {
            if (url[e] == '/' || url[e] == '\\') ++slashes;
            ++e;
        }
        if (slashes >= 3) return Result{kSOk, scheme + u(":") + U16(slashes, u'/') + url.substr(e)};
        return Result{kSOk, scheme + u("://") + url.substr(e)};
    }
    // Other known schemes keep their body; ';' is corrected to ':'.
    return Result{kSOk, scheme + u(":") + url.substr(i + 1)};
}

U16 from_ansi(const std::string& s) {
    std::vector<u16> w(s.size() + 1);
    const auto r = codepage::multi_byte_to_wide(codepage::ansi_code_page(), 0, reinterpret_cast<const u8*>(s.data()), s.size(), w.data(), w.size());
    return U16(w.begin(), w.begin() + r.count);
}

std::string to_ansi(const U16& text) {
    // The A forms return C strings: an embedded NUL (URL_UNESCAPE of %00)
    // ends the result.
    const U16 s = text.substr(0, text.find(u'\0'));
    std::vector<u8> a(s.size() * 4 + 1);
    const auto r = codepage::wide_to_multi_byte(codepage::ansi_code_page(), 0, reinterpret_cast<const u16*>(s.data()), s.size(), a.data(),
                                                a.size(), nullptr, false);
    return std::string(a.begin(), a.begin() + r.count);
}

} // namespace vitreo::winapi::shlwapi_url
