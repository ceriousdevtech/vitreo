// SPDX-License-Identifier: LGPL-2.1-only
/// @file httpapi_server.cpp
/// @brief httpapi.dll data plane: host TCP listeners, HTTP/1.x request
///        parsing and routing, HttpReceiveHttpRequest /
///        HttpReceiveRequestEntityBody / HttpSendHttpResponse /
///        HttpSendResponseEntityBody / HttpCancelHttpRequest /
///        HttpWaitForDisconnect(Ex).
///
/// What a client sees on the wire, and the return codes Learn does not
/// state, are the measurements in tests/oracle_data/httpapi_probe*.expected
/// (comments quote the probe's line keys).

#include "httpapi_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

namespace vitreo::winapi {

using namespace httpapi;

namespace httpapi {

const std::array<const char*, HttpHeaderRequestMaximum> kRequestHeaderNames = {
    "Cache-Control", "Connection", "Date", "Keep-Alive", "Pragma", "Trailer",
    "Transfer-Encoding", "Upgrade", "Via", "Warning", "Allow", "Content-Length",
    "Content-Type", "Content-Encoding", "Content-Language", "Content-Location",
    "Content-MD5", "Content-Range", "Expires", "Last-Modified", "Accept",
    "Accept-Charset", "Accept-Encoding", "Accept-Language", "Authorization",
    "Cookie", "Expect", "From", "Host", "If-Match", "If-Modified-Since",
    "If-None-Match", "If-Range", "If-Unmodified-Since", "Max-Forwards",
    "Proxy-Authorization", "Referer", "Range", "TE", "Translate", "User-Agent",
};

const std::array<const char*, HttpHeaderResponseMaximum> kResponseHeaderNames = {
    "Cache-Control", "Connection", "Date", "Keep-Alive", "Pragma", "Trailer",
    "Transfer-Encoding", "Upgrade", "Via", "Warning", "Allow", "Content-Length",
    "Content-Type", "Content-Encoding", "Content-Language", "Content-Location",
    "Content-MD5", "Content-Range", "Expires", "Last-Modified", "Accept-Ranges",
    "Age", "ETag", "Location", "Proxy-Authenticate", "Retry-After", "Server",
    "Set-Cookie", "Vary", "WWW-Authenticate",
};

} // namespace httpapi

namespace {

constexpr usize kMaxHead = 16384;               // http.sys MaxRequestBytes default
constexpr usize kBodyBufferLimit = 1u << 20;    // read-ahead of an unread body
constexpr u32 kDefaultTimeoutSeconds = 120;     // Learn HTTP_TIMEOUT_LIMIT_INFO defaults
constexpr const char* kServerToken = "Microsoft-HTTPAPI/2.0";   // "send basic: ... Server:"

const char* kVerbs[] = {nullptr, nullptr, nullptr, "OPTIONS", "GET", "HEAD", "POST", "PUT",
                        "DELETE", "TRACE", "CONNECT", "TRACK", "MOVE", "COPY", "PROPFIND",
                        "PROPPATCH", "MKCOL", "LOCK", "UNLOCK", "SEARCH"};

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string_view v) {
    usize b = 0, e = v.size();
    while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
    return std::string(v.substr(b, e - b));
}

std::string http_date() {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    gmtime_r(&now, &t);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[t.tm_wday],
                  t.tm_mday, kMonths[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

/// UTF-8 to UTF-16; a byte sequence that is not UTF-8 maps byte-for-byte.
std::u16string widen(std::string_view s) {
    std::u16string out;
    for (usize i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        u32 cp = c;
        usize n = 1;
        if (c >= 0xC2 && c < 0xE0 && i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            n = 2;
        } else if (c >= 0xE0 && c < 0xF0 && i + 2 < s.size() && (s[i + 1] & 0xC0) == 0x80 &&
                   (s[i + 2] & 0xC0) == 0x80) {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xF0 && c < 0xF5 && i + 3 < s.size() && (s[i + 1] & 0xC0) == 0x80 &&
                   (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) |
                 (s[i + 3] & 0x3Fu);
            n = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return out;
}

std::string percent_decode(std::string_view s) {
    std::string out;
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<usize>(n));
    }
    return true;
}

/// An error page the server produces itself (no queue to answer): status
/// line, "Content-Type: text/html; charset=us-ascii", Server, Date,
/// "Connection: close", Content-Length -- the header set and order of the
/// probe's "nohost / unregistered / unbound / queue closed" lines. The body
/// text is Vitreo's own.
void send_server_error(int fd, u32 status, const char* reason, const char* detail) {
    char body[256];
    std::snprintf(body, sizeof body,
                  "<html><head><title>%u %s</title></head><body><h1>%u %s</h1><p>%s</p></body></html>\r\n",
                  status, reason, status, reason, detail);
    char head[512];
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 %u %s\r\nContent-Type: text/html; charset=us-ascii\r\nServer: %s\r\n"
                  "Date: %s\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n",
                  status, reason, kServerToken, http_date().c_str(), std::strlen(body));
    (void)write_all(fd, std::string(head) + body);
}

void reset_socket(int fd) {
    linger l{1, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
}

std::string sockaddr_image(const sockaddr_storage& ss) {
    // SOCKADDR_IN (16 bytes, AF_INET 2) / SOCKADDR_IN6 (28 bytes, AF_INET6 23).
    std::string img;
    if (ss.ss_family == AF_INET6) {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(ss);
        img.assign(28, '\0');
        const u16 fam = 23;
        std::memcpy(&img[0], &fam, 2);
        std::memcpy(&img[2], &a.sin6_port, 2);
        std::memcpy(&img[4], &a.sin6_flowinfo, 4);
        std::memcpy(&img[8], &a.sin6_addr, 16);
        std::memcpy(&img[24], &a.sin6_scope_id, 4);
    } else {
        const auto& a = reinterpret_cast<const sockaddr_in&>(ss);
        img.assign(16, '\0');
        const u16 fam = 2;
        std::memcpy(&img[0], &fam, 2);
        std::memcpy(&img[2], &a.sin_port, 2);
        std::memcpy(&img[4], &a.sin_addr, 4);
    }
    return img;
}

std::string address_literal(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6&>(ss).sin6_addr, buf, sizeof buf);
        return std::string("[") + buf + "]";
    }
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(ss).sin_addr, buf, sizeof buf);
    return buf;
}

u32 timeout_ms(u16 seconds) { return (seconds ? seconds : kDefaultTimeoutSeconds) * 1000u; }

/// Wait for readable data; false on timeout or error.
bool wait_readable(int fd, u32 ms) {
    pollfd p{fd, POLLIN, 0};
    for (;;) {
        const int r = ::poll(&p, 1, static_cast<int>(ms));
        if (r < 0 && errno == EINTR) continue;
        return r > 0;
    }
}

/// recv() into `buf`; 0 on orderly close or error.
ssize_t read_some(int fd, std::string& buf) {
    char tmp[16384];
    for (;;) {
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) buf.append(tmp, static_cast<usize>(n));
        return n < 0 ? 0 : n;
    }
}

struct ParsedHead {
    std::shared_ptr<Request> req;
    std::string host;           // lower-case, no port: routing key
    std::string path;           // lower-case decoded absolute path
    bool keep_alive = true;
    bool expect_continue = false;
};

/// Parse a request head (request line + header fields, without the blank
/// line). Returns false for a malformed request (answered with 400).
bool parse_head(std::string_view head, const Connection& conn, ParsedHead& out) {
    auto req = std::make_shared<Request>();
    const usize eol = head.find("\r\n");
    const std::string_view line = head.substr(0, eol);
    const usize sp1 = line.find(' ');
    const usize sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0) return false;
    req->method = std::string(line.substr(0, sp1));
    req->raw_url = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    const std::string_view version = line.substr(sp2 + 1);
    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || !std::isdigit(version[5]) ||
        version[6] != '.' || !std::isdigit(version[7]) || req->raw_url.empty())
        return false;
    for (char c : req->method)
        if (c <= ' ' || c >= 0x7F || std::strchr("()<>@,;:\\\"/[]?={}", c)) return false;
    req->major = static_cast<u16>(version[5] - '0');
    req->minor = static_cast<u16>(version[7] - '0');
    if (req->major != 1) return false;
    req->verb = HttpVerbUnknown;
    for (u32 v = HttpVerbOPTIONS; v <= HttpVerbSEARCH; ++v)
        if (req->method == kVerbs[v]) req->verb = v;

    // Header fields. Repeated fields combine with ", " ("recv get: ...
    // 25:"a=1, b=2""); values lose surrounding white space ("X-Custom":"padded value").
    usize pos = eol == std::string_view::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        usize end = head.find("\r\n", pos);
        if (end == std::string_view::npos) end = head.size();
        const std::string_view field = head.substr(pos, end - pos);
        pos = end + 2;
        if (field.empty()) continue;
        if (field[0] == ' ' || field[0] == '\t') {   // obs-fold continues the last field
            const std::string more = trim(field);
            if (!req->unknown.empty()) req->unknown.back().second += " " + more;
            continue;
        }
        const usize colon = field.find(':');
        if (colon == std::string_view::npos || colon == 0) return false;
        const std::string_view name = field.substr(0, colon);
        if (name.find_first_of(" \t") != std::string_view::npos) return false;
        const std::string value = trim(field.substr(colon + 1));
        bool known = false;
        for (u32 id = 0; id < HttpHeaderRequestMaximum; ++id) {
            if (!iequals(name, kRequestHeaderNames[id])) continue;
            auto& slot = req->known[id];
            slot = slot ? *slot + ", " + value : value;
            known = true;
            break;
        }
        if (!known) req->unknown.emplace_back(std::string(name), value);
    }

    // Target: origin-form, or absolute-form whose authority replaces Host.
    std::string target = req->raw_url;
    std::string authority = req->known[HttpHeaderHost].value_or("");
    if (target.size() > 7 && iequals(std::string_view(target).substr(0, 7), "http://")) {
        const usize slash = target.find('/', 7);
        authority = target.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
        target = slash == std::string::npos ? "/" : target.substr(slash);
    } else if (target[0] != '/') {
        if (!(target == "*" && req->verb == HttpVerbOPTIONS)) return false;
        target = "/";
    }
    // HTTP/1.1 requires Host ("nohost: HTTP/1.1 400"); HTTP/1.0 without one is
    // addressed to the local IP ("recv http10: rc=1460" for a localhost URL).
    if (!req->known[HttpHeaderHost] && authority.empty()) {
        if (req->minor >= 1) return false;
        authority = address_literal(conn.local) + ":" + std::to_string(conn.port);
    }
    std::string host_only = authority;
    if (!host_only.empty() && host_only[0] == '[') {
        const usize close = host_only.find(']');
        host_only = close == std::string::npos ? host_only : host_only.substr(0, close + 1);
    } else if (const usize c = host_only.rfind(':'); c != std::string::npos) {
        host_only = host_only.substr(0, c);
    }
    if (authority.find(':', authority.rfind(']') == std::string::npos ? 0 : authority.rfind(']')) ==
        std::string::npos)
        authority += ":" + std::to_string(conn.port);

    const usize q = target.find('?');
    const std::string raw_path = target.substr(0, q);
    const std::string query = q == std::string::npos ? std::string() : target.substr(q);
    const std::string decoded = percent_decode(raw_path);

    // Cooked URL: "http://" host[:port] decoded-path raw-query, the path
    // decoded and the query not ("full="http://localhost:48213/vprobe/a b/c?x=1&y=%41"").
    const std::u16string w_host = widen(authority), w_abs = widen(decoded), w_query = widen(query);
    req->full_url = u"http://" + w_host + w_abs + w_query;
    req->host_at = 7;
    req->host_len = w_host.size();
    req->abs_at = 7 + w_host.size();
    req->abs_len = w_abs.size();
    req->has_query = q != std::string::npos;
    req->query_at = req->abs_at + w_abs.size();
    req->query_len = w_query.size();

    // Entity body framing.
    if (auto te = req->known[HttpHeaderTransferEncoding]) {
        const std::string t = lower(*te);
        if (t.size() < 7 || t.compare(t.size() - 7, 7, "chunked") != 0) return false;
        req->chunked = true;
        req->body_complete = false;
    } else if (auto cl = req->known[HttpHeaderContentLength]) {
        if (cl->empty() || !std::all_of(cl->begin(), cl->end(), [](char c) { return std::isdigit(c); }))
            return false;
        req->content_length = std::stoull(*cl);
        req->body_complete = req->content_length == 0;
    }
    const std::string connection = lower(req->known[HttpHeaderConnection].value_or(""));
    out.keep_alive = req->minor >= 1 ? connection.find("close") == std::string::npos : false;
    out.expect_continue = iequals(req->known[HttpHeaderExpect].value_or(""), "100-continue");
    out.host = lower(host_only);
    out.path = lower(decoded);
    out.req = std::move(req);
    return true;
}

/// Move entity-body bytes from `in` into the request (chunked or
/// Content-Length framing). Returns false for a malformed chunked body.
bool absorb_body(Request& req, std::string& in, u64& chunk_left, bool& in_trailer) {
    if (!req.chunked) {
        const u64 take = std::min<u64>(in.size(), req.content_length - req.body_wire_bytes);
        req.body.append(in, 0, take);
        in.erase(0, take);
        req.body_wire_bytes += take;
        if (req.body_wire_bytes == req.content_length) req.body_complete = true;
        return true;
    }
    for (;;) {
        if (in_trailer) {
            const usize eol = in.find("\r\n");
            if (eol == std::string::npos) return true;
            in.erase(0, eol + 2);
            req.body_wire_bytes += eol + 2;
            if (eol == 0) { req.body_complete = true; return true; }
            continue;
        }
        if (chunk_left == 0) {
            const usize eol = in.find("\r\n");
            if (eol == std::string::npos) return in.size() < 64;
            const std::string size_line = in.substr(0, in.find_first_of(";\r"));
            if (size_line.empty() || !std::all_of(size_line.begin(), size_line.end(),
                                                  [](char c) { return std::isxdigit(c); }))
                return false;
            chunk_left = std::stoull(size_line, nullptr, 16);
            in.erase(0, eol + 2);
            req.body_wire_bytes += eol + 2;
            if (chunk_left == 0) { in_trailer = true; continue; }
            chunk_left += 2;   // the CRLF after the data
        }
        if (in.empty()) return true;
        const u64 data_left = chunk_left > 2 ? chunk_left - 2 : 0;
        const u64 take = std::min<u64>(in.size(), data_left);
        req.body.append(in, 0, take);
        in.erase(0, take);
        req.body_wire_bytes += take;
        chunk_left -= take;
        if (chunk_left <= 2) {
            if (in.size() < chunk_left) return true;
            if (in.compare(0, chunk_left, std::string("\r\n").substr(2 - chunk_left)) != 0) return false;
            in.erase(0, chunk_left);
            req.body_wire_bytes += chunk_left;
            chunk_left = 0;
        }
    }
}

} // namespace

// =============================================================================
// Listeners
// =============================================================================

httpapi::Endpoint::~Endpoint() {
    for (int fd : wake)
        if (fd >= 0) ::close(fd);
}

u32 HttpApiManager::State::ensure_endpoint_locked(u16 port, bool wildcard) {
    if (endpoints.count({port, true})) return NO_ERROR_;
    if (!wildcard && endpoints.count({port, false})) return NO_ERROR_;
    auto ep = std::make_shared<Endpoint>();
    ep->port = port;
    ep->wildcard = wildcard;
    auto listen_on = [&](int family) -> int {
        const int fd = ::socket(family, SOCK_STREAM, 0);
        if (fd < 0) return errno;
        const int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_storage ss{};
        socklen_t len;
        if (family == AF_INET) {
            auto& a = reinterpret_cast<sockaddr_in&>(ss);
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(wildcard ? INADDR_ANY : INADDR_LOOPBACK);
            len = sizeof a;
        } else {
            (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
            auto& a = reinterpret_cast<sockaddr_in6&>(ss);
            a.sin6_family = AF_INET6;
            a.sin6_port = htons(port);
            a.sin6_addr = wildcard ? in6addr_any : in6addr_loopback;
            len = sizeof a;
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0 || ::listen(fd, 128) != 0) {
            const int e = errno;
            ::close(fd);
            return e;
        }
        (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ep->fds.push_back(fd);
        return 0;
    };
    const int e4 = listen_on(AF_INET);
    if (e4 != 0) {
        // Another program holds the port: Windows reports the sharing
        // violation HttpListener surfaces as error 32.
        return e4 == EACCES ? 5u /* ERROR_ACCESS_DENIED */ : kErrorSharingViolation;
    }
    (void)listen_on(AF_INET6);   // best effort: IPv6 may be unavailable
    if (::pipe(ep->wake) != 0) {
        for (int fd : ep->fds) ::close(fd);
        return 8;   // ERROR_NOT_ENOUGH_MEMORY
    }
    endpoints[{port, wildcard}] = ep;
    ++threads;
    std::thread([this, ep] { accept_loop(ep); }).detach();
    return NO_ERROR_;
}

void HttpApiManager::State::release_endpoint_locked(u16 port) {
    for (const auto& u : urls)
        if (u->port == port) return;
    for (bool wildcard : {false, true}) {
        auto it = endpoints.find({port, wildcard});
        if (it == endpoints.end()) continue;
        auto ep = it->second;
        endpoints.erase(it);
        ep->stop = true;
        {
            // Close the listening sockets now, so the port can be registered
            // again at once; the accept thread only ever uses them under ep->mu.
            std::lock_guard ep_lock(ep->mu);
            for (int fd : ep->fds) ::close(fd);
            ep->fds.clear();
        }
        const char b = 1;
        (void)::write(ep->wake[1], &b, 1);
    }
}

void HttpApiManager::State::accept_loop(std::shared_ptr<Endpoint> ep) {
    std::vector<pollfd> fds;
    for (int fd : ep->fds) fds.push_back({fd, POLLIN, 0});
    fds.push_back({ep->wake[0], POLLIN, 0});
    while (!ep->stop) {
        const int r = ::poll(fds.data(), fds.size(), -1);
        if (r < 0 && errno != EINTR) break;
        if (ep->stop) break;
        for (usize i = 0; i + 1 < fds.size(); ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            auto conn = std::make_shared<Connection>();
            socklen_t len = sizeof conn->remote;
            {
                std::lock_guard ep_lock(ep->mu);
                if (ep->stop) break;
                conn->fd = ::accept(fds[i].fd, reinterpret_cast<sockaddr*>(&conn->remote), &len);
            }
            if (conn->fd < 0) continue;
            len = sizeof conn->local;
            (void)::getsockname(conn->fd, reinterpret_cast<sockaddr*>(&conn->local), &len);
            conn->port = ep->port;
            const int one = 1;
            (void)::setsockopt(conn->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            (void)::setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            (void)::fcntl(conn->fd, F_SETFD, FD_CLOEXEC);
            std::lock_guard lock(mu);
            if (stopping) { ::close(conn->fd); continue; }
            conn->id = new_id();
            connections.push_back(conn);
            ++threads;
            std::thread([this, conn] { serve(conn); }).detach();
        }
    }
    {
        std::lock_guard ep_lock(ep->mu);
        for (int fd : ep->fds) ::close(fd);
        ep->fds.clear();
    }
    std::lock_guard lock(mu);
    --threads;
    threads_cv.notify_all();
}

// =============================================================================
// Connections
// =============================================================================

void HttpApiManager::State::abort_request_locked(std::shared_ptr<Request> req) {
    if (req->aborted) return;
    req->aborted = true;
    req->stage = Request::Stage::Done;
    if (auto q = req->queue.lock()) {
        q->requests.erase(req->id);
        std::erase(q->queued, req);
        // Body reads on it end with ERROR_CONNECTION_INVALID.
        wake_entity_waiters_locked(q, deferred);
    }
    if (auto& conn = req->connection) {
        // The connection is reset ("status1000 then-200: ... err=10054"); the
        // reader closes it with SO_LINGER 0 once it wakes.
        conn->aborted = true;
        // `finished` is set under `mu` before the reader closes the fd.
        if (!conn->finished) (void)::shutdown(conn->fd, SHUT_RD);
    }
    cv.notify_all();
}

void HttpApiManager::State::finish_connection(const std::shared_ptr<Connection>& conn) {
    std::vector<Completion> done;
    {
        std::lock_guard lock(mu);
        if (conn->current && conn->current->stage != Request::Stage::Done) {
            // The client went away before the application answered: further
            // sends and body reads fail with ERROR_CONNECTION_INVALID.
            auto req = conn->current;
            if (auto q = req->queue.lock()) {
                std::erase(q->queued, req);
                for (auto it = q->waiters.begin(); it != q->waiters.end();) {
                    if ((*it)->kind == Waiter::Kind::EntityBody && (*it)->request_id == req->id) {
                        deliver(done, *it, kErrorConnectionInvalid, kStatusConnectionInvalid, 0, false);
                        it = q->waiters.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            req->body_complete = true;
        }
        for (auto it = disconnect_waiters.begin(); it != disconnect_waiters.end();) {
            if ((*it)->connection_id == conn->id) {
                deliver(done, *it, NO_ERROR_, kStatusSuccess, 0, false);
                it = disconnect_waiters.erase(it);
            } else {
                ++it;
            }
        }
        std::erase(connections, conn);
        conn->finished = true;
        cv.notify_all();
    }
    {
        std::lock_guard send_lock(conn->send_mutex);
        if (conn->aborted) reset_socket(conn->fd);
        ::close(conn->fd);
        conn->fd = -1;
    }
    publish(done);
    std::lock_guard lock(mu);
    --threads;
    threads_cv.notify_all();
}

void HttpApiManager::State::serve(std::shared_ptr<Connection> conn) {
    const int fd = conn->fd;
    for (;;) {
        if (conn->aborted || stopping) break;
        // --- Request head -------------------------------------------------
        // HeaderWait and IdleConnection apply once a request on the
        // connection was routed (http.h HTTP_TIMEOUT_LIMIT_INFO).
        usize head_end;
        bool timed_out = false, closed = false;
        while ((head_end = conn->inbuf.find("\r\n\r\n")) == std::string::npos) {
            if (conn->inbuf.size() > kMaxHead) break;
            const u16 limit = conn->inbuf.empty() ? conn->timeouts.idle_connection : conn->timeouts.header_wait;
            if (!wait_readable(fd, timeout_ms(limit))) { timed_out = true; break; }
            if (read_some(fd, conn->inbuf) == 0) { closed = true; break; }
        }
        if (timed_out || closed || conn->aborted) break;
        if (head_end == std::string::npos) {
            send_server_error(fd, 400, "Bad Request", "The request header is too long.");
            break;
        }
        ParsedHead parsed;
        const std::string head = conn->inbuf.substr(0, head_end);
        if (!parse_head(head, *conn, parsed)) {
            send_server_error(fd, 400, "Bad Request", "The request is malformed.");   // "garbage: 400"
            break;
        }
        conn->inbuf.erase(0, head_end + 4);
        auto req = parsed.req;
        req->head_bytes = head_end + 4;
        req->connection = conn;

        // --- Routing (Learn "UrlPrefix Strings": strong +, then explicit
        // hosts, then weak *; the longest path prefix wins) -------------------
        enum class Route { Ok, Malformed, NoHost, NoPath, Inactive, Unbound, QueueGone, Held, Busy };
        u64 head_chunk_left = 0;
        bool head_in_trailer = false;
        Route route = Route::Ok;
        u32 verbosity = Http503ResponseVerbosityBasic;
        std::shared_ptr<Queue> q;
        std::vector<Completion> done;
        {
            std::lock_guard lock(mu);
            const UrlEntry* best[3] = {nullptr, nullptr, nullptr};
            bool host_known = false;
            for (const auto& u : urls) {
                if (u->port != conn->port) continue;
                const int rank = u->kind == HostKind::Strong ? 0 : u->kind == HostKind::Named ? 1 : 2;
                if (rank == 1 && u->host != parsed.host) continue;
                host_known = true;
                const bool match = parsed.path.compare(0, u->path.size(), u->path) == 0 ||
                                   parsed.path + "/" == u->path;
                if (match && (!best[rank] || u->path.size() > best[rank]->path.size())) best[rank] = u.get();
            }
            const UrlEntry* chosen = best[0] ? best[0] : best[1] ? best[1] : best[2];
            if (!chosen) {
                route = host_known ? Route::NoPath : Route::NoHost;   // "unregistered: 404", "nohost: 400"
            } else {
                auto g = groups.count(chosen->group) ? groups[chosen->group] : nullptr;
                std::shared_ptr<Session> session =
                    g && sessions.count(g->session) ? sessions[g->session] : nullptr;
                const u32 state = g && g->props.state ? *g->props.state
                                  : session && session->props.state ? *session->props.state
                                                                    : HttpEnabledStateActive;
                conn->timeouts = g && g->props.timeouts ? *g->props.timeouts
                                 : session && session->props.timeouts ? *session->props.timeouts
                                                                      : Timeouts{};
                conn->routed_once = true;
                q = g ? g->queue.lock() : nullptr;
                if (state == HttpEnabledStateInactive) route = Route::Inactive;   // "inactive long: err=10054"
                else if (!g || !g->bound) route = Route::Unbound;                // "unbound: 503"
                else if (!q || q->closed) route = Route::QueueGone;              // "queue closed: 400"
                else if (q->shut_down) route = Route::Held;                      // "shutdown long: err=10060"
                else if (q->state == HttpEnabledStateInactive || q->queued.size() >= q->queue_length) {
                    route = Route::Busy;
                    verbosity = q->verbosity;
                } else {
                    req->id = new_id();
                    req->url_context = chosen->context;
                    req->queue = q;
                    req->queued_at = std::chrono::steady_clock::now();
                    req->close_after = !parsed.keep_alive;
                    // Body bytes that arrived with the head count in BytesReceived
                    // ("recv post: bytes_received=110").
                    if (!req->body_complete &&
                        !absorb_body(*req, conn->inbuf, head_chunk_left, head_in_trailer)) {
                        route = Route::Malformed;
                    } else {
                        conn->current = req;
                        q->requests[req->id] = req;
                        q->queued.push_back(req);
                        dispatch_locked(q, done);
                    }
                }
            }
        }
        publish(done);
        switch (route) {
        case Route::Ok: break;
        case Route::Malformed: send_server_error(fd, 400, "Bad Request", "The request body is malformed."); break;
        case Route::NoHost: send_server_error(fd, 400, "Bad Request", "The request hostname is not served here."); break;
        case Route::NoPath: send_server_error(fd, 404, "Not Found", "No application serves this URL."); break;
        case Route::Unbound: send_server_error(fd, 503, "Service Unavailable", "The URL is not bound to a request queue."); break;
        case Route::QueueGone: send_server_error(fd, 400, "Bad Request", "The request queue for this URL is closed."); break;
        case Route::Busy:
            if (verbosity == Http503ResponseVerbosityBasic) conn->aborted = true;
            else send_server_error(fd, 503, "Service Unavailable", "The request queue is full or disabled.");
            break;
        case Route::Inactive: conn->aborted = true; break;
        case Route::Held: {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return stopping || conn->aborted || !q || q->closed; });
            break;
        }
        }
        if (route != Route::Ok) break;
        if (parsed.expect_continue && !req->body_complete) {
            std::lock_guard send_lock(conn->send_mutex);
            (void)write_all(fd, "HTTP/1.1 100 Continue\r\n\r\n");
        }

        // --- Entity body, then the response --------------------------------
        u64 chunk_left = head_chunk_left;
        bool in_trailer = head_in_trailer;
        bool reject_queued = false;
        for (;;) {
            {
                std::unique_lock lock(mu);
                // Wait while the buffered body is unread, or while there is
                // nothing left to read and the response is still pending.
                const auto queue_limit = std::chrono::milliseconds(timeout_ms(conn->timeouts.request_queue));
                for (;;) {
                    if (conn->aborted || stopping || req->stage == Request::Stage::Done) break;
                    if (!req->body_complete && req->body.size() < kBodyBufferLimit) break;
                    if (req->stage == Request::Stage::Queued) {
                        if (cv.wait_until(lock, req->queued_at + queue_limit) == std::cv_status::timeout &&
                            req->stage == Request::Stage::Queued) {
                            // RequestQueue timeout: the request is withdrawn.
                            std::erase(q->queued, req);
                            q->requests.erase(req->id);
                            req->stage = Request::Stage::Done;
                            reject_queued = true;
                            verbosity = q->verbosity;
                            break;
                        }
                    } else {
                        cv.wait(lock);
                    }
                }
                if (conn->aborted || stopping || req->stage == Request::Stage::Done) break;
            }
            // Read more body.
            const u16 limit = conn->timeouts.entity_body;
            bool got = conn->inbuf.size() > 0;
            if (!got) {
                if (!wait_readable(fd, timeout_ms(limit)) || read_some(fd, conn->inbuf) == 0) {
                    std::lock_guard lock(mu);
                    abort_request_locked(req);
                    break;
                }
            }
            std::vector<Completion> body_done;
            {
                std::lock_guard lock(mu);
                if (conn->opaque) {
                    req->body += conn->inbuf;
                    req->body_wire_bytes += conn->inbuf.size();
                    conn->inbuf.clear();
                } else if (!absorb_body(*req, conn->inbuf, chunk_left, in_trailer)) {
                    abort_request_locked(req);
                }
                wake_entity_waiters_locked(q, body_done);
                dispatch_locked(q, body_done);
            }
            publish(body_done);
        }
        if (reject_queued) {
            if (verbosity == Http503ResponseVerbosityBasic) conn->aborted = true;
            else send_server_error(fd, 503, "Service Unavailable", "The request waited too long in the queue.");
            break;
        }
        if (conn->aborted || stopping) break;
        bool close_now;
        {
            std::lock_guard lock(mu);
            close_now = req->close_after;
            conn->current.reset();
        }
        if (close_now) {   // "send disconnect / http10: closed=1"
            (void)::shutdown(fd, SHUT_WR);
            break;
        }
        // Discard what the application left unread of the body (DrainEntityBody).
        while (!req->body_complete) {
            if (conn->inbuf.empty() &&
                (!wait_readable(fd, timeout_ms(conn->timeouts.drain_entity_body)) ||
                 read_some(fd, conn->inbuf) == 0))
                break;
            if (!absorb_body(*req, conn->inbuf, chunk_left, in_trailer)) break;
            req->body.clear();
        }
        if (!req->body_complete) break;
    }
    finish_connection(conn);
}

// =============================================================================
// Receiving requests
// =============================================================================

namespace {

/// The HTTP_REQUEST image for one request, built at `base` in guest memory.
struct RequestImage {
    std::vector<u8> bytes;
    u32 fixed = 0;
    GuestAddr base = 0;

    u32 place(const void* data, usize n, u32 align) {
        usize at = (bytes.size() + align - 1) & ~usize(align - 1);
        bytes.resize(at + n);
        if (n) std::memcpy(bytes.data() + at, data, n);
        return static_cast<u32>(at);
    }
    template <typename T> void put(u32 at, T v) { std::memcpy(bytes.data() + at, &v, sizeof v); }
    void put_ptr(u32 at, u32 offset, bool x64) {
        const u64 p = offset ? base + offset : 0;
        if (x64) put<u64>(at, p);
        else put<u32>(at, static_cast<u32>(p));
    }
};

RequestImage build_image(const Request& r, const Connection& conn, const Queue& q, GuestAddr base,
                         bool x64, usize body_bytes, bool more_body) {
    const Layout& L = layout_for(x64);
    RequestImage img;
    img.base = base;
    img.fixed = q.version_major == 1 ? L.request_v1_size : L.request_v2_size;
    img.bytes.assign(img.fixed, 0);

    img.put<u32>(L.rq_flags, more_body ? HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS : 0);
    img.put<u64>(L.rq_connection_id, conn.id);
    img.put<u64>(L.rq_request_id, r.id);
    img.put<u64>(L.rq_url_context, r.url_context);
    img.put<u16>(L.rq_version, r.major);
    img.put<u16>(L.rq_version + 2, r.minor);
    img.put<u32>(L.rq_verb, r.verb);
    if (r.verb == HttpVerbUnknown) {
        img.put<u16>(L.rq_unknown_verb_length, static_cast<u16>(r.method.size()));
        img.put_ptr(L.rq_unknown_verb, img.place(r.method.c_str(), r.method.size() + 1, 1), x64);
    }
    img.put<u16>(L.rq_raw_url_length, static_cast<u16>(r.raw_url.size()));
    img.put_ptr(L.rq_raw_url, img.place(r.raw_url.c_str(), r.raw_url.size() + 1, 1), x64);

    const u32 url = img.place(r.full_url.c_str(), (r.full_url.size() + 1) * 2, 2);
    const u32 cooked = L.rq_cooked_url;
    img.put<u16>(cooked + 0, static_cast<u16>(r.full_url.size() * 2));
    img.put<u16>(cooked + 2, static_cast<u16>(r.host_len * 2));
    img.put<u16>(cooked + 4, static_cast<u16>(r.abs_len * 2));
    img.put<u16>(cooked + 6, static_cast<u16>(r.has_query ? r.query_len * 2 : 0));
    img.put_ptr(cooked + L.cooked_full_url, url, x64);
    img.put_ptr(cooked + L.cooked_host, url + static_cast<u32>(r.host_at * 2), x64);
    img.put_ptr(cooked + L.cooked_abs_path, url + static_cast<u32>(r.abs_at * 2), x64);
    img.put_ptr(cooked + L.cooked_query, r.has_query ? url + static_cast<u32>(r.query_at * 2) : 0, x64);

    const std::string remote = sockaddr_image(conn.remote), local = sockaddr_image(conn.local);
    img.put_ptr(L.rq_address, img.place(remote.data(), remote.size(), 4), x64);
    img.put_ptr(L.rq_address + L.ptr, img.place(local.data(), local.size(), 4), x64);

    const u32 hdr = L.rq_headers;
    for (u32 id = 0; id < HttpHeaderRequestMaximum; ++id) {
        if (!r.known[id]) continue;
        const std::string& v = *r.known[id];
        const u32 slot = hdr + L.hdr_known + id * L.known_header_size;
        img.put<u16>(slot, static_cast<u16>(v.size()));
        img.put_ptr(slot + L.known_header_value, img.place(v.c_str(), v.size() + 1, 1), x64);
    }
    if (!r.unknown.empty()) {
        std::vector<u8> zero(r.unknown.size() * L.unknown_header_size, 0);
        const u32 array = img.place(zero.data(), zero.size(), L.ptr);
        for (usize i = 0; i < r.unknown.size(); ++i) {
            const auto& [name, value] = r.unknown[i];
            const u32 entry = array + static_cast<u32>(i) * L.unknown_header_size;
            const u32 n = img.place(name.c_str(), name.size() + 1, 1);
            const u32 v = img.place(value.c_str(), value.size() + 1, 1);
            img.put<u16>(entry, static_cast<u16>(name.size()));
            img.put<u16>(entry + 2, static_cast<u16>(value.size()));
            img.put_ptr(entry + L.unknown_header_name, n, x64);
            img.put_ptr(entry + L.unknown_header_value, v, x64);
        }
        img.put<u16>(hdr + L.hdr_unknown_count, static_cast<u16>(r.unknown.size()));
        img.put_ptr(hdr + L.hdr_unknown, array, x64);
    }
    img.put<u64>(L.rq_bytes_received, r.head_bytes + r.body_wire_bytes);
    if (body_bytes) {
        std::vector<u8> zero(L.data_chunk_size, 0);
        const u32 chunk = img.place(zero.data(), zero.size(), 8);
        const u32 data = img.place(r.body.data(), body_bytes, 1);
        img.put<u32>(chunk, HttpDataChunkFromMemory);
        img.put_ptr(chunk + L.chunk_buffer, data, x64);
        img.put<u32>(chunk + L.chunk_buffer_length, static_cast<u32>(body_bytes));
        img.put<u16>(L.rq_entity_chunk_count, 1);
        img.put_ptr(L.rq_entity_chunks, chunk, x64);
    }
    img.put<u64>(L.rq_raw_connection_id, conn.id);
    // RequestInfoCount stays 0: no HTTP_REQUEST_INFO (timing, sizing, TCP
    // info) is produced.
    return img;
}

} // namespace

std::optional<u32> HttpApiManager::State::try_receive_locked(const std::shared_ptr<Queue>& q, Waiter& w,
                                                             u32& bytes, u32& nt_status) {
    std::shared_ptr<Request> req;
    if (w.request_id != 0) {
        auto it = q->requests.find(w.request_id);
        if (it == q->requests.end()) return kErrorConnectionInvalid;   // "recv bogus-id: rc=1229"
        req = it->second;
        if (req->stage != Request::Stage::AwaitRetry) {
            // A request delivered already is reset ("re-receive delivered:
            // rc=1229 ... err=10054").
            abort_request_locked(req);
            return kErrorConnectionInvalid;
        }
    } else {
        const bool flush = w.flags & HTTP_RECEIVE_REQUEST_FLAG_FLUSH_BODY;
        for (const auto& candidate : q->queued) {
            if (flush && !candidate->body_complete) continue;   // FLUSH_BODY: the whole body
            req = candidate;
            break;
        }
        if (!req) return std::nullopt;
    }
    const bool copy = w.flags & (HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY | HTTP_RECEIVE_REQUEST_FLAG_FLUSH_BODY);
    const usize body_bytes = copy ? req->body.size() : 0;
    const bool more = !req->body_complete || req->body.size() > body_bytes;
    RequestImage img = build_image(*req, *req->connection, *q, w.buffer, w.is_64bit, body_bytes, more);
    const u32 needed = static_cast<u32>(img.bytes.size());
    if (w.length < needed) {
        // ERROR_MORE_DATA: the fixed part is zeroed but for RequestId, and
        // the size needed is returned ("recv small: rc=234 got=1576 ...
        // flags=0x0 verb=0 raw_ptr_null=1").
        std::vector<u8> fixed(img.fixed, 0);
        const u64 id = req->id;
        std::memcpy(fixed.data() + layout_for(w.is_64bit).rq_request_id, &id, 8);
        if (memory->write_bytes(w.buffer, fixed.data(), fixed.size()).is_err()) return kErrorNoAccess;
        std::erase(q->queued, req);
        req->stage = Request::Stage::AwaitRetry;
        bytes = needed;
        nt_status = kStatusBufferOverflow;
        return kErrorMoreData;
    }
    if (memory->write_bytes(w.buffer, img.bytes.data(), img.bytes.size()).is_err()) return kErrorNoAccess;
    req->body.erase(0, body_bytes);
    std::erase(q->queued, req);
    req->stage = Request::Stage::Delivered;
    bytes = needed;
    nt_status = kStatusSuccess;
    cv.notify_all();   // the reader may continue reading a body it held back
    return NO_ERROR_;
}

std::optional<u32> HttpApiManager::State::try_entity_locked(const std::shared_ptr<Queue>& q, Waiter& w,
                                                            u32& bytes, u32& nt_status) {
    auto it = q->requests.find(w.request_id);
    if (it == q->requests.end()) return kErrorConnectionInvalid;
    auto req = it->second;
    if (req->stage != Request::Stage::Delivered && req->stage != Request::Stage::Responding)
        return kErrorConnectionInvalid;
    if (req->body.empty()) {
        if (req->body_complete) {   // "entity eof: rc=38"
            nt_status = kStatusEndOfFile;
            return kErrorHandleEof;
        }
        return std::nullopt;
    }
    if ((w.flags & HTTP_RECEIVE_REQUEST_ENTITY_BODY_FLAG_FILL_BUFFER) && req->body.size() < w.length &&
        !req->body_complete)
        return std::nullopt;
    const usize n = std::min<usize>(w.length, req->body.size());
    if (memory->write_bytes(w.buffer, reinterpret_cast<const u8*>(req->body.data()), n).is_err())
        return kErrorNoAccess;
    req->body.erase(0, n);
    bytes = static_cast<u32>(n);
    nt_status = kStatusSuccess;
    cv.notify_all();
    return NO_ERROR_;
}

void HttpApiManager::State::dispatch_locked(const std::shared_ptr<Queue>& q, std::vector<Completion>& out) {
    for (auto it = q->waiters.begin(); it != q->waiters.end();) {
        auto w = *it;
        if (w->kind != Waiter::Kind::Request || w->request_id != 0) { ++it; continue; }
        u32 bytes = 0, nt = 0;
        auto r = try_receive_locked(q, *w, bytes, nt);
        if (!r) { ++it; continue; }
        it = q->waiters.erase(it);
        if (w->overlapped == 0 && w->bytes_out) (void)memory->write_u32(w->bytes_out, bytes);
        deliver(out, w, *r, nt, bytes, false);
    }
}

void HttpApiManager::State::wake_entity_waiters_locked(const std::shared_ptr<Queue>& q,
                                                       std::vector<Completion>& out) {
    for (auto it = q->waiters.begin(); it != q->waiters.end();) {
        auto w = *it;
        if (w->kind != Waiter::Kind::EntityBody) { ++it; continue; }
        u32 bytes = 0, nt = 0;
        auto r = try_entity_locked(q, *w, bytes, nt);
        if (!r) { ++it; continue; }
        it = q->waiters.erase(it);
        if (w->overlapped == 0 && w->bytes_out) (void)memory->write_u32(w->bytes_out, bytes);
        deliver(out, w, *r, nt, bytes, false);
    }
}

namespace {

/// Publishes the completions a call left in State::deferred once the call
/// has released State::mu (declare it before the lock).
struct DeferredFlush {
    HttpApiManager::State& s;
    ~DeferredFlush() {
        std::vector<Completion> none;
        s.publish(none);
    }
};

void put_guest_ptr(GuestMemory& mem, GuestAddr at, u64 v, bool x64) {
    if (x64) (void)mem.write_u64(at, v);
    else (void)mem.write_u32(at, static_cast<u32>(v));
}

/// Common tail of the receive calls: complete now, pend, or block.
u32 run_receive(HttpApiManager::State& s, const std::shared_ptr<Queue>& q, std::shared_ptr<Waiter> w,
                std::unique_lock<std::mutex>& lock) {
    u32 bytes = 0, nt = 0;
    auto r = w->kind == Waiter::Kind::Request ? s.try_receive_locked(q, *w, bytes, nt)
                                              : s.try_entity_locked(q, *w, bytes, nt);
    std::vector<Completion> done;
    if (r) {
        const u32 err = *r;
        if (w->bytes_out && (err == NO_ERROR_ || err == kErrorMoreData))
            (void)s.memory->write_u32(w->bytes_out, bytes);   // "ovl small: got=0x618", "ovl immediate: got_set=1"
        if (w->overlapped == 0 || (err != NO_ERROR_ && err != kErrorMoreData)) return err;
        if (err == kErrorMoreData && s.has_port && s.has_port(w->handle)) {
            // With a completion port the overflow is reported through it
            // ("iocp small: rc=997 packet=1 le=234").
            done.push_back({w->handle, bytes, w->overlapped, w->is_64bit, nt, err, false});
            lock.unlock();
            s.publish(done);
            return kErrorIoPending;
        }
        // Completed inline: OVERLAPPED, hEvent and (on success) a port
        // packet, as for any file request ("iocp immediate: rc=0 packet=1",
        // "ovl small: internal=0x80000005").
        done.push_back({w->handle, bytes, w->overlapped, w->is_64bit, nt, err, true});
        lock.unlock();
        s.publish(done);
        return err;
    }
    if (w->overlapped != 0) {
        // "ovl pending: rc=997 internal=0x103 event_reset=1"
        put_guest_ptr(*s.memory, w->overlapped, kStatusPending, w->is_64bit);
        put_guest_ptr(*s.memory, w->overlapped + (w->is_64bit ? 8 : 4), 0, w->is_64bit);
        const auto ev = w->is_64bit ? s.memory->read_u64(w->overlapped + 24).value_or(0)
                                    : s.memory->read_u32(w->overlapped + 16).value_or(0);
        if (ev && s.sync) (void)s.sync->reset_event(static_cast<Handle>(ev & ~u64{1}));
        q->waiters.push_back(w);
        return kErrorIoPending;
    }
    q->waiters.push_back(w);
    s.cv.wait(lock, [&] { return w->done || q->closed || s.stopping; });
    if (!w->done) {
        std::erase(q->waiters, w);
        return kErrorOperationAborted;   // "sync-recv released by close: rc=995"
    }
    return w->error;
}

} // namespace

void HttpApiManager::HttpReceiveHttpRequest(Marshaler& m) {
    // ULONG HttpReceiveHttpRequest(HANDLE RequestQueueHandle, HTTP_REQUEST_ID RequestId,
    //   ULONG Flags, PHTTP_REQUEST RequestBuffer, ULONG RequestBufferLength,
    //   PULONG BytesReturned, LPOVERLAPPED Overlapped)
    State& s = *m_state;
    if (!s.memory) s.memory = &m.memory();
    auto w = std::make_shared<Waiter>();
    {
        const bool x64 = m.cpu_is_64bit();
        usize i = 0;
        auto next = [&]() -> u64 { return m.arg(i++); };
        auto p = [&]() -> GuestAddr { const u64 v = next(); return static_cast<GuestAddr>(x64 ? v : v & 0xFFFFFFFFu); };
        w->handle = static_cast<Handle>(p());
        w->request_id = x64 ? next() : ((next() & 0xFFFFFFFFu) | ((next() & 0xFFFFFFFFu) << 32));
        w->flags = static_cast<u32>(next());
        w->buffer = p();
        w->length = static_cast<u32>(next());
        w->bytes_out = p();
        w->overlapped = p();
        w->is_64bit = x64;
    }
    w->kind = Waiter::Kind::Request;
    w->thread_id = ThreadManager::get_current_thread_id_static();
    DeferredFlush flush{s};
    std::unique_lock lock(s.mu);
    auto q = s.queue_for(w->handle);
    // "recv closed-handle / null-handle / after-shutdown sync-recv: rc=6"
    if (!q || q->shut_down) { m.set_return(kErrorInvalidHandle); return; }
    if (w->flags & ~(HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY | HTTP_RECEIVE_REQUEST_FLAG_FLUSH_BODY)) {
        m.set_return(kErrorInvalidParameter);   // "recv flags4: rc=87"
        return;
    }
    if (w->buffer == 0) { m.set_return(kErrorInvalidParameter); return; }   // "recv null-buffer: rc=87"
    const Layout& L = layout_for(w->is_64bit);
    if (w->length < (q->version_major == 1 ? L.request_v1_size : L.request_v2_size)) {
        m.set_return(kErrorInsufficientBuffer);   // "recv v1-size / 16-bytes: rc=122"
        return;
    }
    // Learn: the buffer must have HTTP_REQUEST alignment (8: it holds ULONGLONGs).
    if ((w->buffer & 7) != 0 || !m.memory().guest_can_write(w->buffer, w->length)) {
        m.set_return(kErrorNoAccess);
        return;
    }
    m.set_return(run_receive(s, q, w, lock));
}

void HttpApiManager::HttpReceiveRequestEntityBody(Marshaler& m) {
    // ULONG HttpReceiveRequestEntityBody(HANDLE RequestQueueHandle, HTTP_REQUEST_ID RequestId,
    //   ULONG Flags, PVOID EntityBuffer, ULONG EntityBufferLength, PULONG BytesReturned,
    //   LPOVERLAPPED Overlapped)
    State& s = *m_state;
    if (!s.memory) s.memory = &m.memory();
    auto w = std::make_shared<Waiter>();
    const bool x64 = m.cpu_is_64bit();
    usize i = 0;
    auto next = [&]() -> u64 { return m.arg(i++); };
    auto p = [&]() -> GuestAddr { const u64 v = next(); return static_cast<GuestAddr>(x64 ? v : v & 0xFFFFFFFFu); };
    w->kind = Waiter::Kind::EntityBody;
    w->handle = static_cast<Handle>(p());
    w->request_id = x64 ? next() : ((next() & 0xFFFFFFFFu) | ((next() & 0xFFFFFFFFu) << 32));
    w->flags = static_cast<u32>(next());
    w->buffer = p();
    w->length = static_cast<u32>(next());
    w->bytes_out = p();
    w->overlapped = p();
    w->is_64bit = x64;
    w->thread_id = ThreadManager::get_current_thread_id_static();
    std::unique_lock lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorDllInitFailed); return; }   // Learn
    auto q = s.queue_for(w->handle);
    if (!q) { m.set_return(kErrorInvalidHandle); return; }
    if ((w->flags & ~HTTP_RECEIVE_REQUEST_ENTITY_BODY_FLAG_FILL_BUFFER) || w->buffer == 0 ||
        w->length == 0 || w->request_id == 0) {
        m.set_return(kErrorInvalidParameter);
        return;
    }
    if (!m.memory().guest_can_write(w->buffer, w->length)) { m.set_return(kErrorNoAccess); return; }
    m.set_return(run_receive(s, q, w, lock));
}

// =============================================================================
// Responses
// =============================================================================

namespace {

struct Chunks {
    std::string data;
    u32 error = NO_ERROR_;
};

/// HTTP_DATA_CHUNK array -> bytes. ERROR_NOACCESS for an unreadable array
/// ("send null-chunks: rc=998"), ERROR_INVALID_PARAMETER for an unknown
/// type ("send bad-chunk-type: rc=87").
Chunks read_chunks(GuestMemory& mem, bool x64, u32 count, GuestAddr chunks, const char* api) {
    const Layout& L = layout_for(x64);
    Chunks out;
    if (count == 0) return out;
    if (chunks == 0) { out.error = kErrorNoAccess; return out; }
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr c = chunks + i * L.data_chunk_size;
        auto type = mem.read_u32(c);
        if (type.is_err()) { out.error = kErrorNoAccess; return out; }
        if (type.value() >= HttpDataChunkMaximum) { out.error = kErrorInvalidParameter; return out; }
        if (type.value() != HttpDataChunkFromMemory) {
            unimplemented_once(api, type.value() == HttpDataChunkFromFileHandle
                                               ? "HttpDataChunkFromFileHandle"
                                               : "fragment-cache / trailer / fast-forward data chunks");
            out.error = kErrorNotSupported;
            return out;
        }
        const u64 buf = x64 ? mem.read_u64(c + L.chunk_buffer).value_or(0)
                            : mem.read_u32(c + L.chunk_buffer).value_or(0);
        const u32 len = mem.read_u32(c + L.chunk_buffer_length).value_or(0);
        if (len == 0) continue;
        if (buf == 0) { out.error = kErrorNoAccess; return out; }
        const usize at = out.data.size();
        out.data.resize(at + len);
        if (mem.read_bytes(static_cast<GuestAddr>(buf), reinterpret_cast<u8*>(&out.data[at]), len).is_err()) {
            out.error = kErrorNoAccess;
            return out;
        }
    }
    return out;
}

std::optional<std::string> read_counted(GuestMemory& mem, GuestAddr p, u32 len) {
    std::string s(len, '\0');
    if (len == 0) return s;
    if (p == 0 || mem.read_bytes(p, reinterpret_cast<u8*>(s.data()), len).is_err()) return std::nullopt;
    return s;
}

std::string frame_chunk(std::string_view data) {
    if (data.empty()) return {};
    char size[24];
    std::snprintf(size, sizeof size, "%zx\r\n", data.size());
    return std::string(size) + std::string(data) + "\r\n";
}

struct SendArgs {
    Handle handle = 0;
    u64 request_id = 0;
    u32 flags = 0;
    GuestAddr response = 0;     // HttpSendHttpResponse
    u32 chunk_count = 0;        // HttpSendResponseEntityBody
    GuestAddr chunks = 0;
    GuestAddr cache_policy = 0;
    GuestAddr bytes_sent = 0;
    GuestAddr reserved1 = 0;
    u32 reserved2 = 0;
    GuestAddr overlapped = 0;
};

} // namespace

namespace {

/// Write `wire` to the request's connection and finish the call (BytesSent,
/// the overlapped completion, the request's next stage).
u32 transmit(HttpApiManager::State& s, std::unique_lock<std::mutex>& lock, const std::shared_ptr<Request>& req,
             const SendArgs& a, bool x64, std::string wire, bool final_part, bool nagle) {
    auto conn = req->connection;
    if (final_part) {
        req->stage = Request::Stage::Done;
        if (auto q = req->queue.lock()) q->requests.erase(req->id);
    } else {
        req->stage = Request::Stage::Responding;
    }
    lock.unlock();
    bool ok;
    {
        std::lock_guard send_lock(conn->send_mutex);
        ok = !conn->finished && conn->fd >= 0;
        if (ok && nagle) {
            const int zero = 0;
            (void)::setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &zero, sizeof zero);
        }
        if (ok) ok = write_all(conn->fd, wire);
        if (nagle && conn->fd >= 0) {
            const int one = 1;
            (void)::setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
    }
    lock.lock();
    s.cv.notify_all();
    if (!ok) {
        s.abort_request_locked(req);
        return kErrorConnectionInvalid;
    }
    const u32 sent = static_cast<u32>(wire.size());
    if (a.bytes_sent) (void)s.memory->write_u32(a.bytes_sent, sent);   // "iocp ovl-send: sent=0x6e"
    if (a.overlapped) {
        // Sends complete inline, packet and all ("iocp ovl-send: rc=0 gqcs=1 n=110").
        std::vector<Completion> done{{a.handle, sent, a.overlapped, x64, kStatusSuccess, NO_ERROR_, true}};
        lock.unlock();
        s.publish(done);
        lock.lock();
    }
    return NO_ERROR_;
}

} // namespace

void HttpApiManager::HttpSendHttpResponse(Marshaler& m) {
    // ULONG HttpSendHttpResponse(HANDLE RequestQueueHandle, HTTP_REQUEST_ID RequestId,
    //   ULONG Flags, PHTTP_RESPONSE HttpResponse, PHTTP_CACHE_POLICY CachePolicy,
    //   PULONG BytesSent, PVOID Reserved1, ULONG Reserved2, LPOVERLAPPED Overlapped,
    //   PHTTP_LOG_DATA LogData)
    State& s = *m_state;
    if (!s.memory) s.memory = &m.memory();
    GuestMemory& mem = m.memory();
    const bool x64 = m.cpu_is_64bit();
    const Layout& L = layout_for(x64);
    SendArgs a;
    {
        usize i = 0;
        auto next = [&]() -> u64 { return m.arg(i++); };
        auto p = [&]() -> GuestAddr { const u64 v = next(); return static_cast<GuestAddr>(x64 ? v : v & 0xFFFFFFFFu); };
        a.handle = static_cast<Handle>(p());
        a.request_id = x64 ? next() : ((next() & 0xFFFFFFFFu) | ((next() & 0xFFFFFFFFu) << 32));
        a.flags = static_cast<u32>(next());
        a.response = p();
        a.cache_policy = p();
        a.bytes_sent = p();
        a.reserved1 = p();
        a.reserved2 = static_cast<u32>(next());
        a.overlapped = p();
        (void)p();   // LogData: responses are logged only when logging is enabled
    }
    DeferredFlush flush{s};
    std::unique_lock lock(s.mu);
    auto q = s.queue_for(a.handle);
    if (!q) { m.set_return(kErrorInvalidHandle); return; }   // "send closed-handle: rc=6"
    if (a.response == 0) {
        // httpapi.dll reads through the pointer and faults.
        (void)m.raise_access_violation(false, 0);
        m.set_return(kErrorInvalidParameter);
        return;
    }
    if (a.reserved1 != 0 || a.reserved2 != 0) { m.set_return(kErrorInvalidParameter); return; }   // "send reserved2: rc=87"
    auto it = q->requests.find(a.request_id);
    std::shared_ptr<Request> req = it == q->requests.end() ? nullptr : it->second;
    // A malformed response resets the request it was meant for
    // ("status1000 / bad-flags / flags0x10 / null-header-name then-200: rc=87 ... err=10054").
    auto reject = [&](u32 err) {
        if (err == kErrorInvalidParameter && req && req->stage == Request::Stage::Delivered)
            s.abort_request_locked(req);
        m.set_return(err);
    };
    const u16 status = mem.read_u16(a.response + L.rs_status).value_or(0);
    if ((a.flags & ~kSendFlagsMask) != 0) return reject(kErrorInvalidParameter);
    if (status > 999) return reject(kErrorInvalidParameter);   // "send status1000: rc=87"
    if ((a.flags & HTTP_SEND_RESPONSE_FLAG_OPAQUE) && status != 101) return reject(kErrorInvalidParameter);

    auto ptr_at = [&](GuestAddr at) -> GuestAddr {
        return static_cast<GuestAddr>(x64 ? mem.read_u64(at).value_or(0) : mem.read_u32(at).value_or(0));
    };
    const auto reason = read_counted(mem, ptr_at(a.response + L.rs_reason),
                                     mem.read_u16(a.response + L.rs_reason_length).value_or(0));
    if (!reason) return reject(kErrorNoAccess);
    const GuestAddr hdr = a.response + L.rs_headers;
    std::array<std::optional<std::string>, HttpHeaderResponseMaximum> known;
    for (u32 id = 0; id < HttpHeaderResponseMaximum; ++id) {
        const GuestAddr slot = hdr + L.hdr_known + id * L.known_header_size;
        const u16 len = mem.read_u16(slot).value_or(0);
        const GuestAddr p = ptr_at(slot + L.known_header_value);
        if (len == 0 && p == 0) continue;
        auto v = read_counted(mem, p, len);
        if (!v) return reject(kErrorNoAccess);
        known[id] = *v;
    }
    std::vector<std::pair<std::string, std::string>> unknown;
    const u16 unknown_count = mem.read_u16(hdr + L.hdr_unknown_count).value_or(0);
    const GuestAddr unknown_array = ptr_at(hdr + L.hdr_unknown);
    if (unknown_count && !unknown_array) return reject(kErrorNoAccess);
    for (u16 i = 0; i < unknown_count; ++i) {
        const GuestAddr e = unknown_array + i * L.unknown_header_size;
        const u16 nlen = mem.read_u16(e).value_or(0), vlen = mem.read_u16(e + 2).value_or(0);
        const GuestAddr np = ptr_at(e + L.unknown_header_name), vp = ptr_at(e + L.unknown_header_value);
        if (np == 0) return reject(kErrorInvalidParameter);   // "null-header-name: rc=87"
        auto n = read_counted(mem, np, nlen);
        auto v = read_counted(mem, vp, vlen);
        if (!n || !v) return reject(kErrorNoAccess);
        unknown.emplace_back(*n, *v);
    }
    Chunks body = read_chunks(mem, x64, mem.read_u16(a.response + L.rs_entity_chunk_count).value_or(0),
                              ptr_at(a.response + L.rs_entity_chunks), "HttpSendHttpResponse");
    if (body.error) return reject(body.error);

    // HTTP_RESPONSE_V2 information (HTTP Server API 2.0 queues only).
    std::array<std::vector<std::string>, HttpHeaderResponseMaximum> multiple;
    if (q->version_major >= 2) {
        const u16 info_count = mem.read_u16(a.response + L.rs_response_info_count).value_or(0);
        const GuestAddr infos = ptr_at(a.response + L.rs_response_info);
        if (info_count && !infos) return reject(kErrorNoAccess);
        for (u16 i = 0; i < info_count; ++i) {
            const GuestAddr info = infos + i * L.response_info_size;
            const u32 type = mem.read_u32(info).value_or(~0u);
            if (type != HttpResponseInfoTypeMultipleKnownHeaders) {
                unimplemented_once("HttpSendHttpResponse",
                                          "HTTP_RESPONSE_INFO authentication / QoS / channel-bind");
                return reject(kErrorNotSupported);
            }
            // HTTP_MULTIPLE_KNOWN_HEADERS { HeaderId; Flags; USHORT KnownHeaderCount; PHTTP_KNOWN_HEADER }
            const GuestAddr mk = ptr_at(info + 8);
            const u32 id = mem.read_u32(mk).value_or(~0u);
            if (!mk || id >= HttpHeaderResponseMaximum) return reject(kErrorInvalidParameter);
            const u16 n = mem.read_u16(mk + 8).value_or(0);
            const GuestAddr list = ptr_at(mk + (x64 ? 16 : 12));
            for (u16 k = 0; k < n; ++k) {
                const GuestAddr kh = list + k * L.known_header_size;
                auto v = read_counted(mem, ptr_at(kh + L.known_header_value), mem.read_u16(kh).value_or(0));
                if (!v) return reject(kErrorNoAccess);
                multiple[id].push_back(*v);
            }
        }
    }
    if (a.cache_policy && mem.read_u32(a.cache_policy).value_or(0) != HttpCachePolicyNocache) {
        unimplemented_once("HttpSendHttpResponse", "kernel response cache policies");
        return reject(kErrorNotSupported);
    }
    if (!req || req->stage != Request::Stage::Delivered) {
        m.set_return(kErrorConnectionInvalid);   // "send again / bogus-id / null-id: rc=1229"
        return;
    }
    if ((a.flags & HTTP_SEND_RESPONSE_FLAG_PROCESS_RANGES) && req->known[HttpHeaderRange]) {
        unimplemented_once("HttpSendHttpResponse", "HTTP_SEND_RESPONSE_FLAG_PROCESS_RANGES on a range request");
        return reject(kErrorNotSupported);
    }
    if (a.flags & HTTP_SEND_RESPONSE_FLAG_GOAWAY) {
        unimplemented_once("HttpSendHttpResponse", "HTTP_SEND_RESPONSE_FLAG_GOAWAY (HTTP/2)");
        return reject(kErrorNotSupported);
    }

    // The wire image ("send headers: ... Cache-Control, Content-Length,
    // Content-Type, Location, Server, X-Probe, X-Two, Date"): the status
    // line is always HTTP/1.1 with a three-digit code ("send v10-noreason:
    // HTTP/1.1 404 ", "send status99: HTTP/1.1 099 "); known headers in id
    // order with Server always present; the caller's Date and Connection
    // replaced; unknown headers; Date; "Connection: close" when the
    // connection ends; Content-Length when the body is complete.
    const bool more = a.flags & HTTP_SEND_RESPONSE_FLAG_MORE_DATA;
    const bool close = (a.flags & HTTP_SEND_RESPONSE_FLAG_DISCONNECT) || req->close_after || req->minor == 0;
    char line[64];
    std::snprintf(line, sizeof line, "HTTP/1.1 %03u ", status);
    std::string wire = line + *reason + "\r\n";
    bool has_length = false;
    for (u32 id = 0; id < HttpHeaderResponseMaximum; ++id) {
        if (id == HttpHeaderDate || id == HttpHeaderConnection) continue;
        if (id == HttpHeaderServer) {
            wire += std::string("Server: ") + (known[id] && !known[id]->empty() ? *known[id] + " " : "") +
                    kServerToken + "\r\n";
            continue;
        }
        if (known[id]) {
            wire += std::string(kResponseHeaderNames[id]) + ": " + *known[id] + "\r\n";
            if (id == HttpHeaderContentLength || id == HttpHeaderTransferEncoding) has_length = true;
        }
        for (const auto& v : multiple[id]) wire += std::string(kResponseHeaderNames[id]) + ": " + v + "\r\n";
    }
    for (const auto& [n, v] : unknown) wire += n + ": " + v + "\r\n";
    wire += "Date: " + http_date() + "\r\n";
    if (close) wire += "Connection: close\r\n";
    const bool no_body_status = (status >= 100 && status < 200) || status == 204 || status == 304;
    bool chunking = false;
    if (!has_length && !no_body_status) {
        if (!more && req->verb != HttpVerbHEAD) {
            wire += "Content-Length: " + std::to_string(body.data.size()) + "\r\n";
        } else if (more && (a.flags & HTTP_SEND_RESPONSE_FLAG_AUTOMATIC_CHUNKING)) {
            wire += "Transfer-Encoding: chunked\r\n";
            chunking = true;
        }
    }
    wire += "\r\n";
    wire += chunking ? frame_chunk(body.data) : body.data;
    req->auto_chunk = chunking;
    if (close) req->close_after = true;
    if (a.flags & HTTP_SEND_RESPONSE_FLAG_OPAQUE) {
        // 101 Switching Protocols: the connection carries opaque data from
        // here on, read with HttpReceiveRequestEntityBody and written with
        // HttpSendResponseEntityBody.
        req->connection->opaque = true;
        req->body_complete = false;
        req->chunked = false;
        req->content_length = ~u64{0};
    }
    const bool final_part = !more && !(a.flags & HTTP_SEND_RESPONSE_FLAG_OPAQUE);
    m.set_return(transmit(s, lock, req, a, x64, std::move(wire), final_part,
                          a.flags & HTTP_SEND_RESPONSE_FLAG_ENABLE_NAGLING));
}

void HttpApiManager::HttpSendResponseEntityBody(Marshaler& m) {
    // ULONG HttpSendResponseEntityBody(HANDLE RequestQueueHandle, HTTP_REQUEST_ID RequestId,
    //   ULONG Flags, USHORT EntityChunkCount, PHTTP_DATA_CHUNK EntityChunks, PULONG BytesSent,
    //   PVOID Reserved1, ULONG Reserved2, LPOVERLAPPED Overlapped, PHTTP_LOG_DATA LogData)
    State& s = *m_state;
    if (!s.memory) s.memory = &m.memory();
    const bool x64 = m.cpu_is_64bit();
    SendArgs a;
    {
        usize i = 0;
        auto next = [&]() -> u64 { return m.arg(i++); };
        auto p = [&]() -> GuestAddr { const u64 v = next(); return static_cast<GuestAddr>(x64 ? v : v & 0xFFFFFFFFu); };
        a.handle = static_cast<Handle>(p());
        a.request_id = x64 ? next() : ((next() & 0xFFFFFFFFu) | ((next() & 0xFFFFFFFFu) << 32));
        a.flags = static_cast<u32>(next());
        a.chunk_count = static_cast<u16>(next());
        a.chunks = p();
        a.bytes_sent = p();
        a.reserved1 = p();
        a.reserved2 = static_cast<u32>(next());
        a.overlapped = p();
        (void)p();   // LogData
    }
    DeferredFlush flush{s};
    std::unique_lock lock(s.mu);
    auto q = s.queue_for(a.handle);
    if (!q) { m.set_return(kErrorInvalidHandle); return; }
    if (a.reserved1 != 0 || a.reserved2 != 0) { m.set_return(kErrorInvalidParameter); return; }
    auto it = q->requests.find(a.request_id);
    std::shared_ptr<Request> req = it == q->requests.end() ? nullptr : it->second;
    if ((a.flags & ~kSendFlagsMask) != 0 || (a.flags & HTTP_SEND_RESPONSE_FLAG_OPAQUE)) {
        if (req && req->stage == Request::Stage::Responding) s.abort_request_locked(req);
        m.set_return(kErrorInvalidParameter);
        return;
    }
    Chunks body = read_chunks(m.memory(), x64, a.chunk_count, a.chunks, "HttpSendResponseEntityBody");
    if (body.error) { m.set_return(body.error); return; }
    // Only after HttpSendHttpResponse(MORE_DATA) ("entity-send after-final: rc=1229").
    if (!req || req->stage != Request::Stage::Responding) { m.set_return(kErrorConnectionInvalid); return; }
    const bool more = (a.flags & HTTP_SEND_RESPONSE_FLAG_MORE_DATA) || req->connection->opaque;
    if (a.flags & HTTP_SEND_RESPONSE_FLAG_DISCONNECT) req->close_after = true;
    std::string wire = req->auto_chunk ? frame_chunk(body.data) : body.data;
    if (!more && req->auto_chunk) wire += "0\r\n\r\n";
    m.set_return(transmit(s, lock, req, a, x64, std::move(wire), !more,
                          a.flags & HTTP_SEND_RESPONSE_FLAG_ENABLE_NAGLING));
}

void HttpApiManager::HttpCancelHttpRequest(Marshaler& m) {
    // ULONG HttpCancelHttpRequest(HANDLE RequestQueueHandle, HTTP_REQUEST_ID RequestId,
    //   LPOVERLAPPED Overlapped)
    State& s = *m_state;
    if (!s.memory) s.memory = &m.memory();
    const bool x64 = m.cpu_is_64bit();
    usize i = 0;
    auto next = [&]() -> u64 { return m.arg(i++); };
    const Handle h = static_cast<Handle>(next());
    const u64 id = x64 ? next() : ((next() & 0xFFFFFFFFu) | ((next() & 0xFFFFFFFFu) << 32));
    const u64 ov_raw = next();
    const GuestAddr ov = static_cast<GuestAddr>(x64 ? ov_raw : ov_raw & 0xFFFFFFFFu);
    std::vector<Completion> done;
    {
        std::lock_guard lock(s.mu);
        auto q = s.queue_for(h);
        if (!q) { m.set_return(kErrorInvalidHandle); return; }
        auto it = q->requests.find(id);
        if (it == q->requests.end()) { m.set_return(kErrorConnectionInvalid); return; }
        // The request's connection is reset ("cancel-request: rc=0 ... err=10054").
        s.abort_request_locked(it->second);
        if (ov) done.push_back({h, 0, ov, x64, kStatusSuccess, NO_ERROR_, true});
    }
    s.publish(done);
    m.set_return(NO_ERROR_);
}

namespace {

u32 wait_disconnect(HttpApiManager::State& s, Marshaler& m, bool ex) {
    // HttpWaitForDisconnect(HANDLE, HTTP_CONNECTION_ID, LPOVERLAPPED)
    // HttpWaitForDisconnectEx(HANDLE, HTTP_CONNECTION_ID, ULONG Reserved, LPOVERLAPPED)
    const bool x64 = m.cpu_is_64bit();
    usize i = 0;
    auto next = [&]() -> u64 { return m.arg(i++); };
    auto w = std::make_shared<Waiter>();
    w->kind = Waiter::Kind::Disconnect;
    w->handle = static_cast<Handle>(next());
    w->connection_id = x64 ? next() : ((next() & 0xFFFFFFFFu) | ((next() & 0xFFFFFFFFu) << 32));
    const u32 reserved = ex ? static_cast<u32>(next()) : 0;
    const u64 ov = next();
    w->overlapped = static_cast<GuestAddr>(x64 ? ov : ov & 0xFFFFFFFFu);
    w->is_64bit = x64;
    if (!s.memory) s.memory = &m.memory();
    std::unique_lock lock(s.mu);
    auto q = s.queue_for(w->handle);
    if (!q) return kErrorInvalidHandle;
    if (reserved != 0) return kErrorInvalidParameter;
    const auto conn = std::find_if(s.connections.begin(), s.connections.end(),
                                   [&](const auto& c) { return c->id == w->connection_id; });
    if (conn == s.connections.end()) return kErrorConnectionInvalid;
    s.disconnect_waiters.push_back(w);
    if (w->overlapped) {
        put_guest_ptr(*s.memory, w->overlapped, kStatusPending, x64);
        return kErrorIoPending;
    }
    s.cv.wait(lock, [&] { return w->done || s.stopping || q->closed; });
    if (!w->done) {
        std::erase(s.disconnect_waiters, w);
        return kErrorOperationAborted;
    }
    return w->error;
}

} // namespace

void HttpApiManager::HttpWaitForDisconnect(Marshaler& m) {
    m.set_return(wait_disconnect(*m_state, m, false));
}

void HttpApiManager::HttpWaitForDisconnectEx(Marshaler& m) {
    m.set_return(wait_disconnect(*m_state, m, true));
}

// =============================================================================
// Teardown
// =============================================================================

void HttpApiManager::shutdown_all() {
    State& s = *m_state;
    std::vector<Completion> done;
    {
        std::unique_lock lock(s.mu);
        s.stopping = true;
        for (auto& [key, ep] : s.endpoints) {
            ep->stop = true;
            {
                std::lock_guard ep_lock(ep->mu);
                for (int fd : ep->fds) ::close(fd);
                ep->fds.clear();
            }
            const char b = 1;
            (void)::write(ep->wake[1], &b, 1);
        }
        s.endpoints.clear();
        for (auto& c : s.connections) {
            c->aborted = true;
            if (!c->finished) (void)::shutdown(c->fd, SHUT_RDWR);
        }
        for (auto& [h, q] : s.queues) {
            for (auto& w : q->waiters) s.deliver(done, w, kErrorOperationAborted, kStatusCancelled, 0, false);
            q->waiters.clear();
        }
        s.cv.notify_all();
        s.threads_cv.wait_for(lock, std::chrono::seconds(5), [&] { return s.threads == 0; });
    }
    s.publish(done);
}

} // namespace vitreo::winapi
