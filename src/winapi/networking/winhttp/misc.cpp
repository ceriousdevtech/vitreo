// SPDX-License-Identifier: LGPL-2.1-only
/// @file misc.cpp
/// @brief WinHttpAddRequestHeaders and WinHttpDetectAutoProxyConfigUrl.
///
/// Contract: Learn plus tools/oracle_probe/http_request_probe.expected
/// (header modifiers) and http_winhttp_handles_probe.expected (handles).

#include "winhttp.hpp"
#include "http_bridge.hpp"
#include "http_headers.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <string>

namespace vitreo::winapi {

#include "http_auth_constants.inc"

void WinHttpManager::win_http_add_request_headers(Marshaler& m) {
    // BOOL WinHttpAddRequestHeaders(HINTERNET hRequest, LPCWSTR lpszHeaders,
    //     DWORD dwHeadersLength, DWORD dwModifiers)
    // Measured: NULL/unknown -> 6, wrong type -> 12018, NULL or empty header
    // text or a line without ':' -> 87, ADD_IF_NEW on an existing name ->
    // 12155, REPLACE of a missing name without ADD -> 12150; the index bits
    // of dwModifiers are ignored.
    const u64 raw = m.arg(0);
    const GuestAddr headers = m.arg_ptr(1);
    const u32 length = m.arg32(2);
    const u32 modifiers = m.arg32(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    std::string block;
    if (headers != 0 && length != 0)
        block = length == 0xFFFFFFFFu ? m.read_wide_string(headers).value_or("")
                                      : m.read_wide_string(headers, length).value_or("");

    std::lock_guard lock(m_mutex);
    Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(static_cast<u32>(raw));
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    if (headers == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
    const auto lines = http_headers::parse_block(block, false);
    if (!lines) return fail(error_codes::ERROR_INVALID_PARAMETER);
    auto& request = m_requests[static_cast<u32>(raw)];
    const u32 flags = modifiers & http_contract::WINHTTP_ADDREQ_FLAGS_MASK;
    for (const auto& line : *lines) {
        switch (http_headers::apply(request.headers, line, flags, false,
                                    http_contract::WINHTTP_ADDREQ_FLAG_ADD,
                                    http_contract::WINHTTP_ADDREQ_FLAG_ADD_IF_NEW,
                                    http_contract::WINHTTP_ADDREQ_FLAG_COALESCE,
                                    http_contract::WINHTTP_ADDREQ_FLAG_COALESCE_WITH_COMMA,
                                    http_contract::WINHTTP_ADDREQ_FLAG_COALESCE_WITH_SEMICOLON,
                                    http_contract::WINHTTP_ADDREQ_FLAG_REPLACE)) {
        case http_headers::ApplyResult::Ok: break;
        case http_headers::ApplyResult::AlreadyExists:
            return fail(http_contract::ERROR_WINHTTP_HEADER_ALREADY_EXISTS);
        case http_headers::ApplyResult::NotFound:
            return fail(http_contract::ERROR_WINHTTP_HEADER_NOT_FOUND);
        }
    }
    set_last_error(0);
    m.set_return(1);
}

void WinHttpManager::win_http_detect_auto_proxy_config_url(Marshaler& m) {
    // BOOL WinHttpDetectAutoProxyConfigUrl(DWORD dwAutoDetectFlags,
    //     LPWSTR *ppwstrAutoConfigUrl)
    // WPAD discovery (DHCP option 252, DNS "wpad"); the URL is GlobalAlloc'd.
    const u32 flags = m.arg32(0);
    const GuestAddr out = m.arg_ptr(1);
    const u32 valid = http_contract::WINHTTP_AUTO_DETECT_TYPE_DHCP | http_contract::WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    if (out == 0 || flags == 0 || (flags & ~valid) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto urls = discover_wpad_urls((flags & http_contract::WINHTTP_AUTO_DETECT_TYPE_DHCP) != 0,
                                         (flags & http_contract::WINHTTP_AUTO_DETECT_TYPE_DNS_A) != 0);
    if (urls.empty()) {
        (void)m.write_ptr(out, 0);
        set_last_error(http_contract::ERROR_WINHTTP_AUTODETECTION_FAILED);
        m.set_return(0);
        return;
    }
    const std::string& url = urls.front();
    const GuestAddr text = m_guest_alloc ? m_guest_alloc((url.size() + 1) * 2) : 0;
    if (text == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    (void)m.write_wide_string(text, url);
    (void)m.write_ptr(out, text);
    m.set_return(1);
}

} // namespace vitreo::winapi
