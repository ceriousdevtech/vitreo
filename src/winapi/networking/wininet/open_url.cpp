// SPDX-License-Identifier: LGPL-2.1-only
/// @file wininet.cpp
/// @brief wininet.dll — WinINet implementation with NSURLSession delegation

#include "wininet.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cctype>

/// @file open_url.cpp
/// @brief Open URL
///
/// Closed cut from wininet.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Open URL
// =============================================================================

void WinInetManager::internet_open_url(Marshaler& m, bool wide) {
    // HINTERNET InternetOpenUrl(HINTERNET hInternet, LPCxSTR lpszUrl,
    //     LPCxSTR lpszHeaders, DWORD dwHeadersLength, DWORD dwFlags,
    //     DWORD_PTR dwContext)
    // The response body is streamed: InternetReadFile drains it.
    const u32 session = static_cast<u32>(m.arg(0));
    const GuestAddr url_addr = m.arg_ptr(1);
    if (!url_addr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string url = (wide ? m.read_wide_string(url_addr) : m.read_ansi_string(url_addr)).value_or("");
    const GuestAddr headers_addr = m.arg_ptr(2);
    const u32 headers_len = m.arg32(3);
    std::string block;
    if (headers_addr && headers_len)
        block = (headers_len == 0xFFFFFFFFu
                     ? (wide ? m.read_wide_string(headers_addr) : m.read_ansi_string(headers_addr))
                     : (wide ? m.read_wide_string(headers_addr, headers_len)
                             : m.read_ansi_string(headers_addr, headers_len))).value_or("");
    u32 handle = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_sessions.contains(session)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        handle = alloc_handle();
        RequestInfo ri;
        ri.method = "GET";
        ri.url = url;
        ri.flags = m.arg32(4);
        ri.context = m.arg(5);
        ri.session = session;
        m_requests[handle] = std::move(ri);
    }
    const u32 error = send_request(handle, block, wide, {}, 0, true);
    if (error != 0) {
        std::lock_guard lock(m_mutex);
        m_requests.erase(handle);
        set_last_error(error);
        m.set_return(0);
        return;
    }
    m.set_return(handle);
}

void WinInetManager::internet_open_url_a(Marshaler& m) { internet_open_url(m, false); }
void WinInetManager::internet_open_url_w(Marshaler& m) { internet_open_url(m, true); }

} // namespace vitreo::winapi
