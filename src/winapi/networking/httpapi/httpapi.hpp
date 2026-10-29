// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file httpapi.hpp
/// @brief httpapi.dll: the HTTP Server API (http.h) over host TCP sockets.
///
/// Windows serves HTTP Server API applications from http.sys; Vitreo serves
/// them from a listener per registered port inside the guest process. Server
/// sessions, URL groups and request queues are the same objects http.sys has:
/// a request queue is a Device handle that CloseHandle, CancelIoEx and
/// CreateIoCompletionPort accept. Every contract value comes from the SDK
/// (http.h, httpapi.lib), Microsoft Learn, or tools probe
/// tests/winapi/httpapi/oracle/httpapi_probe.c whose recording is
/// tests/oracle_data/httpapi_probe{32,64}.expected.
///
/// Not provided (each fails with ERROR_NOT_SUPPORTED and one
/// "[vitreo] UNIMPLEMENTED httpapi.dll!<api>: <case>" line):
///   - TLS: https:// prefixes, HttpReceiveClientCertificate.
///   - Server-side authentication schemes, logging, QoS limits, channel
///     binding, protection levels (accepted when disabled / not present),
///     and an enforced HTTP_TIMEOUT_LIMIT_INFO.MinSendRate.
///   - Response cache and fragment cache (cache policies other than
///     HttpCachePolicyNocache, HttpAddFragmentToCache, HttpReadFragmentFromCache,
///     HttpFlushResponseCache), HttpDataChunkFromFileHandle and the other
///     non-memory data chunks, PROCESS_RANGES on a Range request, GOAWAY.
///   - The HTTP.sys configuration store (Http*ServiceConfiguration),
///     HttpControlService, delegation (HttpDelegateRequest[Ex], queue
///     creation properties), HttpDeclarePush, HttpEvaluateRequest,
///     HttpFindUrlGroupId, HttpGetCounters, HttpGetExtension, HttpPrepareUrl,
///     Http{Query,Set}RequestProperty, HttpWaitForDemandStart, and request
///     queue properties other than QueueLength / 503Verbosity / State.
/// Known differences that are not call failures: HTTP_REQUEST_V2 carries no
/// HTTP_REQUEST_INFO entries (Windows adds timing/sizing/TCP info), the
/// buffer size a request needs is therefore smaller, error pages the server
/// sends itself have Vitreo's own body text, and named queues are visible
/// only inside the process.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"

#include <functional>
#include <memory>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

class Marshaler;
class SyncManager;

class HttpApiManager {
public:
    /// Publishes a finished overlapped request exactly as a file request
    /// completes (OVERLAPPED status/count, hEvent, completion port packet);
    /// wired to Kernel32ExtManager::complete_overlapped_file_io.
    using OverlappedCompletion = std::function<void(
        Handle handle, u32 bytes, GuestAddr overlapped, bool is_64bit,
        u32 nt_status, u32 win_error, bool completed_synchronously)>;
    /// Whether CreateIoCompletionPort associated the handle with a port.
    using PortAssociationProbe = std::function<bool(Handle)>;

    HttpApiManager();
    ~HttpApiManager();
    HttpApiManager(const HttpApiManager&) = delete;
    HttpApiManager& operator=(const HttpApiManager&) = delete;

    void set_handle_table(HandleTable* handles);
    void set_memory(GuestMemory* memory);
    /// Without a completion callback (unit tests) a completion writes the
    /// OVERLAPPED and signals hEvent through this SyncManager.
    void set_sync_manager(SyncManager* sync);
    void set_overlapped_completion(OverlappedCompletion completion);
    void set_port_association_probe(PortAssociationProbe probe);

    /// CloseHandle of a Device handle: tears down the request queue it names
    /// (pending receives complete with ERROR_OPERATION_ABORTED). Returns
    /// whether the handle was a request queue.
    bool on_handle_close(Handle handle);
    /// CancelIo / CancelIoEx on a request queue handle; returns how many
    /// pending requests were cancelled.
    usize cancel_io(Handle handle, GuestAddr overlapped, bool only_calling_thread,
                    u32 calling_tid);
    /// Stop every listener and connection (process exit, tests).
    void shutdown_all();

    // Exports (one per httpapi.dll function; see httpapi_exports.inc).
#define HTTPAPI_EXPORT(name, ordinal, bytes) void name(Marshaler& m);
#include "httpapi_exports.inc"
#undef HTTPAPI_EXPORT

    struct State;

private:
    std::unique_ptr<State> m_state;
};

/// Create httpapi.dll export thunks (all 49 SDK exports, SDK ordinals).
DllExports create_httpapi_thunks(HttpApiManager& mgr);

} // namespace vitreo::winapi
