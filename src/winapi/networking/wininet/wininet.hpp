// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file wininet.hpp
/// @brief wininet.dll — WinINet API with NSURLSession delegation

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/networking/winhttp/url_components.hpp"

#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

class WinInetManager {
public:
    WinInetManager() = default;
    ~WinInetManager() = default;
    WinInetManager(const WinInetManager&) = delete;
    WinInetManager& operator=(const WinInetManager&) = delete;

    // Session / connection lifecycle
    void internet_open_a(Marshaler& m);
    void internet_open_w(Marshaler& m);
    void internet_connect_a(Marshaler& m);
    void internet_connect_w(Marshaler& m);
    void internet_close_handle(Marshaler& m);

    // HTTP request / response
    void http_open_request_a(Marshaler& m);
    void http_open_request_w(Marshaler& m);
    void http_send_request_a(Marshaler& m);
    void http_send_request_w(Marshaler& m);
    void http_send_request_ex_a(Marshaler& m);
    void http_end_request_a(Marshaler& m);
    void internet_error_dlg(Marshaler& m);
    void http_query_info_a(Marshaler& m);
    void http_query_info_w(Marshaler& m);
    void http_add_request_headers_a(Marshaler& m);
    void http_add_request_headers_w(Marshaler& m);

    // Reading / writing
    void internet_read_file(Marshaler& m);
    void internet_read_file_ex_a(Marshaler& m);
    void internet_write_file(Marshaler& m);
    void internet_query_data_available(Marshaler& m);

    // Options
    void internet_set_option_a(Marshaler& m);
    void internet_set_option_w(Marshaler& m);
    void internet_query_option_a(Marshaler& m);
    void internet_query_option_w(Marshaler& m);

    // URL helpers
    void internet_crack_url_a(Marshaler& m);
    void internet_crack_url_w(Marshaler& m);
    void internet_canonicalize_url_a(Marshaler& m);

    // Status / callback
    void internet_set_status_callback(Marshaler& m);
    void internet_set_cookie_w(Marshaler& m);
    void internet_get_connected_state(Marshaler& m);
    void internet_get_connected_state_ex_a(Marshaler& m);
    void internet_check_connection_a(Marshaler& m);
    void internet_attempt_connect(Marshaler& m);

    // Open URL
    void internet_open_url_a(Marshaler& m);
    void internet_open_url_w(Marshaler& m);

    // FTP (stubs)
    void ftp_open_file_a(Marshaler& m);

    // Error
    void internet_get_last_response_info_a(Marshaler& m);
    void internet_set_file_pointer(Marshaler& m);

    /// Process-heap (GlobalAlloc GMEM_FIXED) allocations the guest may keep.
    void set_guest_allocator(urlcrack::ScratchAlloc alloc) { m_guest_alloc = std::move(alloc); }

private:
    urlcrack::ScratchAlloc m_guest_alloc;
    u32 m_next_handle = 0xB0010000;

    u32 alloc_handle() { return m_next_handle++; }
    bool is_known_handle(u32 handle) const;
    bool read_response(Marshaler& m, u32 handle, GuestAddr buffer,
                       u32 capacity, u32& bytes_read);
    std::string cookie_header_for_url(std::string_view url) const;
    void attach_cookies(std::vector<std::pair<std::string, std::string>>& headers,
                        std::string_view url) const;
    /// Start the transport for @p handle and wait for the response headers
    /// (HttpSendRequest / HttpEndRequest). Returns the WinINet error or 0.
    u32 send_request(u32 handle, const std::string& extra_headers, bool wide,
                     const std::vector<u8>& body, u64 streamed_total, bool wait);
    u32 wait_response(u32 handle);
    void http_query_info(Marshaler& m, bool wide);
    void http_add_request_headers(Marshaler& m, bool wide);
    void http_send_request(Marshaler& m, bool wide);
    void internet_open_url(Marshaler& m, bool wide);
    std::string request_url(u32 handle) const;
    std::string agent_for(u32 handle) const;

    /// Guards every map below; never held across a transport wait.
    mutable std::mutex m_mutex;

    struct SessionInfo {
        std::string agent;  ///< lpszAgent: the User-Agent request header
    };
    std::unordered_map<u32, SessionInfo> m_sessions;
    std::unordered_map<u32, GuestAddr> m_status_callbacks;
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>> m_cookies;

    // Connection state: handle → {server, port}
    struct ConnInfo {
        std::string server;
        u16 port = 80;   ///< resolved at InternetConnect (0 -> service default)
        u32 session_handle = 0;
    };
    std::unordered_map<u32, ConnInfo> m_connections;

    struct RequestInfo {
        u32 conn_handle = 0;
        std::string method;
        std::string path;
        std::string url;  ///< full URL for InternetOpenUrl requests
        u32 session = 0;  ///< owning session of an InternetOpenUrl request
        std::vector<std::pair<std::string, std::string>> headers;
        u32 flags = 0;
        u64 context = 0;

        // After send
        bool sent = false;
        int  status_code = 0;
        std::unordered_map<std::string, std::string> response_headers;
        u64 total_length = 0;   ///< HttpSendRequestEx dwBufferTotal
        u64 written = 0;

        /// Streaming body; InternetReadFile drains it incrementally.
        std::shared_ptr<HttpStream> stream;
    };
    std::unordered_map<u32, RequestInfo> m_requests;
};

/// Create wininet.dll export thunks
DllExports create_wininet_thunks(WinInetManager& mgr);

} // namespace vitreo::winapi
