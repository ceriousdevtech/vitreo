// SPDX-License-Identifier: LGPL-2.1-only
/// @file wldap32.cpp
/// @brief Wldap32.dll synchronous LDAP client over the host's libldap.
///
/// Learn (winldap.h): ldap_init does not connect; the first operation that
/// needs the server connects. Results, DN/attribute strings, BerElements and
/// berval arrays are caller-freed with ldap_msgfree / ldap_memfree / ber_free
/// / ldap_value_free_len. Measured on Windows
/// (tools/oracle_probe/wldap32_contract_probe.{x86,x64}.expected):
///  - only ld_errno of the public LDAP block tracks the session; the other
///    fields keep their ldap_init values (version 2, refhoplimit 32,
///    ld_options 0x63, ld_host NULL) whatever ldap_set_option does;
///  - the first operation that cannot reach a server returns
///    LDAP_SERVER_DOWN, later ones on the same session LDAP_UNAVAILABLE;
///  - a NULL session is LDAP_PARAM_ERROR (search leaves *res untouched);
///  - ldap_set_option / ldap_get_option failures do not change
///    LdapGetLastError; unknown options are LDAP_PARAM_ERROR;
///  - ldap_err2string returns one static string per code, "" when unknown.
///
/// The host library is Apple's OpenLDAP 2.4 (LDAP.framework). Every libldap
/// symbol is marked deprecated in the SDK headers but is the system's LDAP
/// client; the deprecation warning is silenced for this file only.

#include "wldap32.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/heap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <lber.h>
#include <ldap.h>

namespace vitreo::winapi {

namespace {

// winldap.h result codes.
constexpr u32 W_LDAP_SUCCESS = 0x00;
constexpr u32 W_LDAP_UNAVAILABLE = 0x34;
constexpr u32 W_LDAP_SERVER_DOWN = 0x51;
constexpr u32 W_LDAP_LOCAL_ERROR = 0x52;
constexpr u32 W_LDAP_PARAM_ERROR = 0x59;
constexpr u32 W_LDAP_NO_MEMORY = 0x5a;
constexpr u32 W_LDAP_NOT_SUPPORTED = 0x5c;

constexpr u32 W_LDAP_PORT = 389;
constexpr u32 W_LDAP_SSL_PORT = 636;

// Session options (Learn "Session Options"; LDAP_OPT_ON = 1, OFF = 0).
constexpr i32 OPT_DEREF = 0x02;
constexpr i32 OPT_SIZELIMIT = 0x03;
constexpr i32 OPT_TIMELIMIT = 0x04;
constexpr i32 OPT_REFERRALS = 0x08;
constexpr i32 OPT_SSL = 0x0A;
constexpr i32 OPT_REFERRAL_HOP_LIMIT = 0x10;
constexpr i32 OPT_PROTOCOL_VERSION = 0x11;
constexpr i32 OPT_HOST_NAME = 0x30;
constexpr i32 OPT_ERROR_NUMBER = 0x31;
constexpr i32 OPT_ERROR_STRING = 0x32;
constexpr i32 OPT_AREC_EXCLUSIVE = 0x98;

// Options Learn documents as unsupported: LDAP_LOCAL_ERROR on get and set.
[[nodiscard]] bool is_documented_unsupported(i32 option) {
    switch (option) {
    case 0x05: case 0x06: case 0x07: case 0x09:
    case 0x0B: case 0x0D: case 0x0E: case 0x0F:
        return true;
    default:
        return false;
    }
}

// Options Learn documents that this client does not provide yet.
[[nodiscard]] const char* documented_option_name(i32 option) {
    switch (option) {
    case 0x00: return "LDAP_OPT_API_INFO";
    case 0x01: return "LDAP_OPT_DESC";
    case 0x15: return "LDAP_OPT_API_FEATURE_INFO";
    case 0x33: return "LDAP_OPT_SERVER_ERROR";
    case 0x34: return "LDAP_OPT_SERVER_EXT_ERROR";
    case 0x36: return "LDAP_OPT_PING_KEEP_ALIVE";
    case 0x37: return "LDAP_OPT_PING_WAIT_TIME";
    case 0x38: return "LDAP_OPT_PING_LIMIT";
    case 0x3B: return "LDAP_OPT_DNSDOMAIN_NAME";
    case 0x3D: return "LDAP_OPT_GETDSNAME_FLAGS";
    case 0x3E: return "LDAP_OPT_HOST_REACHABLE";
    case 0x3F: return "LDAP_OPT_PROMPT_CREDENTIALS";
    case 0x40: return "LDAP_OPT_TCP_KEEPALIVE";
    case 0x41: return "LDAP_OPT_FAST_CONCURRENT_BIND";
    case 0x42: return "LDAP_OPT_SEND_TIMEOUT";
    case 0x43: return "LDAP_OPT_SCH_FLAGS";
    case 0x44: return "LDAP_OPT_SOCKET_BIND_ADDRESSES";
    case 0x70: return "LDAP_OPT_REFERRAL_CALLBACK";
    case 0x80: return "LDAP_OPT_CLIENT_CERTIFICATE";
    case 0x81: return "LDAP_OPT_SERVER_CERTIFICATE";
    case 0x91: return "LDAP_OPT_AUTO_RECONNECT";
    case 0x92: return "LDAP_OPT_SSPI_FLAGS";
    case 0x93: return "LDAP_OPT_SSL_INFO";
    case 0x94: return "LDAP_OPT_REF_DEREF_CONN_PER_MSG";
    case 0x95: return "LDAP_OPT_SIGN";
    case 0x96: return "LDAP_OPT_ENCRYPT";
    case 0x97: return "LDAP_OPT_SASL_METHOD";
    case 0x99: return "LDAP_OPT_SECURITY_CONTEXT";
    case 0x9A: return "LDAP_OPT_ROOTDSE_CACHE";
    default: return nullptr;
    }
}

// ldap_err2string text, tools/oracle_probe/wldap32_contract_probe ERR2STR
// (identical on x86 and x64). Codes not listed return "".
struct ErrText { u32 code; const char* text; };
constexpr ErrText kErrText[] = {
    {0x00, "Success"},
    {0x01, "Operations Error"},
    {0x02, "Protocol Error"},
    {0x03, "Time Limit Exceeded"},
    {0x04, "Size Limit Exceeded"},
    {0x05, "Compare False"},
    {0x06, "Compare True"},
    {0x07, "Authentication Method Not Supported"},
    {0x08, "Strong Authentication Required"},
    {0x09, "Referral (v2)"},
    {0x0a, "Referral"},
    {0x0b, "Administration Limit Exceeded"},
    {0x0c, "Unavailable Critical Extension"},
    {0x0d, "Confidentiality Required"},
    {0x10, "No Such Attribute"},
    {0x11, "Undefined Type"},
    {0x12, "Inappropriate Matching"},
    {0x13, "Constraint Violation"},
    {0x14, "Attribute Or Value Exists"},
    {0x15, "Invalid Syntax"},
    {0x20, "No Such Object"},
    {0x21, "Alias Problem"},
    {0x22, "Invalid DN Syntax"},
    {0x23, "Is Leaf"},
    {0x24, "Alias Dereference Problem"},
    {0x30, "Inappropriate Authentication"},
    {0x31, "Invalid Credentials"},
    {0x32, "Insufficient Rights"},
    {0x33, "Busy"},
    {0x34, "Unavailable"},
    {0x35, "Unwilling To Perform"},
    {0x36, "Loop Detected"},
    {0x3c, "Sort Control Missing"},
    {0x3d, "Index range error"},
    {0x40, "Naming Violation"},
    {0x41, "Object Class Violation"},
    {0x42, "Not allowed on Non-leaf"},
    {0x43, "Not allowed on RDN"},
    {0x44, "Already Exists"},
    {0x45, "No Object Class Mods"},
    {0x46, "Results Too Large"},
    {0x47, "Affects Multiple DSAs"},
    {0x4c, "Control Error"},
    {0x50, "Other"},
    {0x51, "Server Down"},
    {0x52, "Local Error"},
    {0x53, "Encoding Error"},
    {0x54, "Decoding Error"},
    {0x55, "Timeout"},
    {0x56, "Auth Unknown"},
    {0x57, "Filter Error"},
    {0x58, "User Cancelled"},
    {0x59, "Parameter Error"},
    {0x5a, "No Memory"},
    {0x5b, "Can't connect to the LDAP server"},
    {0x5c, "Operation not supported by this version of the LDAP protocol"},
    {0x5d, "Specified control was not found in message"},
    {0x5e, "No result present in message"},
    {0x5f, "More results returned"},
    {0x60, "Loop while handling referrals"},
    {0x61, "Referral hop limit exceeded"},
};

// Public structure layouts (winldap.h; offsets from the probe's OFFSET lines).
struct Layout {
    u32 ldap_size, ld_host, ld_version, ld_errno, ld_refhoplimit, ld_options;
    u32 msg_size, lm_msgid, lm_msgtype, lm_chain, lm_next, connection, lm_returncode;
    u32 berval_size, bv_val, ptr;
};
constexpr Layout kLayout64{184, 88, 96, 116, 176, 180, 72, 0, 4, 16, 24, 40, 56, 16, 8, 8};
constexpr Layout kLayout32{160, 76, 80, 100, 152, 156, 44, 0, 4, 12, 16, 24, 32, 8, 4, 4};

[[nodiscard]] const Layout& layout(const Marshaler& m) {
    return m.cpu_is_64bit() ? kLayout64 : kLayout32;
}

// ldap_init's public-block values (probe FIELDS_FRESH).
constexpr u32 kInitVersion = 2;
constexpr u32 kInitRefHopLimit = 32;
constexpr u32 kInitOptions = 0x63;

/// LdapGetLastError: the calling thread's most recent session error.
thread_local u32 t_ldap_last_error = 0;

/// OpenLDAP result code -> winldap.h. Protocol codes (>= 0) are the RFC 4511
/// values in both; the client codes run -1..-17 in OpenLDAP and
/// 0x51..0x61 in winldap.h, in the same order (SDK ldap.h vs winldap.h).
[[nodiscard]] u32 map_host_result(int rc) {
    if (rc >= 0) return static_cast<u32>(rc);
    if (rc >= LDAP_REFERRAL_LIMIT_EXCEEDED) return 0x50u + static_cast<u32>(-rc);
    return W_LDAP_LOCAL_ERROR;
}

void unimplemented_once(const char* what) {
    static std::mutex mu;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> lock(mu);
    if (seen.insert(what).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED wldap32!%s\n", what);
    }
}

[[nodiscard]] std::string read_string(Marshaler& m, GuestAddr p, bool wide) {
    if (p == 0) return {};
    auto r = wide ? m.read_wide_string(p) : m.read_ansi_string(p);
    return r.is_ok() ? std::move(r).value() : std::string{};
}

/// URI list for ldap_initialize from ldap_init's HostName: space-separated
/// hosts, each with an optional ":port" that overrides PortNumber.
[[nodiscard]] std::string build_uri_list(const std::string& hosts, u32 port, bool ssl) {
    const u32 default_port = port != 0 ? port : (ssl ? W_LDAP_SSL_PORT : W_LDAP_PORT);
    std::string out;
    usize i = 0;
    while (i < hosts.size()) {
        while (i < hosts.size() && hosts[i] == ' ') ++i;
        usize j = i;
        while (j < hosts.size() && hosts[j] != ' ') ++j;
        if (j > i) {
            std::string host = hosts.substr(i, j - i);
            std::string port_part;
            if (!host.empty() && host.front() == '[') {
                const usize close = host.find(']');
                if (close != std::string::npos && close + 1 < host.size() && host[close + 1] == ':') {
                    port_part = host.substr(close + 2);
                    host.resize(close + 1);
                }
            } else if (std::count(host.begin(), host.end(), ':') == 1) {
                const usize colon = host.find(':');
                port_part = host.substr(colon + 1);
                host.resize(colon);
            } else if (host.find(':') != std::string::npos) {
                host = "[" + host + "]";  // bare IPv6 literal
            }
            if (port_part.empty()) port_part = std::to_string(default_port);
            if (!out.empty()) out.push_back(' ');
            out += (ssl ? "ldaps://" : "ldap://") + host + ":" + port_part;
        }
        i = j;
    }
    return out;
}

} // namespace

struct Wldap32Manager::Session {
    GuestAddr guest = 0;
    std::mutex op_mu;  // one libldap operation at a time per session
    LDAP* host = nullptr;
    std::string hosts;
    u32 port = 0;
    bool ssl = false;
    bool connected = false;
    bool connect_failed = false;
    u32 version = kInitVersion;
    u32 sizelimit = 0;
    u32 timelimit = 0;
    u32 deref = 0;
    u32 referrals = 1;
    u32 hop_limit = kInitRefHopLimit;
    u32 arec_exclusive = 0;
    u32 error = 0;
    GuestAddr strings[2][2] = {};  // [host name, error string][A, W]
};

struct Wldap32Manager::Chain {
    LDAPMessage* host_head = nullptr;
    GuestAddr guest_head = 0;
    GuestAddr ld = 0;
    std::vector<std::pair<LDAPMessage*, GuestAddr>> messages;
};

Wldap32Manager::~Wldap32Manager() {
    for (auto& [addr, chain] : m_messages) {
        if (chain->guest_head == addr && chain->host_head) ldap_msgfree(chain->host_head);
    }
    for (auto& [addr, ber] : m_bers) ::ber_free(static_cast<BerElement*>(ber), 0);
    for (auto& [addr, s] : m_sessions) {
        if (s->host) ldap_unbind_ext_s(s->host, nullptr, nullptr);
    }
}

GuestAddr Wldap32Manager::alloc(u64 bytes) {
    if (!m_heap) return 0;
    return static_cast<GuestAddr>(
        m_heap->heap_alloc(m_heap->get_process_heap(), HEAP_ZERO_MEMORY, bytes ? bytes : 1));
}

void Wldap32Manager::free_block(GuestAddr addr) {
    if (addr && m_heap) (void)m_heap->heap_free(m_heap->get_process_heap(), 0, addr);
}

GuestAddr Wldap32Manager::alloc_string(Marshaler& m, const std::string& s, bool wide) {
    // Worst case UTF-16 needs one unit per UTF-8 byte.
    const GuestAddr p = alloc(wide ? (s.size() + 1) * 2 : s.size() + 1);
    if (!p) return 0;
    if (!(wide ? m.write_wide_string(p, s) : m.write_ansi_string(p, s)).is_ok()) {
        free_block(p);
        return 0;
    }
    return p;
}

std::shared_ptr<Wldap32Manager::Session> Wldap32Manager::find_session(GuestAddr ld) {
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_sessions.find(ld);
    return it == m_sessions.end() ? nullptr : it->second;
}

void Wldap32Manager::set_session_error(Marshaler& m, Session& s, u32 code) {
    s.error = code;
    t_ldap_last_error = code;
    (void)m.write_u32(s.guest + layout(m).ld_errno, code);
}

/// Readies the host handle for an operation that needs the server (the
/// implicit ldap_connect). libldap opens the socket inside that operation,
/// so finish_operation() records whether it reached a server. Caller holds
/// s.op_mu.
u32 Wldap32Manager::connect(Session& s) {
    if (s.host) return W_LDAP_SUCCESS;
    if (s.connect_failed) return W_LDAP_UNAVAILABLE;
    const std::string uris = build_uri_list(s.hosts, s.port, s.ssl);
    if (uris.empty()) {
        // HostName NULL means the default server of the computer's Active
        // Directory domain. A Vitreo capsule is never a domain member, so
        // there is no such server (probe NULLHOST_BIND = LDAP_SERVER_DOWN).
        s.connect_failed = true;
        return W_LDAP_SERVER_DOWN;
    }
    LDAP* ld = nullptr;
    if (ldap_initialize(&ld, uris.c_str()) != LDAP_SUCCESS || ld == nullptr) {
        s.connect_failed = true;
        return W_LDAP_SERVER_DOWN;
    }
    int version = static_cast<int>(s.version);
    int sizelimit = static_cast<int>(s.sizelimit);
    int timelimit = static_cast<int>(s.timelimit);
    int deref = static_cast<int>(s.deref);
    int hops = static_cast<int>(s.hop_limit);
    (void)::ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);
    (void)::ldap_set_option(ld, LDAP_OPT_SIZELIMIT, &sizelimit);
    (void)::ldap_set_option(ld, LDAP_OPT_TIMELIMIT, &timelimit);
    (void)::ldap_set_option(ld, LDAP_OPT_DEREF, &deref);
    (void)::ldap_set_option(ld, LDAP_OPT_REFERRALS, s.referrals ? LDAP_OPT_ON : LDAP_OPT_OFF);
    (void)::ldap_set_option(ld, LDAP_OPT_REFHOPLIMIT, &hops);
    s.host = ld;
    return W_LDAP_SUCCESS;
}

/// After the first server operation: LDAP_SERVER_DOWN before any connection
/// existed means the connect failed, and later operations on this session
/// report LDAP_UNAVAILABLE (probe BIND_NOSERVER then SEARCH_NOSERVER, and
/// SEARCH_NOBIND then BIND_AFTER_SEARCH).
void Wldap32Manager::finish_operation(Session& s, u32 rc) {
    if (s.connected) return;
    if (rc == W_LDAP_SERVER_DOWN) {
        ldap_unbind_ext_s(s.host, nullptr, nullptr);
        s.host = nullptr;
        s.connect_failed = true;
    } else {
        s.connected = true;
    }
}

GuestAddr Wldap32Manager::guest_message_for(void* host_msg) {
    if (!host_msg) return 0;
    std::lock_guard<std::mutex> lock(m_mu);
    for (auto& [addr, chain] : m_messages) {
        if (chain->guest_head != addr) continue;
        for (auto& [h, g] : chain->messages) {
            if (h == host_msg) return g;
        }
    }
    return 0;
}

void* Wldap32Manager::host_message(GuestAddr msg, GuestAddr* ld_out) {
    std::lock_guard<std::mutex> lock(m_mu);
    auto it = m_messages.find(msg);
    if (it == m_messages.end()) return nullptr;
    if (ld_out) *ld_out = it->second->ld;
    for (auto& [h, g] : it->second->messages) {
        if (g == msg) return h;
    }
    return nullptr;
}

// LDAP* ldap_init(PSTR HostName, ULONG PortNumber)
// LDAP* ldap_sslinit(PSTR HostName, ULONG PortNumber, int secure)
void Wldap32Manager::init(Marshaler& m, bool wide, bool ssl_entry) {
    const GuestAddr host_ptr = m.arg_ptr(0);
    const u32 port = static_cast<u32>(m.arg(1));
    const bool secure = ssl_entry && static_cast<u32>(m.arg(2)) != 0;
    const Layout& L = layout(m);
    const GuestAddr block = alloc(L.ldap_size);
    if (!block) {
        t_ldap_last_error = W_LDAP_NO_MEMORY;
        m.set_return(0);
        return;
    }
    (void)m.write_u32(block + L.ld_version, kInitVersion);
    (void)m.write_u32(block + L.ld_refhoplimit, kInitRefHopLimit);
    (void)m.write_u32(block + L.ld_options, kInitOptions);
    auto s = std::make_shared<Session>();
    s->guest = block;
    s->hosts = read_string(m, host_ptr, wide);
    s->port = port;
    s->ssl = secure;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_sessions[block] = s;
    }
    t_ldap_last_error = W_LDAP_SUCCESS;  // probe INIT ldgle=0
    m.set_return(block);
}

// ULONG ldap_unbind_s(LDAP* ld)
void Wldap32Manager::unbind(Marshaler& m) {
    const GuestAddr ld = m.arg_ptr(0);
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_sessions.find(ld);
        if (it != m_sessions.end()) {
            s = it->second;
            m_sessions.erase(it);
        }
    }
    if (!s) {
        m.set_return(W_LDAP_PARAM_ERROR);  // probe UNBIND_NULL
        return;
    }
    std::lock_guard<std::mutex> op(s->op_mu);
    if (s->host) ldap_unbind_ext_s(s->host, nullptr, nullptr);
    s->host = nullptr;
    for (auto& kind : s->strings) {
        for (GuestAddr& p : kind) { free_block(p); p = 0; }
    }
    free_block(s->guest);
    m.set_return(W_LDAP_SUCCESS);
}

// ULONG ldap_set_option(LDAP* ld, int option, const void* invalue)
void Wldap32Manager::set_option(Marshaler& m, bool wide) {
    (void)wide;  // no settable option carries a string
    const GuestAddr ld = m.arg_ptr(0);
    const i32 option = static_cast<i32>(m.arg(1));
    const GuestAddr value = m.arg_ptr(2);
    auto s = find_session(ld);
    if (!s) { m.set_return(W_LDAP_PARAM_ERROR); return; }  // probe SET_OPT_NULL_LD
    if (is_documented_unsupported(option)) { m.set_return(W_LDAP_LOCAL_ERROR); return; }

    std::lock_guard<std::mutex> op(s->op_mu);
    // On/off options take LDAP_OPT_ON / LDAP_OPT_OFF themselves or a ULONG*
    // (probe SET_REFERRALS_ON / SET_REFERRALS_PTR0).
    auto on_off = [&](u32& out) -> bool {
        if (value <= 1) { out = static_cast<u32>(value); return true; }
        auto v = m.read_u32(value);
        if (!v.is_ok()) return false;
        out = v.value();
        return true;
    };
    auto ulong = [&](u32& out) -> bool {
        if (!value) return false;
        auto v = m.read_u32(value);
        if (!v.is_ok()) return false;
        out = v.value();
        return true;
    };
    auto host_int = [&](int host_option, u32 v) {
        if (s->host) {
            int iv = static_cast<int>(v);
            (void)::ldap_set_option(s->host, host_option, &iv);
        }
    };

    u32 v = 0;
    switch (option) {
    case OPT_PROTOCOL_VERSION:
        if (!ulong(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        if (v != 2 && v != 3) { m.set_return(W_LDAP_LOCAL_ERROR); return; }  // probe SET_VERSION7
        s->version = v;
        host_int(LDAP_OPT_PROTOCOL_VERSION, v);
        break;
    case OPT_SIZELIMIT:
        if (!ulong(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        s->sizelimit = v;
        host_int(LDAP_OPT_SIZELIMIT, v);
        break;
    case OPT_TIMELIMIT:
        if (!ulong(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        s->timelimit = v;
        host_int(LDAP_OPT_TIMELIMIT, v);
        break;
    case OPT_DEREF:
        if (!ulong(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        if (v > 3) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        s->deref = v;
        host_int(LDAP_OPT_DEREF, v);
        break;
    case OPT_REFERRAL_HOP_LIMIT:
        if (!ulong(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        s->hop_limit = v;
        host_int(LDAP_OPT_REFHOPLIMIT, v);
        break;
    case OPT_REFERRALS:
        if (!on_off(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        if (v > 1) {
            // LDAP_CHASE_SUBORDINATE_REFERRALS / LDAP_CHASE_EXTERNAL_REFERRALS:
            // libldap chases all referrals or none.
            unimplemented_once("ldap_set_option: LDAP_OPT_REFERRALS chase-kind flags");
            m.set_return(W_LDAP_NOT_SUPPORTED);
            return;
        }
        s->referrals = v;
        if (s->host) (void)::ldap_set_option(s->host, LDAP_OPT_REFERRALS, v ? LDAP_OPT_ON : LDAP_OPT_OFF);
        break;
    case OPT_SSL:
        if (!on_off(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        if (s->connected && (v != 0) != s->ssl) {
            unimplemented_once("ldap_set_option: LDAP_OPT_SSL on an open connection");
            m.set_return(W_LDAP_NOT_SUPPORTED);
            return;
        }
        s->ssl = v != 0;
        break;
    case OPT_AREC_EXCLUSIVE:
        // Host names are always resolved by A/AAAA lookup (no SRV domain
        // discovery), which is the ON behavior; the value is reported back.
        if (!on_off(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        s->arec_exclusive = v ? 1 : 0;
        break;
    case OPT_ERROR_NUMBER:
        if (!ulong(v)) { m.set_return(W_LDAP_PARAM_ERROR); return; }
        s->error = v;
        (void)m.write_u32(s->guest + layout(m).ld_errno, v);
        break;
    default:
        if (const char* name = documented_option_name(option)) {
            unimplemented_once((std::string("ldap_set_option: ") + name).c_str());
            m.set_return(W_LDAP_NOT_SUPPORTED);
            return;
        }
        if (option == OPT_HOST_NAME || option == OPT_ERROR_STRING) {
            unimplemented_once("ldap_set_option: string options");
            m.set_return(W_LDAP_NOT_SUPPORTED);
            return;
        }
        m.set_return(W_LDAP_PARAM_ERROR);  // probe SET_UNKNOWN
        return;
    }
    m.set_return(W_LDAP_SUCCESS);
}

// ULONG ldap_get_option(LDAP* ld, int option, void* outvalue)
void Wldap32Manager::get_option(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const i32 option = static_cast<i32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    auto s = find_session(ld);
    if (!s) { m.set_return(W_LDAP_PARAM_ERROR); return; }
    if (is_documented_unsupported(option)) { m.set_return(W_LDAP_LOCAL_ERROR); return; }
    if (!out) { m.set_return(W_LDAP_PARAM_ERROR); return; }

    std::lock_guard<std::mutex> op(s->op_mu);
    auto put = [&](u32 v) { m.set_return((m.write_u32(out, v).is_ok()) ? W_LDAP_SUCCESS : W_LDAP_PARAM_ERROR); };
    auto put_string = [&](int which, const std::string& text, bool present) {
        GuestAddr& slot = s->strings[which][wide ? 1 : 0];
        free_block(slot);
        slot = present ? alloc_string(m, text, wide) : 0;
        m.set_return(m.write_ptr(out, slot).is_ok() ? W_LDAP_SUCCESS : W_LDAP_PARAM_ERROR);
    };
    switch (option) {
    case OPT_PROTOCOL_VERSION: put(s->version); return;
    case OPT_SIZELIMIT: put(s->sizelimit); return;
    case OPT_TIMELIMIT: put(s->timelimit); return;
    case OPT_DEREF: put(s->deref); return;
    case OPT_REFERRALS: put(s->referrals); return;
    case OPT_REFERRAL_HOP_LIMIT: put(s->hop_limit); return;
    // Reports the security of the open connection: 0 until an SSL
    // connection exists (probe SSLINIT_FIELDS ssl=0 before connecting).
    case OPT_SSL: put(s->connected && s->ssl ? 1u : 0u); return;
    case OPT_AREC_EXCLUSIVE: put(s->arec_exclusive); return;
    case OPT_ERROR_NUMBER: put(s->error); return;
    case OPT_HOST_NAME: {
        // NULL until connected (probe GET_HOSTNAME); then the server in use.
        std::string name;
        bool present = false;
        if (s->host) {
            char* uri = nullptr;
            if (::ldap_get_option(s->host, LDAP_OPT_HOST_NAME, &uri) == LDAP_OPT_SUCCESS && uri) {
                name = uri;
                present = true;
                ldap_memfree(uri);
            }
        }
        put_string(0, name, present);
        return;
    }
    case OPT_ERROR_STRING: {
        const char* text = "";
        for (const auto& e : kErrText) if (e.code == s->error) text = e.text;
        put_string(1, text, true);
        return;
    }
    default:
        if (const char* name = documented_option_name(option)) {
            unimplemented_once((std::string("ldap_get_option: ") + name).c_str());
            m.set_return(W_LDAP_NOT_SUPPORTED);
            return;
        }
        m.set_return(W_LDAP_PARAM_ERROR);  // probe GET_UNKNOWN
        return;
    }
}

// ULONG ldap_simple_bind_s(LDAP* ld, PSTR dn, PSTR passwd)
void Wldap32Manager::simple_bind_s(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr dn_ptr = m.arg_ptr(1);
    const GuestAddr pw_ptr = m.arg_ptr(2);
    auto s = find_session(ld);
    if (!s) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;
        m.set_return(W_LDAP_PARAM_ERROR);  // probe SIMPLE_BIND_NULL_LD
        return;
    }
    const std::string dn = read_string(m, dn_ptr, wide);
    std::string pw = read_string(m, pw_ptr, wide);
    std::lock_guard<std::mutex> op(s->op_mu);
    u32 rc = connect(*s);
    if (rc == W_LDAP_SUCCESS) {
        struct berval cred{static_cast<ber_len_t>(pw.size()), pw.data()};
        rc = map_host_result(ldap_sasl_bind_s(s->host, dn.c_str(), LDAP_SASL_SIMPLE, &cred,
                                              nullptr, nullptr, nullptr));
        finish_operation(*s, rc);
    }
    set_session_error(m, *s, rc);
    m.set_return(rc);
}

// ULONG ldap_search_s(LDAP* ld, PSTR base, ULONG scope, PSTR filter,
//                     PZPSTR attrs, ULONG attrsonly, PLDAPMessage* res)
void Wldap32Manager::search_s(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr base_ptr = m.arg_ptr(1);
    const u32 scope = static_cast<u32>(m.arg(2));
    const GuestAddr filter_ptr = m.arg_ptr(3);
    const GuestAddr attrs_ptr = m.arg_ptr(4);
    const u32 attrsonly = static_cast<u32>(m.arg(5));
    const GuestAddr res_ptr = m.arg_ptr(6);
    const Layout& L = layout(m);
    auto s = find_session(ld);
    if (!s || !res_ptr) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;
        m.set_return(W_LDAP_PARAM_ERROR);  // probe SEARCH_NULL_LD: *res untouched
        return;
    }
    const std::string base = read_string(m, base_ptr, wide);
    const std::string filter = read_string(m, filter_ptr, wide);
    std::vector<std::string> attrs;
    const bool all_attrs = attrs_ptr == 0;
    for (u32 i = 0; !all_attrs; ++i) {
        u64 p = 0;
        if (m.cpu_is_64bit()) {
            auto r = m.read_u64(attrs_ptr + i * 8);
            if (r.is_ok()) p = r.value();
        } else {
            auto r = m.read_u32(attrs_ptr + i * 4);
            if (r.is_ok()) p = r.value();
        }
        if (p == 0) break;
        attrs.push_back(read_string(m, static_cast<GuestAddr>(p), wide));
    }
    std::vector<char*> attr_ptrs;
    for (auto& a : attrs) attr_ptrs.push_back(a.data());
    attr_ptrs.push_back(nullptr);

    std::lock_guard<std::mutex> op(s->op_mu);
    LDAPMessage* res = nullptr;
    u32 rc = connect(*s);
    if (rc == W_LDAP_SUCCESS) {
        rc = map_host_result(ldap_search_ext_s(
            s->host, base.c_str(), static_cast<int>(scope),
            filter.empty() ? nullptr : filter.c_str(), all_attrs ? nullptr : attr_ptrs.data(),
            attrsonly ? 1 : 0, nullptr, nullptr, nullptr, LDAP_NO_LIMIT, &res));
    }
    const bool searched = s->host != nullptr;
    GuestAddr head = 0;
    if (res) {
        // Partial results (size limit, referrals) are returned with the
        // error code (Learn ldap_search_s).
        auto chain = std::make_shared<Chain>();
        chain->host_head = res;
        chain->ld = ld;
        for (LDAPMessage* h = ldap_first_message(s->host, res); h; h = ldap_next_message(s->host, h)) {
            const GuestAddr g = alloc(L.msg_size);
            if (!g) { rc = W_LDAP_NO_MEMORY; break; }
            (void)m.write_u32(g + L.lm_msgid, static_cast<u32>(ldap_msgid(h)));
            (void)m.write_u32(g + L.lm_msgtype, static_cast<u32>(ldap_msgtype(h)));
            (void)m.write_ptr(g + L.connection, ld);
            if (!chain->messages.empty()) (void)m.write_ptr(chain->messages.back().second + L.lm_chain, g);
            if (ldap_msgtype(h) == LDAP_RES_SEARCH_RESULT) {
                int result = LDAP_SUCCESS;
                (void)ldap_parse_result(s->host, h, &result, nullptr, nullptr, nullptr, nullptr, 0);
                (void)m.write_u32(g + L.lm_returncode, map_host_result(result));
            }
            chain->messages.emplace_back(h, g);
        }
        if (chain->messages.empty()) {
            ldap_msgfree(res);
        } else {
            head = chain->messages.front().second;
            chain->guest_head = head;
            std::lock_guard<std::mutex> lock(m_mu);
            for (auto& [h, g] : chain->messages) m_messages[g] = chain;
        }
    }
    if (searched) finish_operation(*s, rc);  // after the chain stops using s->host
    (void)m.write_ptr(res_ptr, head);
    set_session_error(m, *s, rc);
    m.set_return(rc);
}

// ULONG ldap_msgfree(LDAPMessage* res) -- always LDAP_SUCCESS (Learn).
void Wldap32Manager::msgfree(Marshaler& m) {
    const GuestAddr msg = m.arg_ptr(0);
    std::shared_ptr<Chain> chain;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_messages.find(msg);
        if (it != m_messages.end() && it->second->guest_head == msg) {
            chain = it->second;
            for (auto& [h, g] : chain->messages) m_messages.erase(g);
        }
    }
    if (chain) {
        ::ldap_msgfree(chain->host_head);
        for (auto& [h, g] : chain->messages) free_block(g);
    }
    m.set_return(W_LDAP_SUCCESS);
}

// LDAPMessage* ldap_first_entry(LDAP* ld, LDAPMessage* res)
// LDAPMessage* ldap_next_entry(LDAP* ld, LDAPMessage* entry)
void Wldap32Manager::first_entry(Marshaler& m) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr msg = m.arg_ptr(1);
    auto s = find_session(ld);
    void* h = host_message(msg);
    if (!s || !h) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;  // probe FIRST_ENTRY_NULL
        if (s) set_session_error(m, *s, W_LDAP_PARAM_ERROR);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> op(s->op_mu);
    LDAPMessage* e = ::ldap_first_entry(s->host, static_cast<LDAPMessage*>(h));
    set_session_error(m, *s, W_LDAP_SUCCESS);
    m.set_return(guest_message_for(e));
}

void Wldap32Manager::next_entry(Marshaler& m) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr msg = m.arg_ptr(1);
    auto s = find_session(ld);
    void* h = host_message(msg);
    if (!s || !h) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;
        if (s) set_session_error(m, *s, W_LDAP_PARAM_ERROR);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> op(s->op_mu);
    LDAPMessage* e = ::ldap_next_entry(s->host, static_cast<LDAPMessage*>(h));
    set_session_error(m, *s, W_LDAP_SUCCESS);
    m.set_return(guest_message_for(e));
}

// PCHAR ldap_get_dn(LDAP* ld, LDAPMessage* entry) -- freed by ldap_memfree.
void Wldap32Manager::get_dn(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr msg = m.arg_ptr(1);
    auto s = find_session(ld);
    void* h = host_message(msg);
    if (!s || !h) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;  // probe GET_DN_NULL
        if (s) set_session_error(m, *s, W_LDAP_PARAM_ERROR);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> op(s->op_mu);
    char* dn = ::ldap_get_dn(s->host, static_cast<LDAPMessage*>(h));
    if (!dn) {
        int err = LDAP_SUCCESS;
        (void)::ldap_get_option(s->host, LDAP_OPT_RESULT_CODE, &err);
        set_session_error(m, *s, map_host_result(err ? err : LDAP_DECODING_ERROR));
        m.set_return(0);
        return;
    }
    const GuestAddr out = alloc_string(m, dn, wide);
    ldap_memfree(dn);
    if (!out) { set_session_error(m, *s, W_LDAP_NO_MEMORY); m.set_return(0); return; }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_buffers.insert(out);
    }
    set_session_error(m, *s, W_LDAP_SUCCESS);
    m.set_return(out);
}

// PCHAR ldap_first_attribute(LDAP* ld, LDAPMessage* entry, BerElement** ptr)
void Wldap32Manager::first_attribute(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr msg = m.arg_ptr(1);
    const GuestAddr ber_out = m.arg_ptr(2);
    auto s = find_session(ld);
    void* h = host_message(msg);
    if (!s || !h || !ber_out) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;  // probe FIRST_ATTR_NULL
        if (s) set_session_error(m, *s, W_LDAP_PARAM_ERROR);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> op(s->op_mu);
    BerElement* ber = nullptr;
    char* name = ::ldap_first_attribute(s->host, static_cast<LDAPMessage*>(h), &ber);
    GuestAddr token = 0;
    if (ber) {
        token = alloc(m.cpu_is_64bit() ? 16 : 8);  // opaque BerElement { char* opaque; }
        if (!token) {
            ::ber_free(ber, 0);
            if (name) ldap_memfree(name);
            set_session_error(m, *s, W_LDAP_NO_MEMORY);
            m.set_return(0);
            return;
        }
        std::lock_guard<std::mutex> lock(m_mu);
        m_bers[token] = ber;
    }
    (void)m.write_ptr(ber_out, token);
    GuestAddr out = 0;
    if (name) {
        out = alloc_string(m, name, wide);
        ldap_memfree(name);
        if (out) {
            std::lock_guard<std::mutex> lock(m_mu);
            m_buffers.insert(out);
        }
    }
    set_session_error(m, *s, name && !out ? W_LDAP_NO_MEMORY : W_LDAP_SUCCESS);
    m.set_return(out);
}

// PCHAR ldap_next_attribute(LDAP* ld, LDAPMessage* entry, BerElement* ptr)
void Wldap32Manager::next_attribute(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr msg = m.arg_ptr(1);
    const GuestAddr token = m.arg_ptr(2);
    auto s = find_session(ld);
    void* h = host_message(msg);
    BerElement* ber = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_bers.find(token);
        if (it != m_bers.end()) ber = static_cast<BerElement*>(it->second);
    }
    if (!s || !h || !ber) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;
        if (s) set_session_error(m, *s, W_LDAP_PARAM_ERROR);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> op(s->op_mu);
    char* name = ::ldap_next_attribute(s->host, static_cast<LDAPMessage*>(h), ber);
    GuestAddr out = 0;
    if (name) {
        out = alloc_string(m, name, wide);
        ldap_memfree(name);
        if (out) {
            std::lock_guard<std::mutex> lock(m_mu);
            m_buffers.insert(out);
        }
    }
    set_session_error(m, *s, name && !out ? W_LDAP_NO_MEMORY : W_LDAP_SUCCESS);
    m.set_return(out);
}

// berval** ldap_get_values_len(LDAP* ld, LDAPMessage* entry, PSTR attr)
void Wldap32Manager::get_values_len(Marshaler& m, bool wide) {
    const GuestAddr ld = m.arg_ptr(0);
    const GuestAddr msg = m.arg_ptr(1);
    const GuestAddr attr_ptr = m.arg_ptr(2);
    const Layout& L = layout(m);
    auto s = find_session(ld);
    void* h = host_message(msg);
    if (!s || !h || !attr_ptr) {
        t_ldap_last_error = W_LDAP_PARAM_ERROR;  // probe GET_VALUES_LEN_NULL
        if (s) set_session_error(m, *s, W_LDAP_PARAM_ERROR);
        m.set_return(0);
        return;
    }
    const std::string attr = read_string(m, attr_ptr, wide);
    std::lock_guard<std::mutex> op(s->op_mu);
    auto* entry = static_cast<LDAPMessage*>(h);
    // Learn: an attribute with no values is NULL with the session error 0.
    // libldap reports a missing attribute as LDAP_DECODING_ERROR, so look
    // the name up first (attribute descriptions compare case-insensitively).
    bool present = false;
    {
        BerElement* ber = nullptr;
        for (char* a = ::ldap_first_attribute(s->host, entry, &ber); a;
             a = ::ldap_next_attribute(s->host, entry, ber)) {
            if (strcasecmp(a, attr.c_str()) == 0) present = true;
            ldap_memfree(a);
            if (present) break;
        }
        if (ber) ::ber_free(ber, 0);
    }
    if (!present) {
        set_session_error(m, *s, W_LDAP_SUCCESS);
        m.set_return(0);
        return;
    }
    struct berval** vals = ::ldap_get_values_len(s->host, entry, attr.c_str());
    if (!vals) {
        int err = LDAP_SUCCESS;
        (void)::ldap_get_option(s->host, LDAP_OPT_RESULT_CODE, &err);
        set_session_error(m, *s, err ? map_host_result(err) : W_LDAP_SUCCESS);
        m.set_return(0);
        return;
    }
    // One block: NULL-terminated pointer array, the berval structs, the data.
    usize count = 0;
    u64 data_bytes = 0;
    for (; vals[count]; ++count) data_bytes += vals[count]->bv_len + 1;
    const u64 array_bytes = (count + 1) * L.ptr;
    const u64 structs_bytes = count * L.berval_size;
    const GuestAddr block = alloc(array_bytes + structs_bytes + data_bytes);
    if (!block) {
        ber_bvecfree(vals);
        set_session_error(m, *s, W_LDAP_NO_MEMORY);
        m.set_return(0);
        return;
    }
    GuestAddr data = block + array_bytes + structs_bytes;
    for (usize i = 0; i < count; ++i) {
        const GuestAddr bv = block + array_bytes + i * L.berval_size;
        (void)m.write_ptr(block + i * L.ptr, bv);
        (void)m.write_u32(bv, static_cast<u32>(vals[i]->bv_len));
        (void)m.write_ptr(bv + L.bv_val, data);
        if (vals[i]->bv_len) {
            (void)m.memory().write_bytes(data, reinterpret_cast<const Byte*>(vals[i]->bv_val),
                                         vals[i]->bv_len);
        }
        data += vals[i]->bv_len + 1;
    }
    ber_bvecfree(vals);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_buffers.insert(block);
    }
    set_session_error(m, *s, W_LDAP_SUCCESS);
    m.set_return(block);
}

// ULONG ldap_value_free_len(berval** vals) -- LDAP_SUCCESS, NULL included.
void Wldap32Manager::value_free_len(Marshaler& m) {
    const GuestAddr vals = m.arg_ptr(0);
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        owned = m_buffers.erase(vals) != 0;
    }
    if (owned) free_block(vals);
    m.set_return(W_LDAP_SUCCESS);
}

// VOID ldap_memfree(PCHAR Block)
void Wldap32Manager::memfree(Marshaler& m) {
    const GuestAddr p = m.arg_ptr(0);
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        owned = m_buffers.erase(p) != 0;
    }
    if (owned) free_block(p);
}

// VOID ber_free(BerElement* pBerElement, INT fbuf)
void Wldap32Manager::free_ber(Marshaler& m) {
    const GuestAddr token = m.arg_ptr(0);
    const int fbuf = static_cast<int>(m.arg(1));
    void* ber = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_bers.find(token);
        if (it != m_bers.end()) {
            ber = it->second;
            m_bers.erase(it);
        }
    }
    if (ber) {
        ::ber_free(static_cast<BerElement*>(ber), fbuf);
        free_block(token);
    }
}

// PCHAR ldap_err2string(ULONG err)
void Wldap32Manager::err2string(Marshaler& m, bool wide) {
    const u32 code = static_cast<u32>(m.arg(0));
    const char* text = "";
    for (const auto& e : kErrText) if (e.code == code) text = e.text;
    std::lock_guard<std::mutex> lock(m_mu);
    auto& table = m_err_strings[wide ? 1 : 0];
    // Unknown codes share the "" entry, keyed by a value no code uses.
    const u32 key = *text ? code : 0xFFFFFFFFu;
    auto it = table.find(key);
    if (it == table.end()) {
        it = table.emplace(key, alloc_string(m, text, wide)).first;
    }
    m.set_return(it->second);
}

DllExports create_wldap32_thunks(Wldap32Manager& mgr) {
    DllExports exports;
    // Ordinals: tools/oracle_probe/wldap32_contract_probe.{x86,x64}.expected
    // (identical tables). The undecorated names are the ANSI entry points.
    const auto add = [&](const char* name, u16 ordinal, u32 argc, ApiHandler handler) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(handler),
                                              static_cast<u16>(argc * 4u)});
    };
    struct Variants { const char* plain; u16 plain_ord; const char* a; u16 a_ord; const char* w; u16 w_ord; };
    const auto add_aw = [&](const Variants& v, u32 argc, auto fn) {
        add(v.plain, v.plain_ord, argc, [&mgr, fn](Marshaler& m) { (mgr.*fn)(m, false); });
        if (v.a) add(v.a, v.a_ord, argc, [&mgr, fn](Marshaler& m) { (mgr.*fn)(m, false); });
        if (v.w) add(v.w, v.w_ord, argc, [&mgr, fn](Marshaler& m) { (mgr.*fn)(m, true); });
    };

    add("ldap_init", 143, 2, [&mgr](Marshaler& m) { mgr.init(m, false, false); });
    add("ldap_initA", 144, 2, [&mgr](Marshaler& m) { mgr.init(m, false, false); });
    add("ldap_initW", 145, 2, [&mgr](Marshaler& m) { mgr.init(m, true, false); });
    add("ldap_sslinit", 217, 3, [&mgr](Marshaler& m) { mgr.init(m, false, true); });
    add("ldap_sslinitA", 218, 3, [&mgr](Marshaler& m) { mgr.init(m, false, true); });
    add("ldap_sslinitW", 219, 3, [&mgr](Marshaler& m) { mgr.init(m, true, true); });
    add("ldap_unbind_s", 46, 1, [&mgr](Marshaler& m) { mgr.unbind(m); });
    add_aw({"ldap_set_option", 211, "ldap_set_optionA", 212, "ldap_set_optionW", 14}, 3,
           &Wldap32Manager::set_option);
    add_aw({"ldap_get_option", 136, "ldap_get_optionA", 137, "ldap_get_optionW", 12}, 3,
           &Wldap32Manager::get_option);
    add_aw({"ldap_simple_bind_s", 60, "ldap_simple_bind_sA", 215, "ldap_simple_bind_sW", 216}, 3,
           &Wldap32Manager::simple_bind_s);
    add_aw({"ldap_search_s", 50, "ldap_search_sA", 207, "ldap_search_sW", 208}, 7,
           &Wldap32Manager::search_s);
    add("ldap_msgfree", 41, 1, [&mgr](Marshaler& m) { mgr.msgfree(m); });
    add("ldap_first_entry", 26, 2, [&mgr](Marshaler& m) { mgr.first_entry(m); });
    add("ldap_next_entry", 27, 2, [&mgr](Marshaler& m) { mgr.next_entry(m); });
    add_aw({"ldap_get_dn", 30, "ldap_get_dnA", 132, "ldap_get_dnW", 133}, 2,
           &Wldap32Manager::get_dn);
    add_aw({"ldap_first_attribute", 32, "ldap_first_attributeA", 126, "ldap_first_attributeW", 127}, 3,
           &Wldap32Manager::first_attribute);
    add_aw({"ldap_next_attribute", 33, "ldap_next_attributeA", 166, "ldap_next_attributeW", 167}, 3,
           &Wldap32Manager::next_attribute);
    add_aw({"ldap_get_values_len", 35, "ldap_get_values_lenA", 141, "ldap_get_values_lenW", 142}, 3,
           &Wldap32Manager::get_values_len);
    add("ldap_value_free_len", 79, 1, [&mgr](Marshaler& m) { mgr.value_free_len(m); });
    add("ldap_memfree", 200, 1, [&mgr](Marshaler& m) { mgr.memfree(m); });
    add("ldap_memfreeA", 146, 1, [&mgr](Marshaler& m) { mgr.memfree(m); });
    add("ldap_memfreeW", 147, 1, [&mgr](Marshaler& m) { mgr.memfree(m); });
    add("ber_free", 301, 2, [&mgr](Marshaler& m) { mgr.free_ber(m); });
    add_aw({"ldap_err2string", 22, "ldap_err2stringA", 117, "ldap_err2stringW", 118}, 1,
           &Wldap32Manager::err2string);
    add("LdapGetLastError", 16, 0, [](Marshaler& m) { m.set_return(t_ldap_last_error); });
    return exports;
}

} // namespace vitreo::winapi

#pragma clang diagnostic pop
