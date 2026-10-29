// SPDX-License-Identifier: LGPL-2.1-only
/// @file wininet.cpp
/// @brief wininet.dll — WinINet implementation with NSURLSession delegation

#include "wininet.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cctype>

/// @file session_connection_lifecycle.cpp
/// @brief Session / Connection Lifecycle
///
/// Closed cut from wininet.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Session / Connection Lifecycle
// =============================================================================

void WinInetManager::internet_open_a(Marshaler& m) {
    // HINTERNET InternetOpenA(LPCSTR lpszAgent, DWORD dwAccessType,
    //     LPCSTR lpszProxy, LPCSTR lpszProxyBypass, DWORD dwFlags)
    // lpszAgent is the User-Agent of every request on the session.
    std::lock_guard lock(m_mutex);
    const u32 handle = alloc_handle();
    SessionInfo info;
    if (const GuestAddr agent = m.arg_ptr(0)) info.agent = m.read_ansi_string(agent).value_or("");
    m_sessions.emplace(handle, std::move(info));
    m.set_return(handle);
}

void WinInetManager::internet_open_w(Marshaler& m) {
    // HINTERNET InternetOpenW(LPCWSTR lpszAgent, ...)
    std::lock_guard lock(m_mutex);
    const u32 handle = alloc_handle();
    SessionInfo info;
    if (const GuestAddr agent = m.arg_ptr(0)) info.agent = m.read_wide_string(agent).value_or("");
    m_sessions.emplace(handle, std::move(info));
    m.set_return(handle);
}

namespace {
#include "winapi/networking/winhttp/http_auth_constants.inc"

// INTERNET_INVALID_PORT_NUMBER (0) selects the service's default port at
// connect time (measured: a secure request on such a connection keeps :80).
u16 connect_port(u32 port, u32 service) {
    if (port != http_contract::INTERNET_INVALID_PORT_NUMBER) return static_cast<u16>(port);
    if (service == http_contract::INTERNET_SERVICE_FTP) return static_cast<u16>(http_contract::INTERNET_DEFAULT_FTP_PORT);
    if (service == http_contract::INTERNET_SERVICE_GOPHER) return static_cast<u16>(http_contract::INTERNET_DEFAULT_GOPHER_PORT);
    return static_cast<u16>(http_contract::INTERNET_DEFAULT_HTTP_PORT);
}
} // namespace

void WinInetManager::internet_connect_a(Marshaler& m) {
    // HINTERNET InternetConnectA(HINTERNET hInternet, LPCSTR lpszServerName,
    //     INTERNET_PORT nServerPort, LPCSTR lpszUserName, LPCSTR lpszPassword,
    //     DWORD dwService, DWORD dwFlags, DWORD_PTR dwContext)
    const u32 session = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_mutex);
    if (!m_sessions.contains(session)) {
        set_last_error(is_known_handle(session) ? http_contract::ERROR_INTERNET_INCORRECT_HANDLE_TYPE
                                                : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 handle = alloc_handle();
    ConnInfo ci;
    ci.session_handle = session;
    ci.port = connect_port(static_cast<u16>(m.arg(2)), m.arg32(5));
    if (const GuestAddr server = m.arg_ptr(1)) ci.server = m.read_ansi_string(server).value_or("");
    m_connections[handle] = std::move(ci);
    m.set_return(handle);
}

void WinInetManager::internet_connect_w(Marshaler& m) {
    const u32 session = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_mutex);
    if (!m_sessions.contains(session)) {
        set_last_error(is_known_handle(session) ? http_contract::ERROR_INTERNET_INCORRECT_HANDLE_TYPE
                                                : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 handle = alloc_handle();
    ConnInfo ci;
    ci.session_handle = session;
    ci.port = connect_port(static_cast<u16>(m.arg(2)), m.arg32(5));
    if (const GuestAddr server = m.arg_ptr(1)) ci.server = m.read_wide_string(server).value_or("");
    m_connections[handle] = std::move(ci);
    m.set_return(handle);
}

void WinInetManager::internet_close_handle(Marshaler& m) {
    // BOOL InternetCloseHandle(HINTERNET hInternet)
    const u32 h = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_mutex);
    if (!is_known_handle(h)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (auto r = m_requests.find(h); r != m_requests.end() && r->second.stream)
        r->second.stream->cancel();  // wakes a reader blocked on another thread
    m_sessions.erase(h);
    m_connections.erase(h);
    m_requests.erase(h);
    m_status_callbacks.erase(h);
    m.set_return(1);
}

} // namespace vitreo::winapi
