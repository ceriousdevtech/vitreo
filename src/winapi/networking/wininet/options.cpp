// SPDX-License-Identifier: LGPL-2.1-only
/// @file wininet.cpp
/// @brief wininet.dll — WinINet implementation with NSURLSession delegation

#include "wininet.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

/// @file options.cpp
/// @brief Options
///
/// Closed cut from wininet.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

#include "winapi/networking/winhttp/http_auth_constants.inc"

// =============================================================================
// Options
// =============================================================================

void WinInetManager::internet_set_option_a(Marshaler& m) {
    // BOOL InternetSetOptionA(HINTERNET hInternet, DWORD dwOption,
    //     LPVOID lpBuffer, DWORD dwBufferLength)
    // A and W have the same failure contract for options not backed by this
    // runtime. Never accept settings that are not applied to transport.
    internet_set_option_w(m);
}

void WinInetManager::internet_set_option_w(Marshaler& m) {
    const u64 handle = m.arg(0);
    const u32 option = static_cast<u32>(m.arg(1));
    if (handle != 0 && (handle > UINT32_MAX || !is_known_handle(static_cast<u32>(handle)))) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (option == http_contract::INTERNET_OPTION_HANDLE_TYPE ||
        option == http_contract::INTERNET_OPTION_VERSION) {
        set_last_error(http_contract::ERROR_INTERNET_OPTION_NOT_SETTABLE /* ERROR_INTERNET_OPTION_NOT_SETTABLE */);
        m.set_return(0);
        return;
    }
    if (option == http_contract::INTERNET_OPTION_CONNECT_TIMEOUT ||
        option == http_contract::INTERNET_OPTION_SEND_TIMEOUT ||
        option == http_contract::INTERNET_OPTION_RECEIVE_TIMEOUT) {
        if (m.arg_ptr(2) == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (static_cast<u32>(m.arg(3)) != sizeof(u32)) {
            set_last_error(http_contract::ERROR_INTERNET_BAD_OPTION_LENGTH /* ERROR_INTERNET_BAD_OPTION_LENGTH */);
            m.set_return(0);
            return;
        }
    }
    static std::mutex lock;
    static std::set<u32> reported;
    {
        std::lock_guard guard(lock);
        if (reported.insert(option).second) std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED wininet!InternetSetOptionA/W: option %u\n",option);
    }
    set_last_error(http_contract::ERROR_INTERNET_INVALID_OPTION /* ERROR_INTERNET_INVALID_OPTION */);
    m.set_return(0);
}

// WinINet option constants
static constexpr u32 INTERNET_OPTION_CONNECT_TIMEOUT  = 2;
static constexpr u32 INTERNET_OPTION_SEND_TIMEOUT     = 5;
static constexpr u32 INTERNET_OPTION_RECEIVE_TIMEOUT  = 6;
static constexpr u32 INTERNET_OPTION_HANDLE_TYPE      = 9;
static constexpr u32 INTERNET_OPTION_VERSION           = 40;
static constexpr u32 INTERNET_OPTION_USER_AGENT        = 41;
static constexpr u32 INTERNET_OPTION_CONNECTED_STATE   = 50;

static const char* inet_option_name(u32 opt) {
    switch (opt) {
    case INTERNET_OPTION_CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
    case INTERNET_OPTION_SEND_TIMEOUT:    return "SEND_TIMEOUT";
    case INTERNET_OPTION_RECEIVE_TIMEOUT: return "RECEIVE_TIMEOUT";
    case INTERNET_OPTION_HANDLE_TYPE:     return "HANDLE_TYPE";
    case INTERNET_OPTION_VERSION:         return "VERSION";
    case INTERNET_OPTION_USER_AGENT:      return "USER_AGENT";
    case INTERNET_OPTION_CONNECTED_STATE: return "CONNECTED_STATE";
    default: return nullptr;
    }
}

/// Helper: write a u32 option value to guest buffer with size check
static bool write_u32_option(Marshaler& m, GuestAddr buf_addr, GuestAddr len_ptr,
                              u32 value, u32 option, const char* tag) {
    if (!buf_addr) {
        if (len_ptr) (void)m.write_u32(len_ptr, 4);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        VITREO_TRACE("[wininet] %s(%s) -> FALSE (null buf, need 4)\n", tag, inet_option_name(option) ? inet_option_name(option) : "?");
        m.set_return(0);
        return false;
    }
    u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
    if (caller_len < 4) {
        if (len_ptr) (void)m.write_u32(len_ptr, 4);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return false;
    }
    (void)m.write_u32(buf_addr, value);
    if (len_ptr) (void)m.write_u32(len_ptr, 4);
    return true;
}


// Handle-scoped string/value options shared by the A and W entry points.
// Returns true when @p option was answered.
static bool query_handle_option(Marshaler& m, u32 option, bool wide, bool known_request,
                                const std::string& url, const std::string& agent,
                                bool known_handle, u64 context) {
    const GuestAddr buf_addr = m.arg_ptr(2);
    const GuestAddr len_ptr = m.arg_ptr(3);
    auto string_result = [&](const std::string& text) {
        const u32 unit = wide ? 2u : 1u;
        const u32 needed = static_cast<u32>((text.size() + 1) * unit);
        const u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
        if (!buf_addr || caller_len < needed) {
            if (len_ptr) (void)m.write_u32(len_ptr, needed);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        if (wide) (void)m.write_wide_string(buf_addr, text);
        else (void)m.write_ansi_string(buf_addr, text);
        (void)m.write_u32(len_ptr, needed - unit);
        set_last_error(0);
        m.set_return(1);
    };
    if (option == http_contract::INTERNET_OPTION_URL && known_request) {
        string_result(url);
        return true;
    }
    if (option == http_contract::INTERNET_OPTION_USER_AGENT && known_handle) {
        string_result(agent);
        return true;
    }
    if (option == http_contract::INTERNET_OPTION_CONTEXT_VALUE && known_handle) {
        const u32 pointer = m.cpu_is_64bit() ? 8u : 4u;
        const u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
        if (!buf_addr || caller_len < pointer) {
            if (len_ptr) (void)m.write_u32(len_ptr, pointer);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return true;
        }
        if (pointer == 8) (void)m.write_u64(buf_addr, context);
        else (void)m.write_u32(buf_addr, static_cast<u32>(context));
        (void)m.write_u32(len_ptr, pointer);
        set_last_error(0);
        m.set_return(1);
        return true;
    }
    return false;
}

void WinInetManager::internet_query_option_a(Marshaler& m) {
    // BOOL InternetQueryOptionA(HINTERNET hInternet, DWORD dwOption,
    //     LPVOID lpBuffer, LPDWORD lpdwBufferLength)
    u32 option = static_cast<u32>(m.arg(1));
    GuestAddr buf_addr = m.arg_ptr(2);
    GuestAddr len_ptr = m.arg_ptr(3);
    {
        const u32 handle = static_cast<u32>(m.arg(0));
        std::string url, agent;
        u64 context = 0;
        bool known_request = false, known = false;
        {
            std::lock_guard lock(m_mutex);
            known = is_known_handle(handle);
            if (auto r = m_requests.find(handle); r != m_requests.end()) {
                known_request = true;
                url = request_url(handle);
                context = r->second.context;
            }
            agent = agent_for(handle);
        }
        if (query_handle_option(m, option, false, known_request, url, agent, known, context)) return;
    }

    switch (option) {
    case INTERNET_OPTION_CONNECTED_STATE: {
        // INTERNET_STATE_CONNECTED = 1
        if (write_u32_option(m, buf_addr, len_ptr, 1, option, "InternetQueryOptionA")) {
            VITREO_TRACE("[wininet] InternetQueryOptionA(CONNECTED_STATE) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_VERSION: {
        // INTERNET_VERSION_INFO: {major=1, minor=2}
        if (!buf_addr) {
            if (len_ptr) (void)m.write_u32(len_ptr, 8);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
        if (caller_len < 8) {
            if (len_ptr) (void)m.write_u32(len_ptr, 8);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        (void)m.write_u32(buf_addr + 0, 1); // dwMajorVersion
        (void)m.write_u32(buf_addr + 4, 2); // dwMinorVersion
        if (len_ptr) (void)m.write_u32(len_ptr, 8);
        VITREO_TRACE("[wininet] InternetQueryOptionA(VERSION) -> TRUE (1.2)\n");
        m.set_return(1);
        return;
    }
    case INTERNET_OPTION_USER_AGENT: {
        const char* ua = "Vitreo/1.0";
        u32 needed = static_cast<u32>(std::strlen(ua) + 1);
        if (!buf_addr) {
            if (len_ptr) (void)m.write_u32(len_ptr, needed);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
        if (caller_len < needed) {
            if (len_ptr) (void)m.write_u32(len_ptr, needed);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        (void)m.write_ansi_string(buf_addr, ua);
        if (len_ptr) (void)m.write_u32(len_ptr, needed - 1); // excluding null
        VITREO_TRACE("[wininet] InternetQueryOptionA(USER_AGENT) -> TRUE\n");
        m.set_return(1);
        return;
    }
    case INTERNET_OPTION_CONNECT_TIMEOUT: {
        if (write_u32_option(m, buf_addr, len_ptr, 60000, option, "InternetQueryOptionA")) {
            VITREO_TRACE("[wininet] InternetQueryOptionA(CONNECT_TIMEOUT) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_SEND_TIMEOUT: {
        if (write_u32_option(m, buf_addr, len_ptr, 30000, option, "InternetQueryOptionA")) {
            VITREO_TRACE("[wininet] InternetQueryOptionA(SEND_TIMEOUT) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_RECEIVE_TIMEOUT: {
        if (write_u32_option(m, buf_addr, len_ptr, 30000, option, "InternetQueryOptionA")) {
            VITREO_TRACE("[wininet] InternetQueryOptionA(RECEIVE_TIMEOUT) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_HANDLE_TYPE: {
        // Default: INTERNET_HANDLE_TYPE_INTERNET = 1
        if (write_u32_option(m, buf_addr, len_ptr, 1, option, "InternetQueryOptionA")) {
            VITREO_TRACE("[wininet] InternetQueryOptionA(HANDLE_TYPE) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    default: {
        const char* name = inet_option_name(option);
        if (name) {
            VITREO_TRACE("[wininet] InternetQueryOptionA(%s) -> FALSE (unsupported)\n", name);
        } else {
            VITREO_TRACE("[wininet] InternetQueryOptionA(opt=%u) -> FALSE (unsupported)\n", option);
        }
        m.set_return(0);
        return;
    }
    }
}

void WinInetManager::internet_query_option_w(Marshaler& m) {
    // BOOL InternetQueryOptionW(HINTERNET hInternet, DWORD dwOption,
    //     LPVOID lpBuffer, LPDWORD lpdwBufferLength)
    u32 option = static_cast<u32>(m.arg(1));
    GuestAddr buf_addr = m.arg_ptr(2);
    GuestAddr len_ptr = m.arg_ptr(3);
    {
        const u32 handle = static_cast<u32>(m.arg(0));
        std::string url, agent;
        u64 context = 0;
        bool known_request = false, known = false;
        {
            std::lock_guard lock(m_mutex);
            known = is_known_handle(handle);
            if (auto r = m_requests.find(handle); r != m_requests.end()) {
                known_request = true;
                url = request_url(handle);
                context = r->second.context;
            }
            agent = agent_for(handle);
        }
        if (query_handle_option(m, option, true, known_request, url, agent, known, context)) return;
    }

    switch (option) {
    case INTERNET_OPTION_CONNECTED_STATE: {
        if (write_u32_option(m, buf_addr, len_ptr, 1, option, "InternetQueryOptionW")) {
            VITREO_TRACE("[wininet] InternetQueryOptionW(CONNECTED_STATE) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_VERSION: {
        if (!buf_addr) {
            if (len_ptr) (void)m.write_u32(len_ptr, 8);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
        if (caller_len < 8) {
            if (len_ptr) (void)m.write_u32(len_ptr, 8);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        (void)m.write_u32(buf_addr + 0, 1);
        (void)m.write_u32(buf_addr + 4, 2);
        if (len_ptr) (void)m.write_u32(len_ptr, 8);
        VITREO_TRACE("[wininet] InternetQueryOptionW(VERSION) -> TRUE (1.2)\n");
        m.set_return(1);
        return;
    }
    case INTERNET_OPTION_USER_AGENT: {
        const std::string ua = "Vitreo/1.0";
        // Wide: each char = 2 bytes + null terminator (2 bytes)
        u32 needed_bytes = static_cast<u32>((ua.size() + 1) * 2);
        if (!buf_addr) {
            if (len_ptr) (void)m.write_u32(len_ptr, needed_bytes);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        u32 caller_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
        if (caller_len < needed_bytes) {
            if (len_ptr) (void)m.write_u32(len_ptr, needed_bytes);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        (void)m.write_wide_string(buf_addr, ua);
        if (len_ptr) (void)m.write_u32(len_ptr, static_cast<u32>(ua.size() * 2));
        VITREO_TRACE("[wininet] InternetQueryOptionW(USER_AGENT) -> TRUE\n");
        m.set_return(1);
        return;
    }
    case INTERNET_OPTION_CONNECT_TIMEOUT: {
        if (write_u32_option(m, buf_addr, len_ptr, 60000, option, "InternetQueryOptionW")) {
            VITREO_TRACE("[wininet] InternetQueryOptionW(CONNECT_TIMEOUT) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_SEND_TIMEOUT: {
        if (write_u32_option(m, buf_addr, len_ptr, 30000, option, "InternetQueryOptionW")) {
            VITREO_TRACE("[wininet] InternetQueryOptionW(SEND_TIMEOUT) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_RECEIVE_TIMEOUT: {
        if (write_u32_option(m, buf_addr, len_ptr, 30000, option, "InternetQueryOptionW")) {
            VITREO_TRACE("[wininet] InternetQueryOptionW(RECEIVE_TIMEOUT) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    case INTERNET_OPTION_HANDLE_TYPE: {
        if (write_u32_option(m, buf_addr, len_ptr, 1, option, "InternetQueryOptionW")) {
            VITREO_TRACE("[wininet] InternetQueryOptionW(HANDLE_TYPE) -> TRUE\n");
            m.set_return(1);
        }
        return;
    }
    default: {
        const char* name = inet_option_name(option);
        if (name) {
            VITREO_TRACE("[wininet] InternetQueryOptionW(%s) -> FALSE (unsupported)\n", name);
        } else {
            VITREO_TRACE("[wininet] InternetQueryOptionW(opt=%u) -> FALSE (unsupported)\n", option);
        }
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
