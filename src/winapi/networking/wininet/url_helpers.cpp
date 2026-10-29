// SPDX-License-Identifier: LGPL-2.1-only
/// @file wininet.cpp
/// @brief wininet.dll — WinINet implementation with NSURLSession delegation

#include "wininet.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/networking/winhttp/url_components.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cctype>

/// @file url_helpers.cpp
/// @brief URL Helpers
///
/// Closed cut from wininet.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// URL Helpers
// =============================================================================

void WinInetManager::internet_crack_url_a(Marshaler& m) {
    // BOOL InternetCrackUrlA(LPCSTR lpszUrl, DWORD dwUrlLength, DWORD dwFlags,
    //                        LPURL_COMPONENTSA lpUrlComponents)
    m.set_return(urlcrack::crack(m, urlcrack::Api::WinInetA, m.arg_ptr(0), m.arg32(1),
                                 m.arg32(2), m.arg_ptr(3), m_guest_alloc));
}

void WinInetManager::internet_crack_url_w(Marshaler& m) {
    // BOOL InternetCrackUrlW(LPCWSTR lpszUrl, DWORD dwUrlLength, DWORD dwFlags,
    //                        LPURL_COMPONENTSW lpUrlComponents)
    m.set_return(urlcrack::crack(m, urlcrack::Api::WinInetW, m.arg_ptr(0), m.arg32(1),
                                 m.arg32(2), m.arg_ptr(3), m_guest_alloc));
}

void WinInetManager::internet_canonicalize_url_a(Marshaler& m) {
    // BOOL InternetCanonicalizeUrlA(LPCSTR lpszUrl, LPSTR lpszBuffer,
    //     LPDWORD lpdwBufferLength, DWORD dwFlags)
    GuestAddr url_addr = m.arg_ptr(0);
    GuestAddr buf_addr = m.arg_ptr(1);
    GuestAddr len_ptr  = m.arg_ptr(2);
    // arg(3) = dwFlags (ignored for simple passthrough)

    if (!url_addr || !buf_addr || !len_ptr) {
        VITREO_TRACE("[wininet] InternetCanonicalizeUrlA -> FALSE (null)\n");
        m.set_return(0); return;
    }

    std::string url = m.read_ansi_string(url_addr).value_or("");
    u32 buf_len = m.read_u32(len_ptr).value_or(0);

    VITREO_TRACE("[wininet] InternetCanonicalizeUrlA: '%s' bufLen=%u\n",
                 url.c_str(), buf_len);

    u32 needed = static_cast<u32>(url.size() + 1); // including null
    if (buf_len < needed) {
        // Buffer too small — report needed size
        (void)m.write_u32(len_ptr, needed);
        VITREO_TRACE("[wininet] InternetCanonicalizeUrlA -> FALSE (buf too small, need %u)\n", needed);
        m.set_return(0); return;
    }

    // Simple passthrough: copy URL as-is (already canonical)
    (void)m.write_ansi_string(buf_addr, url);
    (void)m.write_u32(len_ptr, static_cast<u32>(url.size()));
    m.set_return(1);
}

} // namespace vitreo::winapi
