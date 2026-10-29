// SPDX-License-Identifier: LGPL-2.1-only
/// @file proxy.cpp
/// @brief WinHTTP status callbacks and proxy configuration / resolution.
///
/// Contract: Learn (WinHttpSetStatusCallback, WinHttpGetProxyForUrl,
/// WinHttpGetIEProxyConfigForCurrentUser, WinHttpCreateProxyResolver,
/// WinHttpGetProxyForUrlEx, WinHttpGetProxyResult, WinHttpFreeProxyResult)
/// and tools/oracle_probe/http_winhttp_handles_probe.expected (x64) /
/// .x86.expected, which pin every error code, last-error side effect,
/// callback, PAC evaluation rule and structure layout used below.

#include "winhttp.hpp"
#include "http_bridge.hpp"
#include "url_components.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace vitreo::winapi {

#include "http_auth_constants.inc"

namespace {

constexpr u32 ERROR_IO_PENDING_VALUE = 997;
constexpr u32 ERROR_NO_TOKEN_VALUE = 1008;

// Natural-alignment layout from a field-kind sequence (D = DWORD/BOOL/enum,
// P = pointer, W = WORD), the rule the SDK structures follow; offsets are
// checked against the probe's offsetof() line by a unit test.
struct FieldLayout {
    u32 offset[8] = {};
    u32 size = 0;
};
FieldLayout natural_layout(const char* kinds, bool is64) {
    FieldLayout l;
    u32 offset = 0, max_align = 4;
    for (int i = 0; kinds[i] != 0; ++i) {
        const u32 size = kinds[i] == 'P' ? (is64 ? 8u : 4u) : kinds[i] == 'W' ? 2u : 4u;
        offset = (offset + size - 1) / size * size;
        l.offset[i] = offset;
        offset += size;
        max_align = std::max(max_align, size);
    }
    l.size = (offset + max_align - 1) / max_align * max_align;
    return l;
}
// WINHTTP_AUTOPROXY_OPTIONS: dwFlags, dwAutoDetectFlags, lpszAutoConfigUrl,
// lpvReserved, dwReserved, fAutoLogonIfChallenged.
constexpr const char* kAutoProxyOptions = "DDPPDD";
// WINHTTP_PROXY_INFO: dwAccessType, lpszProxy, lpszProxyBypass.
constexpr const char* kProxyInfo = "DPP";
// WINHTTP_CURRENT_USER_IE_PROXY_CONFIG: fAutoDetect, lpszAutoConfigUrl,
// lpszProxy, lpszProxyBypass.
constexpr const char* kIeConfig = "DPPP";
// WINHTTP_PROXY_RESULT: cEntries, pEntries.
constexpr const char* kProxyResult = "DP";
// WINHTTP_PROXY_RESULT_ENTRY: fProxy, fBypass, ProxyScheme, pwszProxy,
// ProxyPort.
constexpr const char* kProxyResultEntry = "DDDPW";
// WINHTTP_ASYNC_RESULT: dwResult (DWORD_PTR), dwError.
constexpr const char* kAsyncResult = "PD";

std::u16string read_units(Marshaler& m, GuestAddr address) {
    std::u16string out;
    for (GuestAddr a = address; a != 0 && out.size() < (1u << 20); a += 2) {
        const u16 c = m.read_u16(a).value_or(0);
        if (c == 0) break;
        out.push_back(c);
    }
    return out;
}

std::u16string utf8_to_units(const std::string& text) {
    std::u16string out;
    for (usize i = 0; i < text.size();) {
        const u8 c = static_cast<u8>(text[i]);
        u32 cp = c;
        usize n = 1;
        if (c >= 0xF0 && i + 3 < text.size()) { cp = ((c & 7u) << 18) | ((text[i + 1] & 0x3Fu) << 12) | ((text[i + 2] & 0x3Fu) << 6) | (text[i + 3] & 0x3Fu); n = 4; }
        else if (c >= 0xE0 && i + 2 < text.size()) { cp = ((c & 0xFu) << 12) | ((text[i + 1] & 0x3Fu) << 6) | (text[i + 2] & 0x3Fu); n = 3; }
        else if (c >= 0xC0 && i + 1 < text.size()) { cp = ((c & 0x1Fu) << 6) | (text[i + 1] & 0x3Fu); n = 2; }
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

struct ProxyRequest {
    u32 error = 0;       ///< synchronous validation failure (0 = ok)
    ProxyQuery query;
};

// Validate a (url, WINHTTP_AUTOPROXY_OPTIONS*) pair and build the query.
ProxyRequest build_proxy_request(Marshaler& m, GuestAddr url, GuestAddr options) {
    ProxyRequest r;
    if (url == 0 || options == 0) { r.error = error_codes::ERROR_INVALID_PARAMETER; return r; }
    const bool is64 = m.cpu_is_64bit();
    const FieldLayout o = natural_layout(kAutoProxyOptions, is64);
    const u32 flags = m.read_u32(options + o.offset[0]).value_or(0);
    const u32 detect = m.read_u32(options + o.offset[1]).value_or(0);
    const GuestAddr config = is64 ? m.read_u64(options + o.offset[2]).value_or(0)
                                  : m.read_u32(options + o.offset[2]).value_or(0);
    const u32 known = http_contract::WINHTTP_AUTOPROXY_AUTO_DETECT | http_contract::WINHTTP_AUTOPROXY_CONFIG_URL;
    if ((flags & known) == 0 ||
        ((flags & http_contract::WINHTTP_AUTOPROXY_CONFIG_URL) != 0 && config == 0) ||
        ((flags & http_contract::WINHTTP_AUTOPROXY_AUTO_DETECT) != 0 && detect == 0)) {
        r.error = error_codes::ERROR_INVALID_PARAMETER;
        return r;
    }
    if (const u32 error = urlcrack::proxy_script_arguments(read_units(m, url), r.query.script_url,
                                                           r.query.script_host)) {
        r.error = error;
        return r;
    }
    r.query.auto_detect = (flags & http_contract::WINHTTP_AUTOPROXY_AUTO_DETECT) != 0;
    r.query.detect_dhcp = (detect & http_contract::WINHTTP_AUTO_DETECT_TYPE_DHCP) != 0;
    r.query.detect_dns = (detect & http_contract::WINHTTP_AUTO_DETECT_TYPE_DNS_A) != 0;
    if ((flags & http_contract::WINHTTP_AUTOPROXY_CONFIG_URL) != 0)
        r.query.config_url = m.read_wide_string(config).value_or("");
    return r;
}

u32 proxy_status_error(ProxyStatus status, bool ex) {
    switch (status) {
    case ProxyStatus::Ok: return 0;
    case ProxyStatus::AutoDetectFailed: return http_contract::ERROR_WINHTTP_AUTODETECTION_FAILED;
    case ProxyStatus::UnrecognizedPacScheme: return http_contract::ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    case ProxyStatus::DownloadFailed: return http_contract::ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;
    case ProxyStatus::ScriptError:
        // Measured: WinHttpGetProxyForUrlEx reports SCRIPT_EXECUTION_ERROR,
        // WinHttpGetProxyForUrl BAD_AUTO_PROXY_SCRIPT for the same script.
        return ex ? http_contract::ERROR_WINHTTP_SCRIPT_EXECUTION_ERROR
                  : http_contract::ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT;
    }
    return http_contract::ERROR_WINHTTP_INTERNAL_ERROR;
}

std::atomic_flag g_no_heap_warned = ATOMIC_FLAG_INIT;

} // namespace

// =============================================================================
// Status callback
// =============================================================================

void WinHttpManager::win_http_set_status_callback(Marshaler& m) {
    // WINHTTP_STATUS_CALLBACK WinHttpSetStatusCallback(HINTERNET hInternet,
    //     WINHTTP_STATUS_CALLBACK lpfnInternetCallback,
    //     DWORD dwNotificationFlags, DWORD_PTR dwReserved)
    // Measured: NULL handle -> 12018, unknown -> 6, dwReserved != 0 or a
    // callback with no notification flags -> 87, each returning
    // WINHTTP_INVALID_STATUS_CALLBACK ((WINHTTP_STATUS_CALLBACK)-1, all bits
    // of the pointer); success returns the previous callback and sets
    // ERROR_SUCCESS.
    const u64 raw = m.arg(0);
    const GuestAddr callback = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const u64 reserved = m.arg(3);
    const u64 invalid = m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull;
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(invalid); };

    std::lock_guard lock(m_mutex);
    if (raw == 0) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(static_cast<u32>(raw));
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (reserved != 0 || (callback != 0 && flags == 0)) return fail(error_codes::ERROR_INVALID_PARAMETER);
    const GuestAddr previous = object->callback.procedure;
    object->callback.procedure = callback;
    object->callback.notification_flags = callback == 0 ? 0 : flags;
    VITREO_TRACE("[winhttp] WinHttpSetStatusCallback(0x%llx, 0x%llx, 0x%x) -> 0x%llx\n",
                 static_cast<unsigned long long>(raw), static_cast<unsigned long long>(callback),
                 flags, static_cast<unsigned long long>(previous));
    set_last_error(0);
    m.set_return(previous);
}

// =============================================================================
// Proxy configuration
// =============================================================================

void WinHttpManager::win_http_get_default_proxy_configuration(Marshaler& m) {
    // BOOL WinHttpGetDefaultProxyConfiguration(WINHTTP_PROXY_INFO *pProxyInfo)
    // The WinHTTP default configuration (netsh winhttp) is not set in a
    // Vitreo capsule, which Windows reports as WINHTTP_ACCESS_TYPE_NO_PROXY.
    const GuestAddr proxy_info = m.arg_ptr(0);
    if (proxy_info == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const FieldLayout l = natural_layout(kProxyInfo, m.cpu_is_64bit());
    (void)m.write_u32(proxy_info + l.offset[0], http_contract::WINHTTP_ACCESS_TYPE_NO_PROXY);
    (void)m.write_ptr(proxy_info + l.offset[1], 0);
    (void)m.write_ptr(proxy_info + l.offset[2], 0);
    m.set_return(1);
}

void WinHttpManager::win_http_get_ie_proxy_config(Marshaler& m) {
    // BOOL WinHttpGetIEProxyConfigForCurrentUser(
    //     WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *pProxyConfig)
    // The macOS system proxy settings are the user's proxy configuration;
    // strings are GlobalAlloc'd for the caller to GlobalFree.
    const GuestAddr cfg = m.arg_ptr(0);
    if (cfg == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const SystemProxyConfig system = m_system_proxy ? m_system_proxy() : SystemProxyConfig{};
    const FieldLayout l = natural_layout(kIeConfig, m.cpu_is_64bit());
    GuestAddr strings[3] = {};
    const std::string* texts[3] = {&system.auto_config_url, &system.proxy, &system.bypass};
    for (int i = 0; i < 3; ++i) {
        if (texts[i]->empty()) continue;
        const std::u16string units = utf8_to_units(*texts[i]);
        strings[i] = m_guest_alloc ? m_guest_alloc((units.size() + 1) * 2) : 0;
        if (strings[i] == 0) {
            for (int k = 0; k < i; ++k)
                if (strings[k] && m_guest_free) m_guest_free(strings[k]);
            if (!m_guest_alloc) {
                if (!g_no_heap_warned.test_and_set())
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED winhttp!guest heap not wired\n");
            }
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        for (usize c = 0; c <= units.size(); ++c)
            (void)m.write_u16(strings[i] + c * 2, c < units.size() ? static_cast<u16>(units[c]) : 0);
    }
    (void)m.write_u32(cfg + l.offset[0], system.auto_detect ? 1u : 0u);
    for (int i = 0; i < 3; ++i) (void)m.write_ptr(cfg + l.offset[i + 1], strings[i]);
    m.set_return(1);
}

void WinHttpManager::win_http_get_proxy_for_url(Marshaler& m) {
    // BOOL WinHttpGetProxyForUrl(HINTERNET hSession, LPCWSTR lpcwszUrl,
    //     WINHTTP_AUTOPROXY_OPTIONS *pAutoProxyOptions,
    //     WINHTTP_PROXY_INFO *pProxyInfo)
    const u64 raw = m.arg(0);
    const GuestAddr url = m.arg_ptr(1);
    const GuestAddr options = m.arg_ptr(2);
    const GuestAddr info = m.arg_ptr(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    {
        std::lock_guard lock(m_mutex);
        Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(static_cast<u32>(raw));
        if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
        if (object->kind != Kind::Session) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    }
    if (info == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
    const ProxyRequest request = build_proxy_request(m, url, options);
    if (request.error != 0) return fail(request.error);

    const ProxyResolution resolution = m_proxy_engine ? m_proxy_engine(request.query)
                                                      : ProxyResolution{ProxyStatus::DownloadFailed, {}};
    const bool is64 = m.cpu_is_64bit();
    const FieldLayout l = natural_layout(kProxyInfo, is64);
    auto write_info = [&](u32 access, GuestAddr proxy) {
        (void)m.write_u32(info + l.offset[0], access);
        (void)m.write_ptr(info + l.offset[1], proxy);
        (void)m.write_ptr(info + l.offset[2], 0);
    };
    if (resolution.status != ProxyStatus::Ok) {
        // Measured: the structure is cleared on failure.
        write_info(0, 0);
        return fail(proxy_status_error(resolution.status, false));
    }
    // The leading PROXY/HTTPS entries become "host[:port]" /
    // "https://host[:port]" joined by ';' (as the script wrote them); a
    // result that does not start with one is WINHTTP_ACCESS_TYPE_NO_PROXY.
    std::string list;
    for (const auto& entry : resolution.entries) {
        if (!entry.proxy || (entry.scheme != http_contract::INTERNET_SCHEME_HTTP &&
                             entry.scheme != http_contract::INTERNET_SCHEME_HTTPS))
            break;
        if (!list.empty()) list += ';';
        if (entry.scheme == http_contract::INTERNET_SCHEME_HTTPS) list += "https://";
        list += entry.text;
    }
    if (list.empty()) {
        write_info(http_contract::WINHTTP_ACCESS_TYPE_NO_PROXY, 0);
    } else {
        const std::u16string units = utf8_to_units(list);
        const GuestAddr text = m_guest_alloc ? m_guest_alloc((units.size() + 1) * 2) : 0;
        if (text == 0) {
            write_info(0, 0);
            return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        }
        for (usize c = 0; c <= units.size(); ++c)
            (void)m.write_u16(text + c * 2, c < units.size() ? static_cast<u16>(units[c]) : 0);
        write_info(http_contract::WINHTTP_ACCESS_TYPE_NAMED_PROXY, text);
    }
    set_last_error(0);
    m.set_return(1);
}

// =============================================================================
// Asynchronous proxy resolver (Windows 8+)
// =============================================================================

void WinHttpManager::win_http_create_proxy_resolver(Marshaler& m) {
    // DWORD WinHttpCreateProxyResolver(HINTERNET hSession, HINTERNET *phResolver)
    // Returns the error code; the last error is left alone except that
    // success sets ERROR_SUCCESS (measured create-async).
    const u64 raw = m.arg(0);
    const GuestAddr out = m.arg_ptr(1);
    std::lock_guard lock(m_mutex);
    Object* session = raw > 0xFFFFFFFFull ? nullptr : live_object(static_cast<u32>(raw));
    if (!session) {
        m.set_return(error_codes::ERROR_INVALID_HANDLE);
        return;
    }
    const auto info = m_sessions.find(static_cast<u32>(raw));
    if (session->kind != Kind::Session || info == m_sessions.end() || !info->second.async) {
        m.set_return(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
        return;
    }
    if (out == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    const u32 handle = alloc_handle();
    m_resolvers[handle] = ResolverInfo{};
    (void)add_object(handle, Kind::Resolver, static_cast<u32>(raw));
    (void)m.write_ptr(out, handle);
    set_last_error(0);
    m.set_return(0);
}

void WinHttpManager::win_http_get_proxy_for_url_ex(Marshaler& m) {
    // DWORD WinHttpGetProxyForUrlEx(HINTERNET hResolver, PCWSTR pcwszUrl,
    //     WINHTTP_AUTOPROXY_OPTIONS *pAutoProxyOptions, DWORD_PTR pContext)
    const u64 raw = m.arg(0);
    const GuestAddr url = m.arg_ptr(1);
    const GuestAddr options = m.arg_ptr(2);
    const u64 context = m.arg(3);
    const u32 handle = static_cast<u32>(raw);
    {
        std::lock_guard lock(m_mutex);
        Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(handle);
        if (!object) { m.set_return(error_codes::ERROR_INVALID_HANDLE); return; }
        if (object->kind != Kind::Resolver) {
            m.set_return(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
            return;
        }
    }
    const ProxyRequest request = build_proxy_request(m, url, options);
    if (request.error == error_codes::ERROR_INVALID_PARAMETER) {
        m.set_return(request.error);
        return;
    }
    std::unique_lock lock(m_mutex);
    Object* object = live_object(handle);
    if (!object) { m.set_return(error_codes::ERROR_INVALID_HANDLE); return; }
    auto& resolver = m_resolvers[handle];
    // A resolver carries a single request (measured second-call: 12019 with
    // ERROR_SUCCESS as the last error).
    if (resolver.state != ResolverInfo::State::Idle) {
        set_last_error(0);
        m.set_return(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
        return;
    }
    if (request.error != 0) {
        // An unparseable target URL fails synchronously and also sets the
        // last error (measured ex-bad-target).
        set_last_error(request.error);
        m.set_return(request.error);
        return;
    }
    if (!m_async_callback_invoker || !m_guest_alloc) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set())
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED winhttp!WinHttpGetProxyForUrlEx: no service-thread callback\n");
        m.set_return(http_contract::ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR);
        return;
    }
    resolver.state = ResolverInfo::State::Pending;
    m_memory = &m.memory();
    m_guest_64 = m.cpu_is_64bit();
    lock.unlock();

    const ProxyQuery query = request.query;
    post_job([this, handle, context, query] {
        const ProxyResolution resolution = m_proxy_engine ? m_proxy_engine(query)
                                                          : ProxyResolution{ProxyStatus::DownloadFailed, {}};
        CallbackInfo callback;
        {
            std::lock_guard relock(m_mutex);
            Object* object = live_object(handle);
            auto it = m_resolvers.find(handle);
            // Closed while pending: HANDLE_CLOSING was the last callback.
            if (!object || it == m_resolvers.end()) return;
            it->second.state = resolution.status == ProxyStatus::Ok ? ResolverInfo::State::Complete
                                                                   : ResolverInfo::State::Failed;
            it->second.result = resolution.entries;
            callback = object->callback;
        }
        if (callback.procedure == 0) return;
        if (resolution.status == ProxyStatus::Ok) {
            if ((callback.notification_flags & http_contract::WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE) != 0)
                m_async_callback_invoker(callback.procedure, handle, context,
                                         http_contract::WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE, 0, 0);
            return;
        }
        if ((callback.notification_flags & http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) == 0) return;
        // lpvStatusInformation -> WINHTTP_ASYNC_RESULT {dwResult =
        // API_GET_PROXY_FOR_URL, dwError}, length sizeof(WINHTTP_ASYNC_RESULT).
        const FieldLayout l = natural_layout(kAsyncResult, m_guest_64);
        const GuestAddr info = m_guest_alloc(l.size);
        if (info == 0 || m_memory == nullptr) return;
        if (m_guest_64) (void)m_memory->write_u64(info + l.offset[0], http_contract::API_GET_PROXY_FOR_URL);
        else (void)m_memory->write_u32(info + l.offset[0], http_contract::API_GET_PROXY_FOR_URL);
        (void)m_memory->write_u32(info + l.offset[1], proxy_status_error(resolution.status, true));
        m_async_callback_invoker(callback.procedure, handle, context,
                                 http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, info, l.size);
        if (m_guest_free) m_guest_free(info);
    });
    // Measured: ERROR_IO_PENDING is returned with ERROR_NO_TOKEN as the
    // thread's last error (the out-of-process service's token probe).
    set_last_error(ERROR_NO_TOKEN_VALUE);
    m.set_return(ERROR_IO_PENDING_VALUE);
}

void WinHttpManager::win_http_get_proxy_result(Marshaler& m) {
    // DWORD WinHttpGetProxyResult(HINTERNET hResolver, WINHTTP_PROXY_RESULT *pProxyResult)
    const u64 raw = m.arg(0);
    const GuestAddr out = m.arg_ptr(1);
    std::vector<ProxyEntry> entries;
    {
        std::lock_guard lock(m_mutex);
        Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(static_cast<u32>(raw));
        if (!object) { m.set_return(error_codes::ERROR_INVALID_HANDLE); return; }
        if (object->kind != Kind::Resolver) {
            m.set_return(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
            return;
        }
        const auto& resolver = m_resolvers[static_cast<u32>(raw)];
        if (resolver.state != ResolverInfo::State::Complete) {
            m.set_return(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
            return;
        }
        entries = resolver.result;
    }
    if (out == 0) { m.set_return(error_codes::ERROR_INVALID_PARAMETER); return; }
    const bool is64 = m.cpu_is_64bit();
    const FieldLayout result = natural_layout(kProxyResult, is64);
    const FieldLayout entry = natural_layout(kProxyResultEntry, is64);
    // One block: the entry array followed by the host strings, freed as one
    // by WinHttpFreeProxyResult.
    u64 size = static_cast<u64>(entries.size()) * entry.size;
    std::vector<std::u16string> hosts;
    for (const auto& e : entries) {
        hosts.push_back(e.proxy ? utf8_to_units(e.host) : std::u16string());
        if (e.proxy) size += (hosts.back().size() + 1) * 2;
    }
    GuestAddr block = 0;
    if (!entries.empty()) {
        block = m_guest_alloc ? m_guest_alloc(size) : 0;
        if (block == 0) { m.set_return(error_codes::ERROR_NOT_ENOUGH_MEMORY); return; }
        GuestAddr strings = block + static_cast<u64>(entries.size()) * entry.size;
        for (usize i = 0; i < entries.size(); ++i) {
            const GuestAddr e = block + i * entry.size;
            (void)m.write_u32(e + entry.offset[0], entries[i].proxy ? 1u : 0u);
            (void)m.write_u32(e + entry.offset[1], entries[i].bypass ? 1u : 0u);
            (void)m.write_u32(e + entry.offset[2], entries[i].proxy ? entries[i].scheme : 0u);
            GuestAddr text = 0;
            if (entries[i].proxy) {
                text = strings;
                for (usize c = 0; c <= hosts[i].size(); ++c)
                    (void)m.write_u16(strings + c * 2, c < hosts[i].size() ? static_cast<u16>(hosts[i][c]) : 0);
                strings += (hosts[i].size() + 1) * 2;
            }
            (void)m.write_ptr(e + entry.offset[3], text);
            (void)m.write_u16(e + entry.offset[4], entries[i].proxy ? entries[i].port : 0);
        }
    }
    (void)m.write_u32(out + result.offset[0], static_cast<u32>(entries.size()));
    (void)m.write_ptr(out + result.offset[1], block);
    m.set_return(0);
}

void WinHttpManager::win_http_free_proxy_result(Marshaler& m) {
    // VOID WinHttpFreeProxyResult(WINHTTP_PROXY_RESULT *pProxyResult)
    // Frees the entries and leaves cEntries = 0, pEntries = NULL (measured).
    const GuestAddr result = m.arg_ptr(0);
    if (result == 0) return;
    const FieldLayout l = natural_layout(kProxyResult, m.cpu_is_64bit());
    const GuestAddr block = m.cpu_is_64bit() ? m.read_u64(result + l.offset[1]).value_or(0)
                                             : m.read_u32(result + l.offset[1]).value_or(0);
    if (block != 0 && m_guest_free) m_guest_free(block);
    (void)m.write_u32(result + l.offset[0], 0);
    (void)m.write_ptr(result + l.offset[1], 0);
}

} // namespace vitreo::winapi
