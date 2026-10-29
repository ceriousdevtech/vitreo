// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file http_bridge.hpp
/// @brief HTTP client using macOS NSURLSession.
///
/// Exposes two interfaces:
/// - `http_request`: synchronous (buffers whole body). Used by WinInet for
///   simple reads.
/// - `HttpStream`: streaming pull-based client matching the WinHTTP
///   state machine (SendRequest → ReceiveResponse → ReadData …). Headers
///   are delivered as soon as they arrive; body bytes flow into a
///   bounded buffer that the guest drains via ReadData.

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <utility>

namespace vitreo::winapi {

/// Result of an HTTP request
struct HttpResponse {
    int status_code = 0;                        ///< HTTP status code (200, 404, etc.)
    std::unordered_map<std::string, std::string> headers; ///< Response headers (lowercase keys)
    std::vector<uint8_t> body;                  ///< Response body
    bool success = false;                       ///< True if request completed
};

/// Perform a synchronous HTTP/HTTPS request using NSURLSession.
/// @param method  HTTP method ("GET", "POST", etc.)
/// @param url     Full URL (e.g. "https://example.com/path")
/// @param headers Request headers as key-value pairs
/// @param body    Request body (empty for GET)
/// @return HttpResponse with status, headers, and body
HttpResponse http_request(
    const std::string& method,
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers = {},
    const std::vector<uint8_t>& body = {});

// -----------------------------------------------------------------------------
// Streaming (WinHTTP / WinInet async-friendly) API
// -----------------------------------------------------------------------------

/// Opaque streaming HTTP connection. Thread-safe: background delegate
/// feeds the body queue, guest thread drains it via `read`.
///
/// Lifecycle:
///   1. `start()`           — kicks off the NSURLSession task. Non-blocking.
///   2. `wait_headers(ms)`  — blocks until response headers arrive or error.
///                            After return, `status_code()`, `headers()`,
///                            and `content_length()` are valid.
///   3. `available()`       — bytes currently buffered (drain with `read`).
///   4. `read(buf, cap, ms)`— copy up to `cap` bytes; blocks up to `ms` ms
///                            for more data. Returns 0 at EOF. -1 on error.
///   5. `cancel()`          — abort the request (safe from any thread).
class HttpStream {
public:
    HttpStream();
    ~HttpStream();

    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;

    /// Kick off the HTTP request. Returns true if the task was launched.
    /// @param headers  request headers in order; repeated names are sent as
    ///                 one comma-joined field (NSURLRequest has no way to emit
    ///                 duplicate header lines).
    /// @param streamed_total  when larger than body.size(), the request body
    ///                 is a stream of exactly this many bytes (Content-Length);
    ///                 the rest is supplied with write_body().
    bool start(const std::string& method,
               const std::string& url,
               const std::vector<std::pair<std::string, std::string>>& headers,
               const std::vector<uint8_t>& body,
               bool unbounded_transport = false,
               uint64_t streamed_total = 0);

    /// Append request-body bytes (streamed requests). Blocks until the
    /// transport has accepted them. False when the transfer failed.
    bool write_body(const void* data, std::size_t size);

    /// Signal the end of a streamed request body.
    void finish_body();

    /// NSURLErrorDomain code of the failure (0 while none).
    int error_code() const noexcept;

    /// Block until headers arrive (or error / timeout). ms<0 = wait forever.
    bool wait_headers(int timeout_ms);

    /// True once the server has responded with final headers.
    bool headers_ready() const noexcept;

    /// True once the full body has been received (or stream closed).
    bool finished() const noexcept;

    /// HTTP status code (valid after wait_headers).
    int status_code() const noexcept;

    /// Response headers (lowercase keys). Valid after wait_headers.
    const std::unordered_map<std::string, std::string>& headers() const noexcept;

    /// Declared content-length from response headers (-1 if unknown/chunked).
    long long content_length() const noexcept;

    /// Bytes currently buffered and ready to read.
    std::size_t available() const noexcept;

    /// Wait (up to @p timeout_ms, <0 forever) until at least one byte is
    /// buffered or the response ended. True with @p available = buffered
    /// bytes (0 at a clean end); false on timeout, cancellation or error.
    bool wait_available(int timeout_ms, std::size_t& available);

    /// Total bytes consumed via read() so far.
    std::size_t bytes_read() const noexcept;

    /// Block (up to `timeout_ms`) for at least one byte, then copy up to
    /// `cap` bytes into `dst`. Returns:
    ///   > 0: number of bytes copied
    ///   = 0: EOF (stream finished, no more data)
    ///   < 0: error
    int read(void* dst, std::size_t cap, int timeout_ms);

    /// Abort the request. Safe to call more than once, from any thread.
    void cancel();

    /// Last error message (empty on success).
    const std::string& error() const noexcept;

    // Public so the Obj-C delegate can access it; opaque to external users.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

/// Message-oriented WebSocket transport used by the WinHTTP WebSocket API.
/// The native task performs and validates the HTTP upgrade; callers can wait
/// for that result before exposing a WinHTTP WebSocket handle.
class WebSocketStream {
public:
    WebSocketStream();
    ~WebSocketStream();

    WebSocketStream(const WebSocketStream&) = delete;
    WebSocketStream& operator=(const WebSocketStream&) = delete;

    bool start(const std::string& url,
               const std::unordered_map<std::string, std::string>& headers);
    bool wait_open(int timeout_ms);
    int send(const void* data, std::size_t size, bool utf8, int timeout_ms);
    int receive(void* data, std::size_t capacity, std::size_t& bytes_read,
                bool& utf8, bool& final, bool& closed, int timeout_ms);
    int shutdown(std::uint16_t status, const void* reason,
                 std::size_t reason_size);
    void cancel();
    const std::string& error() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

// -----------------------------------------------------------------------------
// Proxy configuration and PAC evaluation (WinHTTP autoproxy on macOS)
// -----------------------------------------------------------------------------

/// The macOS system proxy settings (CFNetworkCopySystemProxySettings),
/// expressed in the Internet Explorer vocabulary WinHTTP reports.
struct SystemProxyConfig {
    bool auto_detect = false;          ///< ProxyAutoDiscoveryEnable
    std::string auto_config_url;       ///< ProxyAutoConfigURLString when enabled
    std::string proxy;                 ///< "http=h:p;https=h:p;ftp=h:p;socks=h:p"
    std::string bypass;                ///< ExceptionsList joined by ';' (+ "<local>")
};
SystemProxyConfig system_proxy_config();

/// One entry of a PAC result ("PROXY h:p", "HTTPS h:p", "SOCKS h:p", "DIRECT").
struct ProxyEntry {
    bool proxy = false;
    bool bypass = false;
    uint32_t scheme = 0;       ///< WinHTTP INTERNET_SCHEME (1 http, 2 https, 4 socks)
    std::string host;
    uint16_t port = 0;
    std::string text;          ///< host[:port] exactly as the script wrote it
};

enum class ProxyStatus {
    Ok,
    AutoDetectFailed,      ///< no WPAD script was discovered
    UnrecognizedPacScheme, ///< the PAC URL is not http: or https:
    DownloadFailed,        ///< the PAC file could not be fetched (or not 200)
    ScriptError,           ///< the script did not evaluate or lacks FindProxyForURL
};

struct ProxyQuery {
    std::string script_url;    ///< the url argument handed to FindProxyForURL
    std::string script_host;   ///< the host argument
    bool auto_detect = false;
    bool detect_dhcp = false;
    bool detect_dns = false;
    std::string config_url;    ///< empty when WINHTTP_AUTOPROXY_CONFIG_URL is not set
};

struct ProxyResolution {
    ProxyStatus status = ProxyStatus::Ok;
    std::vector<ProxyEntry> entries;
};

/// Discover/download the PAC script and evaluate FindProxyForURL with
/// JavaScriptCore. Blocking; call from a worker or the requesting thread.
ProxyResolution resolve_proxy(const ProxyQuery& query);

/// WPAD discovery: the PAC URL from DHCP option 252 and/or the DNS name
/// "wpad" (http://wpad/wpad.dat), in that order. Empty when none is found.
std::vector<std::string> discover_wpad_urls(bool dhcp, bool dns);

/// Parse a FindProxyForURL result string with the rules measured on Windows
/// (tools/oracle_probe/http_winhttp_handles_probe.expected).
std::vector<ProxyEntry> parse_pac_result(const std::string& result);

} // namespace vitreo::winapi
