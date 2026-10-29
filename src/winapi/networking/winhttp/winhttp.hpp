// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file winhttp.hpp
/// @brief winhttp.dll — WinHTTP API with NSURLSession delegation
///
/// Implements the WinHTTP surface area with actual HTTP transport via
/// NSURLSession. Session/connection/request handles track state so that
/// the WinHttpConnect → WinHttpOpenRequest → WinHttpSendRequest flow
/// results in a real HTTP request on macOS.
///
/// Threading: every entry point may be called from any guest thread. All
/// handle state is guarded by one mutex that is never held across a blocking
/// transport wait or a guest callback. Guest callbacks for synchronous
/// notifications run on the calling guest thread; asynchronous completions
/// (WinHttpGetProxyForUrlEx) run on the WinHTTP service thread through the
/// async invoker.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "http_bridge.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class Marshaler;
class WinHttpContractTestAccessor;

class WinHttpManager {
public:
    WinHttpManager();
    ~WinHttpManager();
    WinHttpManager(const WinHttpManager&) = delete;
    WinHttpManager& operator=(const WinHttpManager&) = delete;

    // Session / connection lifecycle
    void win_http_open(Marshaler& m);
    void win_http_connect(Marshaler& m);
    void win_http_open_request(Marshaler& m);
    void win_http_close_handle(Marshaler& m);

    // Request / response
    void win_http_send_request(Marshaler& m);
    void win_http_receive_response(Marshaler& m);
    void win_http_query_headers(Marshaler& m);
    void win_http_read_data(Marshaler& m);
    void win_http_write_data(Marshaler& m);
    void win_http_query_data_available(Marshaler& m);

    // WebSocket upgrade / messages
    void win_http_websocket_complete_upgrade(Marshaler& m);
    void win_http_websocket_send(Marshaler& m);
    void win_http_websocket_receive(Marshaler& m);
    void win_http_websocket_shutdown(Marshaler& m);

    // Options
    void win_http_set_option(Marshaler& m);
    void win_http_query_option(Marshaler& m);
    void win_http_set_timeouts(Marshaler& m);

    // URL helpers
    void win_http_crack_url(Marshaler& m);
    void win_http_create_url(Marshaler& m);

    // Proxy
    void win_http_get_proxy_for_url(Marshaler& m);
    void win_http_get_default_proxy_configuration(Marshaler& m);
    void win_http_get_ie_proxy_config(Marshaler& m);
    void win_http_set_status_callback(Marshaler& m);
    void win_http_create_proxy_resolver(Marshaler& m);
    void win_http_get_proxy_for_url_ex(Marshaler& m);
    void win_http_get_proxy_result(Marshaler& m);
    void win_http_free_proxy_result(Marshaler& m);

    // Credentials
    void win_http_set_credentials(Marshaler& m);

    // Misc
    void win_http_add_request_headers(Marshaler& m);
    void win_http_detect_auto_proxy_config_url(Marshaler& m);

    /// Dispatch WINHTTP_STATUS_CALLBACK through the runtime-owned guest
    /// execution boundary. WinHTTP owns notification selection and state;
    /// the runtime owns execution of guest code.
    using StatusCallbackInvoker =
        std::function<void(GuestAddr procedure, u32 handle, u64 context,
                           u32 status, GuestAddr status_information,
                           u32 status_information_length)>;
    /// Runs the callback on the calling guest thread (synchronous
    /// notifications such as HANDLE_CREATED / HANDLE_CLOSING).
    void set_status_callback_invoker(StatusCallbackInvoker invoker) {
        m_status_callback_invoker = std::move(invoker);
    }
    /// Runs the callback from the WinHTTP service (host) thread; the runtime
    /// gives that thread its own guest thread context.
    void set_async_callback_invoker(StatusCallbackInvoker invoker) {
        m_async_callback_invoker = std::move(invoker);
    }

    /// Guest process-heap allocations (GlobalAlloc GMEM_FIXED semantics):
    /// strings the guest frees with GlobalFree, proxy results, and the
    /// lpvStatusInformation blocks handed to callbacks.
    using GuestAlloc = std::function<GuestAddr(u64 size)>;
    using GuestFree = std::function<void(GuestAddr)>;
    void set_guest_heap(GuestAlloc alloc, GuestFree free) {
        m_guest_alloc = std::move(alloc);
        m_guest_free = std::move(free);
    }

    /// Autoproxy engine (PAC discovery/download/evaluation). Defaults to the
    /// macOS implementation; tests substitute a deterministic one.
    using ProxyEngine = std::function<ProxyResolution(const ProxyQuery&)>;
    void set_proxy_engine(ProxyEngine engine) { m_proxy_engine = std::move(engine); }
    /// System (IE-equivalent) proxy settings source; tests substitute one.
    using SystemProxySource = std::function<SystemProxyConfig()>;
    void set_system_proxy_source(SystemProxySource source) {
        m_system_proxy = std::move(source);
    }

private:
    friend class WinHttpContractTestAccessor;

    struct CallbackInfo {
        GuestAddr procedure = 0;
        u32 notification_flags = 0;
    };

    enum class Kind : u8 { Session, Connection, Request, WebSocket, Resolver };

    /// Common handle state. A handle closed by the guest while it still has
    /// open children stays here (guest_closed) until the last child closes;
    /// WinHTTP then reports HANDLE_CLOSING for the child and then the parent
    /// (measured, http_winhttp_handles_probe closing-child-after-parent).
    struct Object {
        Kind kind = Kind::Session;
        u32 parent = 0;
        u32 live_children = 0;
        bool guest_closed = false;
        CallbackInfo callback;
        u64 context = 0;
    };

    struct ReceiveTimeout {
        // Phase-independent deadlines require another transport boundary.
        // This mode is used only when resolve/connect/send are all infinite.
        bool unbounded_transport = false;
        int milliseconds = 30000;
        u32 requested[4] = {0, 60000, 30000, 30000};
    };

    struct SessionInfo {
        ReceiveTimeout receive_timeout;
        bool async = false;
        std::string agent;  ///< pszAgentW: the User-Agent request header
    };

    struct ConnInfo {
        std::string server;
        u16 port = 0;  ///< 0 = INTERNET_DEFAULT_PORT (scheme default)
    };

    struct RequestInfo {
        ReceiveTimeout receive_timeout;
        u32 conn_handle = 0;
        std::string method;
        std::string path;
        std::string agent;
        /// Request headers in insertion order (name, value).
        std::vector<std::pair<std::string, std::string>> headers;
        // Request-scoped authentication, never inherited by another request.
        std::string authorization;
        bool tls = false;

        // After send
        bool sent = false;
        bool websocket_upgrade = false;
        int  status_code = 0;
        std::unordered_map<std::string, std::string> response_headers;

        // Request body streaming (dwTotalLength > dwOptionalLength).
        u64 total_length = 0;
        u64 written = 0;

        std::shared_ptr<HttpStream> stream;
        std::shared_ptr<WebSocketStream> websocket;
        std::size_t bytes_read = 0;
    };

    struct WebSocketInfo {
        std::shared_ptr<WebSocketStream> stream;
        bool send_closed = false;
    };

    struct ResolverInfo {
        enum class State : u8 { Idle, Pending, Complete, Failed };
        State state = State::Idle;
        std::vector<ProxyEntry> result;
    };

    struct Notification {
        u32 handle = 0;
        CallbackInfo callback;
        u64 context = 0;
        u32 status = 0;
    };

    // --- handle table (m_mutex) --------------------------------------------
    mutable std::mutex m_mutex;
    u32 m_next_handle = 0xA0010000;
    std::unordered_map<u32, Object> m_objects;
    std::unordered_map<u32, SessionInfo> m_sessions;
    std::unordered_map<u32, ConnInfo> m_connections;
    std::unordered_map<u32, RequestInfo> m_requests;
    std::unordered_map<u32, WebSocketInfo> m_websockets;
    std::unordered_map<u32, ResolverInfo> m_resolvers;
    // Global option storage (keyed by dwOption) for options not yet modelled
    // per handle.
    std::unordered_map<u32, std::vector<u8>> m_options;

    u32 alloc_handle() { return m_next_handle++; }
    /// Live (not guest-closed) object, or nullptr.
    Object* live_object(u32 handle);
    /// Register a new handle as a child of @p parent (0 = none), inheriting
    /// the parent's callback and context. Returns the HANDLE_CREATED note.
    Notification add_object(u32 handle, Kind kind, u32 parent);
    /// Destroy @p handle (and any guest-closed ancestors it was keeping
    /// alive), appending HANDLE_CLOSING notes in Windows order.
    void destroy_object(u32 handle, std::vector<Notification>& notes);

    // --- notifications ------------------------------------------------------
    /// Invoke on the calling guest thread; must be called without m_mutex.
    void notify_status(u32 handle, const CallbackInfo& callback, u64 context,
                       u32 status, GuestAddr status_information,
                       u32 status_information_length);
    /// HANDLE_CREATED / HANDLE_CLOSING: lpvStatusInformation points to the
    /// handle value (pointer-sized), in guest memory.
    void notify_handle(Marshaler& m, const Notification& note);
    StatusCallbackInvoker m_status_callback_invoker;
    StatusCallbackInvoker m_async_callback_invoker;
    GuestAlloc m_guest_alloc;
    GuestFree m_guest_free;
    ProxyEngine m_proxy_engine;
    SystemProxySource m_system_proxy;

    // --- asynchronous request completions ----------------------------------
    /// What a completion's lpvStatusInformation / dwStatusInformationLength
    /// carry (Learn WINHTTP_STATUS_CALLBACK).
    struct CompletionInfo {
        enum class Kind : u8 {
            None,         ///< NULL, 0
            Buffer,       ///< the caller's buffer, length = value
            Dword,        ///< pointer to a DWORD holding value, sizeof(DWORD)
            AsyncResult,  ///< WINHTTP_ASYNC_RESULT {api, value (dwError)}
        };
        Kind kind = Kind::None;
        GuestAddr address = 0;
        u32 value = 0;
        u32 api = 0;
    };
    /// True when @p request belongs to a WINHTTP_FLAG_ASYNC session.
    /// Caller holds m_mutex.
    [[nodiscard]] bool request_is_async(const RequestInfo& request) const;
    /// Deliver @p status for @p handle from the WinHTTP service thread, after
    /// the API that started the operation has returned. A handle closed in
    /// the meantime gets no completion (HANDLE_CLOSING was its last).
    void post_completion(u32 handle, u32 status, CompletionInfo info);
    /// Run @p wait on its own host thread: it blocks on the transport and
    /// then posts its completion. The calling guest thread is never held.
    void start_async_wait(std::function<void()> wait);

    // --- WinHTTP service thread (asynchronous completions) -----------------
    void post_job(std::function<void()> job);
    void worker_loop();
    /// Waits started by start_async_wait that have not finished; the
    /// destructor drains them before the service thread stops.
    usize m_active_waits = 0;
    std::mutex m_job_mutex;
    std::condition_variable m_job_cv;
    std::deque<std::function<void()>> m_jobs;
    bool m_stop = false;
    std::thread m_worker;
    bool m_guest_64 = true;  ///< bitness of the process (for async writes)
    GuestMemory* m_memory = nullptr;  ///< guest memory for service-thread writes
};

/// Create winhttp.dll export thunks
DllExports create_winhttp_thunks(WinHttpManager& mgr);

} // namespace vitreo::winapi
