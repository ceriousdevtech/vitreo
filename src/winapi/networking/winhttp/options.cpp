// SPDX-License-Identifier: LGPL-2.1-only
/// @file winhttp.cpp
/// @brief winhttp.dll — WinHTTP implementation with NSURLSession delegation
///
/// Implements WinHTTP API surface with real HTTP transport via NSURLSession.
/// Connection and request state is tracked so the multi-step WinHTTP flow
/// (Open → Connect → OpenRequest → SendRequest → ReceiveResponse → ReadData)
/// results in an actual HTTP request on macOS.

#include "winhttp.hpp"
#include "http_bridge.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

/// @file options.cpp
/// @brief Options
///
/// Closed cut from winhttp.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

#include "http_auth_constants.inc"

// =============================================================================
// Options
// =============================================================================

// WinHTTP option constants
static constexpr u32 WINHTTP_OPTION_CONNECT_TIMEOUT  = http_contract::WINHTTP_OPTION_CONNECT_TIMEOUT;
static constexpr u32 WINHTTP_OPTION_SEND_TIMEOUT     = http_contract::WINHTTP_OPTION_SEND_TIMEOUT;
static constexpr u32 WINHTTP_OPTION_RECEIVE_TIMEOUT  = http_contract::WINHTTP_OPTION_RECEIVE_TIMEOUT;
static constexpr u32 WINHTTP_OPTION_SECURITY_FLAGS   = http_contract::WINHTTP_OPTION_SECURITY_FLAGS;
static constexpr u32 WINHTTP_OPTION_URL              = http_contract::WINHTTP_OPTION_URL;
static constexpr u32 WINHTTP_OPTION_CONTEXT_VALUE    = http_contract::WINHTTP_OPTION_CONTEXT_VALUE;
static constexpr u32 WINHTTP_OPTION_DISABLE_FEATURE  = http_contract::WINHTTP_OPTION_DISABLE_FEATURE;
static constexpr u32 WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET = http_contract::WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET;

static const char* winhttp_option_name(u32 opt) {
    switch (opt) {
    case WINHTTP_OPTION_CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
    case WINHTTP_OPTION_SEND_TIMEOUT:    return "SEND_TIMEOUT";
    case WINHTTP_OPTION_RECEIVE_TIMEOUT: return "RECEIVE_TIMEOUT";
    case WINHTTP_OPTION_SECURITY_FLAGS:  return "SECURITY_FLAGS";
    case WINHTTP_OPTION_DISABLE_FEATURE: return "DISABLE_FEATURE";
    case WINHTTP_OPTION_URL:             return "URL";
    case WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET:
        return "UPGRADE_TO_WEB_SOCKET";
    default: return nullptr;
    }
}

void WinHttpManager::win_http_set_option(Marshaler& m) {
    // BOOL WinHttpSetOption(HINTERNET hInternet, DWORD dwOption,
    //     LPVOID lpBuffer, DWORD dwBufferLength)
    const u32 option = m.arg32(1);
    const u64 raw = m.arg(0);
    const u32 handle = static_cast<u32>(raw);
    const GuestAddr buf_addr = m.arg_ptr(2);
    const u32 buf_len = m.arg32(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    std::lock_guard lock(m_mutex);
    // NULL -> INCORRECT_HANDLE_TYPE, unknown -> INVALID_HANDLE
    // (http_winhttp_handles_probe set-option-null / -bogus).
    if (raw == 0) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(handle);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);

    if ((option == http_contract::WINHTTP_OPTION_RESOLVE_TIMEOUT ||
         option == WINHTTP_OPTION_CONNECT_TIMEOUT || option == WINHTTP_OPTION_SEND_TIMEOUT ||
         option == WINHTTP_OPTION_RECEIVE_TIMEOUT)) {
        // Avoid the former process-wide option bag: it did not affect DNS,
        // connection or send timing and leaked values across requests.
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set()) std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED winhttp!WinHttpSetOption: timeout phase option; use WinHttpSetTimeouts\n");
        return fail(http_contract::ERROR_WINHTTP_INTERNAL_ERROR);
    }

    if (option == WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET) {
        if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
        if (buf_addr != 0 || buf_len != 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        m_requests[handle].websocket_upgrade = true;
        set_last_error(0);
        m.set_return(1);
        return;
    }

    if (option == WINHTTP_OPTION_CONTEXT_VALUE) {
        // DWORD_PTR: a buffer shorter than a pointer fails with 122
        // (measured on x64 with 4 bytes; x86 accepts 4).
        const u32 pointer = m.cpu_is_64bit() ? 8u : 4u;
        if (buf_addr == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        if (buf_len < pointer) return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        object->context = pointer == 8 ? m.read_u64(buf_addr).value_or(0)
                                       : static_cast<u64>(m.read_u32(buf_addr).value_or(0));
        set_last_error(0);
        m.set_return(1);
        return;
    }

    if (buf_addr && buf_len > 0 && buf_len <= 65536) {
        std::vector<u8> data(buf_len);
        (void)m.memory().read_bytes(buf_addr, data.data(), buf_len);
        m_options[option] = std::move(data);
    }

    const char* name = winhttp_option_name(option);
    if (name) {
        VITREO_TRACE("[winhttp] WinHttpSetOption(%s, %u bytes) -> TRUE\n", name, buf_len);
    } else {
        VITREO_TRACE("[winhttp] WinHttpSetOption(opt=%u, %u bytes) -> TRUE\n", option, buf_len);
    }
    m.set_return(1);
}

void WinHttpManager::win_http_query_option(Marshaler& m) {
    // BOOL WinHttpQueryOption(HINTERNET hInternet, DWORD dwOption,
    //     LPVOID lpBuffer, LPDWORD lpdwBufferLength)
    u32 option = static_cast<u32>(m.arg(1));
    GuestAddr buf_addr = m.arg_ptr(2);
    GuestAddr buf_len_ptr = m.arg_ptr(3);

    // Find data: stored value or default
    const u8* data_ptr = nullptr;
    u32 data_size = 0;
    std::vector<u8> default_buf;

    const u64 raw = m.arg(0);
    const u32 handle = static_cast<u32>(raw);
    std::lock_guard lock(m_mutex);
    if (raw == 0) {
        set_last_error(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
        m.set_return(0);
        return;
    }
    Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(handle);
    if (!object) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (option == WINHTTP_OPTION_CONTEXT_VALUE) {
        const u32 pointer = m.cpu_is_64bit() ? 8u : 4u;
        default_buf.resize(pointer);
        std::memcpy(default_buf.data(), &object->context, pointer);
        data_ptr = default_buf.data();
        data_size = pointer;
    } else if (option == WINHTTP_OPTION_URL && object->kind == Kind::Request) {
        // The request URL, with the port only when it is not the scheme
        // default (http_request_probe url-*).
        const auto& ri = m_requests[handle];
        const auto conn = m_connections.find(ri.conn_handle);
        std::string url;
        if (conn != m_connections.end()) {
            const u16 default_port = static_cast<u16>(ri.tls ? http_contract::INTERNET_DEFAULT_HTTPS_PORT
                                                             : http_contract::INTERNET_DEFAULT_HTTP_PORT);
            const u16 port = conn->second.port == 0 ? default_port : conn->second.port;
            url = std::string(ri.tls ? "https" : "http") + "://" + conn->second.server;
            if (port != default_port) url += ":" + std::to_string(port);
            if (ri.path.empty() || ri.path[0] != '/') url += "/";
            url += ri.path;
        }
        std::u16string wide;
        for (unsigned char c : url) wide.push_back(c);
        default_buf.resize((wide.size() + 1) * 2, 0);
        std::memcpy(default_buf.data(), wide.data(), wide.size() * 2);
        data_ptr = default_buf.data();
        data_size = static_cast<u32>(default_buf.size());
    }
    const ReceiveTimeout* timeout = nullptr;
    if (auto request = m_requests.find(handle); request != m_requests.end())
        timeout = &request->second.receive_timeout;
    else if (auto session = m_sessions.find(handle); session != m_sessions.end())
        timeout = &session->second.receive_timeout;
    if (timeout && timeout->unbounded_transport && (option == http_contract::WINHTTP_OPTION_RESOLVE_TIMEOUT ||
        option == WINHTTP_OPTION_CONNECT_TIMEOUT || option == WINHTTP_OPTION_SEND_TIMEOUT ||
        option == WINHTTP_OPTION_RECEIVE_TIMEOUT)) {
        const unsigned index = option == http_contract::WINHTTP_OPTION_RESOLVE_TIMEOUT ? 0
            : option == WINHTTP_OPTION_CONNECT_TIMEOUT ? 1
            : option == WINHTTP_OPTION_SEND_TIMEOUT ? 2 : 3;
        const u32 value = timeout->requested[index];
        default_buf.resize(sizeof(value));
        std::memcpy(default_buf.data(), &value, sizeof(value));
        data_ptr = default_buf.data();
        data_size = sizeof(value);
    }

    auto it = m_options.find(option);
    if (data_ptr) {
        // Request/session timeout state above takes precedence.
    } else if (it != m_options.end()) {
        data_ptr = it->second.data();
        data_size = static_cast<u32>(it->second.size());
    } else {
        // Provide common defaults
        u32 default_u32 = 0;
        bool has_default = true;
        switch (option) {
        case WINHTTP_OPTION_CONNECT_TIMEOUT:  default_u32 = 60000; break;
        case WINHTTP_OPTION_SEND_TIMEOUT:     default_u32 = 30000; break;
        case WINHTTP_OPTION_RECEIVE_TIMEOUT:  default_u32 = 30000; break;
        case WINHTTP_OPTION_SECURITY_FLAGS:   default_u32 = 0;     break;
        default:
            has_default = false;
            break;
        }
        if (has_default) {
            default_buf.resize(4);
            std::memcpy(default_buf.data(), &default_u32, 4);
            data_ptr = default_buf.data();
            data_size = 4;
        }
        if (!has_default) {
            const char* name = winhttp_option_name(option);
            if (name) {
                VITREO_TRACE("[winhttp] WinHttpQueryOption(%s) -> FALSE (unknown)\n", name);
            } else {
                VITREO_TRACE("[winhttp] WinHttpQueryOption(opt=%u) -> FALSE (unknown)\n", option);
            }
            m.set_return(0);
            return;
        }
    }

    // If lpBuffer is null, return required size
    if (!buf_addr) {
        if (buf_len_ptr) (void)m.write_u32(buf_len_ptr, data_size);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        VITREO_TRACE("[winhttp] WinHttpQueryOption(opt=%u) -> FALSE (need %u bytes)\n", option, data_size);
        m.set_return(0);
        return;
    }

    // Check buffer size
    u32 caller_size = buf_len_ptr ? m.read_u32(buf_len_ptr).value_or(0) : 0;
    if (caller_size < data_size) {
        if (buf_len_ptr) (void)m.write_u32(buf_len_ptr, data_size);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        VITREO_TRACE("[winhttp] WinHttpQueryOption(opt=%u) -> FALSE (buf %u < %u)\n", option, caller_size, data_size);
        m.set_return(0);
        return;
    }

    // Write data to guest. A string option reports its length without the
    // terminator.
    (void)m.memory().write_bytes(buf_addr, data_ptr, data_size);
    if (buf_len_ptr)
        (void)m.write_u32(buf_len_ptr, option == WINHTTP_OPTION_URL ? data_size - 2 : data_size);
    set_last_error(0);

    const char* name = winhttp_option_name(option);
    if (name) {
        VITREO_TRACE("[winhttp] WinHttpQueryOption(%s, %u bytes) -> TRUE\n", name, data_size);
    } else {
        VITREO_TRACE("[winhttp] WinHttpQueryOption(opt=%u, %u bytes) -> TRUE\n", option, data_size);
    }
    m.set_return(1);
}

void WinHttpManager::win_http_set_timeouts(Marshaler& m) {
    // BOOL WinHttpSetTimeouts(HINTERNET hInternet, int nResolveTimeout,
    //     int nConnectTimeout, int nSendTimeout, int nReceiveTimeout)
    const u64 handle = m.arg(0);
    std::lock_guard lock(m_mutex);
    Object* object = handle > UINT32_MAX ? nullptr : live_object(static_cast<u32>(handle));
    if (!object) {
        // NULL -> INCORRECT_HANDLE_TYPE (probe timeouts-null), unknown ->
        // INVALID_HANDLE (timeouts-bogus).
        set_last_error(handle == 0 ? http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE
                                   : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    auto session = m_sessions.find(static_cast<u32>(handle));
    auto request = m_requests.find(static_cast<u32>(handle));
    if (session == m_sessions.end() && request == m_requests.end()) {
        set_last_error(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
        m.set_return(0);
        return;
    }
    int values[4];
    for (unsigned index = 0; index < 4; ++index) {
        values[index] = static_cast<i32>(m.arg(index + 1));
        if (values[index] < -1) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }
    if (values[0] > 0 || values[1] > 0 || values[2] > 0 || values[3] > 0) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set()) std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED winhttp!WinHttpSetTimeouts: finite deadlines require DNS/connect/send/receive phase boundaries\n");
        set_last_error(http_contract::ERROR_WINHTTP_INTERNAL_ERROR /* ERROR_WINHTTP_INTERNAL_ERROR */);
        m.set_return(0);
        return;
    }
    ReceiveTimeout timeout{true, values[3] <= 0 ? -1 : values[3]};
    for (unsigned index = 0; index < 4; ++index)
        timeout.requested[index] = static_cast<u32>(values[index]);
    if (request != m_requests.end() && request->second.sent &&
        !request->second.receive_timeout.unbounded_transport) {
        set_last_error(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE /* ERROR_WINHTTP_INCORRECT_HANDLE_STATE */);
        m.set_return(0);
        return;
    }
    if (request != m_requests.end()) request->second.receive_timeout = timeout;
    else session->second.receive_timeout = timeout;
    set_last_error(0);
    m.set_return(1);
}

} // namespace vitreo::winapi
