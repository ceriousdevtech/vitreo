// SPDX-License-Identifier: LGPL-2.1-only
/// @file httpapi.cpp
/// @brief httpapi.dll control plane: HttpInitialize/HttpTerminate, server
///        sessions, URL groups, request queues, properties, URL registration,
///        and the export table.
///
/// Return values not stated by Microsoft Learn are the measurements in
/// tests/oracle_data/httpapi_probe{32,64}.expected; the comments quote the
/// probe's line keys.

#include "httpapi_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace vitreo::winapi {

using namespace httpapi;

namespace {

/// Reads stdcall/x64 arguments in declaration order; a 64-bit value
/// (HTTP_OPAQUE_ID) takes two 4-byte slots on x86, one register/slot on x64.
class Args {
public:
    explicit Args(Marshaler& m) : m_(m), x64_(m.cpu_is_64bit()) {}
    u32 u32v() { return static_cast<u32>(m_.arg(slot_++)); }
    GuestAddr ptr() {
        const u64 v = m_.arg(slot_++);
        return static_cast<GuestAddr>(x64_ ? v : (v & 0xFFFFFFFFu));
    }
    u64 u64v() {
        if (x64_) return m_.arg(slot_++);
        const u64 lo = m_.arg(slot_++) & 0xFFFFFFFFu;
        const u64 hi = m_.arg(slot_++) & 0xFFFFFFFFu;
        return lo | (hi << 32);
    }
    Handle handle() { return static_cast<Handle>(ptr()); }

private:
    Marshaler& m_;
    bool x64_;
    usize slot_ = 0;
};

} // namespace

void httpapi::unimplemented_once(const char* api, const char* what) {
    static std::mutex mu;
    static std::vector<std::string> seen;
    std::string key = std::string(api) + ":" + what;
    std::lock_guard lock(mu);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED httpapi.dll!%s: %s\n", api, what);
}

namespace {

void put_ptr(GuestMemory& mem, GuestAddr at, u64 v, bool x64) {
    if (x64) (void)mem.write_u64(at, v);
    else (void)mem.write_u32(at, static_cast<u32>(v));
}

std::optional<u64> get_ptr(GuestMemory& mem, GuestAddr at, bool x64) {
    if (x64) {
        auto v = mem.read_u64(at);
        if (v.is_err()) return std::nullopt;
        return v.value();
    }
    auto v = mem.read_u32(at);
    if (v.is_err()) return std::nullopt;
    return v.value();
}

/// A NUL-terminated UTF-16 string from guest memory (nullopt if unreadable).
std::optional<std::u16string> read_wide(GuestMemory& mem, GuestAddr at) {
    std::u16string s;
    for (u32 i = 0; i < 0x8000; ++i) {
        auto c = mem.read_u16(at + 2 * i);
        if (c.is_err()) return std::nullopt;
        if (c.value() == 0) return s;
        s.push_back(static_cast<char16_t>(c.value()));
    }
    return std::nullopt;
}

std::string narrow_lower(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

/// The UrlPrefix grammar of Learn "UrlPrefix Strings":
/// "scheme://host:port/relativeURI" with a mandatory port. A query or
/// fragment is not part of a prefix (probe "addurl query: rc=87"); a missing
/// trailing '/' is accepted and implied ("addurl no-slash: rc=0").
enum class UrlParse { Ok, Invalid, Https };

UrlParse parse_prefix(const std::u16string& url, UrlEntry& e) {
    const std::string s = narrow_lower(url);
    std::string rest;
    if (s.rfind("http://", 0) == 0) {
        rest = s.substr(7);
    } else if (s.rfind("https://", 0) == 0) {
        return UrlParse::Https;
    } else {
        return UrlParse::Invalid;
    }
    if (s.find_first_of("?#") != std::string::npos) return UrlParse::Invalid;
    const usize slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    usize colon;
    if (!hostport.empty() && hostport[0] == '[') {
        const usize close = hostport.find(']');
        if (close == std::string::npos || close + 1 >= hostport.size() || hostport[close + 1] != ':')
            return UrlParse::Invalid;
        colon = close + 1;
    } else {
        colon = hostport.rfind(':');
    }
    if (colon == std::string::npos || colon == 0) return UrlParse::Invalid;
    const std::string host = hostport.substr(0, colon);
    const std::string port_s = hostport.substr(colon + 1);
    if (port_s.empty() || port_s.size() > 5 ||
        !std::all_of(port_s.begin(), port_s.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return UrlParse::Invalid;
    const unsigned long port = std::stoul(port_s);
    if (port == 0 || port > 65535) return UrlParse::Invalid;
    if (host == "+") e.kind = HostKind::Strong;
    else if (host == "*") e.kind = HostKind::Weak;
    else {
        e.kind = HostKind::Named;
        if (host.find_first_of(" /\\%") != std::string::npos) return UrlParse::Invalid;
        e.host = host;
    }
    if (path.back() != '/') path.push_back('/');
    e.port = static_cast<u16>(port);
    e.path = path;
    e.key = host + ":" + port_s + path;
    return UrlParse::Ok;
}

} // namespace

// =============================================================================
// Manager
// =============================================================================

HttpApiManager::HttpApiManager() : m_state(std::make_unique<State>()) {}

HttpApiManager::~HttpApiManager() { shutdown_all(); }

void HttpApiManager::set_handle_table(HandleTable* handles) { m_state->handles = handles; }
void HttpApiManager::set_memory(GuestMemory* memory) { m_state->memory = memory; }
void HttpApiManager::set_sync_manager(SyncManager* sync) { m_state->sync = sync; }
void HttpApiManager::set_overlapped_completion(OverlappedCompletion completion) {
    m_state->complete = std::move(completion);
}
void HttpApiManager::set_port_association_probe(PortAssociationProbe probe) {
    m_state->has_port = std::move(probe);
}

void HttpApiManager::State::deliver(std::vector<Completion>& out, const std::shared_ptr<Waiter>& w,
                                    u32 error, u32 nt_status, u32 bytes, bool synchronous) {
    if (w->overlapped == 0) {
        w->done = true;
        w->error = error;
        w->bytes = bytes;
        cv.notify_all();
        return;
    }
    out.push_back({w->handle, bytes, w->overlapped, w->is_64bit, nt_status, error, synchronous});
}

void HttpApiManager::State::publish(std::vector<Completion>& done) {
    {
        std::lock_guard lock(mu);
        done.insert(done.end(), deferred.begin(), deferred.end());
        deferred.clear();
    }
    for (const Completion& c : done) {
        if (complete) {
            complete(c.handle, c.bytes, c.overlapped, c.is_64bit, c.nt_status, c.win_error,
                     c.synchronous);
            continue;
        }
        // No kernel32 wiring (unit tests): OVERLAPPED.Internal/InternalHigh,
        // then hEvent.
        if (!memory) continue;
        put_ptr(*memory, c.overlapped, c.nt_status, c.is_64bit);
        put_ptr(*memory, c.overlapped + (c.is_64bit ? 8 : 4), c.bytes, c.is_64bit);
        const auto ev = get_ptr(*memory, c.overlapped + (c.is_64bit ? 24 : 16), c.is_64bit);
        if (ev && *ev && sync) (void)sync->set_event(static_cast<Handle>(*ev & ~u64{1}));
    }
    done.clear();
}

void HttpApiManager::State::close_group_locked(const std::shared_ptr<UrlGroup>& group) {
    std::vector<u16> ports;
    for (const auto& u : group->urls) {
        ports.push_back(u->port);
        std::erase(urls, u);
    }
    group->urls.clear();
    groups.erase(group->id);
    for (u16 p : ports) release_endpoint_locked(p);
}

void HttpApiManager::State::teardown_queue_locked(const std::shared_ptr<Queue>& q,
                                                  std::vector<Completion>& out) {
    q->shut_down = true;
    q->closed = true;
    for (auto& w : q->waiters) {
        deliver(out, w, kErrorOperationAborted, kStatusCancelled, 0, false);
    }
    q->waiters.clear();
    std::vector<std::shared_ptr<Request>> live;
    for (auto& [id, req] : q->requests) live.push_back(req);
    for (auto& req : live) abort_request_locked(req);
    q->requests.clear();
    q->queued.clear();
    if (q->implicit_group) close_group_locked(q->implicit_group);
    cv.notify_all();
}

bool HttpApiManager::on_handle_close(Handle handle) {
    State& s = *m_state;
    std::vector<Completion> done;
    {
        std::lock_guard lock(s.mu);
        auto it = s.queues.find(handle);
        if (it == s.queues.end()) return false;
        auto q = it->second;
        s.queues.erase(it);
        std::erase(q->handles, handle);
        if (q->handles.empty()) s.teardown_queue_locked(q, done);
    }
    s.publish(done);
    return true;
}

usize HttpApiManager::cancel_io(Handle handle, GuestAddr overlapped, bool only_calling_thread,
                                u32 calling_tid) {
    State& s = *m_state;
    std::vector<Completion> done;
    usize n = 0;
    {
        std::lock_guard lock(s.mu);
        auto q = s.queue_for(handle);
        if (!q) return 0;
        auto cancel = [&](std::deque<std::shared_ptr<Waiter>>& list, bool by_handle) {
            for (auto it = list.begin(); it != list.end();) {
                const auto& w = *it;
                const bool match = w->overlapped != 0 && (!by_handle || w->handle == handle) &&
                                   (overlapped == 0 || w->overlapped == overlapped) &&
                                   (!only_calling_thread || w->thread_id == calling_tid);
                if (!match) { ++it; continue; }
                s.deliver(done, w, kErrorOperationAborted, kStatusCancelled, 0, false);
                it = list.erase(it);
                ++n;
            }
        };
        cancel(q->waiters, false);
        std::deque<std::shared_ptr<Waiter>> disc(s.disconnect_waiters.begin(), s.disconnect_waiters.end());
        cancel(disc, true);
        s.disconnect_waiters.assign(disc.begin(), disc.end());
    }
    s.publish(done);
    return n;
}

// =============================================================================
// HttpInitialize / HttpTerminate
// =============================================================================

void HttpApiManager::HttpInitialize(Marshaler& m) {
    // ULONG HttpInitialize(HTTPAPI_VERSION Version, ULONG Flags, PVOID pReserved)
    // HTTPAPI_VERSION is a 4-byte struct passed by value: major in the low word.
    Args a(m);
    const u32 version = a.u32v();
    const u32 flags = a.u32v();
    const GuestAddr reserved = a.ptr();
    const u16 major = static_cast<u16>(version), minor = static_cast<u16>(version >> 16);
    // probe "init v3/v2.1/v1.1/v0: rc=1154" -- only 1.0 and 2.0 exist.
    if (!((major == 1 || major == 2) && minor == 0)) { m.set_return(kErrorInvalidDll); return; }
    // "init flags8: rc=87", "init reserved: rc=87"
    if ((flags & ~(HTTP_INITIALIZE_SERVER | HTTP_INITIALIZE_CONFIG | HTTP_DEMAND_CBT)) != 0 ||
        reserved != 0) {
        m.set_return(kErrorInvalidParameter);
        return;
    }
    std::lock_guard lock(m_state->mu);
    // Each flag is counted: two initializations need two terminations
    // ("fresh init2-term1 session: rc=0", "fresh init2-term2 session: rc=6").
    if (flags & HTTP_INITIALIZE_SERVER) ++m_state->server_refs;
    if (flags & HTTP_INITIALIZE_CONFIG) ++m_state->config_refs;
    m.set_return(NO_ERROR_);
}

void HttpApiManager::HttpTerminate(Marshaler& m) {
    // ULONG HttpTerminate(ULONG Flags, PVOID pReserved)
    Args a(m);
    const u32 flags = a.u32v();
    const GuestAddr reserved = a.ptr();
    if ((flags & ~(HTTP_INITIALIZE_SERVER | HTTP_INITIALIZE_CONFIG | HTTP_DEMAND_CBT)) != 0 ||
        reserved != 0) {
        m.set_return(kErrorInvalidParameter);   // "terminate flags8/reserved: rc=87"
        return;
    }
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if ((flags & HTTP_INITIALIZE_SERVER) && s.server_refs > 0 && --s.server_refs == 0) {
        // The last server reference takes the server sessions and their URL
        // groups with it ("fresh session-after-terminate: create=0 close=6").
        std::vector<std::shared_ptr<UrlGroup>> groups;
        for (auto& [id, g] : s.groups)
            if (g->session != 0) groups.push_back(g);
        for (auto& g : groups) s.close_group_locked(g);
        s.sessions.clear();
    }
    if ((flags & HTTP_INITIALIZE_CONFIG) && s.config_refs > 0) --s.config_refs;
    m.set_return(NO_ERROR_);   // "pre-init terminate: rc=0"
}

// =============================================================================
// Server sessions
// =============================================================================

void HttpApiManager::HttpCreateServerSession(Marshaler& m) {
    // ULONG HttpCreateServerSession(HTTPAPI_VERSION Version,
    //   PHTTP_SERVER_SESSION_ID ServerSessionId, ULONG Reserved)
    Args a(m);
    const u32 version = a.u32v();
    const GuestAddr out = a.ptr();
    const u32 reserved = a.u32v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }   // "pre-init session: rc=6"
    // "session null-id: rc=87", "session reserved: rc=87 sid=untouched"
    if (out == 0 || reserved != 0) { m.set_return(kErrorInvalidParameter); return; }
    if (m.memory().write_u64(out, 0).is_err()) { m.set_return(kErrorNoAccess); return; }
    const u16 major = static_cast<u16>(version), minor = static_cast<u16>(version >> 16);
    // "session v3: rc=1306 sid=zero"; version 1.0 is accepted ("session v1: rc=0").
    if (!((major == 1 || major == 2) && minor == 0)) { m.set_return(kErrorRevisionMismatch); return; }
    auto session = std::make_shared<Session>();
    session->id = s.new_id();
    session->version_major = major;
    s.sessions[session->id] = session;
    (void)m.memory().write_u64(out, session->id);
    m.set_return(NO_ERROR_);
}

void HttpApiManager::HttpCloseServerSession(Marshaler& m) {
    // ULONG HttpCloseServerSession(HTTP_SERVER_SESSION_ID ServerSessionId)
    Args a(m);
    const u64 id = a.u64v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.sessions.find(id);
    // "close-session 0/bogus/as-group/again: rc=87"
    if (it == s.sessions.end()) { m.set_return(kErrorInvalidParameter); return; }
    // Its URL groups close with it ("close-session with-group: rc=0 then-group-close=87").
    std::vector<std::shared_ptr<UrlGroup>> owned;
    for (auto& [gid, g] : s.groups)
        if (g->session == id) owned.push_back(g);
    for (auto& g : owned) s.close_group_locked(g);
    s.sessions.erase(it);
    m.set_return(NO_ERROR_);
}

// =============================================================================
// URL groups
// =============================================================================

void HttpApiManager::HttpCreateUrlGroup(Marshaler& m) {
    // ULONG HttpCreateUrlGroup(HTTP_SERVER_SESSION_ID ServerSessionId,
    //   PHTTP_URL_GROUP_ID pUrlGroupId, ULONG Reserved)
    Args a(m);
    const u64 session = a.u64v();
    const GuestAddr out = a.ptr();
    const u32 reserved = a.u32v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    // "group bogus-session/zero-session/null-out/reserved: rc=87", gid untouched
    if (out == 0 || reserved != 0 || !s.sessions.count(session)) {
        m.set_return(kErrorInvalidParameter);
        return;
    }
    auto group = std::make_shared<UrlGroup>();
    group->id = s.new_id();
    group->session = session;
    if (m.memory().write_u64(out, group->id).is_err()) { m.set_return(kErrorNoAccess); return; }
    s.groups[group->id] = group;
    m.set_return(NO_ERROR_);
}

void HttpApiManager::HttpCloseUrlGroup(Marshaler& m) {
    // ULONG HttpCloseUrlGroup(HTTP_URL_GROUP_ID UrlGroupId)
    Args a(m);
    const u64 id = a.u64v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.groups.find(id);
    if (it == s.groups.end() || it->second->session == 0) {
        m.set_return(kErrorInvalidParameter);   // "close-group bogus/0/as-session/again: rc=87"
        return;
    }
    // Closing frees its URLs for other groups ("other-group after close: rc=0").
    s.close_group_locked(it->second);
    m.set_return(NO_ERROR_);
}

// =============================================================================
// Properties
// =============================================================================

namespace {

/// The property-structure checks shared by server sessions and URL groups.
/// Writes into `scope` on success.
u32 set_scope_property(GuestMemory& mem, bool x64, PropertyScope& scope, u32 property,
                       GuestAddr info, u32 length, const char* api) {
    const Layout& L = layout_for(x64);
    auto need = [&](u32 size) -> u32 {
        if (info == 0) return kErrorInvalidParameter;          // "bind null: rc=87"
        if (length < size) return kErrorInsufficientBuffer;    // "bind short: rc=122"
        return NO_ERROR_;
    };
    auto present = [&]() { return (mem.read_u32(info).value_or(0) & 1) != 0; };
    switch (property) {
    case HttpServerStateProperty: {
        if (u32 e = need(L.state_size)) return e;
        const u32 state = mem.read_u32(info + 4).value_or(~0u);
        if (!present()) return NO_ERROR_;                       // "state not-present: rc=0"
        if (state > HttpEnabledStateInactive) return kErrorInvalidParameter;   // "state 2: rc=87"
        scope.state = state;
        return NO_ERROR_;
    }
    case HttpServerTimeoutsProperty: {
        if (u32 e = need(L.timeout_size)) return e;
        if (!present()) return NO_ERROR_;
        Timeouts t;
        t.entity_body = mem.read_u16(info + 4).value_or(0);
        t.drain_entity_body = mem.read_u16(info + 6).value_or(0);
        t.request_queue = mem.read_u16(info + 8).value_or(0);
        t.idle_connection = mem.read_u16(info + 10).value_or(0);
        t.header_wait = mem.read_u16(info + 12).value_or(0);
        t.min_send_rate = mem.read_u32(info + 16).value_or(0);
        // MinSendRate (bytes/s a response must sustain) is not enforced.
        if (t.min_send_rate != 0 && t.min_send_rate != 0xFFFFFFFFu) {
            unimplemented_once(api, "HttpServerTimeoutsProperty MinSendRate enforcement");
            return kErrorNotSupported;
        }
        scope.timeouts = t;
        return NO_ERROR_;
    }
    case HttpServerAuthenticationProperty:
    case HttpServerExtendedAuthenticationProperty: {
        if (u32 e = need(L.auth_size)) return e;
        // Disabled or no schemes: no authentication, which is what Vitreo does
        // ("auth not-present: rc=0", "auth present-none: rc=0").
        if (!present() || mem.read_u32(info + 4).value_or(0) == 0) return NO_ERROR_;
        unimplemented_once(api, "server-side authentication schemes");
        return kErrorNotSupported;
    }
    case HttpServerLoggingProperty:
        if (u32 e = need(L.logging_size)) return e;
        if (!present()) return NO_ERROR_;                       // "logging not-present: rc=0"
        unimplemented_once(api, "HttpServerLoggingProperty (W3C/IIS logging)");
        return kErrorNotSupported;
    case HttpServerProtectionLevelProperty:
        if (u32 e = need(L.protection_size)) return e;
        if (!present()) return NO_ERROR_;
        unimplemented_once(api, "HttpServerProtectionLevelProperty");
        return kErrorNotSupported;
    case HttpServerQosProperty: {
        if (u32 e = need(L.qos_size)) return e;
        // HTTP_QOS_SETTING_INFO.QosSetting is dereferenced ("qos zero: rc=998").
        const auto setting = get_ptr(mem, info + L.ptr, x64);
        if (!setting || *setting == 0 || mem.read_u32(static_cast<GuestAddr>(*setting)).is_err())
            return kErrorNoAccess;
        unimplemented_once(api, "HttpServerQosProperty (bandwidth/connection limits)");
        return kErrorNotSupported;
    }
    case HttpServerChannelBindProperty:
        if (u32 e = need(L.channel_bind_size)) return e;
        unimplemented_once(api, "HttpServerChannelBindProperty");
        return kErrorNotSupported;
    default:
        // QueueLength/503Verbosity belong to request queues ("queuelength
        // on-group: rc=87", "property 6/99: rc=87").
        return kErrorInvalidParameter;
    }
}

u32 query_scope_property(GuestMemory& mem, bool x64, const PropertyScope& scope, u32 property,
                         GuestAddr info, u32 length, GuestAddr ret_len, const char* api) {
    const Layout& L = layout_for(x64);
    std::vector<u8> image;
    if (property == HttpServerStateProperty) {
        // "query state: rc=0 ret=8 present=0 state=0"
        image.assign(L.state_size, 0);
        const u32 state = scope.state.value_or(HttpEnabledStateActive);
        std::memcpy(image.data() + 4, &state, 4);
    } else if (property == HttpServerTimeoutsProperty) {
        // "query timeouts: rc=0 ret=20 idle=30 header=20"
        image.assign(L.timeout_size, 0);
        const Timeouts t = scope.timeouts.value_or(Timeouts{});
        std::memcpy(image.data() + 4, &t.entity_body, 2);
        std::memcpy(image.data() + 6, &t.drain_entity_body, 2);
        std::memcpy(image.data() + 8, &t.request_queue, 2);
        std::memcpy(image.data() + 10, &t.idle_connection, 2);
        std::memcpy(image.data() + 12, &t.header_wait, 2);
        std::memcpy(image.data() + 16, &t.min_send_rate, 4);
    } else if (property == HttpServerBindingProperty) {
        return kErrorInvalidParameter;   // "query bind: rc=87"
    } else if (property == HttpServerAuthenticationProperty ||
               property == HttpServerLoggingProperty || property == HttpServerQosProperty ||
               property == HttpServerChannelBindProperty ||
               property == HttpServerProtectionLevelProperty ||
               property == HttpServerExtendedAuthenticationProperty) {
        unimplemented_once(api, "query of an authentication/logging/QoS/channel-bind property");
        return kErrorNotSupported;
    } else {
        return kErrorInvalidParameter;
    }
    if (ret_len) (void)mem.write_u32(ret_len, static_cast<u32>(image.size()));
    if (info == 0 || length < image.size()) return kErrorMoreData;
    if (mem.write_bytes(info, image.data(), image.size()).is_err()) return kErrorNoAccess;
    return NO_ERROR_;
}

} // namespace

void HttpApiManager::HttpSetUrlGroupProperty(Marshaler& m) {
    // ULONG HttpSetUrlGroupProperty(HTTP_URL_GROUP_ID UrlGroupId,
    //   HTTP_SERVER_PROPERTY Property, PVOID PropertyInformation,
    //   ULONG PropertyInformationLength)
    Args a(m);
    const u64 id = a.u64v();
    const u32 property = a.u32v();
    const GuestAddr info = a.ptr();
    const u32 length = a.u32v();
    const bool x64 = m.cpu_is_64bit();
    const Layout& L = layout_for(x64);
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.groups.find(id);
    if (it == s.groups.end() || it->second->session == 0) {
        m.set_return(kErrorInvalidParameter);   // "bind bogus-group/on-session: rc=87"
        return;
    }
    UrlGroup& group = *it->second;
    if (property == HttpServerBindingProperty) {
        if (info == 0) { m.set_return(kErrorInvalidParameter); return; }
        if (length < L.binding_size) { m.set_return(kErrorInsufficientBuffer); return; }
        const bool present = (m.memory().read_u32(info).value_or(0) & 1) != 0;
        if (!present) {                       // "unbind: rc=0"
            group.bound = false;
            group.queue.reset();
            m.set_return(NO_ERROR_);
            return;
        }
        const Handle h = static_cast<Handle>(get_ptr(m.memory(), info + L.binding_queue, x64).value_or(0));
        auto q = s.queue_for(h);
        // "bind bogus-handle/null-handle/event-handle: rc=6"
        if (!q || q->closed) { m.set_return(kErrorInvalidHandle); return; }
        group.bound = true;
        group.queue = q;
        m.set_return(NO_ERROR_);
        return;
    }
    m.set_return(set_scope_property(m.memory(), x64, group.props, property, info, length,
                                    "HttpSetUrlGroupProperty"));
}

void HttpApiManager::HttpQueryUrlGroupProperty(Marshaler& m) {
    // ULONG HttpQueryUrlGroupProperty(HTTP_URL_GROUP_ID, HTTP_SERVER_PROPERTY,
    //   PVOID PropertyInformation, ULONG PropertyInformationLength, PULONG ReturnLength)
    Args a(m);
    const u64 id = a.u64v();
    const u32 property = a.u32v();
    const GuestAddr info = a.ptr();
    const u32 length = a.u32v();
    const GuestAddr ret_len = a.ptr();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.groups.find(id);
    if (it == s.groups.end() || it->second->session == 0) { m.set_return(kErrorInvalidParameter); return; }
    m.set_return(query_scope_property(m.memory(), m.cpu_is_64bit(), it->second->props, property,
                                      info, length, ret_len, "HttpQueryUrlGroupProperty"));
}

void HttpApiManager::HttpSetServerSessionProperty(Marshaler& m) {
    // ULONG HttpSetServerSessionProperty(HTTP_SERVER_SESSION_ID, HTTP_SERVER_PROPERTY,
    //   PVOID PropertyInformation, ULONG PropertyInformationLength)
    Args a(m);
    const u64 id = a.u64v();
    const u32 property = a.u32v();
    const GuestAddr info = a.ptr();
    const u32 length = a.u32v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.sessions.find(id);
    if (it == s.sessions.end()) { m.set_return(kErrorInvalidParameter); return; }
    // A binding is a URL-group property ("session bind: rc=87").
    if (property == HttpServerBindingProperty) { m.set_return(kErrorInvalidParameter); return; }
    m.set_return(set_scope_property(m.memory(), m.cpu_is_64bit(), it->second->props, property,
                                    info, length, "HttpSetServerSessionProperty"));
}

void HttpApiManager::HttpQueryServerSessionProperty(Marshaler& m) {
    // ULONG HttpQueryServerSessionProperty(HTTP_SERVER_SESSION_ID, HTTP_SERVER_PROPERTY,
    //   PVOID PropertyInformation, ULONG PropertyInformationLength, PULONG ReturnLength)
    Args a(m);
    const u64 id = a.u64v();
    const u32 property = a.u32v();
    const GuestAddr info = a.ptr();
    const u32 length = a.u32v();
    const GuestAddr ret_len = a.ptr();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.sessions.find(id);
    if (it == s.sessions.end()) { m.set_return(kErrorInvalidParameter); return; }
    m.set_return(query_scope_property(m.memory(), m.cpu_is_64bit(), it->second->props, property,
                                      info, length, ret_len, "HttpQueryServerSessionProperty"));
}

// =============================================================================
// Request queues
// =============================================================================

namespace {

u32 create_queue(HttpApiManager::State& s, Marshaler& m, u32 version, GuestAddr name_ptr,
                 GuestAddr security, u32 flags, GuestAddr out) {
    const bool x64 = m.cpu_is_64bit();
    std::lock_guard lock(s.mu);
    // "pre-init queue: rc=1114 handle=null"
    if (s.server_refs == 0) {
        if (out) put_ptr(m.memory(), out, 0, x64);
        return kErrorDllInitFailed;
    }
    if (out == 0) return kErrorInvalidParameter;                     // "queue null-out: rc=87"
    put_ptr(m.memory(), out, 0, x64);                                // failures leave NULL
    constexpr u32 kKnown = HTTP_CREATE_REQUEST_QUEUE_FLAG_OPEN_EXISTING |
                           HTTP_CREATE_REQUEST_QUEUE_FLAG_CONTROLLER |
                           HTTP_CREATE_REQUEST_QUEUE_FLAG_DELEGATION;
    if (flags & ~kKnown) return kErrorInvalidParameter;              // "queue flags0x10: rc=87"
    const u16 major = static_cast<u16>(version), minor = static_cast<u16>(version >> 16);
    if (!((major == 1 || major == 2) && minor == 0)) return kErrorRevisionMismatch;
    std::u16string name;
    if (name_ptr) {
        auto n = read_wide(m.memory(), name_ptr);
        if (!n) return kErrorNoAccess;
        name = *n;
    }
    const bool open_existing = flags & HTTP_CREATE_REQUEST_QUEUE_FLAG_OPEN_EXISTING;
    // OPEN_EXISTING and CONTROLLER name a queue ("queue open-existing unnamed:
    // rc=87", "queue controller unnamed: rc=87").
    if ((open_existing || (flags & HTTP_CREATE_REQUEST_QUEUE_FLAG_CONTROLLER)) && name.empty())
        return kErrorInvalidParameter;
    std::shared_ptr<Queue> q;
    if (!name.empty()) {
        for (auto& [h, existing] : s.queues) {
            if (existing->name == name && !existing->closed) { q = existing; break; }
        }
        if (q && !open_existing) return kErrorAlreadyExists;         // "queue named dup: rc=183"
        if (!q && open_existing) return kErrorFileNotFound;          // "open-existing missing: rc=2"
    }
    u32 handle_flags = 0;
    if (security) {
        // SECURITY_ATTRIBUTES { nLength; lpSecurityDescriptor; bInheritHandle }
        if (get_ptr(m.memory(), security + (x64 ? 16 : 8), false).value_or(0) & 1)
            handle_flags |= HandleFlags::Inherit;
    }
    if (!s.handles) return kErrorNotSupported;
    const Handle h = s.handles->allocate(HandleType::Device,
                                         new std::string("\\Device\\Http\\ReqQueue"), handle_flags);
    if (h == NULL_HANDLE) return 8;   // ERROR_NOT_ENOUGH_MEMORY
    if (!q) {
        q = std::make_shared<Queue>();
        q->version_major = major;
        q->name = name;
        q->controller = flags & HTTP_CREATE_REQUEST_QUEUE_FLAG_CONTROLLER;
    }
    q->handles.push_back(h);
    s.queues[h] = q;
    put_ptr(m.memory(), out, h, x64);
    return NO_ERROR_;
}

} // namespace

void HttpApiManager::HttpCreateRequestQueue(Marshaler& m) {
    // ULONG HttpCreateRequestQueue(HTTPAPI_VERSION Version, PCWSTR Name,
    //   PSECURITY_ATTRIBUTES SecurityAttributes, ULONG Flags, PHANDLE RequestQueueHandle)
    Args a(m);
    const u32 version = a.u32v();
    const GuestAddr name = a.ptr();
    const GuestAddr security = a.ptr();
    const u32 flags = a.u32v();
    const GuestAddr out = a.ptr();
    m.set_return(create_queue(*m_state, m, version, name, security, flags, out));
}

void HttpApiManager::HttpCreateRequestQueueEx(Marshaler& m) {
    // ULONG HttpCreateRequestQueueEx(HTTPAPI_VERSION, PCWSTR Name, PSECURITY_ATTRIBUTES,
    //   ULONG Flags, PHTTP_CREATE_REQUEST_QUEUE_PROPERTY_INFO, ULONG PropertyCount, PHANDLE)
    Args a(m);
    const u32 version = a.u32v();
    const GuestAddr name = a.ptr();
    const GuestAddr security = a.ptr();
    const u32 flags = a.u32v();
    const GuestAddr properties = a.ptr();
    const u32 count = a.u32v();
    const GuestAddr out = a.ptr();
    if (properties != 0 || count != 0) {
        // CreateRequestQueueExternalIdProperty ties the queue to an external id.
        unimplemented_once("HttpCreateRequestQueueEx", "creation properties");
        m.set_return(kErrorNotSupported);
        return;
    }
    m.set_return(create_queue(*m_state, m, version, name, security, flags, out));
}

void HttpApiManager::HttpCreateHttpHandle(Marshaler& m) {
    // ULONG HttpCreateHttpHandle(PHANDLE RequestQueueHandle, ULONG Reserved)
    // The HTTP Server API 1.0 request queue.
    Args a(m);
    const GuestAddr out = a.ptr();
    const u32 reserved = a.u32v();
    if (reserved != 0) { m.set_return(kErrorInvalidParameter); return; }
    m.set_return(create_queue(*m_state, m, 1, 0, 0, 0, out));
}

void HttpApiManager::HttpCloseRequestQueue(Marshaler& m) {
    // ULONG HttpCloseRequestQueue(HANDLE RequestQueueHandle)
    // It closes the handle like CloseHandle does, whatever it names ("close-queue
    // event: rc=0", "close-queue invalid: rc=0" -- the pseudo handle).
    Args a(m);
    const Handle h = a.handle();
    State& s = *m_state;
    if (h == INVALID_HANDLE) { m.set_return(NO_ERROR_); return; }
    if (!s.handles || h == NULL_HANDLE || !s.handles->is_valid(h)) {
        m.set_return(kErrorInvalidHandle);      // "close-queue null / again: rc=6"
        return;
    }
    (void)s.handles->close(h);   // the close callback runs on_handle_close
    m.set_return(NO_ERROR_);
}

void HttpApiManager::HttpShutdownRequestQueue(Marshaler& m) {
    // ULONG HttpShutdownRequestQueue(HANDLE RequestQueueHandle)
    Args a(m);
    const Handle h = a.handle();
    State& s = *m_state;
    std::vector<Completion> done;
    {
        std::lock_guard lock(s.mu);
        auto q = s.queue_for(h);
        if (!q) { m.set_return(kErrorInvalidHandle); return; }   // "shutdown-queue event/null: rc=6"
        // Pending receives end with STATUS_CANCELLED ("shutdown: ... le=995
        // internal=0xc0000120"); later ones fail with ERROR_INVALID_HANDLE.
        q->shut_down = true;
        for (auto& w : q->waiters) s.deliver(done, w, kErrorOperationAborted, kStatusCancelled, 0, false);
        q->waiters.clear();
        s.cv.notify_all();
    }
    s.publish(done);
    m.set_return(NO_ERROR_);   // "shutdown again: rc=0"
}

namespace {

u32 queue_property(HttpApiManager::State& s, Marshaler& m, bool set) {
    // Set: (HANDLE, HTTP_SERVER_PROPERTY, PVOID, ULONG, ULONG Reserved1, PVOID Reserved2)
    // Query: (HANDLE, HTTP_SERVER_PROPERTY, PVOID, ULONG, ULONG Reserved1,
    //         PULONG ReturnLength, PVOID Reserved2)
    Args a(m);
    const Handle h = a.handle();
    const u32 property = a.u32v();
    const GuestAddr info = a.ptr();
    const u32 length = a.u32v();
    const u32 reserved1 = a.u32v();
    const GuestAddr ret_len = set ? 0 : a.ptr();
    const GuestAddr reserved2 = a.ptr();
    std::lock_guard lock(s.mu);
    auto q = s.queue_for(h);
    if (!q) return kErrorInvalidHandle;
    if (reserved1 != 0 || reserved2 != 0) return kErrorInvalidParameter;
    GuestMemory& mem = m.memory();
    u32* field = nullptr;
    u32 limit_lo = 0, limit_hi = 0;
    switch (property) {
    case HttpServerQueueLengthProperty:
        field = &q->queue_length; limit_lo = HTTP_MIN_SERVER_QUEUE_LENGTH; limit_hi = HTTP_MAX_SERVER_QUEUE_LENGTH;
        break;
    case HttpServer503VerbosityProperty:
        field = &q->verbosity; limit_lo = 0; limit_hi = Http503ResponseVerbosityFull;
        break;
    case HttpServerStateProperty:
        field = &q->state; limit_lo = 0; limit_hi = HttpEnabledStateInactive;
        break;
    default:
        if (property == HttpServerBindingProperty || property == HttpServerTimeoutsProperty)
            return kErrorInvalidParameter;
        unimplemented_once(set ? "HttpSetRequestQueueProperty" : "HttpQueryRequestQueueProperty",
                           "property other than QueueLength/503Verbosity/State");
        return kErrorNotSupported;
    }
    // On a request queue every one of these is a bare ULONG/enum value
    // (Learn HttpSetRequestQueueProperty), not a flagged structure.
    if (set) {
        if (info == 0) return kErrorInvalidParameter;
        if (length < 4) return kErrorInsufficientBuffer;
        auto v = mem.read_u32(info);
        if (v.is_err()) return kErrorNoAccess;
        if (v.value() < limit_lo || v.value() > limit_hi) return kErrorInvalidParameter;
        *field = v.value();
        return NO_ERROR_;
    }
    if (ret_len) (void)mem.write_u32(ret_len, 4);
    if (info == 0 || length < 4) return kErrorMoreData;
    if (mem.write_u32(info, *field).is_err()) return kErrorNoAccess;
    return NO_ERROR_;
}

} // namespace

void HttpApiManager::HttpSetRequestQueueProperty(Marshaler& m) {
    m.set_return(queue_property(*m_state, m, true));
}

void HttpApiManager::HttpQueryRequestQueueProperty(Marshaler& m) {
    m.set_return(queue_property(*m_state, m, false));
}

// =============================================================================
// URL registration
// =============================================================================

namespace {

u32 add_url_locked(HttpApiManager::State& s, GuestMemory& mem, const std::shared_ptr<UrlGroup>& group,
                   GuestAddr url_ptr, u64 context, const char* api) {
    if (url_ptr == 0) return kErrorInvalidParameter;   // "addurl null: rc=87"
    auto url = read_wide(mem, url_ptr);
    if (!url) return kErrorNoAccess;
    auto e = std::make_shared<UrlEntry>();
    switch (parse_prefix(*url, *e)) {
    case UrlParse::Invalid: return kErrorInvalidParameter;
    case UrlParse::Https:
        unimplemented_once(api, "https:// URL prefixes (no TLS server)");
        return kErrorNotSupported;
    case UrlParse::Ok: break;
    }
    // A prefix is registered once in the whole server, case-insensitively
    // ("addurl dup / dup-case / other-group same: rc=183").
    for (const auto& u : s.urls)
        if (u->key == e->key) return kErrorAlreadyExists;
    const bool wildcard = e->kind != HostKind::Named ||
                          (e->host != "localhost" && e->host != "127.0.0.1" && e->host != "[::1]");
    if (u32 err = s.ensure_endpoint_locked(e->port, wildcard)) return err;
    e->text = *url;
    e->context = context;
    e->group = group->id;
    group->urls.push_back(e);
    s.urls.push_back(e);
    return NO_ERROR_;
}

u32 remove_url_locked(HttpApiManager::State& s, GuestMemory& mem, const std::shared_ptr<UrlGroup>& group,
                      GuestAddr url_ptr, u32 flags) {
    if (flags & ~HTTP_URL_FLAG_REMOVE_ALL) return kErrorInvalidParameter;   // "removeurl flags2: rc=87"
    std::vector<std::shared_ptr<UrlEntry>> victims;
    if (flags & HTTP_URL_FLAG_REMOVE_ALL) {
        if (url_ptr != 0) return kErrorInvalidParameter;   // "removeurl all-with-url: rc=87"
        victims = group->urls;                             // "removeurl all ... again=0"
    } else {
        if (url_ptr == 0) return kErrorInvalidParameter;   // "removeurl null-noflag: rc=87"
        auto url = read_wide(mem, url_ptr);
        if (!url) return kErrorNoAccess;
        UrlEntry e;
        if (parse_prefix(*url, e) != UrlParse::Ok) return kErrorInvalidParameter;
        for (const auto& u : group->urls)
            if (u->key == e.key) victims.push_back(u);
        if (victims.empty()) return kErrorFileNotFound;    // "removeurl missing / again: rc=2"
    }
    for (const auto& v : victims) {
        std::erase(group->urls, v);
        std::erase(s.urls, v);
        s.release_endpoint_locked(v->port);
    }
    return NO_ERROR_;
}

} // namespace

void HttpApiManager::HttpAddUrlToUrlGroup(Marshaler& m) {
    // ULONG HttpAddUrlToUrlGroup(HTTP_URL_GROUP_ID UrlGroupId, PCWSTR pFullyQualifiedUrl,
    //   HTTP_URL_CONTEXT UrlContext, ULONG Reserved)
    Args a(m);
    const u64 id = a.u64v();
    const GuestAddr url = a.ptr();
    const u64 context = a.u64v();
    const u32 reserved = a.u32v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.groups.find(id);
    // "addurl reserved / bogus-group: rc=87"
    if (reserved != 0 || it == s.groups.end() || it->second->session == 0) {
        m.set_return(kErrorInvalidParameter);
        return;
    }
    m.set_return(add_url_locked(s, m.memory(), it->second, url, context, "HttpAddUrlToUrlGroup"));
}

void HttpApiManager::HttpRemoveUrlFromUrlGroup(Marshaler& m) {
    // ULONG HttpRemoveUrlFromUrlGroup(HTTP_URL_GROUP_ID UrlGroupId,
    //   PCWSTR pFullyQualifiedUrl, ULONG Flags)
    Args a(m);
    const u64 id = a.u64v();
    const GuestAddr url = a.ptr();
    const u32 flags = a.u32v();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    if (s.server_refs == 0) { m.set_return(kErrorInvalidHandle); return; }
    auto it = s.groups.find(id);
    if (it == s.groups.end() || it->second->session == 0) {
        m.set_return(kErrorInvalidParameter);   // "removeurl bogus-group: rc=87"
        return;
    }
    m.set_return(remove_url_locked(s, m.memory(), it->second, url, flags));
}

void HttpApiManager::HttpAddUrl(Marshaler& m) {
    // ULONG HttpAddUrl(HANDLE RequestQueueHandle, PCWSTR FullyQualifiedUrl, PVOID Reserved)
    // HTTP Server API 1.0: the URL routes straight to the queue (context 0).
    Args a(m);
    const Handle h = a.handle();
    const GuestAddr url = a.ptr();
    const GuestAddr reserved = a.ptr();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    auto q = s.queue_for(h);
    if (!q) { m.set_return(kErrorInvalidHandle); return; }
    if (reserved != 0) { m.set_return(kErrorInvalidParameter); return; }
    if (!q->implicit_group) {
        q->implicit_group = std::make_shared<UrlGroup>();
        q->implicit_group->id = s.new_id();
        q->implicit_group->bound = true;
        q->implicit_group->queue = q;
        s.groups[q->implicit_group->id] = q->implicit_group;
    }
    m.set_return(add_url_locked(s, m.memory(), q->implicit_group, url, 0, "HttpAddUrl"));
}

void HttpApiManager::HttpRemoveUrl(Marshaler& m) {
    // ULONG HttpRemoveUrl(HANDLE RequestQueueHandle, PCWSTR FullyQualifiedUrl)
    Args a(m);
    const Handle h = a.handle();
    const GuestAddr url = a.ptr();
    State& s = *m_state;
    std::lock_guard lock(s.mu);
    auto q = s.queue_for(h);
    if (!q) { m.set_return(kErrorInvalidHandle); return; }
    if (!q->implicit_group) { m.set_return(url ? kErrorFileNotFound : kErrorInvalidParameter); return; }
    m.set_return(remove_url_locked(s, m.memory(), q->implicit_group, url, 0));
}

// =============================================================================
// Exports that are not implemented: the documented failure plus one log line.
// =============================================================================

#define HTTPAPI_UNIMPLEMENTED(name, what)                \
    void HttpApiManager::name(Marshaler& m) {            \
        unimplemented_once(#name, what);                 \
        m.set_return(kErrorNotSupported);                \
    }

HTTPAPI_UNIMPLEMENTED(HttpAddFragmentToCache, "response fragment cache")
HTTPAPI_UNIMPLEMENTED(HttpReadFragmentFromCache, "response fragment cache")
HTTPAPI_UNIMPLEMENTED(HttpFlushResponseCache, "response fragment cache")
HTTPAPI_UNIMPLEMENTED(HttpSetServiceConfiguration, "HTTP.sys configuration store (urlacl/sslcert/iplisten)")
HTTPAPI_UNIMPLEMENTED(HttpQueryServiceConfiguration, "HTTP.sys configuration store (urlacl/sslcert/iplisten)")
HTTPAPI_UNIMPLEMENTED(HttpDeleteServiceConfiguration, "HTTP.sys configuration store (urlacl/sslcert/iplisten)")
HTTPAPI_UNIMPLEMENTED(HttpUpdateServiceConfiguration, "HTTP.sys configuration store (urlacl/sslcert/iplisten)")
HTTPAPI_UNIMPLEMENTED(HttpControlService, "HTTP service control")
HTTPAPI_UNIMPLEMENTED(HttpDeclarePush, "HTTP/2 server push")
HTTPAPI_UNIMPLEMENTED(HttpDelegateRequest, "request delegation between queues")
HTTPAPI_UNIMPLEMENTED(HttpDelegateRequestEx, "request delegation between queues")
HTTPAPI_UNIMPLEMENTED(HttpEvaluateRequest, "request evaluation")
HTTPAPI_UNIMPLEMENTED(HttpFindUrlGroupId, "URL group lookup by URL")
HTTPAPI_UNIMPLEMENTED(HttpGetCounters, "performance counters")
HTTPAPI_UNIMPLEMENTED(HttpGetExtension, "extension function tables")
HTTPAPI_UNIMPLEMENTED(HttpPrepareUrl, "URL canonicalization")
HTTPAPI_UNIMPLEMENTED(HttpQueryRequestProperty, "per-request properties (TCP info, QUIC stats, ...)")
HTTPAPI_UNIMPLEMENTED(HttpSetRequestProperty, "per-request properties")
HTTPAPI_UNIMPLEMENTED(HttpReceiveClientCertificate, "TLS client certificates (no TLS server)")
HTTPAPI_UNIMPLEMENTED(HttpWaitForDemandStart, "demand-start request queues")

#undef HTTPAPI_UNIMPLEMENTED

void HttpApiManager::HttpIsFeatureSupported(Marshaler& m) {
    // BOOL HttpIsFeatureSupported(HTTP_FEATURE_ID FeatureId)
    // Of the optional HTTP_FEATURE_ID features only automatic chunked
    // encoding (HTTP_SEND_RESPONSE_FLAG_AUTOMATIC_CHUNKING) is provided.
    Args a(m);
    m.set_return(a.u32v() == HttpFeatureAutomaticChunkedEncoding ? 1 : 0);
}

// =============================================================================
// Export table
// =============================================================================

DllExports create_httpapi_thunks(HttpApiManager& mgr) {
    DllExports exports;
#define HTTPAPI_EXPORT(name, ordinal, bytes) \
    exports.entries.push_back(ExportEntry{#name, ordinal, [&mgr](Marshaler& m) { mgr.name(m); }, bytes});
#include "httpapi_exports.inc"
#undef HTTPAPI_EXPORT
    return exports;
}

} // namespace vitreo::winapi
