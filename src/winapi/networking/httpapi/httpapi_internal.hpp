// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file httpapi_internal.hpp
/// @brief httpapi.dll private state: http.h layouts and constants, server
///        sessions, URL groups, request queues, connections and requests.

#include "httpapi.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>

namespace vitreo::winapi::httpapi {

// ---------------------------------------------------------------------------
// http.h / winerror.h / ntstatus.h values (Windows SDK 10.0.26100.0)
// ---------------------------------------------------------------------------

constexpr u32 NO_ERROR_ = 0;
constexpr u32 kErrorFileNotFound = 2;
constexpr u32 kErrorInvalidHandle = 6;
constexpr u32 kErrorBadCommand = 22;
constexpr u32 kErrorSharingViolation = 32;
constexpr u32 kErrorHandleEof = 38;
constexpr u32 kErrorNotSupported = 50;
constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorInsufficientBuffer = 122;
constexpr u32 kErrorAlreadyExists = 183;
constexpr u32 kErrorMoreData = 234;
constexpr u32 kErrorOperationAborted = 995;
constexpr u32 kErrorIoPending = 997;
constexpr u32 kErrorNoAccess = 998;
constexpr u32 kErrorDllInitFailed = 1114;
constexpr u32 kErrorInvalidDll = 1154;
constexpr u32 kErrorConnectionInvalid = 1229;
constexpr u32 kErrorRevisionMismatch = 1306;

constexpr u32 kStatusSuccess = 0;
constexpr u32 kStatusPending = 0x00000103;
constexpr u32 kStatusBufferOverflow = 0x80000005;
constexpr u32 kStatusEndOfFile = 0xC0000011;
constexpr u32 kStatusCancelled = 0xC0000120;
constexpr u32 kStatusConnectionInvalid = 0xC000023A;

constexpr u32 HTTP_INITIALIZE_SERVER = 0x1;
constexpr u32 HTTP_INITIALIZE_CONFIG = 0x2;
constexpr u32 HTTP_DEMAND_CBT = 0x4;

constexpr u32 HTTP_CREATE_REQUEST_QUEUE_FLAG_OPEN_EXISTING = 0x1;
constexpr u32 HTTP_CREATE_REQUEST_QUEUE_FLAG_CONTROLLER = 0x2;
constexpr u32 HTTP_CREATE_REQUEST_QUEUE_FLAG_DELEGATION = 0x8;

constexpr u32 HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY = 0x1;
constexpr u32 HTTP_RECEIVE_REQUEST_FLAG_FLUSH_BODY = 0x2;
constexpr u32 HTTP_RECEIVE_REQUEST_ENTITY_BODY_FLAG_FILL_BUFFER = 0x1;

constexpr u32 HTTP_SEND_RESPONSE_FLAG_DISCONNECT = 0x1;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_MORE_DATA = 0x2;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_BUFFER_DATA = 0x4;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_ENABLE_NAGLING = 0x8;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_PROCESS_RANGES = 0x20;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_OPAQUE = 0x40;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_GOAWAY = 0x100;
constexpr u32 HTTP_SEND_RESPONSE_FLAG_AUTOMATIC_CHUNKING = 0x200;
constexpr u32 kSendFlagsMask = 0x36F;

constexpr u32 HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS = 0x1;
constexpr u32 HTTP_URL_FLAG_REMOVE_ALL = 0x1;

// HTTP_SERVER_PROPERTY
constexpr u32 HttpServerAuthenticationProperty = 0;
constexpr u32 HttpServerLoggingProperty = 1;
constexpr u32 HttpServerQosProperty = 2;
constexpr u32 HttpServerTimeoutsProperty = 3;
constexpr u32 HttpServerQueueLengthProperty = 4;
constexpr u32 HttpServerStateProperty = 5;
constexpr u32 HttpServer503VerbosityProperty = 6;
constexpr u32 HttpServerBindingProperty = 7;
constexpr u32 HttpServerExtendedAuthenticationProperty = 8;
constexpr u32 HttpServerListenEndpointProperty = 9;
constexpr u32 HttpServerChannelBindProperty = 10;
constexpr u32 HttpServerProtectionLevelProperty = 11;
constexpr u32 HttpServerDelegationProperty = 16;
constexpr u32 HttpServerFastForwardingProperty = 18;
constexpr u32 HttpServerRequestInfoProperty = 19;

constexpr u32 HttpEnabledStateActive = 0;
constexpr u32 HttpEnabledStateInactive = 1;
constexpr u32 Http503ResponseVerbosityBasic = 0;
constexpr u32 Http503ResponseVerbosityFull = 2;
constexpr u32 HTTP_MAX_SERVER_QUEUE_LENGTH = 0x7FFFFFFF;
constexpr u32 HTTP_MIN_SERVER_QUEUE_LENGTH = 1;

// HTTP_DATA_CHUNK_TYPE
constexpr u32 HttpDataChunkFromMemory = 0;
constexpr u32 HttpDataChunkFromFileHandle = 1;
constexpr u32 HttpDataChunkFromFragmentCache = 2;
constexpr u32 HttpDataChunkFromFragmentCacheEx = 3;
constexpr u32 HttpDataChunkTrailers = 4;
constexpr u32 HttpDataChunkFromWinHttpFastForwarding = 5;
constexpr u32 HttpDataChunkMaximum = 6;

// HTTP_RESPONSE_INFO_TYPE
constexpr u32 HttpResponseInfoTypeMultipleKnownHeaders = 0;

// HTTP_HEADER_ID
constexpr u32 HttpHeaderConnection = 1;
constexpr u32 HttpHeaderDate = 2;
constexpr u32 HttpHeaderTransferEncoding = 6;
constexpr u32 HttpHeaderContentLength = 11;
constexpr u32 HttpHeaderExpect = 26;
constexpr u32 HttpHeaderHost = 28;
constexpr u32 HttpHeaderRange = 37;
constexpr u32 HttpHeaderServer = 26;  // response-header id space
constexpr u32 HttpHeaderRequestMaximum = 41;
constexpr u32 HttpHeaderResponseMaximum = 30;

// HTTP_FEATURE_ID
constexpr u32 HttpFeatureAutomaticChunkedEncoding = 8;

// HTTP_CACHE_POLICY_TYPE
constexpr u32 HttpCachePolicyNocache = 0;

// HTTP_VERB
enum : u32 {
    HttpVerbUnparsed, HttpVerbUnknown, HttpVerbInvalid, HttpVerbOPTIONS,
    HttpVerbGET, HttpVerbHEAD, HttpVerbPOST, HttpVerbPUT, HttpVerbDELETE,
    HttpVerbTRACE, HttpVerbCONNECT, HttpVerbTRACK, HttpVerbMOVE, HttpVerbCOPY,
    HttpVerbPROPFIND, HttpVerbPROPPATCH, HttpVerbMKCOL, HttpVerbLOCK,
    HttpVerbUNLOCK, HttpVerbSEARCH,
};

// ---------------------------------------------------------------------------
// Structure layouts. Every number is a line of httpapi_probe{32,64}.expected
// (offsetof / sizeof from the SDK http.h); test_httpapi.cpp audits them.
// ---------------------------------------------------------------------------

struct Layout {
    u32 ptr;
    u32 request_v1_size, request_v2_size;
    u32 rq_flags, rq_connection_id, rq_request_id, rq_url_context, rq_version,
        rq_verb, rq_unknown_verb_length, rq_raw_url_length, rq_unknown_verb,
        rq_raw_url, rq_cooked_url, rq_address, rq_headers, rq_bytes_received,
        rq_entity_chunk_count, rq_entity_chunks, rq_raw_connection_id,
        rq_ssl_info, rq_request_info_count, rq_request_info;
    // HTTP_COOKED_URL: four USHORT lengths, then four pointers.
    u32 cooked_full_url, cooked_host, cooked_abs_path, cooked_query;
    // HTTP_REQUEST_HEADERS / HTTP_RESPONSE_HEADERS
    u32 hdr_unknown_count, hdr_unknown, hdr_trailer_count, hdr_trailers, hdr_known;
    u32 known_header_size, known_header_value;
    u32 unknown_header_size, unknown_header_name, unknown_header_value;
    u32 data_chunk_size, chunk_buffer, chunk_buffer_length;
    u32 response_v1_size, response_v2_size;
    u32 rs_flags, rs_version, rs_status, rs_reason_length, rs_reason, rs_headers,
        rs_entity_chunk_count, rs_entity_chunks, rs_response_info_count, rs_response_info;
    u32 response_info_size, multiple_known_size;
    u32 binding_size, binding_queue;
    u32 state_size, timeout_size, qos_size, auth_size, logging_size,
        channel_bind_size, protection_size, listen_endpoint_size, cache_policy_size;
};

inline constexpr Layout kLayout64{
    8, 848, 864,
    0, 8, 16, 24, 32, 36, 40, 42, 48, 56, 64, 104, 120, 808, 816, 824, 832, 840, 848, 856,
    8, 16, 24, 32,
    0, 8, 16, 24, 32,
    16, 8,
    24, 8, 16,
    32, 8, 16,
    552, 568,
    0, 4, 8, 10, 16, 24, 536, 544, 552, 560,
    16, 24,
    16, 8,
    8, 20, 16, 64, 72, 24, 8, 8, 8,
};

inline constexpr Layout kLayout32{
    4, 464, 472,
    0, 8, 16, 24, 32, 36, 40, 42, 44, 48, 52, 76, 84, 432, 440, 444, 448, 456, 464, 468,
    8, 12, 16, 20,
    0, 4, 8, 12, 16,
    8, 4,
    12, 4, 8,
    32, 8, 12,
    280, 288,
    0, 4, 8, 10, 12, 16, 272, 276, 280, 284,
    12, 16,
    8, 4,
    8, 20, 8, 36, 48, 16, 8, 8, 8,
};

[[nodiscard]] inline const Layout& layout_for(bool is_64bit) {
    return is_64bit ? kLayout64 : kLayout32;
}

/// One "[vitreo] UNIMPLEMENTED httpapi.dll!<api>: <case>" line per case.
void unimplemented_once(const char* api, const char* what);

// Header names for HTTP_HEADER_ID (RFC 7230-7235 spellings).
extern const std::array<const char*, HttpHeaderRequestMaximum> kRequestHeaderNames;
extern const std::array<const char*, HttpHeaderResponseMaximum> kResponseHeaderNames;

// ---------------------------------------------------------------------------
// Object model
// ---------------------------------------------------------------------------

/// HTTP_TIMEOUT_LIMIT_INFO in seconds (0 = use the default, two minutes).
struct Timeouts {
    u16 entity_body = 0, drain_entity_body = 0, request_queue = 0,
        idle_connection = 0, header_wait = 0;
    u32 min_send_rate = 0;
};

/// The configuration a server session or URL group carries; a URL group
/// inherits each property its own scope has not set.
struct PropertyScope {
    std::optional<u32> state;
    std::optional<Timeouts> timeouts;
};

struct Queue;
struct Connection;

enum class HostKind { Strong, Named, Weak };

struct UrlEntry {
    std::u16string text;        // as registered (for HttpRemoveUrl*)
    std::string key;            // canonical, lower-case: identity of the URL
    HostKind kind = HostKind::Named;
    std::string host;           // lower-case, no port; empty for + and *
    u16 port = 0;
    std::string path;           // lower-case decoded path, ends in '/'
    u64 context = 0;
    u64 group = 0;
};

struct UrlGroup {
    u64 id = 0;
    u64 session = 0;            // 0: the implicit group of an HttpAddUrl queue
    PropertyScope props;
    bool bound = false;
    std::weak_ptr<Queue> queue;
    std::vector<std::shared_ptr<UrlEntry>> urls;
};

struct Session {
    u64 id = 0;
    u32 version_major = 2;
    PropertyScope props;
};

/// A receive waiting for a request, an entity body, or a disconnect.
struct Waiter {
    enum class Kind { Request, EntityBody, Disconnect } kind = Kind::Request;
    Handle handle = 0;
    u64 request_id = 0;         // EntityBody: the request; Request: retry id or 0
    u64 connection_id = 0;      // Disconnect
    u32 flags = 0;
    GuestAddr buffer = 0;
    u32 length = 0;
    GuestAddr bytes_out = 0;
    GuestAddr overlapped = 0;   // 0: a synchronous caller blocked on State::cv
    bool is_64bit = true;
    u32 thread_id = 0;
    // Result for a synchronous caller.
    bool done = false;
    u32 error = 0;
    u32 bytes = 0;
};

struct Request {
    u64 id = 0;
    std::shared_ptr<Connection> connection;
    std::weak_ptr<Queue> queue;
    enum class Stage { Queued, AwaitRetry, Delivered, Responding, Done } stage = Stage::Queued;

    // Request line and headers.
    std::string method;
    u32 verb = HttpVerbUnknown;
    u16 major = 1, minor = 1;
    std::string raw_url;
    std::u16string full_url;
    usize host_at = 0, host_len = 0, abs_at = 0, abs_len = 0, query_at = 0, query_len = 0;
    bool has_query = false;
    std::array<std::optional<std::string>, HttpHeaderRequestMaximum> known;
    std::vector<std::pair<std::string, std::string>> unknown;
    u64 url_context = 0;
    usize head_bytes = 0;

    // Entity body, filled by the connection's reader.
    bool chunked = false;
    u64 content_length = 0;     // when !chunked
    u64 body_wire_bytes = 0;    // body bytes read off the wire
    std::string body;           // decoded, not yet handed to the application
    bool body_complete = true;

    // Response state.
    bool close_after = false;
    bool auto_chunk = false;
    bool aborted = false;
    std::chrono::steady_clock::time_point queued_at;
};

struct Queue {
    std::vector<Handle> handles;
    u32 version_major = 2;
    std::u16string name;
    bool controller = false;
    bool shut_down = false;
    bool closed = false;
    u32 queue_length = 1000;
    u32 verbosity = Http503ResponseVerbosityBasic;
    u32 state = HttpEnabledStateActive;
    std::deque<std::shared_ptr<Request>> queued;
    std::unordered_map<u64, std::shared_ptr<Request>> requests;
    std::deque<std::shared_ptr<Waiter>> waiters;
    std::shared_ptr<UrlGroup> implicit_group;   // HttpAddUrl / HttpRemoveUrl
};

struct Connection {
    u64 id = 0;
    int fd = -1;
    u16 port = 0;
    sockaddr_storage local{};
    sockaddr_storage remote{};
    std::mutex send_mutex;      // one response writer at a time
    std::atomic<bool> aborted{false};
    std::atomic<bool> finished{false};
    bool opaque = false;
    bool routed_once = false;
    Timeouts timeouts;          // of the URL group of the last routed request
    std::string inbuf;
    std::shared_ptr<Request> current;
    std::vector<std::shared_ptr<Waiter>> disconnect_waiters;
};

struct Endpoint {
    u16 port = 0;
    bool wildcard = false;
    std::mutex mu;              // the listening fds: accept() vs release
    std::vector<int> fds;
    int wake[2] = {-1, -1};     // closed with the last reference
    std::atomic<bool> stop{false};
    ~Endpoint();
};

struct Completion {
    Handle handle;
    u32 bytes;
    GuestAddr overlapped;
    bool is_64bit;
    u32 nt_status;
    u32 win_error;
    bool synchronous;
};

} // namespace vitreo::winapi::httpapi

namespace vitreo::winapi {

struct HttpApiManager::State {
    HandleTable* handles = nullptr;
    GuestMemory* memory = nullptr;
    SyncManager* sync = nullptr;
    OverlappedCompletion complete;
    PortAssociationProbe has_port;

    std::mutex mu;
    std::condition_variable cv;     // synchronous receivers and readers
    u32 server_refs = 0;
    u32 config_refs = 0;
    u64 next_id = 0;
    std::map<u64, std::shared_ptr<httpapi::Session>> sessions;
    std::map<u64, std::shared_ptr<httpapi::UrlGroup>> groups;
    std::unordered_map<Handle, std::shared_ptr<httpapi::Queue>> queues;
    std::vector<std::shared_ptr<httpapi::UrlEntry>> urls;     // all registrations
    std::map<std::pair<u16, bool>, std::shared_ptr<httpapi::Endpoint>> endpoints;
    std::vector<std::shared_ptr<httpapi::Connection>> connections;
    std::vector<std::shared_ptr<httpapi::Waiter>> disconnect_waiters;
    bool stopping = false;
    // Completions produced where no caller-owned list is at hand (a request
    // reset from inside a check); publish() delivers them.
    std::vector<httpapi::Completion> deferred;
    // Listener and connection threads are detached; they hold this count
    // while they run so teardown never joins under `mu`.
    u32 threads = 0;
    std::condition_variable threads_cv;

    [[nodiscard]] u64 new_id() { return 0xFE00000000000000ull | ++next_id; }

    // Control plane helpers (httpapi.cpp).
    [[nodiscard]] std::shared_ptr<httpapi::Queue> queue_for(Handle h) const {
        auto it = queues.find(h);
        return it == queues.end() ? nullptr : it->second;
    }
    void deliver(std::vector<httpapi::Completion>& out, const std::shared_ptr<httpapi::Waiter>& w,
                 u32 error, u32 nt_status, u32 bytes, bool synchronous);
    void publish(std::vector<httpapi::Completion>& done);
    void close_group_locked(const std::shared_ptr<httpapi::UrlGroup>& group);
    void release_endpoint_locked(u16 port);
    void teardown_queue_locked(const std::shared_ptr<httpapi::Queue>& q,
                               std::vector<httpapi::Completion>& out);

    // Data plane (httpapi_server.cpp).
    [[nodiscard]] u32 ensure_endpoint_locked(u16 port, bool wildcard);
    void accept_loop(std::shared_ptr<httpapi::Endpoint> endpoint);
    void serve(std::shared_ptr<httpapi::Connection> conn);
    void abort_request_locked(std::shared_ptr<httpapi::Request> req);   // by value: it erases the map entry a caller may name
    /// Hand queued requests to waiting receivers.
    void dispatch_locked(const std::shared_ptr<httpapi::Queue>& q,
                         std::vector<httpapi::Completion>& out);
    /// Try to satisfy one request receive; nullopt while it has to wait.
    std::optional<u32> try_receive_locked(const std::shared_ptr<httpapi::Queue>& q,
                                          httpapi::Waiter& w, u32& bytes, u32& nt_status);
    std::optional<u32> try_entity_locked(const std::shared_ptr<httpapi::Queue>& q,
                                         httpapi::Waiter& w, u32& bytes, u32& nt_status);
    void wake_entity_waiters_locked(const std::shared_ptr<httpapi::Queue>& q,
                                    std::vector<httpapi::Completion>& out);
    void finish_connection(const std::shared_ptr<httpapi::Connection>& conn);
};

} // namespace vitreo::winapi
