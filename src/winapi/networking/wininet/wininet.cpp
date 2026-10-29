// SPDX-License-Identifier: LGPL-2.1-only
/// @file wininet.cpp
/// @brief wininet.dll — WinINet implementation with NSURLSession delegation

#include "wininet.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/networking/winhttp/http_headers.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <optional>

namespace vitreo::winapi {

// =============================================================================
// WinINet Constants
// =============================================================================

#include "winapi/networking/winhttp/http_auth_constants.inc"

static constexpr u32 ERROR_INTERNET_INCORRECT_HANDLE_TYPE = http_contract::ERROR_INTERNET_INCORRECT_HANDLE_TYPE;
static constexpr u32 ERROR_INTERNET_INCORRECT_HANDLE_STATE = http_contract::ERROR_INTERNET_INCORRECT_HANDLE_STATE;
static constexpr u32 ERROR_INTERNET_CONNECTION_ABORTED = http_contract::ERROR_INTERNET_CONNECTION_ABORTED;

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string url_host(std::string_view url) {
    usize start = url.find("://");
    start = start == std::string_view::npos ? 0 : start + 3;
    usize end = url.find_first_of("/?#", start);
    std::string authority(url.substr(
        start, end == std::string_view::npos ? url.size() - start
                                             : end - start));
    if (const usize at = authority.rfind('@'); at != std::string::npos) {
        authority.erase(0, at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        if (const usize close = authority.find(']'); close != std::string::npos) {
            authority.resize(close + 1);
        }
    } else if (const usize colon = authority.rfind(':');
               colon != std::string::npos) {
        authority.resize(colon);
    }
    return ascii_lower(std::move(authority));
}

std::string trim_ascii(std::string value) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(value.front())) value.erase(value.begin());
    while (!value.empty() && is_space(value.back())) value.pop_back();
    return value;
}

} // namespace


// =============================================================================
// HTTP Request / Response
// =============================================================================
// Contract: Learn plus tools/oracle_probe/http_request_probe.wininet.expected
// (x64) / .wininet.x86.expected.

namespace {

u32 inet_transport_error(int code) {
    switch (code) {
    case -1001: return http_contract::ERROR_INTERNET_TIMEOUT;           // TimedOut
    case -1003: return 12007u;  // ERROR_INTERNET_NAME_NOT_RESOLVED (CannotFindHost)
    case -1004: return http_contract::ERROR_INTERNET_CANNOT_CONNECT;    // CannotConnectToHost
    case -1005: return http_contract::ERROR_INTERNET_CONNECTION_RESET;  // NetworkConnectionLost
    default: return http_contract::ERROR_INTERNET_CONNECTION_ABORTED;
    }
}

std::string read_block(Marshaler& m, GuestAddr address, u32 length, bool wide) {
    if (address == 0 || length == 0) return {};
    if (length == 0xFFFFFFFFu)
        return (wide ? m.read_wide_string(address) : m.read_ansi_string(address)).value_or("");
    return (wide ? m.read_wide_string(address, length) : m.read_ansi_string(address, length)).value_or("");
}

void apply_inet_header(std::vector<std::pair<std::string, std::string>>& list,
                       const http_headers::Line& line, u32 flags, http_headers::ApplyResult& result) {
    result = http_headers::apply(list, line, flags, true,
                                 http_contract::HTTP_ADDREQ_FLAG_ADD,
                                 http_contract::HTTP_ADDREQ_FLAG_ADD_IF_NEW,
                                 http_contract::HTTP_ADDREQ_FLAG_COALESCE,
                                 http_contract::HTTP_ADDREQ_FLAG_COALESCE_WITH_COMMA,
                                 http_contract::HTTP_ADDREQ_FLAG_COALESCE_WITH_SEMICOLON,
                                 http_contract::HTTP_ADDREQ_FLAG_REPLACE);
}

} // namespace

// Caller holds m_mutex (is_known_handle / cookie_header_for_url / agent_for /
// request_url are the table reads the locked entry points compose).
bool WinInetManager::is_known_handle(u32 handle) const {
    return m_sessions.contains(handle) ||
           m_connections.contains(handle) ||
           m_requests.contains(handle);
}

// Caller holds m_mutex.
std::string WinInetManager::cookie_header_for_url(std::string_view url) const {
    const auto it = m_cookies.find(url_host(url));
    if (it == m_cookies.end()) return {};

    std::string result;
    for (const auto& [name, value] : it->second) {
        if (!result.empty()) result.append("; ");
        result.append(name);
        result.push_back('=');
        result.append(value);
    }
    return result;
}

void WinInetManager::attach_cookies(
    std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view url) const {
    for (const auto& [name, _] : headers) {
        if (ascii_lower(name) == "cookie") return;
    }
    std::string cookie = cookie_header_for_url(url);
    if (!cookie.empty()) headers.emplace_back("Cookie", std::move(cookie));
}

// Caller holds m_mutex.
std::string WinInetManager::agent_for(u32 handle) const {
    u32 current = handle;
    if (auto r = m_requests.find(current); r != m_requests.end()) {
        if (r->second.session != 0) current = r->second.session;
        auto c = m_connections.find(r->second.conn_handle);
        if (r->second.session != 0) c = m_connections.end();
        if (r->second.session != 0) {
            auto s = m_sessions.find(current);
            return s == m_sessions.end() ? std::string() : s->second.agent;
        }
        if (c == m_connections.end()) return {};
        current = c->second.session_handle;
    } else if (auto c = m_connections.find(current); c != m_connections.end()) {
        current = c->second.session_handle;
    }
    auto s = m_sessions.find(current);
    return s == m_sessions.end() ? std::string() : s->second.agent;
}

// Caller holds m_mutex.
std::string WinInetManager::request_url(u32 handle) const {
    const auto r = m_requests.find(handle);
    if (r == m_requests.end()) return {};
    if (!r->second.url.empty()) return r->second.url;
    const auto c = m_connections.find(r->second.conn_handle);
    if (c == m_connections.end()) return {};
    // The port was fixed at InternetConnect; it is omitted only when it is
    // the default of the request's scheme (probe url-port0-secure keeps :80).
    const bool tls = (r->second.flags & http_contract::INTERNET_FLAG_SECURE) != 0;
    const u16 default_port = static_cast<u16>(tls ? http_contract::INTERNET_DEFAULT_HTTPS_PORT
                                                  : http_contract::INTERNET_DEFAULT_HTTP_PORT);
    std::string url = std::string(tls ? "https" : "http") + "://" + c->second.server;
    if (c->second.port != default_port) url += ":" + std::to_string(c->second.port);
    if (r->second.path.empty() || r->second.path[0] != '/') url += "/";
    url += r->second.path;
    return url;
}

void WinInetManager::http_open_request_a(Marshaler& m) {
    // HINTERNET HttpOpenRequestA(HINTERNET hConnect, LPCSTR lpszVerb,
    //     LPCSTR lpszObjectName, LPCSTR lpszVersion, LPCSTR lpszReferrer,
    //     LPCSTR *lplpszAcceptTypes, DWORD dwFlags, DWORD_PTR dwContext)
    // arg 5 is lplpszAcceptTypes; dwFlags is arg 6 and dwContext arg 7.
    const u32 conn = static_cast<u32>(m.arg(0));
    const GuestAddr verb_addr = m.arg_ptr(1);
    const GuestAddr path_addr = m.arg_ptr(2);
    std::lock_guard lock(m_mutex);
    if (!m_connections.contains(conn)) {
        set_last_error(m.arg(0) == 0 || !is_known_handle(conn) ? error_codes::ERROR_INVALID_HANDLE
                                                                : ERROR_INTERNET_INCORRECT_HANDLE_TYPE);
        m.set_return(0);
        return;
    }
    const u32 handle = alloc_handle();
    RequestInfo ri;
    ri.conn_handle = conn;
    ri.flags = m.arg32(6);
    ri.context = m.arg(7);
    ri.method = verb_addr ? m.read_ansi_string(verb_addr).value_or("GET") : "GET";
    if (ri.method.empty()) ri.method = "GET";
    ri.path = path_addr ? m.read_ansi_string(path_addr).value_or("/") : "/";
    m_requests[handle] = std::move(ri);
    m.set_return(handle);
}

void WinInetManager::http_open_request_w(Marshaler& m) {
    // HINTERNET HttpOpenRequestW(HINTERNET hConnect, LPCWSTR lpszVerb,
    //     LPCWSTR lpszObjectName, LPCWSTR lpszVersion, LPCWSTR lpszReferrer,
    //     LPCWSTR *lplpszAcceptTypes, DWORD dwFlags, DWORD_PTR dwContext)
    const u32 conn = static_cast<u32>(m.arg(0));
    const GuestAddr verb_addr = m.arg_ptr(1);
    const GuestAddr path_addr = m.arg_ptr(2);
    std::lock_guard lock(m_mutex);
    if (!m_connections.contains(conn)) {
        set_last_error(m.arg(0) == 0 || !is_known_handle(conn) ? error_codes::ERROR_INVALID_HANDLE
                                                                : ERROR_INTERNET_INCORRECT_HANDLE_TYPE);
        m.set_return(0);
        return;
    }
    const u32 handle = alloc_handle();
    RequestInfo ri;
    ri.conn_handle = conn;
    ri.flags = m.arg32(6);
    ri.context = m.arg(7);
    ri.method = verb_addr ? m.read_wide_string(verb_addr).value_or("GET") : "GET";
    if (ri.method.empty()) ri.method = "GET";
    ri.path = path_addr ? m.read_wide_string(path_addr).value_or("/") : "/";
    m_requests[handle] = std::move(ri);
    m.set_return(handle);
}

u32 WinInetManager::send_request(u32 handle, const std::string& extra_headers, bool /*wide*/,
                                 const std::vector<u8>& body, u64 streamed_total, bool wait) {
    std::shared_ptr<HttpStream> stream;
    {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(handle);
        if (rit == m_requests.end())
            return is_known_handle(handle) ? ERROR_INTERNET_INCORRECT_HANDLE_TYPE
                                           : error_codes::ERROR_INVALID_HANDLE;
        auto& ri = rit->second;
        if (!extra_headers.empty()) {
            // Measured (sendw-bad-header): a malformed header block fails
            // with ERROR_HTTP_HEADER_NOT_FOUND.
            const auto lines = http_headers::parse_block(extra_headers, true);
            if (!lines) return http_contract::ERROR_HTTP_HEADER_NOT_FOUND;
            for (const auto& line : *lines) {
                http_headers::ApplyResult ignored;
                apply_inet_header(ri.headers, line, http_contract::HTTP_ADDREQ_FLAG_ADD, ignored);
            }
        }
        const std::string url = request_url(handle);
        auto headers = ri.headers;
        const std::string agent = agent_for(handle);
        if (!agent.empty() && http_headers::find(headers, "User-Agent") == headers.end())
            headers.emplace_back("User-Agent", agent);
        attach_cookies(headers, url);
        stream = std::make_shared<HttpStream>();
        if (!stream->start(ri.method, url, headers, body, false, streamed_total))
            return inet_transport_error(stream->error_code());
        ri.stream = stream;
        ri.sent = false;
        ri.status_code = 0;
        ri.response_headers.clear();
        ri.total_length = streamed_total;
        ri.written = body.size();
    }
    return wait ? wait_response(handle) : 0;
}

u32 WinInetManager::wait_response(u32 handle) {
    std::shared_ptr<HttpStream> stream;
    {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(handle);
        if (rit == m_requests.end() || !rit->second.stream) return ERROR_INTERNET_INCORRECT_HANDLE_STATE;
        stream = rit->second.stream;
        if (rit->second.total_length != 0 && rit->second.written == rit->second.total_length)
            stream->finish_body();
    }
    // HttpSendRequest is synchronous: it returns once the response headers
    // arrived (or the request failed).
    const bool ok = stream->wait_headers(60'000);
    std::lock_guard lock(m_mutex);
    auto rit = m_requests.find(handle);
    if (rit == m_requests.end()) return ERROR_INTERNET_CONNECTION_ABORTED;
    if (!ok) {
        const int code = stream->error_code();
        stream->cancel();
        return code != 0 ? inet_transport_error(code) : http_contract::ERROR_INTERNET_TIMEOUT;
    }
    rit->second.sent = true;
    rit->second.status_code = stream->status_code();
    rit->second.response_headers = stream->headers();
    return 0;
}

void WinInetManager::http_send_request(Marshaler& m, bool wide) {
    // BOOL HttpSendRequest(HINTERNET hRequest, LPCxSTR lpszHeaders,
    //     DWORD dwHeadersLength, LPVOID lpOptional, DWORD dwOptionalLength)
    const u32 h = static_cast<u32>(m.arg(0));
    const std::string block = read_block(m, m.arg_ptr(1), m.arg32(2), wide);
    const GuestAddr body_addr = m.arg_ptr(3);
    const u32 body_len = m.arg32(4);
    std::vector<u8> body;
    if (body_addr && body_len > 0) {
        body.resize(body_len);
        (void)m.memory().read_bytes(body_addr, body.data(), body_len);
    }
    const u32 error = send_request(h, block, wide, body, 0, true);
    if (error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    set_last_error(0);
    m.set_return(1);
}

void WinInetManager::http_send_request_a(Marshaler& m) { http_send_request(m, false); }
void WinInetManager::http_send_request_w(Marshaler& m) { http_send_request(m, true); }

void WinInetManager::http_send_request_ex_a(Marshaler& m) {
    // BOOL HttpSendRequestExA(HINTERNET hRequest, LPINTERNET_BUFFERSA lpBuffersIn,
    //     LPINTERNET_BUFFERSA lpBuffersOut, DWORD dwFlags, DWORD_PTR dwContext)
    // INTERNET_BUFFERSA: dwStructSize, Next, lpcszHeader, dwHeadersLength,
    // dwHeadersTotal, lpvBuffer, dwBufferLength, dwBufferTotal, dwOffsetLow,
    // dwOffsetHigh (wininet.h). The request starts with Content-Length =
    // dwBufferTotal; InternetWriteFile supplies the rest and HttpEndRequest
    // waits for the response.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr in = m.arg_ptr(1);
    const bool is64 = m.cpu_is_64bit();
    const u32 p = is64 ? 8u : 4u;
    // Offsets by natural alignment: D P P D D P D D D D.
    const u32 off_header = is64 ? 16u : 8u, off_header_len = is64 ? 24u : 12u,
              off_buffer = is64 ? 32u : 20u, off_buffer_len = is64 ? 40u : 24u,
              off_total = is64 ? 44u : 28u;
    (void)p;
    std::string block;
    std::vector<u8> body;
    u32 total = 0;
    if (in != 0) {
        const GuestAddr header = is64 ? m.read_u64(in + off_header).value_or(0) : m.read_u32(in + off_header).value_or(0);
        block = read_block(m, header, m.read_u32(in + off_header_len).value_or(0), false);
        const GuestAddr buffer = is64 ? m.read_u64(in + off_buffer).value_or(0) : m.read_u32(in + off_buffer).value_or(0);
        const u32 buffer_len = m.read_u32(in + off_buffer_len).value_or(0);
        total = m.read_u32(in + off_total).value_or(0);
        if (buffer && buffer_len) {
            body.resize(buffer_len);
            (void)m.memory().read_bytes(buffer, body.data(), buffer_len);
        }
    }
    if (total < body.size()) total = static_cast<u32>(body.size());
    const u32 error = send_request(h, block, false, body, total > body.size() ? total : 0,
                                   /*wait=*/total <= body.size() ? false : false);
    if (error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    set_last_error(0);
    m.set_return(1);
}

void WinInetManager::http_end_request_a(Marshaler& m) {
    // HttpEndRequestA/W — finalize a request started with HttpSendRequestEx.
    // Learn: lpBuffersOut is reserved and must be NULL; a non-NULL pointer
    // fails with ERROR_INVALID_PARAMETER. dwFlags and dwContext are reserved
    // and must be 0.
    const u32 req_h = static_cast<u32>(m.arg(0));
    const GuestAddr buffers = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const u64 context = m.arg(3);
    if (buffers != 0 || flags != 0 || context != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(req_h);
        if (rit == m_requests.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (!rit->second.stream) {
            set_last_error(ERROR_INTERNET_INCORRECT_HANDLE_STATE);
            m.set_return(0);
            return;
        }
    }
    const u32 error = wait_response(req_h);
    if (error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void WinInetManager::internet_error_dlg(Marshaler& m) {
    // DWORD InternetErrorDlg(HWND, HINTERNET, DWORD dwError, DWORD dwFlags, LPVOID*)
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/wininet/nf-wininet-interneterrordlg
    // Flag values from the Windows SDK wininet.h (1, 2, 4, 8).
    constexpr u32 FILTER_FOR_ERRORS = 0x01;
    constexpr u32 NO_UI = 0x08;
    constexpr u32 KNOWN_FLAGS = 0x01 | 0x02 | 0x04 | 0x08;
    constexpr u32 ERROR_SUCCESS_VALUE = 0;
    constexpr u32 ERROR_INVALID_HANDLE_VALUE = 6;
    constexpr u32 ERROR_CANCELLED_VALUE = 1223;
    constexpr u32 CLIENT_AUTH_CERT_NEEDED = 12044; // INTERNET_ERROR_BASE+44

    const GuestAddr hwnd = m.arg_ptr(0);
    const u32 request = static_cast<u32>(m.arg(1));
    const u32 dw_error = m.arg32(2);
    const u32 flags = m.arg32(3);

    if ((flags & ~KNOWN_FLAGS) != 0) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED wininet!InternetErrorDlg: flags 0x%x\n",
                     flags);
        m.set_return(ERROR_CANCELLED_VALUE);
        return;
    }
    if (dw_error == CLIENT_AUTH_CERT_NEEDED) {
        // Learn: this error always returns ERROR_SUCCESS.
        m.set_return(ERROR_SUCCESS_VALUE);
        return;
    }

    bool needs_dialog = dw_error != 0;
    if ((flags & FILTER_FOR_ERRORS) != 0) {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(request);
        if (rit == m_requests.end() || !rit->second.sent) {
            m.set_return(ERROR_INVALID_HANDLE_VALUE);
            return;
        }
        const int status = rit->second.status_code;
        if (status == 401 || status == 407) needs_dialog = true;
    }
    if (!needs_dialog) {
        m.set_return(ERROR_SUCCESS_VALUE);
        return;
    }
    if (hwnd == 0 && (flags & NO_UI) == 0) {
        m.set_return(ERROR_INVALID_HANDLE_VALUE);
        return;
    }
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED wininet!InternetErrorDlg: error %u\n",
                 dw_error);
    m.set_return(ERROR_CANCELLED_VALUE);
}

void WinInetManager::http_query_info(Marshaler& m, bool wide) {
    // BOOL HttpQueryInfo(HINTERNET hRequest, DWORD dwInfoLevel,
    //     LPVOID lpBuffer, LPDWORD lpdwBufferLength, LPDWORD lpdwIndex)
    // HTTP_QUERY_CUSTOM passes the header name in lpBuffer. Measured: string
    // results report the length without the terminator (A: bytes = chars,
    // W: bytes); a short buffer fails with 122 and the required size
    // including the terminator; W with a NULL buffer fails with 87; a
    // FLAG_NUMBER buffer shorter than 4 fails with 122 and length 4; a
    // missing header or index fails with 12150 leaving length and index;
    // success sets ERROR_SUCCESS and advances the index.
    const u32 h = static_cast<u32>(m.arg(0));
    const u32 info_level = m.arg32(1);
    const GuestAddr buf = m.arg_ptr(2);
    const GuestAddr len_ptr = m.arg_ptr(3);
    const GuestAddr index_ptr = m.arg_ptr(4);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    std::lock_guard lock(m_mutex);
    auto rit = m_requests.find(h);
    if (rit == m_requests.end())
        return fail(is_known_handle(h) ? ERROR_INTERNET_INCORRECT_HANDLE_TYPE : error_codes::ERROR_INVALID_HANDLE);
    const auto& ri = rit->second;
    if (len_ptr == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
    const u32 query = info_level & 0xFFFFu;
    const bool request_headers = (info_level & http_contract::HTTP_QUERY_FLAG_REQUEST_HEADERS) != 0;
    const bool as_number = (info_level & http_contract::HTTP_QUERY_FLAG_NUMBER) != 0;
    const bool as_time = (info_level & http_contract::HTTP_QUERY_FLAG_SYSTEMTIME) != 0;
    const char* level_name = nullptr;
    for (const auto& level : http_contract::wininet_query_levels)
        if (level.level == query) level_name = level.name;
    if (query != http_contract::HTTP_QUERY_CUSTOM &&
        (level_name == nullptr || query > http_contract::HTTP_QUERY_MAX))
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (!request_headers && !ri.sent) return fail(ERROR_INTERNET_INCORRECT_HANDLE_STATE);
    if (wide && buf == 0 && !as_number && !as_time) return fail(error_codes::ERROR_INVALID_PARAMETER);

    std::string header;
    if (query == http_contract::HTTP_QUERY_CUSTOM) {
        if (buf == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        header = (wide ? m.read_wide_string(buf) : m.read_ansi_string(buf)).value_or("");
        if (header.empty()) return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    const u32 index = index_ptr != 0 ? m.read_u32(index_ptr).value_or(0) : 0;
    std::optional<std::string> value;
    const std::string level = level_name ? level_name : "";
    if (query != http_contract::HTTP_QUERY_CUSTOM && level == "STATUS_CODE") {
        if (index == 0 && !request_headers) value = std::to_string(ri.status_code);
    } else if (level == "REQUEST_METHOD") {
        if (index == 0) value = ri.method;
    } else if (level == "VERSION" || level == "STATUS_TEXT" || level == "RAW_HEADERS" ||
               level == "RAW_HEADERS_CRLF" || level.rfind("ECHO_", 0) == 0) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set())
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED wininet!HttpQueryInfo: status line / raw header levels\n");
        return fail(http_contract::ERROR_INTERNET_INTERNAL_ERROR);
    } else {
        if (header.empty()) header = http_headers::header_name_for_level(level_name);
        if (request_headers) {
            u32 seen = 0;
            for (const auto& [k, v] : ri.headers)
                if (http_headers::iequals(k, header) && seen++ == index) { value = v; break; }
        } else if (index == 0) {
            // NSHTTPURLResponse merges repeated fields: only occurrence 0.
            if (auto it = ri.response_headers.find(ascii_lower(header)); it != ri.response_headers.end())
                value = it->second;
        }
    }
    if (!value) return fail(http_contract::ERROR_HTTP_HEADER_NOT_FOUND);

    const u32 supplied = m.read_u32(len_ptr).value_or(0);
    if (as_number) {
        if (buf == 0 || supplied < 4) {
            (void)m.write_u32(len_ptr, 4);
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        (void)m.write_u32(buf, http_headers::leading_number(*value));
        (void)m.write_u32(len_ptr, 4);
    } else if (as_time) {
        if (buf == 0 || supplied < 16) {
            (void)m.write_u32(len_ptr, 16);
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        const auto time = http_headers::parse_http_date(*value);
        if (!time) return fail(http_contract::ERROR_HTTP_INVALID_QUERY_REQUEST);
        const u16 fields[8] = {time->year, time->month, time->day_of_week, time->day,
                               time->hour, time->minute, time->second, time->milliseconds};
        for (int i = 0; i < 8; ++i) (void)m.write_u16(buf + static_cast<GuestAddr>(i) * 2, fields[i]);
        (void)m.write_u32(len_ptr, 16);
    } else {
        const u32 unit = wide ? 2u : 1u;
        const u64 required = (static_cast<u64>(value->size()) + 1) * unit;
        if (buf == 0 || supplied < required) {
            (void)m.write_u32(len_ptr, static_cast<u32>(required));
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        if (wide) (void)m.write_wide_string(buf, *value);
        else (void)m.write_ansi_string(buf, *value);
        (void)m.write_u32(len_ptr, static_cast<u32>(required - unit));
    }
    if (index_ptr != 0) (void)m.write_u32(index_ptr, index + 1);
    set_last_error(0);
    m.set_return(1);
}

void WinInetManager::http_query_info_a(Marshaler& m) { http_query_info(m, false); }
void WinInetManager::http_query_info_w(Marshaler& m) { http_query_info(m, true); }

void WinInetManager::http_add_request_headers(Marshaler& m, bool wide) {
    // BOOL HttpAddRequestHeaders(HINTERNET hRequest, LPCxSTR lpszHeaders,
    //     DWORD dwHeadersLength, DWORD dwModifiers)
    // Measured (http_request_probe wininet): NULL or empty text or a line
    // without ':' -> 87; ADD_IF_NEW on an existing name -> 12155; REPLACE of a
    // missing name adds it; values are trimmed.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr text = m.arg_ptr(1);
    const std::string block = read_block(m, text, m.arg32(2), wide);
    const u32 flags = m.arg32(3) & http_contract::HTTP_ADDREQ_FLAGS_MASK;
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    std::lock_guard lock(m_mutex);
    auto rit = m_requests.find(h);
    if (rit == m_requests.end())
        return fail(is_known_handle(h) ? ERROR_INTERNET_INCORRECT_HANDLE_TYPE : error_codes::ERROR_INVALID_HANDLE);
    if (text == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
    const auto lines = http_headers::parse_block(block, true);
    if (!lines) return fail(error_codes::ERROR_INVALID_PARAMETER);
    for (const auto& line : *lines) {
        http_headers::ApplyResult result;
        apply_inet_header(rit->second.headers, line, flags, result);
        if (result == http_headers::ApplyResult::AlreadyExists)
            return fail(http_contract::ERROR_HTTP_HEADER_ALREADY_EXISTS);
    }
    set_last_error(0);
    m.set_return(1);
}

void WinInetManager::http_add_request_headers_a(Marshaler& m) { http_add_request_headers(m, false); }
void WinInetManager::http_add_request_headers_w(Marshaler& m) { http_add_request_headers(m, true); }

// =============================================================================
// Reading / Writing
// =============================================================================

bool WinInetManager::read_response(Marshaler& m, u32 req_h,
                                   GuestAddr buf_addr, u32 to_read,
                                   u32& bytes_read) {
    // Synchronous InternetReadFile fills the buffer or reaches the end of the
    // response; a transfer that fails first returns FALSE and no bytes
    // (measured: /reset -> ERROR_INTERNET_CONNECTION_RESET, 0 bytes).
    bytes_read = 0;
    std::shared_ptr<HttpStream> stream;
    {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(req_h);
        if (rit == m_requests.end()) {
            set_last_error(is_known_handle(req_h) ? ERROR_INTERNET_INCORRECT_HANDLE_TYPE
                                                  : error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (!rit->second.sent || !rit->second.stream) {
            set_last_error(ERROR_INTERNET_INCORRECT_HANDLE_STATE);
            return false;
        }
        stream = rit->second.stream;
    }
    if (to_read != 0 && buf_addr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    std::vector<u8> tmp(to_read);
    u32 filled = 0;
    while (filled < to_read) {
        const int got = stream->read(tmp.data() + filled, to_read - filled, 60'000);
        if (got < 0) {
            const int code = stream->error_code();
            set_last_error(code != 0 ? inet_transport_error(code) : http_contract::ERROR_INTERNET_TIMEOUT);
            return false;
        }
        if (got == 0) break;
        filled += static_cast<u32>(got);
    }
    if (filled > 0 && m.memory().write_bytes(buf_addr, tmp.data(), filled).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    bytes_read = filled;
    return true;
}

void WinInetManager::internet_read_file(Marshaler& m) {
    // BOOL InternetReadFile(HINTERNET, LPVOID, DWORD, LPDWORD)
    const u32 req_h = static_cast<u32>(m.arg(0));
    const GuestAddr buf_addr = m.arg_ptr(1);
    const u32 to_read = m.arg32(2);
    const GuestAddr bytes_read_ptr = m.arg_ptr(3);
    u32 bytes_read = 0;
    if (!read_response(m, req_h, buf_addr, to_read, bytes_read)) {
        if (bytes_read_ptr) (void)m.write_u32(bytes_read_ptr, 0);
        m.set_return(0);
        return;
    }
    if (bytes_read_ptr) (void)m.write_u32(bytes_read_ptr, bytes_read);
    m.set_return(1);
}

void WinInetManager::internet_read_file_ex_a(Marshaler& m) {
    // BOOL InternetReadFileExA(HINTERNET, LPINTERNET_BUFFERSA, DWORD, DWORD_PTR)
    const u32 req_h = static_cast<u32>(m.arg(0));
    const GuestAddr buffers = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    constexpr u32 IRF_ASYNC = 0x00000001;

    if (buffers == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if ((flags & IRF_ASYNC) != 0) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    const bool is64 = m.cpu_is_64bit();
    const u32 expected_size = is64 ? 56u : 40u;
    const u32 buffer_ptr_off = is64 ? 32u : 20u;
    const u32 buffer_len_off = is64 ? 40u : 24u;
    const u32 struct_size = m.read_u32(buffers).value_or(0);
    if (struct_size < expected_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    GuestAddr output = is64
        ? m.read_u64(buffers + buffer_ptr_off).value_or(0)
        : m.read_u32(buffers + buffer_ptr_off).value_or(0);
    const u32 capacity = m.read_u32(buffers + buffer_len_off).value_or(0);
    u32 bytes_read = 0;
    if (!read_response(m, req_h, output, capacity, bytes_read)) {
        (void)m.write_u32(buffers + buffer_len_off, 0);
        m.set_return(0);
        return;
    }
    (void)m.write_u32(buffers + buffer_len_off, bytes_read);
    m.set_return(1);
}

void WinInetManager::internet_write_file(Marshaler& m) {
    // BOOL InternetWriteFile(HINTERNET hFile, LPCVOID lpBuffer,
    //     DWORD dwNumberOfBytesToWrite, LPDWORD lpdwNumberOfBytesWritten)
    // Writes the body of a request started with HttpSendRequestEx; anything
    // else (FTP files) has no transport and fails.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 to_write = m.arg32(2);
    const GuestAddr written_ptr = m.arg_ptr(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    if (written_ptr) (void)m.write_u32(written_ptr, 0);
    std::shared_ptr<HttpStream> stream;
    {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(h);
        if (rit == m_requests.end())
            return fail(is_known_handle(h) ? ERROR_INTERNET_INCORRECT_HANDLE_TYPE : error_codes::ERROR_INVALID_HANDLE);
        auto& ri = rit->second;
        if (!ri.stream || ri.total_length == 0 || ri.sent) return fail(ERROR_INTERNET_INCORRECT_HANDLE_STATE);
        if (ri.written + to_write > ri.total_length) return fail(error_codes::ERROR_INVALID_PARAMETER);
        ri.written += to_write;
        stream = ri.stream;
    }
    std::vector<u8> bytes(to_write);
    if (to_write != 0 &&
        (buffer == 0 || m.memory().read_bytes(buffer, bytes.data(), bytes.size()).is_err()))
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (to_write != 0 && !stream->write_body(bytes.data(), bytes.size()))
        return fail(inet_transport_error(stream->error_code()));
    if (written_ptr) (void)m.write_u32(written_ptr, to_write);
    m.set_return(1);
}

void WinInetManager::internet_query_data_available(Marshaler& m) {
    // BOOL InternetQueryDataAvailable(HINTERNET hFile,
    //     LPDWORD lpdwNumberOfBytesAvailable, DWORD dwFlags, DWORD_PTR dwContext)
    // Waits for data or the end of the response. Measured: a nonzero result
    // leaves ERROR_IO_PENDING as the last error, the end (0) leaves it alone.
    const u32 req_h = static_cast<u32>(m.arg(0));
    const GuestAddr bytes_ptr = m.arg_ptr(1);
    std::shared_ptr<HttpStream> stream;
    {
        std::lock_guard lock(m_mutex);
        auto rit = m_requests.find(req_h);
        if (rit == m_requests.end() || !rit->second.sent || !rit->second.stream) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        stream = rit->second.stream;
    }
    std::size_t available = 0;
    if (!stream->wait_available(60'000, available)) {
        const int code = stream->error_code();
        set_last_error(code != 0 ? inet_transport_error(code) : http_contract::ERROR_INTERNET_TIMEOUT);
        m.set_return(0);
        return;
    }
    const u32 avail = static_cast<u32>(std::min<std::size_t>(available, 0x7FFFFFFFu));
    if (bytes_ptr) (void)m.write_u32(bytes_ptr, avail);
    if (avail != 0) set_last_error(997u /* ERROR_IO_PENDING */);
    m.set_return(1);
}

// =============================================================================
// Status / Connectivity
// =============================================================================

void WinInetManager::internet_set_status_callback(Marshaler& m) {
    // INTERNET_STATUS_CALLBACK InternetSetStatusCallback(HINTERNET hInternet,
    //     INTERNET_STATUS_CALLBACK lpfnInternetCallback)
    const u32 handle = static_cast<u32>(m.arg(0));
    const GuestAddr callback = m.arg_ptr(1);
    std::lock_guard lock(m_mutex);
    if (!is_known_handle(handle)) {
        // INTERNET_INVALID_STATUS_CALLBACK is (INTERNET_STATUS_CALLBACK)-1L:
        // every bit of the pointer.
        set_last_error(ERROR_INTERNET_INCORRECT_HANDLE_TYPE);
        m.set_return(m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull);
        return;
    }
    const auto it = m_status_callbacks.find(handle);
    const GuestAddr previous =
        it == m_status_callbacks.end() ? 0 : it->second;
    if (callback == 0) {
        m_status_callbacks.erase(handle);
    } else {
        m_status_callbacks[handle] = callback;
    }
    VITREO_TRACE("[wininet] InternetSetStatusCallback(0x%x, 0x%llx)"
                 " -> 0x%llx\n", handle,
                 static_cast<unsigned long long>(callback),
                 static_cast<unsigned long long>(previous));
    m.set_return(previous);
}

void WinInetManager::internet_set_cookie_w(Marshaler& m) {
    // BOOL InternetSetCookieW(LPCWSTR url, LPCWSTR name, LPCWSTR data)
    const GuestAddr url_ptr = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr data_ptr = m.arg_ptr(2);
    if (url_ptr == 0 || data_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const std::string url = m.read_wide_string(url_ptr).value_or("");
    std::string name =
        name_ptr == 0 ? "" : m.read_wide_string(name_ptr).value_or("");
    std::string data = m.read_wide_string(data_ptr).value_or("");
    const std::string host = url_host(url);
    if (url.empty() || host.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const usize attributes = data.find(';');
    std::string first =
        trim_ascii(data.substr(0, attributes == std::string::npos
                                     ? data.size()
                                     : attributes));
    std::string value;
    if (name.empty()) {
        const usize equals = first.find('=');
        if (equals == std::string::npos || equals == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        name = trim_ascii(first.substr(0, equals));
        value = trim_ascii(first.substr(equals + 1));
    } else {
        value = std::move(first);
    }
    if (name.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    std::lock_guard lock(m_mutex);
    m_cookies[host][name] = value;
    VITREO_TRACE("[wininet] InternetSetCookieW(%s, %s) -> TRUE\n",
                 host.c_str(), name.c_str());
    m.set_return(1);
}

void WinInetManager::internet_get_connected_state(Marshaler& m) {
    // BOOL InternetGetConnectedState(LPDWORD lpdwFlags, DWORD dwReserved)
    GuestAddr flags_ptr = m.arg_ptr(0);
    if (flags_ptr != 0) {
        // INTERNET_CONNECTION_LAN = 0x02
        (void)m.write_u32(flags_ptr, 0x02);
    }
    VITREO_TRACE("[wininet] InternetGetConnectedState -> TRUE (LAN)\n");
    m.set_return(1);
}

void WinInetManager::internet_get_connected_state_ex_a(Marshaler& m) {
    // BOOL InternetGetConnectedStateExA(LPDWORD lpdwFlags,
    //     LPSTR lpszConnectionName, DWORD dwNameLen, DWORD dwReserved)
    GuestAddr flags_ptr = m.arg_ptr(0);
    if (flags_ptr != 0) {
        (void)m.write_u32(flags_ptr, 0x02);
    }
    VITREO_TRACE("[wininet] InternetGetConnectedStateExA -> TRUE (LAN)\n");
    m.set_return(1);
}

void WinInetManager::internet_check_connection_a(Marshaler& m) {
    // BOOL InternetCheckConnectionA(LPCSTR lpszUrl, DWORD dwFlags, DWORD dwReserved)
    VITREO_TRACE("[wininet] InternetCheckConnectionA -> TRUE\n");
    m.set_return(1);
}

void WinInetManager::internet_attempt_connect(Marshaler& m) {
    // DWORD InternetAttemptConnect(DWORD dwReserved)
    // Returns ERROR_SUCCESS (0) on success
    VITREO_TRACE("[wininet] InternetAttemptConnect -> 0 (success)\n");
    m.set_return(0);
}


// =============================================================================
// FTP (Stubs)
// =============================================================================

void WinInetManager::ftp_open_file_a(Marshaler& m) {
    // HINTERNET FtpOpenFileA(...)
    VITREO_TRACE("[wininet] FtpOpenFileA -> NULL (stub)\n");
    m.set_return(0);
}

// =============================================================================
// Error / Misc
// =============================================================================

void WinInetManager::internet_get_last_response_info_a(Marshaler& m) {
    // BOOL InternetGetLastResponseInfoA(LPDWORD lpdwError,
    //     LPSTR lpszBuffer, LPDWORD lpdwBufferLength)
    GuestAddr err_ptr = m.arg_ptr(0);
    GuestAddr len_ptr = m.arg_ptr(2);
    if (err_ptr != 0) (void)m.write_u32(err_ptr, 0);
    if (len_ptr != 0) (void)m.write_u32(len_ptr, 0);
    VITREO_TRACE("[wininet] InternetGetLastResponseInfoA -> FALSE\n");
    m.set_return(0);
}

void WinInetManager::internet_set_file_pointer(Marshaler& m) {
    // DWORD InternetSetFilePointer(HINTERNET hFile, LONG lDistanceToMove,
    //     PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod, DWORD_PTR dwContext)
    VITREO_TRACE("[wininet] InternetSetFilePointer -> -1 (stub)\n");
    m.set_return(0xFFFFFFFF); // INVALID_SET_FILE_POINTER
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_InternetOpenA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_open_a(m);
}
static void impl_InternetOpenW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_open_w(m);
}
static void impl_InternetConnectA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_connect_a(m);
}
static void impl_InternetConnectW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_connect_w(m);
}
static void impl_InternetCloseHandle(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_close_handle(m);
}
static void impl_HttpOpenRequestA(Marshaler& m, WinInetManager& mgr) {
    mgr.http_open_request_a(m);
}
static void impl_HttpOpenRequestW(Marshaler& m, WinInetManager& mgr) {
    mgr.http_open_request_w(m);
}
static void impl_HttpSendRequestA(Marshaler& m, WinInetManager& mgr) {
    mgr.http_send_request_a(m);
}
static void impl_HttpSendRequestW(Marshaler& m, WinInetManager& mgr) {
    mgr.http_send_request_w(m);
}
static void impl_HttpSendRequestExA(Marshaler& m, WinInetManager& mgr) {
    mgr.http_send_request_ex_a(m);
}
static void impl_HttpEndRequestA(Marshaler& m, WinInetManager& mgr) {
    mgr.http_end_request_a(m);
}
static void impl_HttpQueryInfoA(Marshaler& m, WinInetManager& mgr) {
    mgr.http_query_info_a(m);
}
static void impl_HttpQueryInfoW(Marshaler& m, WinInetManager& mgr) {
    mgr.http_query_info_w(m);
}
static void impl_HttpAddRequestHeadersA(Marshaler& m, WinInetManager& mgr) {
    mgr.http_add_request_headers_a(m);
}
static void impl_HttpAddRequestHeadersW(Marshaler& m, WinInetManager& mgr) {
    mgr.http_add_request_headers_w(m);
}
static void impl_InternetReadFile(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_read_file(m);
}
static void impl_InternetReadFileExA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_read_file_ex_a(m);
}
static void impl_InternetWriteFile(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_write_file(m);
}
static void impl_InternetQueryDataAvailable(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_query_data_available(m);
}
static void impl_InternetSetOptionA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_set_option_a(m);
}
static void impl_InternetSetOptionW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_set_option_w(m);
}
static void impl_InternetQueryOptionA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_query_option_a(m);
}
static void impl_InternetQueryOptionW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_query_option_w(m);
}
static void impl_InternetCrackUrlA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_crack_url_a(m);
}
static void impl_InternetCrackUrlW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_crack_url_w(m);
}
static void impl_InternetCanonicalizeUrlA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_canonicalize_url_a(m);
}
static void impl_InternetSetStatusCallback(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_set_status_callback(m);
}
static void impl_InternetSetCookieW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_set_cookie_w(m);
}
static void impl_InternetGetConnectedState(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_get_connected_state(m);
}
static void impl_InternetGetConnectedStateExA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_get_connected_state_ex_a(m);
}
static void impl_InternetCheckConnectionA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_check_connection_a(m);
}
static void impl_InternetAttemptConnect(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_attempt_connect(m);
}
static void impl_InternetOpenUrlA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_open_url_a(m);
}
static void impl_InternetOpenUrlW(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_open_url_w(m);
}
static void impl_FtpOpenFileA(Marshaler& m, WinInetManager& mgr) {
    mgr.ftp_open_file_a(m);
}
static void impl_InternetGetLastResponseInfoA(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_get_last_response_info_a(m);
}
static void impl_InternetSetFilePointer(Marshaler& m, WinInetManager& mgr) {
    mgr.internet_set_file_pointer(m);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_wininet_thunks(WinInetManager& mgr) {
    DllExports exports;
    exports.entries.reserve(38);

    exports.entries.push_back(ExportEntry{
        "InternetOpenA", 1,
        [&mgr](Marshaler& m) { impl_InternetOpenA(m, mgr); },
        20 // 5 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetOpenW", 2,
        [&mgr](Marshaler& m) { impl_InternetOpenW(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "InternetConnectA", 3,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        impl_InternetConnectA(m, mgr); },
        32 // 8 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetConnectW", 4,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        impl_InternetConnectW(m, mgr); },
        32
    });
    exports.entries.push_back(ExportEntry{
        "InternetCloseHandle", 5,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        impl_InternetCloseHandle(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "HttpOpenRequestA", 6,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(0x2F8Cu /*ERROR_INTERNET_INCORRECT_HANDLE_TYPE*/); m.set_return(0); return; }
        impl_HttpOpenRequestA(m, mgr); },
        32 // 8 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpOpenRequestW", 7,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(0x2F8Cu /*ERROR_INTERNET_INCORRECT_HANDLE_TYPE*/); m.set_return(0); return; }
        impl_HttpOpenRequestW(m, mgr); },
        32
    });
    exports.entries.push_back(ExportEntry{
        "HttpSendRequestA", 8,
        [&mgr](Marshaler& m) { impl_HttpSendRequestA(m, mgr); },
        20 // 5 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpSendRequestW", 9,
        [&mgr](Marshaler& m) { impl_HttpSendRequestW(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "HttpSendRequestExA", 10,
        [&mgr](Marshaler& m) { impl_HttpSendRequestExA(m, mgr); },
        20 // 5 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpEndRequestA", 11,
        [&mgr](Marshaler& m) { impl_HttpEndRequestA(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpEndRequestW", 0,
        [&mgr](Marshaler& m) { impl_HttpEndRequestA(m, mgr); },
        16 // 4 args; W differs only in the unused buffers type
    });
    exports.entries.push_back(ExportEntry{
        "InternetErrorDlg", 0,
        [&mgr](Marshaler& m) { mgr.internet_error_dlg(m); },
        20 // 5 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpQueryInfoA", 12,
        [&mgr](Marshaler& m) { impl_HttpQueryInfoA(m, mgr); },
        20 // 5 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpQueryInfoW", 13,
        [&mgr](Marshaler& m) { impl_HttpQueryInfoW(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "HttpAddRequestHeadersA", 14,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(12172u); m.set_return(0); return; }
        impl_HttpAddRequestHeadersA(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "HttpAddRequestHeadersW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        impl_HttpAddRequestHeadersW(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetReadFile", 15,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        impl_InternetReadFile(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetReadFileExA", 0,
        [&mgr](Marshaler& m) { impl_InternetReadFileExA(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetWriteFile", 16,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        impl_InternetWriteFile(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetQueryDataAvailable", 17,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        impl_InternetQueryDataAvailable(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetSetOptionA", 18,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        impl_InternetSetOptionA(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetSetOptionW", 19,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        impl_InternetSetOptionW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetQueryOptionA", 20,
        [&mgr](Marshaler& m) { impl_InternetQueryOptionA(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetQueryOptionW", 21,
        [&mgr](Marshaler& m) { impl_InternetQueryOptionW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetCrackUrlA", 22,
        [&mgr](Marshaler& m) { impl_InternetCrackUrlA(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetCrackUrlW", 23,
        [&mgr](Marshaler& m) { impl_InternetCrackUrlW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetCanonicalizeUrlA", 24,
        [&mgr](Marshaler& m) { impl_InternetCanonicalizeUrlA(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "InternetSetStatusCallbackA", 25,
        [&mgr](Marshaler& m) { impl_InternetSetStatusCallback(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "InternetSetStatusCallbackW", 0,
        [&mgr](Marshaler& m) { impl_InternetSetStatusCallback(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "InternetSetCookieW", 0,
        [&mgr](Marshaler& m) { impl_InternetSetCookieW(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "InternetGetConnectedState", 26,
        [&mgr](Marshaler& m) { impl_InternetGetConnectedState(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "InternetGetConnectedStateExA", 27,
        [&mgr](Marshaler& m) { impl_InternetGetConnectedStateExA(m, mgr); },
        16 // 4 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetCheckConnectionA", 28,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(2250u); m.set_return(0); return; }
        impl_InternetCheckConnectionA(m, mgr); },
        12 // 3 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetAttemptConnect", 29,
        [&mgr](Marshaler& m) { impl_InternetAttemptConnect(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "InternetOpenUrlA", 30,
        [&mgr](Marshaler& m) { impl_InternetOpenUrlA(m, mgr); },
        24 // 6 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetOpenUrlW", 31,
        [&mgr](Marshaler& m) { impl_InternetOpenUrlW(m, mgr); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "FtpOpenFileA", 32,
        [&mgr](Marshaler& m) { impl_FtpOpenFileA(m, mgr); },
        20 // 5 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetGetLastResponseInfoA", 33,
        [&mgr](Marshaler& m) { impl_InternetGetLastResponseInfoA(m, mgr); },
        12 // 3 args
    });
    exports.entries.push_back(ExportEntry{
        "InternetSetFilePointer", 34,
        [&mgr](Marshaler& m) { impl_InternetSetFilePointer(m, mgr); },
        20 // 5 args
    });

    return exports;
}

} // namespace vitreo::winapi
