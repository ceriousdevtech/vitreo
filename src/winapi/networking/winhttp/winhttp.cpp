// SPDX-License-Identifier: LGPL-2.1-only
/// @file winhttp.cpp
/// @brief winhttp.dll — WinHTTP implementation with NSURLSession delegation
///
/// Implements WinHTTP API surface with real HTTP transport via NSURLSession.
/// Connection and request state is tracked so the multi-step WinHTTP flow
/// (Open → Connect → OpenRequest → SendRequest → ReceiveResponse → ReadData)
/// results in an actual HTTP request on macOS.
///
/// Handle, callback and request/response behaviour follows Learn plus the
/// measurements in tools/oracle_probe/http_winhttp_handles_probe.expected and
/// http_request_probe.expected (Windows 11, x86 and x64).

#include "winhttp.hpp"
#include "http_bridge.hpp"
#include "http_headers.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace vitreo::winapi {

#include "http_auth_constants.inc"

namespace {

void winhttp_diag(const char* operation, u32 handle, u64 detail = 0) {
    static const bool enabled = std::getenv("VITREO_WINHTTP_DIAG") != nullptr;
    static std::atomic<u32> emitted{0};
    if (!enabled || emitted.fetch_add(1, std::memory_order_relaxed) >= 512) {
        return;
    }
    std::fprintf(stderr,
                 "[winhttp-diag] op=%s handle=0x%x detail=0x%llx thread=%zu\n",
                 operation, handle,
                 static_cast<unsigned long long>(detail),
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void unimplemented_once(std::atomic_flag& flag, const char* what) {
    if (!flag.test_and_set()) std::fprintf(stderr, "[vitreo] UNIMPLEMENTED winhttp!%s\n", what);
}

// Read a wide header block: dwHeadersLength characters, or up to the
// terminator when the length is (DWORD)-1.
std::string read_header_block(Marshaler& m, GuestAddr address, u32 length) {
    if (address == 0 || length == 0) return {};
    if (length == 0xFFFFFFFFu) return m.read_wide_string(address).value_or("");
    return m.read_wide_string(address, length).value_or("");
}

// NSURLErrorDomain codes (Foundation NSURLError.h) -> WinHTTP errors.
u32 map_transport_error(int code, bool finished) {
    switch (code) {
    case -1001: return http_contract::ERROR_WINHTTP_TIMEOUT;           // TimedOut
    case -1003: return http_contract::ERROR_WINHTTP_NAME_NOT_RESOLVED; // CannotFindHost
    case -1004: return http_contract::ERROR_WINHTTP_CANNOT_CONNECT;    // CannotConnectToHost
    case -1999: case -999: return http_contract::ERROR_WINHTTP_OPERATION_CANCELLED;
    default: break;
    }
    return finished ? http_contract::ERROR_WINHTTP_CONNECTION_ERROR
                    : http_contract::ERROR_WINHTTP_TIMEOUT;
}

} // namespace

// =============================================================================
// WinHTTP Constants
// =============================================================================


// =============================================================================
// Manager lifetime, service thread, handle table
// =============================================================================

WinHttpManager::WinHttpManager()
    : m_proxy_engine(resolve_proxy), m_system_proxy(system_proxy_config) {}

WinHttpManager::~WinHttpManager() {
    {
        std::lock_guard lock(m_job_mutex);
        m_stop = true;
    }
    m_job_cv.notify_all();
    // Pending asynchronous waits return once their transport is cancelled.
    {
        std::lock_guard lock(m_mutex);
        for (auto& [handle, request] : m_requests) {
            (void)handle;
            if (request.stream) request.stream->cancel();
        }
    }
    {
        std::unique_lock lock(m_job_mutex);
        m_job_cv.wait(lock, [&] { return m_active_waits == 0; });
    }
    if (m_worker.joinable()) m_worker.join();
}

void WinHttpManager::post_job(std::function<void()> job) {
    std::lock_guard lock(m_job_mutex);
    if (m_stop) return;
    m_jobs.push_back(std::move(job));
    if (!m_worker.joinable()) m_worker = std::thread([this] { worker_loop(); });
    m_job_cv.notify_one();
}

void WinHttpManager::worker_loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(m_job_mutex);
            m_job_cv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
            if (m_stop) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        job();
    }
}

void WinHttpManager::start_async_wait(std::function<void()> wait) {
    {
        std::lock_guard lock(m_job_mutex);
        if (m_stop) return;
        ++m_active_waits;
    }
    std::thread([this, wait = std::move(wait)] {
        wait();
        std::lock_guard lock(m_job_mutex);
        --m_active_waits;
        m_job_cv.notify_all();
    }).detach();
}

bool WinHttpManager::request_is_async(const RequestInfo& request) const {
    const auto connection = m_objects.find(request.conn_handle);
    if (connection == m_objects.end()) return false;
    const auto session = m_sessions.find(connection->second.parent);
    return session != m_sessions.end() && session->second.async;
}

void WinHttpManager::post_completion(u32 handle, u32 status,
                                     CompletionInfo info) {
    post_job([this, handle, status, info] {
        CallbackInfo callback;
        u64 context = 0;
        {
            std::lock_guard lock(m_mutex);
            const Object* object = live_object(handle);
            if (!object) return;
            callback = object->callback;
            context = object->context;
        }
        if (callback.procedure == 0 ||
            (callback.notification_flags & status) == 0 ||
            !m_async_callback_invoker) {
            return;
        }
        GuestAddr address = 0;
        u32 length = 0;
        bool owned = false;
        switch (info.kind) {
        case CompletionInfo::Kind::None:
            break;
        case CompletionInfo::Kind::Buffer:
            address = info.address;
            length = info.value;
            break;
        case CompletionInfo::Kind::Dword:
            length = sizeof(u32);
            address = m_guest_alloc ? m_guest_alloc(length) : 0;
            if (address == 0 || m_memory == nullptr) return;
            owned = true;
            (void)m_memory->write_u32(address, info.value);
            break;
        case CompletionInfo::Kind::AsyncResult:
            // WINHTTP_ASYNC_RESULT: DWORD_PTR dwResult, DWORD dwError,
            // padded to pointer alignment.
            length = m_guest_64 ? 16u : 8u;
            address = m_guest_alloc ? m_guest_alloc(length) : 0;
            if (address == 0 || m_memory == nullptr) return;
            owned = true;
            if (m_guest_64) (void)m_memory->write_u64(address, info.api);
            else (void)m_memory->write_u32(address, info.api);
            (void)m_memory->write_u32(address + length / 2, info.value);
            break;
        }
        VITREO_TRACE("[winhttp] async completion handle=0x%x status=0x%x "
                     "info=0x%llx len=%u\n",
                     handle, status, static_cast<unsigned long long>(address),
                     length);
        m_async_callback_invoker(callback.procedure, handle, context, status,
                                 address, length);
        if (owned && m_guest_free) m_guest_free(address);
    });
}

WinHttpManager::Object* WinHttpManager::live_object(u32 handle) {
    auto it = m_objects.find(handle);
    if (it == m_objects.end() || it->second.guest_closed) return nullptr;
    return &it->second;
}

// Caller holds m_mutex (add_object / destroy_object are the table operations
// the locked Open*/Close entry points compose).
WinHttpManager::Notification WinHttpManager::add_object(u32 handle, Kind kind, u32 parent) {
    Object object;
    object.kind = kind;
    object.parent = parent;
    if (auto it = m_objects.find(parent); parent != 0 && it != m_objects.end()) {
        // Children inherit the callback and the context value
        // (http_winhttp_handles_probe created-connect-ctx / child-context).
        object.callback = it->second.callback;
        object.context = it->second.context;
        ++it->second.live_children;
    }
    m_objects[handle] = object;
    return Notification{handle, object.callback, object.context,
                        http_contract::WINHTTP_CALLBACK_STATUS_HANDLE_CREATED};
}

// Caller holds m_mutex.
void WinHttpManager::destroy_object(u32 handle, std::vector<Notification>& notes) {
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return;
    const Object object = it->second;
    notes.push_back(Notification{handle, object.callback, object.context,
                                 http_contract::WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING});
    if (auto request = m_requests.find(handle); request != m_requests.end()) {
        if (request->second.stream) request->second.stream->cancel();
        if (request->second.websocket) request->second.websocket->cancel();
        m_requests.erase(request);
    }
    if (auto socket = m_websockets.find(handle); socket != m_websockets.end()) {
        if (socket->second.stream) socket->second.stream->cancel();
        m_websockets.erase(socket);
    }
    m_sessions.erase(handle);
    m_connections.erase(handle);
    m_resolvers.erase(handle);
    m_objects.erase(it);
    if (object.parent != 0) {
        auto parent = m_objects.find(object.parent);
        if (parent != m_objects.end()) {
            if (parent->second.live_children > 0) --parent->second.live_children;
            if (parent->second.guest_closed && parent->second.live_children == 0)
                destroy_object(object.parent, notes);
        }
    }
}

void WinHttpManager::notify_status(u32 handle, const CallbackInfo& callback,
                                   u64 context, u32 status,
                                   GuestAddr status_information,
                                   u32 status_information_length) {
    if (callback.procedure == 0 ||
        (callback.notification_flags & status) == 0 ||
        !m_status_callback_invoker) {
        return;
    }
    VITREO_TRACE("[winhttp] status callback handle=0x%x proc=0x%llx"
                 " context=0x%llx status=0x%x info=0x%llx len=%u\n",
                 handle,
                 static_cast<unsigned long long>(callback.procedure),
                 static_cast<unsigned long long>(context), status,
                 static_cast<unsigned long long>(status_information),
                 status_information_length);
    m_status_callback_invoker(callback.procedure, handle, context, status,
                              status_information, status_information_length);
}

void WinHttpManager::notify_handle(Marshaler& m, const Notification& note) {
    if (note.callback.procedure == 0 || (note.callback.notification_flags & note.status) == 0 ||
        !m_status_callback_invoker)
        return;
    // lpvStatusInformation points to the HINTERNET value;
    // dwStatusInformationLength is sizeof(HINTERNET) (probe: len=8 / len=4).
    const u32 size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr info = m_guest_alloc ? m_guest_alloc(size) : 0;
    if (info == 0) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        unimplemented_once(warned, "status callback: no guest heap for lpvStatusInformation");
        return;
    }
    (void)m.write_ptr(info, note.handle);
    notify_status(note.handle, note.callback, note.context, note.status, info, size);
    if (m_guest_free) m_guest_free(info);
}

// =============================================================================
// Session / Connection Lifecycle
// =============================================================================

void WinHttpManager::win_http_open(Marshaler& m) {
    // HINTERNET WinHttpOpen(LPCWSTR pszAgentW, DWORD dwAccessType,
    //                       LPCWSTR pszProxyW, LPCWSTR pszProxyBypassW,
    //                       DWORD dwFlags)
    const u32 flags = m.arg32(4);
    std::lock_guard lock(m_mutex);
    m_guest_64 = m.cpu_is_64bit();
    const u32 handle = alloc_handle();
    SessionInfo info;
    info.async = (flags & http_contract::WINHTTP_FLAG_ASYNC) != 0;
    if (const GuestAddr agent = m.arg_ptr(0)) info.agent = m.read_wide_string(agent).value_or("");
    m_sessions.emplace(handle, std::move(info));
    (void)add_object(handle, Kind::Session, 0);
    winhttp_diag("open", handle, flags);
    VITREO_TRACE("[winhttp] WinHttpOpen -> 0x%x\n", handle);
    m.set_return(handle);
}

void WinHttpManager::win_http_connect(Marshaler& m) {
    // HINTERNET WinHttpConnect(HINTERNET hSession, LPCWSTR pswzServerName,
    //                          INTERNET_PORT nServerPort, DWORD dwReserved)
    const u32 session = static_cast<u32>(m.arg(0));
    const GuestAddr server_addr = m.arg_ptr(1);
    const u16 port = static_cast<u16>(m.arg(2));

    Notification created;
    u32 handle = 0;
    {
        std::lock_guard lock(m_mutex);
        Object* parent = live_object(session);
        if (!parent) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (parent->kind != Kind::Session) {
            set_last_error(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
            m.set_return(0);
            return;
        }
        handle = alloc_handle();
        ConnInfo ci;
        ci.port = port;
        if (server_addr) ci.server = m.read_wide_string(server_addr).value_or("");
        m_connections[handle] = std::move(ci);
        created = add_object(handle, Kind::Connection, session);
    }
    notify_handle(m, created);
    VITREO_TRACE("[winhttp] WinHttpConnect(port %u) -> 0x%x\n", port, handle);
    m.set_return(handle);
}

void WinHttpManager::win_http_open_request(Marshaler& m) {
    // HINTERNET WinHttpOpenRequest(HINTERNET hConnect, LPCWSTR pwszVerb,
    //     LPCWSTR pwszObjectName, LPCWSTR pwszVersion,
    //     LPCWSTR pwszReferrer, LPCWSTR *ppwszAcceptTypes, DWORD dwFlags)
    const u32 conn = static_cast<u32>(m.arg(0));
    const GuestAddr verb_addr = m.arg_ptr(1);
    const GuestAddr path_addr = m.arg_ptr(2);
    const u32 flags = m.arg32(6);

    Notification created;
    u32 handle = 0;
    {
        std::lock_guard lock(m_mutex);
        Object* parent = live_object(conn);
        if (!parent) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (parent->kind != Kind::Connection) {
            set_last_error(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
            m.set_return(0);
            return;
        }
        handle = alloc_handle();
        RequestInfo ri;
        ri.conn_handle = conn;
        if (const auto session = m_sessions.find(parent->parent); session != m_sessions.end()) {
            ri.receive_timeout = session->second.receive_timeout;
            if (!session->second.agent.empty()) ri.agent = session->second.agent;
        }
        ri.method = verb_addr ? m.read_wide_string(verb_addr).value_or("GET") : "GET";
        if (ri.method.empty()) ri.method = "GET";
        ri.path = path_addr ? m.read_wide_string(path_addr).value_or("/") : "/";
        // Only WINHTTP_FLAG_SECURE selects TLS; a connection to port 443
        // without it is plain HTTP (http_request_probe url-port443-plain).
        ri.tls = (flags & http_contract::WINHTTP_FLAG_SECURE) != 0;
        m_requests[handle] = std::move(ri);
        created = add_object(handle, Kind::Request, conn);
    }
    notify_handle(m, created);
    VITREO_TRACE("[winhttp] WinHttpOpenRequest -> 0x%x\n", handle);
    m.set_return(handle);
}

void WinHttpManager::win_http_close_handle(Marshaler& m) {
    // BOOL WinHttpCloseHandle(HINTERNET hInternet)
    // NULL, unknown and already-closed handles fail with ERROR_INVALID_HANDLE.
    // HANDLE_CLOSING is delivered on the calling thread before the call
    // returns, whether or not a context value was set; a parent closed while
    // children are open is torn down (with its notification) after its last
    // child (http_winhttp_handles_probe).
    const u64 raw = m.arg(0);
    std::vector<Notification> notes;
    {
        std::lock_guard lock(m_mutex);
        Object* object = raw > 0xFFFFFFFFull ? nullptr : live_object(static_cast<u32>(raw));
        if (!object) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 h = static_cast<u32>(raw);
        object->guest_closed = true;
        // Cancel transport work immediately so blocked calls on other
        // threads return.
        if (auto request = m_requests.find(h); request != m_requests.end()) {
            if (request->second.stream) request->second.stream->cancel();
            if (request->second.websocket) request->second.websocket->cancel();
        }
        if (object->live_children == 0) destroy_object(h, notes);
    }
    for (const auto& note : notes) notify_handle(m, note);
    VITREO_TRACE("[winhttp] WinHttpCloseHandle(0x%llx) -> TRUE\n",
                 static_cast<unsigned long long>(raw));
    set_last_error(0);
    m.set_return(1);
}

// =============================================================================
// Request / Response
// =============================================================================

void WinHttpManager::win_http_send_request(Marshaler& m) {
    // BOOL WinHttpSendRequest(HINTERNET hRequest, LPCWSTR lpszHeaders,
    //     DWORD dwHeadersLength, LPVOID lpOptional, DWORD dwOptionalLength,
    //     DWORD dwTotalLength, DWORD_PTR dwContext)
    //
    // The request is started here; WinHttpReceiveResponse waits for the
    // response headers and WinHttpReadData pulls body bytes progressively.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr headers_addr = m.arg_ptr(1);
    const u32 headers_length = m.arg32(2);
    const GuestAddr opt_addr = m.arg_ptr(3);
    const u32 opt_len = m.arg32(4);
    const u32 total_len = m.arg32(5);
    const u64 send_context = m.arg(6);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    // Headers, body and the transport are prepared outside the lock.
    std::string block = read_header_block(m, headers_addr, headers_length);
    std::vector<u8> body;
    if (opt_addr && opt_len > 0) {
        body.resize(opt_len);
        if (m.memory().read_bytes(opt_addr, reinterpret_cast<Byte*>(body.data()), opt_len).is_err())
            return fail(error_codes::ERROR_INVALID_PARAMETER);
    }

    std::unique_lock lock(m_mutex);
    Object* object = live_object(h);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    auto& ri = m_requests[h];
    winhttp_diag("send-request", h, ri.websocket_upgrade);
    // dwTotalLength is the Content-Length and may not be smaller than the
    // optional data (measured: 3 < 6 and 0 < 6 both fail with 87).
    if (total_len < opt_len) return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (!block.empty()) {
        auto lines = http_headers::parse_block(block, false);
        if (!lines) return fail(error_codes::ERROR_INVALID_PARAMETER);
        for (const auto& line : *lines)
            (void)http_headers::apply(ri.headers, line, http_contract::WINHTTP_ADDREQ_FLAG_ADD, false,
                                      http_contract::WINHTTP_ADDREQ_FLAG_ADD,
                                      http_contract::WINHTTP_ADDREQ_FLAG_ADD_IF_NEW,
                                      http_contract::WINHTTP_ADDREQ_FLAG_COALESCE,
                                      http_contract::WINHTTP_ADDREQ_FLAG_COALESCE_WITH_COMMA,
                                      http_contract::WINHTTP_ADDREQ_FLAG_COALESCE_WITH_SEMICOLON,
                                      http_contract::WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (http_headers::find(ri.headers, "Transfer-Encoding") != ri.headers.end()) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        unimplemented_once(warned, "WinHttpSendRequest: Transfer-Encoding request bodies");
        return fail(http_contract::ERROR_WINHTTP_INTERNAL_ERROR);
    }
    if (send_context != 0) object->context = send_context;
    auto cit = m_connections.find(ri.conn_handle);
    if (cit == m_connections.end()) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);

    // INTERNET_DEFAULT_PORT (0) is the scheme's default port, chosen per
    // request (http_request_probe url-port0 / url-port0-secure).
    const u16 default_port = static_cast<u16>(ri.tls ? http_contract::INTERNET_DEFAULT_HTTPS_PORT
                                                     : http_contract::INTERNET_DEFAULT_HTTP_PORT);
    const u16 port = cit->second.port == 0 ? default_port : cit->second.port;
    std::string scheme = ri.websocket_upgrade ? (ri.tls ? "wss" : "ws") : (ri.tls ? "https" : "http");
    std::string url = scheme + "://" + cit->second.server;
    if (port != default_port) url += ":" + std::to_string(port);
    if (!ri.path.empty() && ri.path[0] != '/') url += "/";
    url += ri.path;

    auto transport_headers = ri.headers;
    if (!ri.agent.empty() && http_headers::find(transport_headers, "User-Agent") == transport_headers.end())
        transport_headers.emplace_back("User-Agent", ri.agent);
    if (!ri.authorization.empty()) {
        transport_headers.erase(
            std::remove_if(transport_headers.begin(), transport_headers.end(),
                           [](const auto& hdr) { return http_headers::iequals(hdr.first, "Authorization"); }),
            transport_headers.end());
        transport_headers.emplace_back("Authorization", ri.authorization);
    }
    const bool async = [&] {
        const auto conn_object = m_objects.find(ri.conn_handle);
        if (conn_object == m_objects.end()) return false;
        const auto session = m_sessions.find(conn_object->second.parent);
        return session != m_sessions.end() && session->second.async;
    }();

    if (ri.websocket_upgrade) {
        if (ri.receive_timeout.unbounded_transport) {
            static std::atomic_flag warned = ATOMIC_FLAG_INIT;
            unimplemented_once(warned, "WinHttpSendRequest: WebSocket timeout phases");
            return fail(http_contract::ERROR_WINHTTP_INTERNAL_ERROR);
        }
        std::unordered_map<std::string, std::string> ws_headers;
        for (const auto& [k, v] : transport_headers) ws_headers[k] = v;
        ri.websocket = std::make_shared<WebSocketStream>();
        if (!ri.websocket->start(url, ws_headers)) {
            ri.websocket.reset();
            return fail(http_contract::ERROR_WINHTTP_CANNOT_CONNECT);
        }
        ri.sent = true;
        ri.status_code = 0;
        ri.response_headers.clear();
    } else {
        auto stream = std::make_shared<HttpStream>();
        const bool streamed = total_len > opt_len;
        if (!stream->start(ri.method, url, transport_headers, body,
                           ri.receive_timeout.unbounded_transport,
                           streamed ? total_len : 0)) {
            VITREO_TRACE("[winhttp] WinHttpSendRequest -> FALSE (start failed: %s)\n",
                         stream->error().c_str());
            return fail(map_transport_error(stream->error_code(), true));
        }
        ri.stream = std::move(stream);
        ri.sent = true;
        ri.status_code = 0;
        ri.response_headers.clear();
        ri.bytes_read = 0;
        ri.total_length = streamed ? total_len : 0;
        ri.written = opt_len;
    }
    lock.unlock();
    VITREO_TRACE("[winhttp] WinHttpSendRequest: %s (body=%u total=%u)\n", url.c_str(), opt_len,
                 total_len);
    // Completion notifications are delivered only in asynchronous mode
    // (Learn: WinHttpSendRequest remarks), and from a WinHTTP thread after
    // the call has returned (tools/oracle_probe/winhttp_async_flow_probe:
    // SENDREQUEST_COMPLETE main_thread=0, outside the call).
    if (async) {
        m_memory = &m.memory();
        m_guest_64 = m.cpu_is_64bit();
        post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,
                        {CompletionInfo::Kind::Buffer, opt_len != 0 ? opt_addr : 0, opt_len, 0});
    }
    set_last_error(0);
    m.set_return(1u);
}

void WinHttpManager::win_http_receive_response(Marshaler& m) {
    // BOOL WinHttpReceiveResponse(HINTERNET hRequest, LPVOID lpReserved)
    const u32 h = static_cast<u32>(m.arg(0));
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    std::unique_lock lock(m_mutex);
    Object* object = live_object(h);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    auto& ri = m_requests[h];
    if (!ri.sent) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    winhttp_diag("receive-response-enter", h, ri.websocket_upgrade);
    const bool async = request_is_async(ri);
    if (async) {
        m_memory = &m.memory();
        m_guest_64 = m.cpu_is_64bit();
    }

    if (ri.websocket_upgrade) {
        auto socket = ri.websocket;
        // didOpenWithProtocol is emitted only after NSURLSession validates
        // the server's HTTP 101 upgrade response.
        auto finish = [this, h, socket](bool open) -> u32 {
            std::lock_guard relock(m_mutex);
            if (!live_object(h)) return http_contract::ERROR_WINHTTP_OPERATION_CANCELLED;
            if (!open) {
                VITREO_TRACE("[winhttp] WebSocket handshake failed: %s\n",
                             socket ? socket->error().c_str() : "no transport");
                return http_contract::ERROR_WINHTTP_CANNOT_CONNECT;
            }
            auto& req = m_requests[h];
            req.status_code = 101;
            req.response_headers.clear();
            return 0;
        };
        lock.unlock();
        if (async) {
            // Learn WinHttpReceiveResponse: in asynchronous mode the call
            // returns at once and HEADERS_AVAILABLE (or REQUEST_ERROR with
            // API_RECEIVE_RESPONSE) reports the outcome.
            start_async_wait([this, h, socket, finish] {
                const u32 error = finish(socket && socket->wait_open(60 * 1000));
                if (error == http_contract::ERROR_WINHTTP_OPERATION_CANCELLED) return;
                if (error == 0)
                    post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, {});
                else
                    post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR,
                                    {CompletionInfo::Kind::AsyncResult, 0, error,
                                     http_contract::API_RECEIVE_RESPONSE});
            });
            set_last_error(0);
            m.set_return(1);
            return;
        }
        const u32 error = finish(socket && socket->wait_open(60 * 1000));
        if (error != 0) return fail(error);
        set_last_error(0);
        m.set_return(1);
        return;
    }
    auto stream = ri.stream;
    if (!stream) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    // A streamed body that received exactly dwTotalLength bytes is complete.
    // A short body stays open: the server waits for the rest and the wait
    // below times out (measured: receive-short-body -> ERROR_WINHTTP_TIMEOUT).
    if (ri.total_length != 0 && ri.written == ri.total_length) stream->finish_body();
    const int timeout_ms = ri.receive_timeout.milliseconds;
    lock.unlock();

    // Waits for the response headers and records them; returns the WinHTTP
    // error, or 0 once they are available.
    auto receive = [this, h, stream, timeout_ms]() -> u32 {
        const bool ok = stream->wait_headers(timeout_ms);
        std::lock_guard relock(m_mutex);
        if (!live_object(h)) return http_contract::ERROR_WINHTTP_OPERATION_CANCELLED;
        if (!ok) {
            const bool finished = stream->finished();
            stream->cancel();
            VITREO_TRACE("[winhttp] WinHttpReceiveResponse failed (%s)\n", stream->error().c_str());
            return finished ? map_transport_error(stream->error_code(), true)
                            : http_contract::ERROR_WINHTTP_TIMEOUT;
        }
        auto& req = m_requests[h];
        req.status_code = stream->status_code();
        req.response_headers = stream->headers();
        VITREO_TRACE("[winhttp] WinHttpReceiveResponse headers (status=%d)\n", req.status_code);
        return 0;
    };
    if (async) {
        start_async_wait([this, h, receive] {
            const u32 error = receive();
            if (error == http_contract::ERROR_WINHTTP_OPERATION_CANCELLED) return;
            if (error == 0)
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, {});
            else
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR,
                                {CompletionInfo::Kind::AsyncResult, 0, error,
                                 http_contract::API_RECEIVE_RESPONSE});
        });
        set_last_error(0);
        m.set_return(1);
        return;
    }
    const u32 error = receive();
    if (error != 0) return fail(error);
    set_last_error(0);
    m.set_return(1);
}

void WinHttpManager::win_http_query_headers(Marshaler& m) {
    // BOOL WinHttpQueryHeaders(HINTERNET hRequest, DWORD dwInfoLevel,
    //     LPCWSTR pwszName, LPVOID lpBuffer, LPDWORD lpdwBufferLength,
    //     LPDWORD lpdwIndex)
    const u32 h = static_cast<u32>(m.arg(0));
    const u32 info_level = m.arg32(1);
    const GuestAddr name_ptr = m.arg_ptr(2);
    const GuestAddr buf_ptr = m.arg_ptr(3);
    const GuestAddr buf_len_ptr = m.arg_ptr(4);
    const GuestAddr index_ptr = m.arg_ptr(5);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    std::lock_guard lock(m_mutex);
    Object* object = live_object(h);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    const auto& ri = m_requests[h];
    const u32 query = info_level & 0xFFFFu;
    const bool request_headers = (info_level & http_contract::WINHTTP_QUERY_FLAG_REQUEST_HEADERS) != 0;
    const bool as_number = (info_level & http_contract::WINHTTP_QUERY_FLAG_NUMBER) != 0;
    const bool as_number64 = (info_level & http_contract::WINHTTP_QUERY_FLAG_NUMBER64) != 0;
    const bool as_time = (info_level & http_contract::WINHTTP_QUERY_FLAG_SYSTEMTIME) != 0;
    if (buf_len_ptr == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);

    // Resolve the level to a header name (or a special value).
    const char* level_name = nullptr;
    for (const auto& level : http_contract::winhttp_query_levels)
        if (level.level == query) level_name = level.name;
    if (query != http_contract::WINHTTP_QUERY_CUSTOM &&
        (level_name == nullptr || query > http_contract::WINHTTP_QUERY_MAX))
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (!request_headers && (!ri.sent || ri.status_code == 0))
        return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);

    std::string name;
    if (query == http_contract::WINHTTP_QUERY_CUSTOM) {
        if (name_ptr == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        name = m.read_wide_string(name_ptr).value_or("");
        if (name.empty()) return fail(error_codes::ERROR_INVALID_PARAMETER);
    } else {
        name = level_name;
    }
    u32 index = 0;
    if (index_ptr != 0) index = m.read_u32(index_ptr).value_or(0);

    std::optional<std::string> value;
    if (query == http_contract::WINHTTP_QUERY_CUSTOM || std::strcmp(level_name, "STATUS_CODE") != 0) {
        // Special non-header levels.
        const std::string level = query == http_contract::WINHTTP_QUERY_CUSTOM ? "" : level_name;
        if (level == "REQUEST_METHOD") {
            if (index == 0) value = ri.method;
        } else if (level == "VERSION" || level == "STATUS_TEXT" || level == "RAW_HEADERS" ||
                   level == "RAW_HEADERS_CRLF" || level == "ORIG_URI") {
            // NSURLSession exposes neither the status line (protocol version,
            // reason phrase) nor the raw header block.
            static std::atomic_flag warned = ATOMIC_FLAG_INIT;
            unimplemented_once(warned, "WinHttpQueryHeaders: status line / raw header levels");
            return fail(http_contract::ERROR_WINHTTP_INTERNAL_ERROR);
        } else {
            const std::string header = query == http_contract::WINHTTP_QUERY_CUSTOM
                ? name : http_headers::header_name_for_level(level_name);
            if (request_headers) {
                u32 seen = 0;
                for (const auto& [k, v] : ri.headers)
                    if (http_headers::iequals(k, header) && seen++ == index) { value = v; break; }
            } else if (index == 0) {
                // NSHTTPURLResponse merges repeated fields into one value, so
                // only occurrence 0 exists (see report: repeated headers).
                std::string lower = header;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (auto it = ri.response_headers.find(lower); it != ri.response_headers.end())
                    value = it->second;
            }
        }
    } else if (index == 0 && !request_headers) {
        value = std::to_string(ri.status_code);
    }
    if (!value) return fail(http_contract::ERROR_WINHTTP_HEADER_NOT_FOUND);

    const u32 supplied = m.read_u32(buf_len_ptr).value_or(0);
    if (as_number || as_number64) {
        const u32 size = as_number64 ? 8u : 4u;
        // Measured: a short buffer fails with 122 and leaves the length.
        if (buf_ptr == 0 || supplied < size) return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        if (as_number64) {
            u64 v = 0;
            for (char c : http_headers::trim(*value)) { if (c < '0' || c > '9') break; v = v * 10 + static_cast<u64>(c - '0'); }
            (void)m.write_u64(buf_ptr, v);
        } else {
            (void)m.write_u32(buf_ptr, http_headers::leading_number(*value));
        }
        (void)m.write_u32(buf_len_ptr, size);
    } else if (as_time) {
        constexpr u32 size = 16;
        if (buf_ptr == 0 || supplied < size) {
            (void)m.write_u32(buf_len_ptr, size);
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        const auto time = http_headers::parse_http_date(*value);
        if (!time) return fail(http_contract::ERROR_WINHTTP_INVALID_QUERY_REQUEST);
        const u16 fields[8] = {time->year, time->month, time->day_of_week, time->day,
                               time->hour, time->minute, time->second, time->milliseconds};
        for (int i = 0; i < 8; ++i) (void)m.write_u16(buf_ptr + static_cast<GuestAddr>(i) * 2, fields[i]);
        (void)m.write_u32(buf_len_ptr, size);
    } else {
        const u64 required = (static_cast<u64>(value->size()) + 1u) * sizeof(u16);
        if (buf_ptr == 0 || supplied < required) {
            (void)m.write_u32(buf_len_ptr, static_cast<u32>(required));
            return fail(error_codes::ERROR_INSUFFICIENT_BUFFER);
        }
        if (m.write_wide_string(buf_ptr, *value).is_err()) return fail(error_codes::ERROR_NOACCESS);
        (void)m.write_u32(buf_len_ptr, static_cast<u32>(required - sizeof(u16)));
    }
    if (index_ptr != 0) (void)m.write_u32(index_ptr, index + 1);
    set_last_error(0);
    m.set_return(1);
}

void WinHttpManager::win_http_read_data(Marshaler& m) {
    // BOOL WinHttpReadData(HINTERNET hRequest, LPVOID lpBuffer,
    //     DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead)
    //
    // WinHTTP returns whatever is available (at least one byte, or zero at
    // the end of the response); callers loop until zero bytes come back.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr buf = m.arg_ptr(1);
    const u32 to_read = m.arg32(2);
    const GuestAddr bytes_read_ptr = m.arg_ptr(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    // The count is written only on success (measured: read-null/-session
    // leave it untouched).

    std::unique_lock lock(m_mutex);
    Object* object = live_object(h);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    auto& ri = m_requests[h];
    if (!ri.sent || ri.status_code == 0 || !ri.stream)
        return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    if (to_read != 0 && buf == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
    auto stream = ri.stream;
    const int timeout_ms = ri.receive_timeout.milliseconds;
    const bool async = request_is_async(ri);
    GuestMemory* memory = &m.memory();
    if (async) {
        m_memory = memory;
        m_guest_64 = m.cpu_is_64bit();
    }
    lock.unlock();

    // Reads into lpBuffer; returns the byte count, or -(WinHTTP error).
    auto read = [this, h, stream, timeout_ms, to_read, buf, memory]() -> i64 {
        int got = 0;
        std::vector<u8> scratch;
        if (to_read != 0) {
            scratch.resize(to_read);
            got = stream->read(scratch.data(), scratch.size(), timeout_ms);
        }
        std::lock_guard relock(m_mutex);
        if (!live_object(h)) return -static_cast<i64>(http_contract::ERROR_WINHTTP_OPERATION_CANCELLED);
        if (got < 0) {
            const bool finished = stream->finished() || stream->error_code() != 0;
            stream->cancel();
            VITREO_TRACE("[winhttp] WinHttpReadData failed (%s)\n", stream->error().c_str());
            return -static_cast<i64>(finished ? map_transport_error(stream->error_code(), true)
                                              : http_contract::ERROR_WINHTTP_TIMEOUT);
        }
        if (got > 0) {
            if (memory->write_bytes(buf, reinterpret_cast<const Byte*>(scratch.data()),
                                    static_cast<size_t>(got)).is_err())
                return -static_cast<i64>(error_codes::ERROR_NOACCESS);
            m_requests[h].bytes_read += static_cast<size_t>(got);
        }
        return got;
    };
    if (async) {
        // Learn WinHttpReadData: asynchronously, the call returns at once and
        // READ_COMPLETE carries lpBuffer and the byte count; the caller
        // passes lpdwNumberOfBytesRead as NULL.
        start_async_wait([this, h, buf, read] {
            const i64 result = read();
            if (result == -static_cast<i64>(http_contract::ERROR_WINHTTP_OPERATION_CANCELLED)) return;
            if (result >= 0)
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_READ_COMPLETE,
                                {CompletionInfo::Kind::Buffer, buf, static_cast<u32>(result), 0});
            else
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR,
                                {CompletionInfo::Kind::AsyncResult, 0, static_cast<u32>(-result),
                                 http_contract::API_READ_DATA});
        });
        set_last_error(0);
        m.set_return(1);
        return;
    }
    const i64 result = read();
    if (result < 0) return fail(static_cast<u32>(-result));
    if (bytes_read_ptr) (void)m.write_u32(bytes_read_ptr, static_cast<u32>(result));
    set_last_error(0);
    m.set_return(1);
}

void WinHttpManager::win_http_write_data(Marshaler& m) {
    // BOOL WinHttpWriteData(HINTERNET hRequest, LPCVOID lpBuffer,
    //     DWORD dwNumberOfBytesToWrite, LPDWORD lpdwNumberOfBytesWritten)
    // Measured: before WinHttpSendRequest, or on a request sent without
    // dwTotalLength > dwOptionalLength, fails with INCORRECT_HANDLE_STATE;
    // writing past dwTotalLength fails with ERROR_INVALID_PARAMETER; the
    // count is written only on success.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 to_write = m.arg32(2);
    const GuestAddr written_ptr = m.arg_ptr(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    std::unique_lock lock(m_mutex);
    Object* object = live_object(h);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    auto& ri = m_requests[h];
    if (!ri.sent || !ri.stream || ri.total_length == 0 || ri.status_code != 0)
        return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    if (ri.written + to_write > ri.total_length) return fail(error_codes::ERROR_INVALID_PARAMETER);
    std::vector<u8> bytes(to_write);
    if (to_write != 0 &&
        (buffer == 0 || m.memory().read_bytes(buffer, bytes.data(), bytes.size()).is_err()))
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    auto stream = ri.stream;
    ri.written += to_write;  // reserved before the (blocking) transfer
    const bool async = request_is_async(ri);
    if (async) {
        m_memory = &m.memory();
        m_guest_64 = m.cpu_is_64bit();
    }
    lock.unlock();

    // Sends the bytes; returns the WinHTTP error, or 0.
    auto write = [this, h, stream, bytes = std::move(bytes)]() -> u32 {
        const bool ok = bytes.empty() || stream->write_body(bytes.data(), bytes.size());
        std::lock_guard relock(m_mutex);
        if (!live_object(h)) return http_contract::ERROR_WINHTTP_OPERATION_CANCELLED;
        return ok ? 0u : map_transport_error(stream->error_code(), true);
    };
    if (async) {
        // WRITE_COMPLETE: lpvStatusInformation -> DWORD bytes written.
        start_async_wait([this, h, to_write, write = std::move(write)] {
            const u32 error = write();
            if (error == http_contract::ERROR_WINHTTP_OPERATION_CANCELLED) return;
            if (error == 0)
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,
                                {CompletionInfo::Kind::Dword, 0, to_write, 0});
            else
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR,
                                {CompletionInfo::Kind::AsyncResult, 0, error,
                                 http_contract::API_WRITE_DATA});
        });
        set_last_error(0);
        m.set_return(1);
        return;
    }
    const u32 error = write();
    if (error != 0) return fail(error);
    if (written_ptr) (void)m.write_u32(written_ptr, to_write);
    VITREO_TRACE("[winhttp] WinHttpWriteData(%u) -> TRUE\n", to_write);
    set_last_error(0);
    m.set_return(1);
}

void WinHttpManager::win_http_query_data_available(Marshaler& m) {
    // BOOL WinHttpQueryDataAvailable(HINTERNET hRequest,
    //     LPDWORD lpdwNumberOfBytesAvailable)
    // Waits until at least one byte is available or the response ended
    // (0 bytes). Measured: at the end of the response it succeeds with 0 and
    // ERROR_SUCCESS; on a slow body it waits for data.
    const u32 h = static_cast<u32>(m.arg(0));
    const GuestAddr avail_ptr = m.arg_ptr(1);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };

    std::unique_lock lock(m_mutex);
    Object* object = live_object(h);
    if (!object) return fail(error_codes::ERROR_INVALID_HANDLE);
    if (object->kind != Kind::Request) return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    auto& ri = m_requests[h];
    if (!ri.sent || ri.status_code == 0 || !ri.stream)
        return fail(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    auto stream = ri.stream;
    const int timeout_ms = ri.receive_timeout.milliseconds;
    const bool async = request_is_async(ri);
    if (async) {
        m_memory = &m.memory();
        m_guest_64 = m.cpu_is_64bit();
    }
    lock.unlock();

    // Returns the available byte count, or -(WinHTTP error).
    auto query = [this, h, stream, timeout_ms]() -> i64 {
        std::size_t available = 0;
        const bool ok = stream->wait_available(timeout_ms, available);
        std::lock_guard relock(m_mutex);
        if (!live_object(h)) return -static_cast<i64>(http_contract::ERROR_WINHTTP_OPERATION_CANCELLED);
        if (!ok) {
            const bool finished = stream->error_code() != 0;
            stream->cancel();
            return -static_cast<i64>(finished ? map_transport_error(stream->error_code(), true)
                                              : http_contract::ERROR_WINHTTP_TIMEOUT);
        }
        return static_cast<i64>(std::min<std::size_t>(available, 0xFFFFFFFFu));
    };
    if (async) {
        // Learn: asynchronously the count arrives with DATA_AVAILABLE,
        // lpvStatusInformation -> DWORD (winhttp_async_flow_probe: len=4,
        // value=577); lpdwNumberOfBytesAvailable is NULL and not written.
        start_async_wait([this, h, query] {
            const i64 result = query();
            if (result == -static_cast<i64>(http_contract::ERROR_WINHTTP_OPERATION_CANCELLED)) return;
            if (result >= 0)
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,
                                {CompletionInfo::Kind::Dword, 0, static_cast<u32>(result), 0});
            else
                post_completion(h, http_contract::WINHTTP_CALLBACK_STATUS_REQUEST_ERROR,
                                {CompletionInfo::Kind::AsyncResult, 0, static_cast<u32>(-result),
                                 http_contract::API_QUERY_DATA_AVAILABLE});
        });
        set_last_error(0);
        m.set_return(1);
        return;
    }
    const i64 result = query();
    if (result < 0) return fail(static_cast<u32>(-result));
    const u32 avail = static_cast<u32>(result);
    if (avail_ptr) (void)m.write_u32(avail_ptr, avail);
    VITREO_TRACE("[winhttp] WinHttpQueryDataAvailable -> TRUE (%u bytes)\n", avail);
    set_last_error(0);
    m.set_return(1);
}

// =============================================================================
// WebSocket
// =============================================================================

void WinHttpManager::win_http_websocket_complete_upgrade(Marshaler& m) {
    const u32 request_handle = static_cast<u32>(m.arg(0));
    const u64 context = m.arg(1);
    std::lock_guard lock(m_mutex);
    Object* object = live_object(request_handle);
    if (!object || object->kind != Kind::Request) {
        set_last_error(object ? http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_TYPE
                              : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    auto& request = m_requests[request_handle];
    if (!request.websocket_upgrade || request.status_code != 101 || !request.websocket) {
        set_last_error(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
        m.set_return(0);
        return;
    }
    const u32 handle = alloc_handle();
    WebSocketInfo info;
    info.stream = std::move(request.websocket);
    m_websockets.emplace(handle, std::move(info));
    (void)add_object(handle, Kind::WebSocket, 0);
    m_objects[handle].callback = object->callback;
    m_objects[handle].context = context != 0 ? context : object->context;
    winhttp_diag("websocket-complete-upgrade", handle, request_handle);
    m.set_return(handle);
}

void WinHttpManager::win_http_websocket_send(Marshaler& m) {
    constexpr u32 ERROR_INVALID_OPERATION = 4317;
    const u32 handle = static_cast<u32>(m.arg(0));
    const u32 buffer_type = m.arg32(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 length = m.arg32(3);
    std::unique_lock lock(m_mutex);
    auto socket = m_websockets.find(handle);
    winhttp_diag("websocket-send-enter", handle,
                 (static_cast<u64>(buffer_type) << 32) | length);
    if (socket == m_websockets.end() || !live_object(handle)) {
        m.set_return(error_codes::ERROR_INVALID_HANDLE);
        return;
    }
    if (socket->second.send_closed) {
        m.set_return(ERROR_INVALID_OPERATION);
        return;
    }
    if (buffer_type > 3 || (buffer == 0 && length != 0)) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    std::vector<u8> bytes(length);
    if (length != 0 &&
        !m.memory().read_bytes(buffer, bytes.data(), bytes.size()).is_ok()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    auto stream = socket->second.stream;
    lock.unlock();
    const bool utf8 = buffer_type == 2 || buffer_type == 3;
    const int result = stream->send(bytes.data(), bytes.size(), utf8, 30 * 1000);
    winhttp_diag("websocket-send-exit", handle, static_cast<u32>(result));
    m.set_return(result == 0 ? 0u : http_contract::ERROR_WINHTTP_CONNECTION_ERROR);
}

void WinHttpManager::win_http_websocket_receive(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 capacity = m.arg32(2);
    const GuestAddr bytes_read_out = m.arg_ptr(3);
    const GuestAddr buffer_type_out = m.arg_ptr(4);
    std::unique_lock lock(m_mutex);
    auto socket = m_websockets.find(handle);
    winhttp_diag("websocket-receive-enter", handle, capacity);
    if (socket == m_websockets.end() || !live_object(handle)) {
        m.set_return(error_codes::ERROR_INVALID_HANDLE);
        return;
    }
    if (buffer == 0 || capacity == 0 || bytes_read_out == 0 ||
        buffer_type_out == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    auto stream = socket->second.stream;
    lock.unlock();
    std::vector<u8> bytes(capacity);
    std::size_t bytes_read = 0;
    bool utf8 = false;
    bool final = true;
    bool closed = false;
    const int result = stream->receive(bytes.data(), bytes.size(), bytes_read, utf8, final,
                                       closed, 30 * 1000);
    winhttp_diag("websocket-receive-exit", handle,
                 (static_cast<u64>(static_cast<u32>(result)) << 32) |
                     static_cast<u32>(bytes_read));
    if (result != 0) {
        m.set_return(http_contract::ERROR_WINHTTP_TIMEOUT);
        return;
    }
    if (bytes_read != 0 &&
        !m.memory().write_bytes(buffer, bytes.data(), bytes_read).is_ok()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    (void)m.write_u32(bytes_read_out, static_cast<u32>(bytes_read));
    const u32 buffer_type = closed ? 4u
        : utf8 ? (final ? 2u : 3u)
               : (final ? 0u : 1u);
    (void)m.write_u32(buffer_type_out, buffer_type);
    m.set_return(0);
}

void WinHttpManager::win_http_websocket_shutdown(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    const u16 status = static_cast<u16>(m.arg(1));
    const GuestAddr reason = m.arg_ptr(2);
    const u32 reason_length = m.arg32(3);
    std::unique_lock lock(m_mutex);
    auto socket = m_websockets.find(handle);
    winhttp_diag("websocket-shutdown", handle,
                 (static_cast<u64>(status) << 32) | reason_length);
    if (socket == m_websockets.end() || !live_object(handle)) {
        m.set_return(error_codes::ERROR_INVALID_HANDLE);
        return;
    }
    if ((reason == 0 && reason_length != 0) || reason_length > 123) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    std::vector<u8> reason_bytes(reason_length);
    if (reason_length != 0 &&
        !m.memory().read_bytes(reason, reason_bytes.data(), reason_length).is_ok()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    auto stream = socket->second.stream;
    lock.unlock();
    const int result = stream->shutdown(status, reason_bytes.data(), reason_bytes.size());
    if (result == 0) {
        std::lock_guard relock(m_mutex);
        if (auto again = m_websockets.find(handle); again != m_websockets.end())
            again->second.send_closed = true;
    }
    m.set_return(result == 0 ? 0u : error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// Credentials
// =============================================================================

void WinHttpManager::win_http_set_credentials(Marshaler& m) {
    // BOOL WinHttpSetCredentials(HINTERNET hRequest, DWORD AuthTargets,
    //     DWORD AuthScheme, LPCWSTR pwszUserName, LPCWSTR pwszPassword,
    //     LPVOID pAuthParams)
    std::lock_guard lock(m_mutex);
    Object* object = m.arg(0) > UINT32_MAX ? nullptr : live_object(static_cast<u32>(m.arg(0)));
    if (!object || object->kind != Kind::Request) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    auto& request = m_requests[static_cast<u32>(m.arg(0))];
    const u32 target = m.arg32(1);
    const u32 scheme = m.arg32(2);
    if (target > http_contract::WINHTTP_AUTH_TARGET_PROXY ||
        (scheme != http_contract::WINHTTP_AUTH_SCHEME_BASIC && scheme != http_contract::WINHTTP_AUTH_SCHEME_NTLM &&
         scheme != http_contract::WINHTTP_AUTH_SCHEME_PASSPORT && scheme != http_contract::WINHTTP_AUTH_SCHEME_DIGEST &&
         scheme != http_contract::WINHTTP_AUTH_SCHEME_NEGOTIATE)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (target != http_contract::WINHTTP_AUTH_TARGET_SERVER || scheme != http_contract::WINHTTP_AUTH_SCHEME_BASIC) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        unimplemented_once(warned, "WinHttpSetCredentials: proxy or non-Basic authentication");
        set_last_error(http_contract::ERROR_WINHTTP_INTERNAL_ERROR);
        m.set_return(0);
        return;
    }
    // Windows Basic credentials encode each UTF-16 unit as ISO-8859-1;
    // nonrepresentable characters fail rather than being replaced. Measured
    // by http_options_contract_probe.c on Windows in both guest modes.
    std::string credentials;
    auto read_latin1 = [&](GuestAddr address) {
        if (!address) return false;
        for (GuestAddr current = address; current <= UINT64_MAX - 2; current += 2) {
            auto value = m.memory().read_u16(current);
            if (value.is_err() || value.value() > 255) return false;
            if (value.value() == 0) return true;
            credentials.push_back(static_cast<char>(value.value()));
        }
        return false;
    };
    if (!read_latin1(m.arg_ptr(3))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return;
    }
    credentials.push_back(':');
    if (!read_latin1(m.arg_ptr(4))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return;
    }
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded = "Basic ";
    for (size_t i = 0; i < credentials.size(); i += 3) {
        const u32 a = static_cast<u8>(credentials[i]);
        const u32 b = i + 1 < credentials.size() ? static_cast<u8>(credentials[i + 1]) : 0;
        const u32 c = i + 2 < credentials.size() ? static_cast<u8>(credentials[i + 2]) : 0;
        encoded.push_back(alphabet[a >> 2]);
        encoded.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        encoded.push_back(i + 1 < credentials.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        encoded.push_back(i + 2 < credentials.size() ? alphabet[c & 63] : '=');
    }
    request.authorization = std::move(encoded);
    set_last_error(0);
    m.set_return(1);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_winhttp_thunks(WinHttpManager& mgr) {
    DllExports exports;
    exports.entries.reserve(40);
    auto add = [&](const char* name, u16 ordinal, void (WinHttpManager::*method)(Marshaler&),
                   u16 stdcall_bytes) {
        exports.entries.push_back(ExportEntry{
            name, ordinal, [&mgr, method](Marshaler& m) { (mgr.*method)(m); }, stdcall_bytes});
    };
    // Argument counts from the SDK winhttp.h prototypes (x86 stdcall bytes).
    add("WinHttpOpen", 1, &WinHttpManager::win_http_open, 20);
    add("WinHttpConnect", 2, &WinHttpManager::win_http_connect, 16);
    add("WinHttpOpenRequest", 3, &WinHttpManager::win_http_open_request, 28);
    add("WinHttpCloseHandle", 4, &WinHttpManager::win_http_close_handle, 4);
    add("WinHttpSendRequest", 5, &WinHttpManager::win_http_send_request, 28);
    add("WinHttpReceiveResponse", 6, &WinHttpManager::win_http_receive_response, 8);
    add("WinHttpQueryHeaders", 7, &WinHttpManager::win_http_query_headers, 24);
    add("WinHttpReadData", 8, &WinHttpManager::win_http_read_data, 16);
    add("WinHttpWriteData", 9, &WinHttpManager::win_http_write_data, 16);
    add("WinHttpQueryDataAvailable", 10, &WinHttpManager::win_http_query_data_available, 8);
    add("WinHttpSetOption", 11, &WinHttpManager::win_http_set_option, 16);
    add("WinHttpQueryOption", 12, &WinHttpManager::win_http_query_option, 16);
    add("WinHttpSetTimeouts", 13, &WinHttpManager::win_http_set_timeouts, 20);
    add("WinHttpCrackUrl", 14, &WinHttpManager::win_http_crack_url, 16);
    add("WinHttpCreateUrl", 15, &WinHttpManager::win_http_create_url, 16);
    add("WinHttpGetProxyForUrl", 16, &WinHttpManager::win_http_get_proxy_for_url, 16);
    add("WinHttpGetDefaultProxyConfiguration", 17, &WinHttpManager::win_http_get_default_proxy_configuration, 4);
    add("WinHttpGetIEProxyConfigForCurrentUser", 18, &WinHttpManager::win_http_get_ie_proxy_config, 4);
    add("WinHttpSetStatusCallback", 19, &WinHttpManager::win_http_set_status_callback, 16);
    add("WinHttpSetCredentials", 20, &WinHttpManager::win_http_set_credentials, 24);
    add("WinHttpAddRequestHeaders", 21, &WinHttpManager::win_http_add_request_headers, 16);
    add("WinHttpDetectAutoProxyConfigUrl", 22, &WinHttpManager::win_http_detect_auto_proxy_config_url, 8);
    add("WinHttpCreateProxyResolver", 0, &WinHttpManager::win_http_create_proxy_resolver, 8);
    add("WinHttpGetProxyForUrlEx", 0, &WinHttpManager::win_http_get_proxy_for_url_ex, 16);
    add("WinHttpGetProxyResult", 0, &WinHttpManager::win_http_get_proxy_result, 8);
    add("WinHttpFreeProxyResult", 0, &WinHttpManager::win_http_free_proxy_result, 4);

    // WinHttpGetProxyForUrlEx2 / WinHttpGetProxyResultEx take an interface
    // selection context and WINHTTP_PROXY_RESULT_EX, which Vitreo does not
    // model; they fail loudly instead of ignoring those fields.
    exports.entries.push_back(ExportEntry{
        "WinHttpGetProxyForUrlEx2", 0,
        [](Marshaler& m) {
            static std::atomic_flag warned = ATOMIC_FLAG_INIT;
            unimplemented_once(warned, "WinHttpGetProxyForUrlEx2: interface selection context");
            (void)m;
            m.set_return(http_contract::ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR);
        },
        24});
    exports.entries.push_back(ExportEntry{
        "WinHttpGetProxyResultEx", 0,
        [](Marshaler& m) {
            static std::atomic_flag warned = ATOMIC_FLAG_INIT;
            unimplemented_once(warned, "WinHttpGetProxyResultEx: WINHTTP_PROXY_RESULT_EX");
            m.set_return(http_contract::ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "WinHttpFreeProxyResultEx", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); },  // nothing is ever produced by ResultEx
        4});
    exports.entries.push_back(ExportEntry{
        "WinHttpFreeProxySettings", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); },  // no Vitreo API produces WINHTTP_PROXY_SETTINGS
        4});

    add("WinHttpWebSocketCompleteUpgrade", 0, &WinHttpManager::win_http_websocket_complete_upgrade, 8);
    add("WinHttpWebSocketSend", 0, &WinHttpManager::win_http_websocket_send, 16);
    add("WinHttpWebSocketReceive", 0, &WinHttpManager::win_http_websocket_receive, 20);
    add("WinHttpWebSocketShutdown", 0, &WinHttpManager::win_http_websocket_shutdown, 16);
    return exports;
}

} // namespace vitreo::winapi
