/// @file test_httpapi.cpp
/// @brief httpapi.dll (HTTP Server API) contracts, x86 and x64.
///
/// Every expected value comes from tests/oracle_data/httpapi_probe{32,64}.expected,
/// recorded on Windows 11 by tests/winapi/httpapi/oracle/httpapi_probe.c, or
/// from the SDK (http.h, httpapi.lib) -- never from running Vitreo. Comments
/// name the probe's line keys. The server listens on real loopback sockets
/// and the tests are its TCP clients.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <winapi/networking/httpapi/httpapi.hpp>
#include <winapi/networking/httpapi/httpapi_internal.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kStackBase = 0x80000;
constexpr GuestAddr kStackSize = 0x10000;
constexpr GuestAddr kData = 0x200000;
constexpr GuestAddr kDataSize = 0x100000;
constexpr GuestAddr kBuf = kData + 0x10000;     // 16 KiB request buffer, 8-aligned
constexpr u32 kBufSize = 16384;
constexpr u16 kPort = 48213;                    // the probe's port
constexpr u32 kV1 = 0x00000001, kV2 = 0x00000002;   // HTTPAPI_VERSION {major, minor}

/// The probe recording for one bitness: "key: rest" lines.
class Oracle {
public:
    explicit Oracle(bool x64) {
        std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + (x64 ? "/httpapi_probe64.expected"
                                                                     : "/httpapi_probe32.expected"));
        std::string line;
        while (std::getline(in, line)) {
            const usize c = line.find(": ");
            if (c == std::string::npos) continue;
            lines_.emplace_back(line.substr(0, c), line.substr(c + 2));
        }
    }
    /// The n-th line with this key.
    [[nodiscard]] std::string get(const std::string& key, int n = 0) const {
        for (const auto& [k, v] : lines_)
            if (k == key && n-- == 0) return v;
        ADD_FAILURE() << "no oracle line " << key;
        return {};
    }
    /// Integer field "name=value" of a line.
    [[nodiscard]] u64 field(const std::string& key, const std::string& name, int n = 0) const {
        const std::string v = get(key, n);
        const std::regex re("(^| )" + name + "=(0x[0-9a-f]+|[0-9]+)");
        std::smatch m;
        if (!std::regex_search(v, m, re)) {
            ADD_FAILURE() << key << " has no " << name;
            return ~0ull;
        }
        return std::stoull(m[2], nullptr, 0);
    }
    [[nodiscard]] std::string str(const std::string& key, const std::string& name, int n = 0) const {
        const std::string v = get(key, n);
        const usize at = v.find(name + "=\"");
        if (at == std::string::npos) { ADD_FAILURE() << key << " has no " << name; return {}; }
        std::string out;
        for (usize i = at + name.size() + 2; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size()) {
                const char e = v[++i];
                out.push_back(e == 'r' ? '\r' : e == 'n' ? '\n' : e);
                continue;
            }
            if (v[i] == '"') break;
            out.push_back(v[i]);
        }
        return out;
    }

private:
    std::vector<std::pair<std::string, std::string>> lines_;
};

/// A loopback TCP client on its own thread.
struct Client {
    std::string response;
    bool server_closed = false;
    int error = 0;          // ECONNRESET or ETIMEDOUT-ish (EAGAIN) when recv failed
    std::thread t;

    void start(std::string request, int delay_ms = 0, int wait_ms = 800) {
        t = std::thread([this, request, delay_ms, wait_ms] {
            if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(kPort);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
                error = errno;
                ::close(s);
                return;
            }
            (void)::send(s, request.data(), request.size(), 0);
            timeval tv{0, wait_ms * 1000};
            (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            char buf[8192];
            for (;;) {
                const ssize_t n = ::recv(s, buf, sizeof buf, 0);
                if (n == 0) { server_closed = true; break; }
                if (n < 0) { error = errno; break; }
                response.append(buf, static_cast<usize>(n));
            }
            ::close(s);
        });
    }
    Client() = default;
    Client(Client&&) = default;   // returned only after join()
    void join() { if (t.joinable()) t.join(); }
    ~Client() { join(); }
    /// The response with the Date value masked as the probe prints it.
    [[nodiscard]] std::string masked() const {
        return std::regex_replace(response, std::regex("\r\nDate: [^\r]*"), "\r\nDate: <date>");
    }
    /// The probe's print_status form: status line, header names, a few values.
    [[nodiscard]] std::string status_summary() const {
        std::string out = response.substr(0, response.find("\r\n")) + " headers=";
        usize p = response.find("\r\n");
        while (p != std::string::npos && response.compare(p, 4, "\r\n\r\n") != 0) {
            p += 2;
            const usize colon = response.find(':', p), eol = response.find("\r\n", p);
            const std::string name = response.substr(p, colon - p);
            out += name;
            if (name == "Content-Type" || name == "Connection" || name == "Server")
                out += "=" + response.substr(colon + 2, eol - colon - 2);
            out += ",";
            p = eol;
        }
        return out;
    }
};

constexpr const char* kGet = "GET /vprobe/r HTTP/1.1\r\nHost: localhost:48213\r\n\r\n";

class HttpApiTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(mem_.map_region(kStackBase, kStackSize, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem_.map_region(kData, kDataSize, MemoryProtection::ReadWrite).is_ok());
        handles_ = std::make_unique<HandleTable>([this](Handle h, HandleType type, void* object) {
            if (type == HandleType::Device) {
                (void)mgr_.on_handle_close(h);
                delete static_cast<std::string*>(object);
            }
            return true;
        });
        sync_ = std::make_unique<SyncManager>(*handles_);
        mgr_.set_handle_table(handles_.get());
        mgr_.set_memory(&mem_);
        mgr_.set_sync_manager(sync_.get());
        mgr_.set_overlapped_completion([this](Handle h, u32 bytes, GuestAddr ov, bool x64, u32 nt,
                                              u32 err, bool sync) {
            std::lock_guard lock(completions_mu_);
            completions_.push_back({h, bytes, ov, x64, nt, err, sync});
            put_ptr(ov, nt);
            put_ptr(ov + (is64() ? 8 : 4), bytes);
            const u64 ev = get_ptr(ov + (is64() ? 24 : 16));
            if (ev) (void)sync_->set_event(static_cast<Handle>(ev));
        });
        mgr_.set_port_association_probe([this](Handle h) { return ported_.count(h) != 0; });
        if (is64()) cpu_.init_long_mode();
        else cpu_.init_protected_mode();
        cpu_.set_sp(kStackBase + kStackSize - 0x200);
        exp_ = create_httpapi_thunks(mgr_);
    }

    void TearDown() override {
        mgr_.shutdown_all();
        sync_.reset();
        handles_.reset();
    }

    [[nodiscard]] bool is64() const { return GetParam(); }
    [[nodiscard]] const httpapi::Layout& L() const { return httpapi::layout_for(is64()); }
    [[nodiscard]] const Oracle& oracle() const {
        static Oracle o64(true), o32(false);
        return is64() ? o64 : o32;
    }

    /// Argument builder: an HTTP_OPAQUE_ID takes two stack slots on x86.
    struct A {
        bool x64;
        std::vector<u64> v;
        A& p(u64 x) { v.push_back(x); return *this; }
        A& id(u64 x) {
            if (x64) v.push_back(x);
            else { v.push_back(x & 0xFFFFFFFFu); v.push_back(x >> 32); }
            return *this;
        }
    };
    A args() const { return A{is64(), {}}; }

    u32 call(const std::string& name, const A& a) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exp_.entries)
            if (e.name == name) { entry = &e; break; }
        if (!entry) { ADD_FAILURE() << "missing export " << name; return ~0u; }
        if (!is64()) {
            // stdcall: the argument bytes the SDK import library declares.
            EXPECT_EQ(a.v.size() * 4, entry->stdcall_bytes) << name;
        }
        Marshaler m(mem_);
        m.attach_cpu(cpu_);
        const u64 saved = cpu_.sp();
        if (is64()) {
            const u64 sp = (saved - 0x28 - 8 * (a.v.size() > 4 ? a.v.size() - 4 : 0) - 0x40) & ~u64{0xF};
            cpu_.set_sp(sp);
            u64* regs[4] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
            for (usize i = 0; i < a.v.size(); ++i) {
                if (i < 4) *regs[i] = a.v[i];
                else (void)mem_.write_u64(sp + 0x28 + 8 * (i - 4), a.v[i]);
            }
        } else {
            const u64 sp = saved - 4 - 4 * a.v.size();
            cpu_.set_sp(sp);
            for (usize i = 0; i < a.v.size(); ++i)
                (void)mem_.write_u32(static_cast<GuestAddr>(sp + 4 + 4 * i), static_cast<u32>(a.v[i]));
        }
        cpu_.gpr.rax = 0x5A5A5A5A5A5A5A5Aull;
        entry->handler(m);
        cpu_.set_sp(saved);
        return static_cast<u32>(cpu_.gpr.rax);
    }

    void put_ptr(GuestAddr at, u64 v) {
        if (is64()) (void)mem_.write_u64(at, v);
        else (void)mem_.write_u32(at, static_cast<u32>(v));
    }
    [[nodiscard]] u64 get_ptr(GuestAddr at) const {
        return is64() ? mem_.read_u64(at).value() : mem_.read_u32(at).value();
    }
    GuestAddr wstr(GuestAddr at, const std::u16string& s) {
        for (usize i = 0; i < s.size(); ++i) (void)mem_.write_u16(at + 2 * i, s[i]);
        (void)mem_.write_u16(at + 2 * s.size(), 0);
        return at;
    }
    GuestAddr astr(GuestAddr at, const std::string& s) {
        (void)mem_.write_bytes(at, reinterpret_cast<const u8*>(s.c_str()), s.size() + 1);
        return at;
    }
    [[nodiscard]] std::string read_a(GuestAddr p, usize n) const {
        std::string s(n, '\0');
        (void)mem_.read_bytes(p, reinterpret_cast<u8*>(s.data()), n);
        return s;
    }
    [[nodiscard]] std::string read_w(GuestAddr p, usize bytes) const {
        std::string s;
        for (usize i = 0; i < bytes / 2; ++i) s.push_back(static_cast<char>(mem_.read_u16(p + 2 * i).value()));
        return s;
    }

    // --- Server set-up as the probe's server_open ---------------------------
    u64 sid_ = 0, gid_ = 0;
    Handle q_ = 0;

    void open_server(bool add_url = true, u64 context = 1) {
        ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(0)), 0u);
        ASSERT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), 0u);
        sid_ = mem_.read_u64(kData).value();
        ASSERT_EQ(call("HttpCreateUrlGroup", args().id(sid_).p(kData).p(0)), 0u);
        gid_ = mem_.read_u64(kData).value();
        ASSERT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0).p(kData)), 0u);
        q_ = static_cast<Handle>(get_ptr(kData));
        bind(q_);
        if (add_url) {
            ASSERT_EQ(call("HttpAddUrlToUrlGroup",
                           args().id(gid_).p(wstr(kData + 0x100, u"http://localhost:48213/vprobe/")).id(context).p(0)),
                      0u);
        }
    }
    u32 bind(Handle q, bool present = true) {
        const GuestAddr b = kData + 0x40;
        (void)mem_.write_u32(b, present ? 1 : 0);
        put_ptr(b + L().binding_queue, q);
        return call("HttpSetUrlGroupProperty", args().id(gid_).p(7).p(b).p(L().binding_size));
    }
    u32 receive(u64 id = 0, u32 flags = 0, u32 length = kBufSize, GuestAddr ov = 0, GuestAddr got = kData + 0x20) {
        return call("HttpReceiveHttpRequest", args().p(q_).id(id).p(flags).p(kBuf).p(length).p(got).p(ov));
    }
    [[nodiscard]] u64 request_id() const { return mem_.read_u64(kBuf + L().rq_request_id).value(); }

    /// An HTTP_RESPONSE_V2 at kData + 0x2000 with one memory chunk.
    GuestAddr response(u16 status, const std::string& reason, const std::string* body) {
        const GuestAddr r = kData + 0x2000;
        std::vector<u8> zero(L().response_v2_size, 0);
        (void)mem_.write_bytes(r, zero.data(), zero.size());
        (void)mem_.write_u16(r + L().rs_version, 1);
        (void)mem_.write_u16(r + L().rs_version + 2, 1);
        (void)mem_.write_u16(r + L().rs_status, status);
        (void)mem_.write_u16(r + L().rs_reason_length, static_cast<u16>(reason.size()));
        put_ptr(r + L().rs_reason, astr(kData + 0x3000, reason));
        if (body) {
            const GuestAddr c = kData + 0x3100;
            std::vector<u8> cz(32, 0);
            (void)mem_.write_bytes(c, cz.data(), cz.size());
            put_ptr(c + L().chunk_buffer, astr(kData + 0x3200, *body));
            (void)mem_.write_u32(c + L().chunk_buffer_length, static_cast<u32>(body->size()));
            (void)mem_.write_u16(r + L().rs_entity_chunk_count, 1);
            put_ptr(r + L().rs_entity_chunks, c);
        }
        return r;
    }
    void known_header(GuestAddr r, u32 id, const std::string& v, GuestAddr at) {
        const GuestAddr slot = r + L().rs_headers + L().hdr_known + id * L().known_header_size;
        (void)mem_.write_u16(slot, static_cast<u16>(v.size()));
        put_ptr(slot + L().known_header_value, astr(at, v));
    }
    u32 send(u64 id, u32 flags, GuestAddr resp, GuestAddr sent = 0, GuestAddr ov = 0) {
        return call("HttpSendHttpResponse",
                    args().p(q_).id(id).p(flags).p(resp).p(0).p(sent).p(0).p(0).p(ov).p(0));
    }
    u32 send_simple(u64 id, u16 status, const std::string& reason, const std::string* body, u32 flags = 0,
                    GuestAddr sent = 0) {
        return send(id, flags, response(status, reason, body), sent);
    }
    /// Receive one request sent by `c` (synchronously).
    u32 recv_one(Client& c, const std::string& request) {
        c.start(request);
        return receive();
    }

    GuestMemory mem_;
    cpu::CpuState cpu_;
    HttpApiManager mgr_;
    std::unique_ptr<HandleTable> handles_;
    std::unique_ptr<SyncManager> sync_;
    DllExports exp_;
    std::mutex completions_mu_;
    std::vector<httpapi::Completion> completions_;
    std::set<Handle> ported_;
};

INSTANTIATE_TEST_SUITE_P(Bitness, HttpApiTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// =============================================================================
// Export table and layouts
// =============================================================================

TEST_P(HttpApiTest, ExportTableMatchesSdkImportLibrary) {
    // httpapi.lib (x86 decoration @N) and the httpapi.dll export table.
    ASSERT_EQ(exp_.entries.size(), 49u);
    std::map<std::string, std::pair<u32, u32>> want = {
        {"HttpAddFragmentToCache", {1, 20}}, {"HttpAddUrlToUrlGroup", {3, 24}},
        {"HttpCreateRequestQueue", {10, 20}}, {"HttpCreateServerSession", {12, 12}},
        {"HttpCreateUrlGroup", {13, 16}}, {"HttpInitialize", {23, 12}},
        {"HttpReceiveHttpRequest", {33, 32}}, {"HttpRemoveUrlFromUrlGroup", {36, 16}},
        {"HttpSendHttpResponse", {37, 44}}, {"HttpSetUrlGroupProperty", {43, 20}},
        {"HttpShutdownRequestQueue", {44, 4}}, {"HttpTerminate", {45, 8}},
        {"HttpWaitForDisconnectEx", {49, 20}},
    };
    u32 ordinal = 0;
    for (const auto& e : exp_.entries) {
        EXPECT_EQ(e.ordinal, ++ordinal) << e.name;   // ordinal base 1, dense
        if (auto it = want.find(e.name); it != want.end()) {
            EXPECT_EQ(e.ordinal, it->second.first) << e.name;
            EXPECT_EQ(e.stdcall_bytes, it->second.second) << e.name;
            want.erase(it);
        }
    }
    EXPECT_TRUE(want.empty());
}

TEST_P(HttpApiTest, LayoutsMatchSdkHeader) {
    const Oracle& o = oracle();
    const auto& l = L();
    EXPECT_EQ(o.field("sizes", "HTTP_REQUEST_V1"), l.request_v1_size);
    EXPECT_EQ(o.field("sizes", "HTTP_REQUEST_V2"), l.request_v2_size);
    EXPECT_EQ(o.field("sizes", "HTTP_RESPONSE_V1"), l.response_v1_size);
    EXPECT_EQ(o.field("sizes", "HTTP_RESPONSE_V2"), l.response_v2_size);
    EXPECT_EQ(o.field("sizes", "HTTP_KNOWN_HEADER"), l.known_header_size);
    EXPECT_EQ(o.field("sizes", "HTTP_UNKNOWN_HEADER"), l.unknown_header_size);
    EXPECT_EQ(o.field("sizes", "HTTP_DATA_CHUNK"), l.data_chunk_size);
    EXPECT_EQ(o.field("sizes", "HTTP_RESPONSE_INFO"), l.response_info_size);
    EXPECT_EQ(o.field("sizes", "HTTP_MULTIPLE_KNOWN_HEADERS"), l.multiple_known_size);
    EXPECT_EQ(o.field("sizes", "HTTP_BINDING_INFO"), l.binding_size);
    EXPECT_EQ(o.field("sizes", "HTTP_STATE_INFO"), l.state_size);
    EXPECT_EQ(o.field("sizes", "HTTP_TIMEOUT_LIMIT_INFO"), l.timeout_size);
    EXPECT_EQ(o.field("sizes", "HTTP_QOS_SETTING_INFO"), l.qos_size);
    EXPECT_EQ(o.field("sizes", "HTTP_SERVER_AUTHENTICATION_INFO"), l.auth_size);
    EXPECT_EQ(o.field("sizes", "HTTP_LOGGING_INFO"), l.logging_size);
    EXPECT_EQ(o.field("sizes", "HTTP_CHANNEL_BIND_INFO"), l.channel_bind_size);
    EXPECT_EQ(o.field("sizes", "HTTP_PROTECTION_LEVEL_INFO"), l.protection_size);
    EXPECT_EQ(o.field("sizes", "HTTP_LISTEN_ENDPOINT_INFO"), l.listen_endpoint_size);
    EXPECT_EQ(o.field("sizes", "HTTP_CACHE_POLICY"), l.cache_policy_size);
    EXPECT_EQ(o.field("sizes", "HTTP_REQUEST_HEADERS"), l.hdr_known + 41 * l.known_header_size);
    EXPECT_EQ(o.field("sizes", "HTTP_RESPONSE_HEADERS"), l.hdr_known + 30 * l.known_header_size);
    EXPECT_EQ(o.field("sizes", "HTTP_COOKED_URL"), l.cooked_query + l.ptr);
    const std::pair<const char*, u32> rq[] = {
        {"Flags", l.rq_flags}, {"ConnectionId", l.rq_connection_id}, {"RequestId", l.rq_request_id},
        {"UrlContext", l.rq_url_context}, {"Version", l.rq_version}, {"Verb", l.rq_verb},
        {"UnknownVerbLength", l.rq_unknown_verb_length}, {"RawUrlLength", l.rq_raw_url_length},
        {"pUnknownVerb", l.rq_unknown_verb}, {"pRawUrl", l.rq_raw_url}, {"CookedUrl", l.rq_cooked_url},
        {"Address", l.rq_address}, {"Headers", l.rq_headers}, {"BytesReceived", l.rq_bytes_received},
        {"EntityChunkCount", l.rq_entity_chunk_count}, {"pEntityChunks", l.rq_entity_chunks},
        {"RawConnectionId", l.rq_raw_connection_id}, {"pSslInfo", l.rq_ssl_info},
        {"RequestInfoCount", l.rq_request_info_count}, {"pRequestInfo", l.rq_request_info}};
    for (const auto& [name, off] : rq) EXPECT_EQ(o.field("HTTP_REQUEST_V2", name), off) << name;
    const std::pair<const char*, u32> cooked[] = {{"pFullUrl", l.cooked_full_url}, {"pHost", l.cooked_host},
                                                  {"pAbsPath", l.cooked_abs_path}, {"pQueryString", l.cooked_query}};
    for (const auto& [name, off] : cooked) EXPECT_EQ(o.field("HTTP_COOKED_URL", name), off) << name;
    EXPECT_EQ(o.field("HTTP_REQUEST_HEADERS", "pUnknownHeaders"), l.hdr_unknown);
    EXPECT_EQ(o.field("HTTP_REQUEST_HEADERS", "TrailerCount"), l.hdr_trailer_count);
    EXPECT_EQ(o.field("HTTP_REQUEST_HEADERS", "pTrailers"), l.hdr_trailers);
    EXPECT_EQ(o.field("HTTP_REQUEST_HEADERS", "KnownHeaders"), l.hdr_known);
    const std::pair<const char*, u32> rs[] = {
        {"Version", l.rs_version}, {"StatusCode", l.rs_status}, {"ReasonLength", l.rs_reason_length},
        {"pReason", l.rs_reason}, {"Headers", l.rs_headers}, {"EntityChunkCount", l.rs_entity_chunk_count},
        {"pEntityChunks", l.rs_entity_chunks}, {"ResponseInfoCount", l.rs_response_info_count},
        {"pResponseInfo", l.rs_response_info}};
    for (const auto& [name, off] : rs) EXPECT_EQ(o.field("HTTP_RESPONSE_V2", name), off) << name;
    EXPECT_EQ(o.field("HTTP_DATA_CHUNK", "FromMemory.pBuffer"), l.chunk_buffer);
    EXPECT_EQ(o.field("HTTP_DATA_CHUNK", "FromMemory.BufferLength"), l.chunk_buffer_length);
    EXPECT_EQ(o.field("HTTP_UNKNOWN_HEADER", "pName"), l.unknown_header_name);
    EXPECT_EQ(o.field("HTTP_UNKNOWN_HEADER", "pRawValue"), l.unknown_header_value);
    EXPECT_EQ(o.field("HTTP_KNOWN_HEADER", "pRawValue"), l.known_header_value);
    EXPECT_EQ(o.field("HTTP_BINDING_INFO", "RequestQueueHandle"), l.binding_queue);
    EXPECT_EQ(o.field("HTTP_TIMEOUT_LIMIT_INFO", "IdleConnection"), 10u);
    EXPECT_EQ(o.field("HTTP_TIMEOUT_LIMIT_INFO", "HeaderWait"), 12u);
    EXPECT_EQ(o.field("HTTP_TIMEOUT_LIMIT_INFO", "MinSendRate"), 16u);
}

// =============================================================================
// HttpInitialize / HttpTerminate / sessions / groups / queues
// =============================================================================

TEST_P(HttpApiTest, InitializeArgumentsAndPreInitErrors) {
    const Oracle& o = oracle();
    // Before any HttpInitialize.
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("pre-init session", "rc"));
    put_ptr(kData, 0x3333);
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0).p(kData)), o.field("pre-init queue", "rc"));
    EXPECT_EQ(get_ptr(kData), 0u);   // "handle=null"
    EXPECT_EQ(call("HttpTerminate", args().p(1).p(0)), o.field("pre-init terminate", "rc"));

    EXPECT_EQ(call("HttpInitialize", args().p(0x00000003).p(1).p(0)), o.field("init v3", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(0x00010002).p(1).p(0)), o.field("init v2.1", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(0).p(1).p(0)), o.field("init v0", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(kV2).p(8).p(0)), o.field("init flags8", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(kData)), o.field("init reserved", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(kV2).p(0).p(0)), o.field("init flags0", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(kV2).p(5).p(0)), o.field("init flags-cbt", "rc"));
    EXPECT_EQ(call("HttpInitialize", args().p(kV1).p(1).p(0)), o.field("init v1 server", "rc"));
    EXPECT_EQ(call("HttpTerminate", args().p(8).p(0)), o.field("terminate flags8", "rc"));
    EXPECT_EQ(call("HttpTerminate", args().p(2).p(kData)), o.field("terminate reserved", "rc"));
    EXPECT_EQ(call("HttpTerminate", args().p(0).p(0)), o.field("terminate flags0", "rc"));
}

TEST_P(HttpApiTest, InitializeIsCountedPerFlag) {
    const Oracle& o = oracle();
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(0)), 0u);
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(0)), 0u);
    ASSERT_EQ(call("HttpTerminate", args().p(1).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("fresh init2-term1 session", "rc"));
    EXPECT_EQ(call("HttpCloseServerSession", args().id(mem_.read_u64(kData).value())), 0u);
    ASSERT_EQ(call("HttpTerminate", args().p(1).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("fresh init2-term2 session", "rc"));
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0).p(kData)), o.field("fresh init2-term2 queue", "rc"));
    EXPECT_EQ(call("HttpTerminate", args().p(1).p(0)), o.field("fresh terminate-extra", "rc"));
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(2).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("fresh config-only session", "rc"));
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0).p(kData)), o.field("fresh config-only queue", "rc"));
    ASSERT_EQ(call("HttpTerminate", args().p(2).p(0)), 0u);
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(3).p(0)), 0u);
    ASSERT_EQ(call("HttpTerminate", args().p(2).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("fresh both-then-term-config session", "rc"));
    ASSERT_EQ(call("HttpTerminate", args().p(1).p(0)), 0u);
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(0).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("fresh flags0 session", "rc"));
    // The last HttpTerminate ends the sessions ("fresh session-after-terminate").
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)),
              o.field("fresh session-after-terminate", "create"));
    const u64 sid = mem_.read_u64(kData).value();
    ASSERT_EQ(call("HttpTerminate", args().p(1).p(0)), 0u);
    EXPECT_EQ(call("HttpCloseServerSession", args().id(sid)), o.field("fresh session-after-terminate", "close"));
}

TEST_P(HttpApiTest, SessionAndGroupErrors) {
    const Oracle& o = oracle();
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(0)), 0u);
    (void)mem_.write_u64(kData, 0x1111);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(0x00000003).p(kData).p(0)), o.field("session v3", "rc"));
    EXPECT_EQ(mem_.read_u64(kData).value(), 0u);   // "sid=zero"
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(0).p(0)), o.field("session null-id", "rc"));
    (void)mem_.write_u64(kData, 0x1111);
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(1)), o.field("session reserved", "rc"));
    EXPECT_EQ(mem_.read_u64(kData).value(), 0x1111u);   // "sid=untouched"
    EXPECT_EQ(call("HttpCloseServerSession", args().id(0)), o.field("close-session 0", "rc"));
    EXPECT_EQ(call("HttpCloseServerSession", args().id(0x1234567812345678ull)), o.field("close-session bogus", "rc"));
    EXPECT_EQ(call("HttpCreateServerSession", args().p(kV1).p(kData).p(0)), o.field("session v1", "rc"));
    EXPECT_EQ(call("HttpCloseServerSession", args().id(mem_.read_u64(kData).value())), 0u);

    ASSERT_EQ(call("HttpCreateServerSession", args().p(kV2).p(kData).p(0)), o.field("session ok", "rc"));
    const u64 sid = mem_.read_u64(kData).value();
    EXPECT_NE(sid, 0u);
    (void)mem_.write_u64(kData + 8, 0x2222);
    EXPECT_EQ(call("HttpCreateUrlGroup", args().id(0x1234567812345678ull).p(kData + 8).p(0)),
              o.field("group bogus-session", "rc"));
    EXPECT_EQ(mem_.read_u64(kData + 8).value(), 0x2222u);
    EXPECT_EQ(call("HttpCreateUrlGroup", args().id(0).p(kData + 8).p(0)), o.field("group zero-session", "rc"));
    EXPECT_EQ(call("HttpCreateUrlGroup", args().id(sid).p(0).p(0)), o.field("group null-out", "rc"));
    EXPECT_EQ(call("HttpCreateUrlGroup", args().id(sid).p(kData + 8).p(1)), o.field("group reserved", "rc"));
    EXPECT_EQ(mem_.read_u64(kData + 8).value(), 0x2222u);
    ASSERT_EQ(call("HttpCreateUrlGroup", args().id(sid).p(kData + 8).p(0)), o.field("group ok", "rc"));
    const u64 gid = mem_.read_u64(kData + 8).value();
    EXPECT_NE(gid, sid);
    EXPECT_EQ(call("HttpCloseUrlGroup", args().id(0x1234567812345678ull)), o.field("close-group bogus", "rc"));
    EXPECT_EQ(call("HttpCloseUrlGroup", args().id(0)), o.field("close-group 0", "rc"));
    EXPECT_EQ(call("HttpCloseServerSession", args().id(gid)), o.field("close-group as-session", "rc"));
    EXPECT_EQ(call("HttpCloseUrlGroup", args().id(sid)), o.field("close-session as-group", "rc"));
    EXPECT_EQ(call("HttpCloseUrlGroup", args().id(gid)), o.field("close-group ok", "rc"));
    EXPECT_EQ(call("HttpCloseUrlGroup", args().id(gid)), o.field("close-group ok", "again"));
    ASSERT_EQ(call("HttpCreateUrlGroup", args().id(sid).p(kData + 8).p(0)), 0u);
    const u64 gid2 = mem_.read_u64(kData + 8).value();
    EXPECT_EQ(call("HttpCloseServerSession", args().id(sid)), o.field("close-session with-group", "rc"));
    EXPECT_EQ(call("HttpCloseUrlGroup", args().id(gid2)), o.field("close-session with-group", "then-group-close"));
    EXPECT_EQ(call("HttpCloseServerSession", args().id(sid)), o.field("close-session again", "rc"));
}

TEST_P(HttpApiTest, RequestQueueCreationAndClose) {
    const Oracle& o = oracle();
    ASSERT_EQ(call("HttpInitialize", args().p(kV2).p(1).p(0)), 0u);
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV1).p(0).p(0).p(0).p(kData)), o.field("queue v1", "rc"));
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(get_ptr(kData))), 0u);
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0).p(0)), o.field("queue null-out", "rc"));
    put_ptr(kData, 0x3333);
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0x10).p(kData)), o.field("queue flags0x10", "rc"));
    EXPECT_EQ(get_ptr(kData), 0u);   // "handle=null"
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(1).p(kData)), o.field("queue open-existing unnamed", "rc"));
    const GuestAddr missing = wstr(kData + 0x100, u"vprobe_missing_queue");
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(missing).p(0).p(1).p(kData)),
              o.field("queue open-existing missing", "rc"));
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(2).p(kData)), o.field("queue controller unnamed", "rc"));
    const GuestAddr named = wstr(kData + 0x200, u"vprobe_named_queue");
    ASSERT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(named).p(0).p(0).p(kData)), o.field("queue named", "rc"));
    const u64 nq = get_ptr(kData);
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(named).p(0).p(0).p(kData + 8)), o.field("queue named dup", "rc"));
    // OPEN_EXISTING of the name gives a second handle to the same queue.
    EXPECT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(named).p(0).p(1).p(kData + 8)), 0u);
    EXPECT_NE(get_ptr(kData + 8), nq);
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(get_ptr(kData + 8))), 0u);
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(nq)), 0u);

    ASSERT_EQ(call("HttpCreateRequestQueue", args().p(kV2).p(0).p(0).p(0).p(kData)), o.field("queue ok", "rc"));
    const Handle q = static_cast<Handle>(get_ptr(kData));
    EXPECT_EQ(handles_->get_type(q), HandleType::Device);
    EXPECT_EQ(handles_->get_flags(q), o.field("queue handle-info", "flags"));
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(q)), o.field("close-queue ok", "rc"));
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(q)), o.field("close-queue ok", "again"));
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(0)), o.field("close-queue null", "rc"));
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(is64() ? ~0ull : 0xFFFFFFFFu)), o.field("close-queue invalid", "rc"));
    const Handle ev = sync_->create_event(true, false, "");
    EXPECT_EQ(call("HttpShutdownRequestQueue", args().p(ev)), o.field("shutdown-queue event", "rc"));
    EXPECT_EQ(call("HttpCloseRequestQueue", args().p(ev)), o.field("close-queue event", "rc"));
    EXPECT_FALSE(handles_->is_valid(ev));   // it closed the event
    EXPECT_EQ(call("HttpShutdownRequestQueue", args().p(0)), o.field("shutdown-queue null", "rc"));
}

// =============================================================================
// Properties and URL registration
// =============================================================================

TEST_P(HttpApiTest, UrlGroupProperties) {
    const Oracle& o = oracle();
    open_server(false);
    const GuestAddr b = kData + 0x40;
    (void)mem_.write_u32(b, 1);
    put_ptr(b + L().binding_queue, q_);
    auto set = [&](u64 group, u32 prop, GuestAddr info, u32 len) {
        return call("HttpSetUrlGroupProperty", args().id(group).p(prop).p(info).p(len));
    };
    EXPECT_EQ(set(gid_, 7, b, L().binding_size), o.field("bind ok", "rc"));
    EXPECT_EQ(set(gid_, 7, b, L().binding_size - 1), o.field("bind short", "rc"));
    EXPECT_EQ(set(gid_, 7, b, L().binding_size + 4), o.field("bind long", "rc"));
    EXPECT_EQ(set(gid_, 7, 0, L().binding_size), o.field("bind null", "rc"));
    EXPECT_EQ(set(0x1234567812345678ull, 7, b, L().binding_size), o.field("bind bogus-group", "rc"));
    EXPECT_EQ(set(sid_, 7, b, L().binding_size), o.field("bind on-session", "rc"));
    EXPECT_EQ(call("HttpSetServerSessionProperty", args().id(sid_).p(7).p(b).p(L().binding_size)),
              o.field("session bind", "rc"));
    put_ptr(b + L().binding_queue, 0x7ffc);
    EXPECT_EQ(set(gid_, 7, b, L().binding_size), o.field("bind bogus-handle", "rc"));
    put_ptr(b + L().binding_queue, 0);
    EXPECT_EQ(set(gid_, 7, b, L().binding_size), o.field("bind null-handle", "rc"));
    put_ptr(b + L().binding_queue, sync_->create_event(true, false, ""));
    EXPECT_EQ(set(gid_, 7, b, L().binding_size), o.field("bind event-handle", "rc"));
    (void)mem_.write_u32(b, 0);
    EXPECT_EQ(set(gid_, 7, b, L().binding_size), o.field("unbind", "rc"));
    EXPECT_EQ(call("HttpQueryUrlGroupProperty", args().id(gid_).p(7).p(kData + 0x400).p(64).p(kData + 0x20)),
              o.field("query bind", "rc"));

    const GuestAddr st = kData + 0x80;
    (void)mem_.write_u32(st, 1);
    (void)mem_.write_u32(st + 4, 1);
    EXPECT_EQ(set(gid_, 5, st, 8), o.field("state inactive", "rc"));
    (void)mem_.write_u32(st + 4, 2);
    EXPECT_EQ(set(gid_, 5, st, 8), o.field("state 2", "rc"));
    (void)mem_.write_u32(st, 0);
    (void)mem_.write_u32(st + 4, 0);
    EXPECT_EQ(set(gid_, 5, st, 8), o.field("state not-present", "rc"));
    (void)mem_.write_u32(st, 1);
    EXPECT_EQ(set(gid_, 5, st, 8), o.field("state active", "rc"));
    EXPECT_EQ(call("HttpQueryUrlGroupProperty", args().id(gid_).p(5).p(kData + 0x400).p(64).p(kData + 0x20)),
              o.field("query state", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), o.field("query state", "ret"));
    EXPECT_EQ(mem_.read_u32(kData + 0x400).value() & 1, o.field("query state", "present"));
    EXPECT_EQ(mem_.read_u32(kData + 0x404).value(), o.field("query state", "state"));

    const GuestAddr t = kData + 0xC0;
    std::vector<u8> zero(20, 0);
    (void)mem_.write_bytes(t, zero.data(), zero.size());
    (void)mem_.write_u32(t, 1);
    (void)mem_.write_u16(t + 10, 30);
    (void)mem_.write_u16(t + 12, 20);
    EXPECT_EQ(set(gid_, 3, t, 20), o.field("timeouts", "rc"));
    (void)mem_.write_u16(t + 8, 5);
    EXPECT_EQ(set(gid_, 3, t, 20), o.field("timeouts requestqueue-on-group", "rc"));
    EXPECT_EQ(call("HttpQueryUrlGroupProperty", args().id(gid_).p(3).p(kData + 0x400).p(64).p(kData + 0x20)),
              o.field("query timeouts", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), o.field("query timeouts", "ret"));
    EXPECT_EQ(mem_.read_u16(kData + 0x40A).value(), o.field("query timeouts", "idle"));
    EXPECT_EQ(mem_.read_u16(kData + 0x40C).value(), o.field("query timeouts", "header"));

    (void)mem_.write_u32(kData + 0x100, 100);
    EXPECT_EQ(set(gid_, 4, kData + 0x100, 4), o.field("queuelength on-group", "rc"));
    EXPECT_EQ(call("HttpSetRequestQueueProperty", args().p(q_).p(4).p(kData + 0x100).p(4).p(0).p(0)),
              o.field("queuelength on-queue", "rc"));
    EXPECT_EQ(set(gid_, 6, kData + 0x100, 4), o.field("property 6", "rc"));
    EXPECT_EQ(set(gid_, 99, kData + 0x100, 4), o.field("property 99", "rc"));
    std::vector<u8> big(L().logging_size + L().auth_size + L().qos_size, 0);
    (void)mem_.write_bytes(kData + 0x500, big.data(), big.size());
    EXPECT_EQ(set(gid_, 2, kData + 0x500, L().qos_size), o.field("qos zero", "rc"));
    EXPECT_EQ(set(gid_, 0, kData + 0x500, L().auth_size), o.field("auth not-present", "rc"));
    (void)mem_.write_u32(kData + 0x500, 1);
    EXPECT_EQ(set(gid_, 0, kData + 0x500, L().auth_size), o.field("auth present-none", "rc"));
    (void)mem_.write_u32(kData + 0x500, 0);
    EXPECT_EQ(set(gid_, 1, kData + 0x500, L().logging_size), o.field("logging not-present", "rc"));
}

TEST_P(HttpApiTest, UrlRegistration) {
    const Oracle& o = oracle();
    open_server(false);
    auto add = [&](const std::u16string& url, u64 group, u32 reserved = 0) {
        return call("HttpAddUrlToUrlGroup", args().id(group).p(wstr(kData + 0x800, url)).id(7).p(reserved));
    };
    auto remove = [&](const char16_t* url, u32 flags, u64 group = 0) {
        return call("HttpRemoveUrlFromUrlGroup",
                    args().id(group ? group : gid_).p(url ? wstr(kData + 0x800, url) : 0).p(flags));
    };
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/", gid_), o.field("addurl ok", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/", gid_), o.field("addurl dup", "rc"));
    EXPECT_EQ(add(u"HTTP://LOCALHOST:48213/VPROBE/", gid_), o.field("addurl dup-case", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/sub/", gid_), o.field("addurl nested", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe2", gid_), o.field("addurl no-slash", "rc"));
    EXPECT_EQ(add(u"http://localhost/vprobe3/", gid_), o.field("addurl no-port", "rc"));
    EXPECT_EQ(add(u"http://localhost:0/vprobe3/", gid_), o.field("addurl port0", "rc"));
    EXPECT_EQ(add(u"http://localhost:70000/vprobe3/", gid_), o.field("addurl port70000", "rc"));
    EXPECT_EQ(add(u"ftp://localhost:48213/vprobe3/", gid_), o.field("addurl ftp", "rc"));
    EXPECT_EQ(add(u"not a url", gid_), o.field("addurl garbage", "rc"));
    EXPECT_EQ(add(u"", gid_), o.field("addurl empty", "rc"));
    EXPECT_EQ(call("HttpAddUrlToUrlGroup", args().id(gid_).p(0).id(0).p(0)), o.field("addurl null", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe4/", gid_, 1), o.field("addurl reserved", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe4/", 0x1234567812345678ull), o.field("addurl bogus-group", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe5/?x=1", gid_), o.field("addurl query", "rc"));
    EXPECT_EQ(add(u"http://+:48213/vprobe6/", gid_), o.field("addurl strong", "rc"));
    EXPECT_EQ(add(u"http://*:48213/vprobe7/", gid_), o.field("addurl weak", "rc"));
    EXPECT_EQ(add(u"http://127.0.0.1:48213/vprobe8/", gid_), o.field("addurl ip", "rc"));

    ASSERT_EQ(call("HttpCreateUrlGroup", args().id(sid_).p(kData + 8).p(0)), 0u);
    const u64 other = mem_.read_u64(kData + 8).value();
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/", other), o.field("addurl other-group same", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/other/", other), o.field("addurl other-group child", "rc"));

    EXPECT_EQ(remove(u"http://localhost:48213/nothere/", 0), o.field("removeurl missing", "rc"));
    EXPECT_EQ(remove(u"http://localhost:48213/vprobe2", 0), o.field("removeurl no-slash", "rc"));
    EXPECT_EQ(remove(u"http://localhost:48213/vprobe/", 2), o.field("removeurl flags2", "rc"));
    EXPECT_EQ(remove(nullptr, 0), o.field("removeurl null-noflag", "rc"));
    EXPECT_EQ(remove(u"http://localhost:48213/vprobe/", 1), o.field("removeurl all-with-url", "rc"));
    EXPECT_EQ(remove(u"http://localhost:48213/vprobe/", 0, 0x1234567812345678ull), o.field("removeurl bogus-group", "rc"));
    EXPECT_EQ(remove(u"http://localhost:48213/vprobe/sub/", 0), o.field("removeurl ok", "rc"));
    EXPECT_EQ(remove(u"http://localhost:48213/vprobe/sub/", 0), o.field("removeurl ok", "again"));
    EXPECT_EQ(remove(u"HTTP://LOCALHOST:48213/VPROBE/", 0), o.field("removeurl case", "rc"));
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/", gid_), o.field("removeurl all", "re-add"));
    EXPECT_EQ(remove(nullptr, 1), o.field("removeurl all", "rc"));
    EXPECT_EQ(remove(nullptr, 1), o.field("removeurl all", "again"));
    // Closing a group frees its URLs ("other-group before/after close").
    ASSERT_EQ(add(u"http://localhost:48213/vprobe/", gid_), 0u);
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/", other), o.field("other-group before close", "rc"));
    ASSERT_EQ(call("HttpCloseUrlGroup", args().id(gid_)), 0u);
    EXPECT_EQ(add(u"http://localhost:48213/vprobe/", other), o.field("other-group after close", "rc"));
}

// =============================================================================
// Receiving requests
// =============================================================================

TEST_P(HttpApiTest, ReceiveParsesTheRequest) {
    const Oracle& o = oracle();
    open_server(true, 7);
    ASSERT_EQ(call("HttpAddUrlToUrlGroup",
                   args().id(gid_).p(wstr(kData + 0x100, u"http://localhost:48213/vprobe/deep/")).id(8).p(0)),
              0u);
    Client c;
    c.start("GET /vprobe/a%20b/c?x=1&y=%41 HTTP/1.1\r\nHost: localhost:48213\r\n"
            "User-Agent: vprobe\r\nAccept: */*\r\nX-Custom:   padded value  \r\nx-lower: v\r\n"
            "Cookie: a=1\r\nCookie: b=2\r\n\r\n");
    ASSERT_EQ(receive(), o.field("recv get", "rc"));
    const auto& l = L();
    EXPECT_EQ(mem_.read_u32(kBuf + l.rq_flags).value(), o.field("recv get", "flags", 1));
    EXPECT_NE(mem_.read_u64(kBuf + l.rq_connection_id).value(), 0u);
    EXPECT_NE(request_id(), 0u);
    EXPECT_EQ(mem_.read_u64(kBuf + l.rq_url_context).value(), o.field("recv get", "ctx", 1));
    EXPECT_EQ(mem_.read_u32(kBuf + l.rq_verb).value(), o.field("recv get", "verb", 1));
    EXPECT_EQ(mem_.read_u16(kBuf + l.rq_version).value(), 1u);
    EXPECT_EQ(mem_.read_u16(kBuf + l.rq_version + 2).value(), 1u);
    const GuestAddr raw = static_cast<GuestAddr>(get_ptr(kBuf + l.rq_raw_url));
    EXPECT_EQ(read_a(raw, mem_.read_u16(kBuf + l.rq_raw_url_length).value()), o.str("recv get", "raw", 1));
    EXPECT_EQ(mem_.read_u8(raw + mem_.read_u16(kBuf + l.rq_raw_url_length).value()).value(), 0u);   // raw_nul=1
    const GuestAddr cooked = kBuf + l.rq_cooked_url;
    const GuestAddr full = static_cast<GuestAddr>(get_ptr(cooked + l.cooked_full_url));
    EXPECT_EQ(read_w(full, mem_.read_u16(cooked).value()), o.str("recv get", "full", 1));
    EXPECT_EQ(read_w(static_cast<GuestAddr>(get_ptr(cooked + l.cooked_host)), mem_.read_u16(cooked + 2).value()),
              o.str("recv get", "host", 1));
    EXPECT_EQ(read_w(static_cast<GuestAddr>(get_ptr(cooked + l.cooked_abs_path)), mem_.read_u16(cooked + 4).value()),
              o.str("recv get", "abs", 1));
    EXPECT_EQ(read_w(static_cast<GuestAddr>(get_ptr(cooked + l.cooked_query)), mem_.read_u16(cooked + 6).value()),
              o.str("recv get", "query", 1));
    EXPECT_EQ(get_ptr(cooked + l.cooked_host), full + 14);   // host_in_full=1
    // Address: SOCKADDR_IN, family 2, 127.0.0.1, local port 48213.
    const GuestAddr remote = static_cast<GuestAddr>(get_ptr(kBuf + l.rq_address));
    const GuestAddr local = static_cast<GuestAddr>(get_ptr(kBuf + l.rq_address + l.ptr));
    EXPECT_EQ(mem_.read_u16(remote).value(), o.field("recv get", "remote_family", 1));
    EXPECT_EQ(ntohl(mem_.read_u32(remote + 4).value()), o.field("recv get", "remote_ip", 1));
    EXPECT_EQ(mem_.read_u16(local).value(), o.field("recv get", "local_family", 1));
    EXPECT_EQ(ntohs(mem_.read_u16(local + 2).value()), o.field("recv get", "local_port", 1));
    // Known headers: [20:"*/*",25:"a=1, b=2",28:"localhost:48213",40:"vprobe"]
    const std::string want_known = o.get("recv get", 1);
    std::string known = "known=[";
    for (u32 id = 0; id < 41; ++id) {
        const GuestAddr slot = kBuf + l.rq_headers + l.hdr_known + id * l.known_header_size;
        const u16 len = mem_.read_u16(slot).value();
        const GuestAddr p = static_cast<GuestAddr>(get_ptr(slot + l.known_header_value));
        if (!len && !p) continue;
        known += std::to_string(id) + ":\"" + read_a(p, len) + "\"(nul=" +
                 (mem_.read_u8(p + len).value() == 0 ? "1" : "0") + "),";
    }
    known += "]";
    EXPECT_NE(want_known.find(known), std::string::npos) << known;
    std::string unknown = "unknown=[";
    const u16 n = mem_.read_u16(kBuf + l.rq_headers + l.hdr_unknown_count).value();
    const GuestAddr arr = static_cast<GuestAddr>(get_ptr(kBuf + l.rq_headers + l.hdr_unknown));
    for (u16 i = 0; i < n; ++i) {
        const GuestAddr e = arr + i * l.unknown_header_size;
        unknown += "\"" + read_a(static_cast<GuestAddr>(get_ptr(e + l.unknown_header_name)), mem_.read_u16(e).value()) +
                   "\":\"" +
                   read_a(static_cast<GuestAddr>(get_ptr(e + l.unknown_header_value)), mem_.read_u16(e + 2).value()) +
                   "\",";
    }
    unknown += "]";
    EXPECT_NE(want_known.find(unknown), std::string::npos) << unknown;
    EXPECT_EQ(mem_.read_u64(kBuf + l.rq_bytes_received).value(), o.field("recv get", "bytes_received", 1));
    EXPECT_EQ(mem_.read_u16(kBuf + l.rq_entity_chunk_count).value(), o.field("recv get", "chunks", 1));
    EXPECT_NE(mem_.read_u64(kBuf + l.rq_raw_connection_id).value(), 0u);   // raw_conn_nonzero=1
    EXPECT_EQ(get_ptr(kBuf + l.rq_ssl_info), 0u);                           // ssl=0
    // The variable data lies inside the buffer, after the fixed part.
    EXPECT_GE(raw, kBuf + l.request_v2_size);
    EXPECT_LT(raw, kBuf + kBufSize);
    EXPECT_GT(mem_.read_u32(kData + 0x20).value(), l.request_v2_size);   // BytesReturned

    const std::string hello = "hello";
    ASSERT_EQ(send_simple(request_id(), 200, "OK", &hello, 0, kData + 0x24), o.field("send basic", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x24).value(), o.field("send basic", "sent"));
    EXPECT_EQ(send_simple(request_id(), 200, "OK", &hello), o.field("send again", "rc"));
    c.join();
    EXPECT_EQ(c.masked(), o.str("send basic", "wire", 1));

    // Longest-prefix routing chooses the URL context ("recv deep: ctx=8").
    Client d;
    ASSERT_EQ(recv_one(d, "GET /vprobe/deep/x HTTP/1.1\r\nHost: localhost:48213\r\n\r\n"), o.field("recv deep", "rc"));
    EXPECT_EQ(mem_.read_u64(kBuf + l.rq_url_context).value(), o.field("recv deep", "ctx"));
    ASSERT_EQ(send_simple(request_id(), 204, "No Content", nullptr), 0u);
    d.join();
    EXPECT_EQ(d.masked(), o.str("send 204", "wire"));
}

TEST_P(HttpApiTest, ReceiveBufferSizes) {
    const Oracle& o = oracle();
    open_server();
    Client c;
    c.start("GET /vprobe/small HTTP/1.1\r\nHost: localhost:48213\r\nUser-Agent: vprobe\r\n\r\n");
    std::vector<u8> fill(kBufSize, 0xCC);
    (void)mem_.write_bytes(kBuf, fill.data(), fill.size());
    ASSERT_EQ(receive(0, 0, L().request_v2_size), o.field("recv small", "rc"));
    const u32 needed = mem_.read_u32(kData + 0x20).value();
    EXPECT_GT(needed, L().request_v2_size);                                  // needed_gt_fixed=1
    EXPECT_EQ(mem_.read_u32(kBuf + L().rq_flags).value(), o.field("recv small", "flags"));
    EXPECT_EQ(mem_.read_u32(kBuf + L().rq_verb).value(), o.field("recv small", "verb"));
    EXPECT_EQ(get_ptr(kBuf + L().rq_raw_url), 0u);                          // raw_ptr_null=1
    const u64 id = request_id();
    EXPECT_NE(id, 0u);
    EXPECT_EQ(receive(id, 0, needed - 1), o.field("recv retry-short", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), needed);                  // same=1
    EXPECT_EQ(receive(id, 0, needed), o.field("recv retry", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), needed);
    EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(kBuf + L().rq_raw_url)), 13), "/vprobe/small");
    // A delivered request asked for again is reset.
    EXPECT_EQ(receive(id), o.field("recv retry-after-success", "rc"));
    const std::string empty;
    EXPECT_EQ(send_simple(id, 200, "OK", &empty), o.field("recv retry-after-success", "send"));
    c.join();
    EXPECT_EQ(c.response, "");                 // "send empty-body: len=0 ... err=10054"
    EXPECT_EQ(c.error, ECONNRESET);

    Client t;
    t.start("GET /vprobe/tiny HTTP/1.1\r\nHost: localhost:48213\r\n\r\n");
    (void)mem_.write_u32(kData + 0x20, 0xDEAD);
    EXPECT_EQ(receive(0, 0, L().request_v1_size), o.field("recv v1-size", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), 0xDEADu);                  // got_gt=0 (untouched)
    EXPECT_EQ(receive(0, 0, 16), o.field("recv 16-bytes", "rc"));
    EXPECT_EQ(call("HttpReceiveHttpRequest", args().p(q_).id(0).p(0).p(0).p(kBufSize).p(kData + 0x20).p(0)),
              o.field("recv null-buffer", "rc"));
    EXPECT_EQ(receive(0, 4), o.field("recv flags4", "rc"));
    EXPECT_EQ(receive(0x1234567812345678ull), o.field("recv bogus-id", "rc"));
    ASSERT_EQ(receive(), o.field("recv after-small", "rc"));
    const std::string tb = "t";
    ASSERT_EQ(send_simple(request_id(), 200, "OK", &tb, 1), 0u);   // DISCONNECT
    t.join();
    EXPECT_EQ(t.masked(), o.str("send disconnect", "wire"));
    EXPECT_EQ(t.server_closed, o.field("send disconnect", "closed") == 1);
}

TEST_P(HttpApiTest, EntityBodies) {
    const Oracle& o = oracle();
    open_server(true, 7);
    const auto& l = L();
    {
        Client c;
        c.start("POST /vprobe/post HTTP/1.1\r\nHost: localhost:48213\r\nContent-Length: 11\r\n"
                "Content-Type: text/plain\r\n\r\nhello world");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_EQ(receive(), 0u);
        EXPECT_EQ(mem_.read_u32(kBuf + l.rq_flags).value(), o.field("recv post", "flags"));
        EXPECT_EQ(mem_.read_u32(kBuf + l.rq_verb).value(), o.field("recv post", "verb"));
        EXPECT_EQ(mem_.read_u64(kBuf + l.rq_bytes_received).value(), o.field("recv post", "bytes_received"));
        EXPECT_EQ(mem_.read_u16(kBuf + l.rq_entity_chunk_count).value(), o.field("recv post", "chunks"));
        const u64 id = request_id();
        auto entity = [&](u32 len) {
            return call("HttpReceiveRequestEntityBody",
                        args().p(q_).id(id).p(0).p(kData + 0x5000).p(len).p(kData + 0x28).p(0));
        };
        EXPECT_EQ(entity(5), o.field("entity first5", "rc"));
        EXPECT_EQ(mem_.read_u32(kData + 0x28).value(), o.field("entity first5", "n"));
        EXPECT_EQ(read_a(kData + 0x5000, 5), "hello");
        EXPECT_EQ(entity(64), o.field("entity rest", "rc"));
        EXPECT_EQ(mem_.read_u32(kData + 0x28).value(), o.field("entity rest", "n"));
        EXPECT_EQ(read_a(kData + 0x5000, 6), " world");
        EXPECT_EQ(entity(64), o.field("entity eof", "rc"));
        const std::string ok = "ok";
        ASSERT_EQ(send_simple(id, 201, "Created", &ok), 0u);
        c.join();
        EXPECT_EQ(c.masked(), o.str("send 201", "wire"));
    }
    {
        Client c;
        c.start("POST /vprobe/post HTTP/1.1\r\nHost: localhost:48213\r\nContent-Length: 11\r\n\r\nhello world");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_EQ(receive(0, 1), 0u);   // COPY_BODY
        EXPECT_EQ(mem_.read_u32(kBuf + l.rq_flags).value(), o.field("recv post copy", "flags"));
        EXPECT_EQ(mem_.read_u64(kBuf + l.rq_bytes_received).value(), o.field("recv post copy", "bytes_received"));
        ASSERT_EQ(mem_.read_u16(kBuf + l.rq_entity_chunk_count).value(), o.field("recv post copy", "chunks"));
        const GuestAddr chunk = static_cast<GuestAddr>(get_ptr(kBuf + l.rq_entity_chunks));
        EXPECT_EQ(mem_.read_u32(chunk).value(), 0u);   // HttpDataChunkFromMemory
        EXPECT_EQ(mem_.read_u32(chunk + l.chunk_buffer_length).value(), 11u);
        EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(chunk + l.chunk_buffer)), 11), "hello world");
        EXPECT_EQ(call("HttpReceiveRequestEntityBody",
                       args().p(q_).id(request_id()).p(0).p(kData + 0x5000).p(16).p(kData + 0x28).p(0)),
                  o.field("entity after-copy", "rc"));
        ASSERT_EQ(send_simple(request_id(), 200, "OK", nullptr), 0u);
        c.join();
        EXPECT_EQ(c.masked(), o.str("send no-body", "wire"));
    }
    {
        Client c;
        c.start("POST /vprobe/chunked HTTP/1.1\r\nHost: localhost:48213\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ASSERT_EQ(receive(0, 1), 0u);
        EXPECT_EQ(mem_.read_u32(kBuf + l.rq_flags).value(), o.field("recv chunked", "flags"));
        EXPECT_EQ(mem_.read_u64(kBuf + l.rq_bytes_received).value(), o.field("recv chunked", "bytes_received"));
        const GuestAddr chunk = static_cast<GuestAddr>(get_ptr(kBuf + l.rq_entity_chunks));
        EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(chunk + l.chunk_buffer)), 11), "hello world");
        EXPECT_EQ(call("HttpReceiveRequestEntityBody",
                       args().p(q_).id(request_id()).p(0).p(kData + 0x5000).p(32).p(kData + 0x28).p(0)),
                  o.field("entity chunked-rest", "rc"));
        const std::string cb = "c";
        ASSERT_EQ(send_simple(request_id(), 200, "OK", &cb), 0u);
        c.join();
        EXPECT_EQ(c.masked(), o.str("send after-chunked", "wire"));
    }
}

TEST_P(HttpApiTest, VerbsVersionsAndServerAnswers) {
    const Oracle& o = oracle();
    open_server(true, 7);
    const auto& l = L();
    auto one = [&](const std::string& req, const std::string& key, u16 status, const std::string& reason,
                   const std::string* body, const std::string& wire_key) {
        Client c;
        c.start(req);
        ASSERT_EQ(receive(), 0u) << key;
        EXPECT_EQ(mem_.read_u32(kBuf + l.rq_verb).value(), o.field(key, "verb")) << key;
        EXPECT_EQ(mem_.read_u16(kBuf + l.rq_version + 2).value(), o.get(key).find("ver=1.0") != std::string::npos ? 0u : 1u);
        EXPECT_EQ(mem_.read_u64(kBuf + l.rq_bytes_received).value(), o.field(key, "bytes_received")) << key;
        const GuestAddr cooked = kBuf + l.rq_cooked_url;
        EXPECT_EQ(read_w(static_cast<GuestAddr>(get_ptr(cooked + l.cooked_full_url)), mem_.read_u16(cooked).value()),
                  o.str(key, "full"));
        if (o.get(key).find("unknown_verb=\"") != std::string::npos) {
            EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(kBuf + l.rq_unknown_verb)),
                             mem_.read_u16(kBuf + l.rq_unknown_verb_length).value()),
                      o.str(key, "unknown_verb"));
        }
        ASSERT_EQ(send_simple(request_id(), status, reason, body), 0u);
        c.join();
        if (!wire_key.empty()) {
            EXPECT_EQ(c.masked(), o.str(wire_key, "wire")) << wire_key;
            EXPECT_EQ(c.server_closed, o.field(wire_key, "closed") == 1) << wire_key;
        }
    };
    const std::string old = "old", headbody = "headbody";
    one("GET /vprobe/old HTTP/1.0\r\nHost: localhost:48213\r\n\r\n", "recv http10 host", 200, "OK", &old,
        "send http10 host");
    one("HEAD /vprobe/head HTTP/1.1\r\nHost: localhost:48213\r\n\r\n", "recv head", 200, "OK", &headbody, "send head");
    one("BREW /vprobe/pot HTTP/1.1\r\nHost: localhost:48213\r\n\r\n", "recv brew", 418, "Teapot", nullptr, "send 418");
    one("GET http://localhost:48213/vprobe/abs?q HTTP/1.1\r\nHost: localhost:48213\r\n\r\n", "recv absolute", 200,
        "OK", nullptr, "");

    // Requests the server answers itself.
    auto status_of = [&](const std::string& req, int wait_ms = 800) {
        Client c;
        c.start(req, 0, wait_ms);
        c.join();
        return c;
    };
    // The error page body is Vitreo's own: compare all but its length.
    auto summary_prefix = [&](const std::string& key) {
        const std::string v = o.get(key);
        return std::regex_replace(v.substr(0, v.find(" body_len=")), std::regex("Content-Length=[0-9]+"),
                                  "Content-Length");
    };
    // HTTP/1.0 without Host is addressed to the IP, not "localhost"
    // ("recv http10: rc=1460", "send http10: ... 400").
    EXPECT_EQ(status_of("GET /vprobe/old HTTP/1.0\r\n\r\n").response.substr(0, 24), "HTTP/1.1 400 Bad Request");
    EXPECT_EQ(status_of("GET /vprobe/nohost HTTP/1.1\r\n\r\n").status_summary(), summary_prefix("nohost"));
    EXPECT_EQ(status_of("GET /elsewhere HTTP/1.1\r\nHost: localhost:48213\r\n\r\n").status_summary(),
              summary_prefix("unregistered"));
    EXPECT_EQ(status_of("garbage\r\n\r\n").status_summary(), summary_prefix("garbage"));

    // Keep-alive: two requests on one connection.
    {
        Client k;
        k.start("GET /vprobe/k1 HTTP/1.1\r\nHost: localhost:48213\r\n\r\n"
                "GET /vprobe/k2 HTTP/1.1\r\nHost: localhost:48213\r\n\r\n");
        ASSERT_EQ(receive(), 0u);
        const u64 c1 = mem_.read_u64(kBuf + l.rq_connection_id).value();
        EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(kBuf + l.rq_raw_url)), 10), "/vprobe/k1");
        const std::string one_s = "1", two_s = "2";
        ASSERT_EQ(send_simple(request_id(), 200, "OK", &one_s), 0u);
        ASSERT_EQ(receive(), 0u);
        EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(kBuf + l.rq_raw_url)), 10), "/vprobe/k2");
        EXPECT_EQ(mem_.read_u64(kBuf + l.rq_connection_id).value() == c1, o.field("keepalive second", "same_conn") == 1);
        ASSERT_EQ(send_simple(request_id(), 200, "OK", &two_s), 0u);
        k.join();
        EXPECT_EQ(k.masked(), o.str("keepalive", "wire"));
    }
    // An inactive group resets ("inactive long: len=0 closed=0 err=10054").
    const GuestAddr st = kData + 0x80;
    (void)mem_.write_u32(st, 1);
    (void)mem_.write_u32(st + 4, 1);
    ASSERT_EQ(call("HttpSetUrlGroupProperty", args().id(gid_).p(5).p(st).p(8)), 0u);
    {
        Client c = status_of(kGet);
        EXPECT_EQ(c.response, "");
        EXPECT_EQ(c.error, ECONNRESET);
    }
    (void)mem_.write_u32(st + 4, 0);
    ASSERT_EQ(call("HttpSetUrlGroupProperty", args().id(gid_).p(5).p(st).p(8)), 0u);
    // An unbound group answers 503.
    ASSERT_EQ(bind(0, false), 0u);
    EXPECT_EQ(status_of(kGet).status_summary(), summary_prefix("unbound"));
}

TEST_P(HttpApiTest, ResponseWireFormat) {
    const Oracle& o = oracle();
    open_server();
    const auto& l = L();
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const std::string body = "abcd";
        const GuestAddr r = response(200, "Fine", &body);
        known_header(r, 12, "text/html", kData + 0x3400);   // Content-Type
        known_header(r, 11, "8", kData + 0x3420);           // Content-Length
        known_header(r, 23, "/x", kData + 0x3440);          // Location
        known_header(r, 0, "no-cache", kData + 0x3460);     // Cache-Control
        const GuestAddr unk = kData + 0x3500;
        const std::pair<std::string, std::string> u[2] = {{"X-Probe", "one"}, {"X-Two", "2"}};
        for (int i = 0; i < 2; ++i) {
            const GuestAddr e = unk + i * l.unknown_header_size;
            (void)mem_.write_u16(e, static_cast<u16>(u[i].first.size()));
            (void)mem_.write_u16(e + 2, static_cast<u16>(u[i].second.size()));
            put_ptr(e + l.unknown_header_name, astr(kData + 0x3600 + i * 0x20, u[i].first));
            put_ptr(e + l.unknown_header_value, astr(kData + 0x3700 + i * 0x20, u[i].second));
        }
        (void)mem_.write_u16(r + l.rs_headers + l.hdr_unknown_count, 2);
        put_ptr(r + l.rs_headers + l.hdr_unknown, unk);
        // Second chunk "efgh".
        const GuestAddr chunks = kData + 0x3800;
        std::vector<u8> cz(64, 0);
        (void)mem_.write_bytes(chunks, cz.data(), cz.size());
        put_ptr(chunks + l.chunk_buffer, astr(kData + 0x3900, "abcd"));
        (void)mem_.write_u32(chunks + l.chunk_buffer_length, 4);
        put_ptr(chunks + 32 + l.chunk_buffer, astr(kData + 0x3910, "efgh"));
        (void)mem_.write_u32(chunks + 32 + l.chunk_buffer_length, 4);
        (void)mem_.write_u16(r + l.rs_entity_chunk_count, 2);
        put_ptr(r + l.rs_entity_chunks, chunks);
        ASSERT_EQ(send(request_id(), 0, r, kData + 0x24), o.field("send headers", "rc"));
        EXPECT_EQ(mem_.read_u32(kData + 0x24).value(), o.field("send headers", "sent"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send headers", "wire", 1));
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const GuestAddr r = response(200, "", nullptr);
        known_header(r, 26, "Mine/1", kData + 0x3400);                          // Server
        known_header(r, 2, "Thu, 01 Jan 2026 00:00:00 GMT", kData + 0x3420);    // Date
        known_header(r, 1, "close", kData + 0x3460);                            // Connection
        ASSERT_EQ(send(request_id(), 0, r), o.field("send caller-server", "rc"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send caller-server", "wire", 1));
        EXPECT_EQ(c.server_closed, o.field("send caller-server", "closed", 1) == 1);
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const GuestAddr r = response(404, "", nullptr);
        (void)mem_.write_u16(r + l.rs_version + 2, 0);   // HTTP/1.0 in the response is ignored
        ASSERT_EQ(send(request_id(), 0, r), o.field("send v10-noreason", "rc"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send v10-noreason", "wire", 1));
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const GuestAddr r = response(500, "Bad", nullptr);
        (void)mem_.write_u16(r + l.rs_version, 0);
        (void)mem_.write_u16(r + l.rs_version + 2, 0);
        ASSERT_EQ(send(request_id(), 0, r), o.field("send v00", "rc"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send v00", "wire", 1));
    }
    {
        // MORE_DATA, then HttpSendResponseEntityBody: no Content-Length.
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const u64 id = request_id();
        const std::string p1 = "part1";
        ASSERT_EQ(send_simple(id, 200, "OK", &p1, 2, kData + 0x24), o.field("send more-data", "rc"));
        EXPECT_EQ(mem_.read_u32(kData + 0x24).value(), o.field("send more-data", "sent"));
        const GuestAddr chunk = kData + 0x3800;
        std::vector<u8> cz(32, 0);
        (void)mem_.write_bytes(chunk, cz.data(), cz.size());
        put_ptr(chunk + l.chunk_buffer, astr(kData + 0x3900, "part2"));
        (void)mem_.write_u32(chunk + l.chunk_buffer_length, 5);
        auto entity = [&](u32 flags, u32 count, GuestAddr chunks) {
            return call("HttpSendResponseEntityBody",
                        args().p(q_).id(id).p(flags).p(count).p(chunks).p(kData + 0x24).p(0).p(0).p(0).p(0));
        };
        EXPECT_EQ(entity(2, 1, chunk), o.field("entity-send more", "rc"));
        EXPECT_EQ(mem_.read_u32(kData + 0x24).value(), o.field("entity-send more", "sent"));
        EXPECT_EQ(entity(0, 0, 0), o.field("entity-send final-empty", "rc"));
        EXPECT_EQ(mem_.read_u32(kData + 0x24).value(), o.field("entity-send final-empty", "sent"));
        EXPECT_EQ(entity(0, 1, chunk), o.field("entity-send after-final", "rc"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send streamed", "wire"));
        EXPECT_EQ(c.server_closed, o.field("send streamed", "closed") == 1);
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const u64 id = request_id();
        const GuestAddr r = response(200, "", nullptr);
        known_header(r, 11, "6", kData + 0x3400);
        ASSERT_EQ(send(id, 2, r), o.field("send cl-more", "rc"));
        const GuestAddr chunks = kData + 0x3800;
        std::vector<u8> cz(64, 0);
        (void)mem_.write_bytes(chunks, cz.data(), cz.size());
        put_ptr(chunks + l.chunk_buffer, astr(kData + 0x3900, "abc"));
        (void)mem_.write_u32(chunks + l.chunk_buffer_length, 3);
        put_ptr(chunks + 32 + l.chunk_buffer, astr(kData + 0x3910, "def"));
        (void)mem_.write_u32(chunks + 32 + l.chunk_buffer_length, 3);
        EXPECT_EQ(call("HttpSendResponseEntityBody",
                       args().p(q_).id(id).p(0).p(2).p(chunks).p(0).p(0).p(0).p(0).p(0)),
                  o.field("send cl-more", "entity"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send cl-streamed", "wire"));
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        ASSERT_EQ(send(request_id(), 0, response(99, "", nullptr)), o.field("send status99", "rc"));
        EXPECT_EQ(send(request_id(), 0, response(200, "", nullptr)), o.field("send status99", "then-200"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send status99", "wire", 1));
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        ASSERT_EQ(send(request_id(), 0, response(100, "", nullptr)), o.field("send status100", "rc"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("send status100", "wire", 1));
    }
}

TEST_P(HttpApiTest, BadResponsesResetTheRequest) {
    const Oracle& o = oracle();
    open_server();
    const auto& l = L();
    auto expect_reset = [&](Client& c, const std::string& key) {
        c.join();
        EXPECT_EQ(c.response, "") << key;
        EXPECT_EQ(c.error, ECONNRESET) << key;   // err=10054
        (void)o;
    };
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        EXPECT_EQ(send(request_id(), 0, response(1000, "", nullptr)), o.field("status1000 then-200", "rc"));
        EXPECT_NE(send(request_id(), 0, response(200, "", nullptr)), 0u);
        expect_reset(c, "status1000 then-200");
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        EXPECT_EQ(send(request_id(), 0x80000000u, response(200, "", nullptr)), o.field("bad-flags then-200", "rc"));
        expect_reset(c, "bad-flags then-200");
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        EXPECT_EQ(send(request_id(), 0x10, response(200, "", nullptr)), o.field("flags0x10 then-200", "rc"));
        expect_reset(c, "flags0x10 then-200");
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const GuestAddr r = response(200, "", nullptr);
        const GuestAddr e = kData + 0x3500;
        (void)mem_.write_u16(e, 3);
        (void)mem_.write_u16(e + 2, 1);
        put_ptr(e + l.unknown_header_name, 0);
        put_ptr(e + l.unknown_header_value, astr(kData + 0x3600, "v"));
        (void)mem_.write_u16(r + l.rs_headers + l.hdr_unknown_count, 1);
        put_ptr(r + l.rs_headers + l.hdr_unknown, e);
        EXPECT_EQ(send(request_id(), 0, r), o.field("null-header-name then-200", "rc"));
        expect_reset(c, "null-header-name then-200");
    }
    {
        // Parameter checks that come before the request id is looked up.
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const u64 id = request_id();
        EXPECT_EQ(call("HttpSendHttpResponse",
                       args().p(q_).id(id).p(0).p(response(200, "", nullptr)).p(0).p(0).p(0).p(1).p(0).p(0)),
                  o.field("send reserved2", "rc"));
        GuestAddr r = response(200, "", nullptr);
        (void)mem_.write_u16(r + l.rs_entity_chunk_count, 1);
        EXPECT_EQ(send(id, 0, r), o.field("send null-chunks", "rc"));
        EXPECT_EQ(send(0x1234567812345678ull, 0, response(200, "", nullptr)), o.field("send bogus-id", "rc"));
        EXPECT_EQ(send(0, 0, response(200, "", nullptr)), o.field("send null-id", "rc"));
        // A space in a header name is passed through ("space-header-name").
        r = response(200, "", nullptr);
        const GuestAddr e = kData + 0x3500;
        (void)mem_.write_u16(e, 8);
        (void)mem_.write_u16(e + 2, 1);
        put_ptr(e + l.unknown_header_name, astr(kData + 0x3600, "Bad Name"));
        put_ptr(e + l.unknown_header_value, astr(kData + 0x3620, "v"));
        (void)mem_.write_u16(r + l.rs_headers + l.hdr_unknown_count, 1);
        put_ptr(r + l.rs_headers + l.hdr_unknown, e);
        EXPECT_EQ(send(id, 0, r), o.field("space-header-name", "rc"));
        c.join();
        EXPECT_EQ(c.masked(), o.str("space-header-name", "wire", 1));
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        const GuestAddr r = response(200, "", nullptr);
        (void)mem_.write_u16(r + l.rs_entity_chunk_count, 1);
        put_ptr(r + l.rs_entity_chunks, kData + 0x3800);
        (void)mem_.write_u32(kData + 0x3800, 99);
        EXPECT_EQ(send(request_id(), 0, r), o.field("send bad-chunk-type", "rc"));
        c.join();
    }
    {
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        EXPECT_EQ(call("HttpCancelHttpRequest", args().p(q_).id(request_id()).p(0)), o.field("cancel-request", "rc"));
        expect_reset(c, "cancel-request");
    }
    {
        // A NULL HTTP_RESPONSE faults inside httpapi.dll on Windows.
        Client c;
        ASSERT_EQ(recv_one(c, kGet), 0u);
        (void)send(request_id(), 0, 0);
        EXPECT_TRUE(cpu_.pending_raise.active);
        EXPECT_EQ(cpu_.pending_raise.code, 0xC0000005u);
        cpu_.pending_raise.active = false;
        ASSERT_EQ(send(request_id(), 0, response(204, "", nullptr)), 0u);
    }
}

// =============================================================================
// Overlapped receive, completion ports, cancel, shutdown
// =============================================================================

TEST_P(HttpApiTest, OverlappedReceive) {
    const Oracle& o = oracle();
    open_server();
    const GuestAddr ov = kData + 0x6000;
    std::vector<u8> zero(32, 0);
    auto reset_ov = [&](Handle ev) {
        (void)mem_.write_bytes(ov, zero.data(), zero.size());
        put_ptr(ov + (is64() ? 24 : 16), ev);
        completions_.clear();
    };
    const Handle ev = sync_->create_event(true, true, "");
    reset_ov(ev);
    (void)mem_.write_u32(kData + 0x20, 0);
    ASSERT_EQ(receive(0, 0, kBufSize, ov), o.field("ovl pending", "rc"));
    EXPECT_EQ(get_ptr(ov), o.field("ovl pending", "internal"));
    EXPECT_EQ(sync_->wait_for_single_object(ev, 0), 0x102u);   // event_reset=1 (WAIT_TIMEOUT)
    Client c;
    c.start(kGet, 50);
    ASSERT_EQ(sync_->wait_for_single_object(ev, 5000), 0u);
    EXPECT_EQ(get_ptr(ov), o.field("ovl complete", "internal"));
    EXPECT_GT(get_ptr(ov + (is64() ? 8 : 4)), L().request_v2_size);   // n_eq_used=1
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_FALSE(completions_[0].synchronous);
    EXPECT_EQ(read_a(static_cast<GuestAddr>(get_ptr(kBuf + L().rq_raw_url)), 9), "/vprobe/r");
    ASSERT_EQ(send_simple(request_id(), 200, "OK", nullptr), 0u);
    c.join();

    // Request already queued: immediate success, completion posted inline.
    Client d;
    d.start(kGet);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    reset_ov(ev);
    (void)mem_.write_u32(kData + 0x20, 0xDEAD);
    ASSERT_EQ(receive(0, 0, kBufSize, ov), o.field("ovl immediate", "rc"));
    EXPECT_NE(mem_.read_u32(kData + 0x20).value(), 0xDEADu);   // got_set=1
    EXPECT_EQ(get_ptr(ov), o.field("ovl immediate", "internal"));
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_TRUE(completions_[0].synchronous);   // a port packet unless skip-on-success ("iocp immediate: packet=1")
    ASSERT_EQ(send_simple(request_id(), 200, "OK", nullptr), 0u);
    d.join();

    // Too small, no port: 234 inline with STATUS_BUFFER_OVERFLOW.
    Client e;
    e.start(kGet);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    reset_ov(ev);
    ASSERT_EQ(receive(0, 0, L().request_v2_size, ov), o.field("ovl small", "rc"));
    EXPECT_EQ(get_ptr(ov), o.field("ovl small", "internal"));
    EXPECT_EQ(mem_.read_u32(kData + 0x20).value(), get_ptr(ov + (is64() ? 8 : 4)));
    ASSERT_EQ(receive(request_id()), o.field("ovl small retry", "rc"));
    ASSERT_EQ(send_simple(request_id(), 200, "OK", nullptr), 0u);
    e.join();

    // Too small with a completion port: pends, and the port reports 234
    // ("iocp small: rc=997 packet=1 le=234").
    ported_.insert(q_);
    Client f;
    f.start(kGet);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    reset_ov(0);
    ASSERT_EQ(receive(0, 0, L().request_v2_size, ov), o.field("iocp small", "rc"));
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_EQ(completions_[0].win_error, o.field("iocp small", "le"));
    EXPECT_EQ(completions_[0].nt_status, 0x80000005u);
    ASSERT_EQ(receive(request_id()), 0u);
    ASSERT_EQ(send_simple(request_id(), 200, "OK", nullptr), 0u);
    f.join();

    // Overlapped send completes inline with a packet ("iocp ovl-send").
    Client g;
    ASSERT_EQ(recv_one(g, kGet), 0u);
    reset_ov(0);
    (void)mem_.write_u32(kData + 0x24, 0xDEAD);
    const std::string body = "ovsend";
    ASSERT_EQ(send(request_id(), 0, response(200, "", &body), kData + 0x24, ov), o.field("iocp ovl-send", "rc"));
    EXPECT_EQ(mem_.read_u32(kData + 0x24).value(), o.field("iocp ovl-send", "sent"));
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_EQ(completions_[0].bytes, o.field("iocp ovl-send", "n"));
    g.join();
    EXPECT_EQ(g.masked(), o.str("iocp ovl-send", "wire", 1));
}

TEST_P(HttpApiTest, CancelShutdownAndClose) {
    const Oracle& o = oracle();
    open_server();
    const GuestAddr ov = kData + 0x6000;
    std::vector<u8> zero(32, 0);
    (void)mem_.write_bytes(ov, zero.data(), zero.size());
    ASSERT_EQ(receive(0, 0, kBufSize, ov), o.field("cancel pending", "rc"));
    EXPECT_EQ(mgr_.cancel_io(q_, ov, false, 0), 1u);   // CancelIoEx: cancel=1
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_EQ(completions_[0].win_error, o.field("cancel pending", "le"));
    EXPECT_EQ(completions_[0].nt_status, o.field("cancel pending", "internal"));
    EXPECT_EQ(completions_[0].bytes, o.field("cancel pending", "n"));
    EXPECT_EQ(mgr_.cancel_io(q_, ov, false, 0), 0u);   // cancel nothing: ok=0

    completions_.clear();
    ASSERT_EQ(receive(0, 0, kBufSize, ov), o.field("shutdown", "pending"));
    EXPECT_EQ(call("HttpShutdownRequestQueue", args().p(q_)), o.field("shutdown", "rc"));
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_EQ(completions_[0].win_error, o.field("shutdown", "le"));
    EXPECT_EQ(completions_[0].nt_status, o.field("shutdown", "internal"));
    EXPECT_EQ(call("HttpShutdownRequestQueue", args().p(q_)), o.field("shutdown again", "rc"));
    EXPECT_EQ(receive(), o.field("after-shutdown sync-recv", "rc"));
    EXPECT_EQ(receive(0, 0, kBufSize, ov), o.field("after-shutdown ovl-recv", "rc"));
    EXPECT_EQ(call("HttpAddUrlToUrlGroup",
                   args().id(gid_).p(wstr(kData + 0x100, u"http://localhost:48213/vprobe9/")).id(0).p(0)),
              o.field("after-shutdown addurl", "rc"));
    {
        // A request for a shut-down queue is held: no answer ("shutdown long: err=10060").
        Client c;
        c.start(kGet, 0, 500);
        c.join();
        EXPECT_EQ(c.response, "");
        EXPECT_FALSE(c.server_closed);
        EXPECT_EQ(c.error, EAGAIN);
    }
    ASSERT_EQ(call("HttpCloseRequestQueue", args().p(q_)), 0u);
    EXPECT_EQ(receive(), o.field("recv closed-handle", "rc"));
    EXPECT_EQ(send_simple(1, 200, "OK", nullptr), o.field("send closed-handle", "rc"));
    EXPECT_EQ(call("HttpReceiveHttpRequest", args().p(0).id(0).p(0).p(kBuf).p(kBufSize).p(kData + 0x20).p(0)),
              o.field("recv null-handle", "rc"));
    {
        // The group is still bound to the closed queue ("queue closed: 400").
        Client c;
        c.start(kGet);
        c.join();
        EXPECT_EQ(c.response.substr(0, 24), "HTTP/1.1 400 Bad Request");
    }
}

TEST_P(HttpApiTest, SynchronousReceiveReleasedByShutdownAndClose) {
    const Oracle& o = oracle();
    open_server();
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cpu::CpuState cpu2;
        if (is64()) cpu2.init_long_mode();
        else cpu2.init_protected_mode();
        Marshaler m(mem_);
        m.attach_cpu(cpu2);
        if (is64()) cpu2.gpr.rcx = q_;
        else {
            cpu2.set_sp(kStackBase + 0x1000);
            (void)mem_.write_u32(kStackBase + 0x1004, q_);
        }
        for (const auto& e : exp_.entries)
            if (e.name == "HttpShutdownRequestQueue") e.handler(m);
    });
    EXPECT_EQ(receive(), o.field("sync-recv released by shutdown", "rc"));
    t.join();
}

TEST_P(HttpApiTest, WaitForDisconnect) {
    open_server();
    Client c;
    ASSERT_EQ(recv_one(c, "GET /vprobe/r HTTP/1.1\r\nHost: localhost:48213\r\nConnection: close\r\n\r\n"), 0u);
    const u64 conn = mem_.read_u64(kBuf + L().rq_connection_id).value();
    const GuestAddr ov = kData + 0x6000;
    std::vector<u8> zero(32, 0);
    (void)mem_.write_bytes(ov, zero.data(), zero.size());
    ASSERT_EQ(call("HttpWaitForDisconnect", args().p(q_).id(conn).p(ov)), 997u);
    EXPECT_EQ(call("HttpWaitForDisconnectEx", args().p(q_).id(conn).p(1).p(ov)), 87u);
    ASSERT_EQ(send_simple(request_id(), 200, "OK", nullptr), 0u);
    c.join();
    for (int i = 0; i < 100 && completions_.empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(completions_.size(), 1u);
    EXPECT_EQ(completions_[0].win_error, 0u);
}

TEST_P(HttpApiTest, UnimplementedExportsFailHonestly) {
    for (const char* name : {"HttpAddFragmentToCache", "HttpReadFragmentFromCache", "HttpFlushResponseCache",
                             "HttpSetServiceConfiguration", "HttpQueryServiceConfiguration",
                             "HttpDeleteServiceConfiguration", "HttpUpdateServiceConfiguration",
                             "HttpControlService", "HttpDeclarePush", "HttpDelegateRequest",
                             "HttpDelegateRequestEx", "HttpEvaluateRequest", "HttpFindUrlGroupId",
                             "HttpGetCounters", "HttpGetExtension", "HttpPrepareUrl", "HttpQueryRequestProperty",
                             "HttpSetRequestProperty", "HttpReceiveClientCertificate", "HttpWaitForDemandStart"}) {
        Marshaler m(mem_);
        m.attach_cpu(cpu_);
        for (const auto& e : exp_.entries)
            if (e.name == name) e.handler(m);
        EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 50u) << name;   // ERROR_NOT_SUPPORTED
    }
    EXPECT_EQ(call("HttpIsFeatureSupported", args().p(8)), 1u);   // HttpFeatureAutomaticChunkedEncoding
    EXPECT_EQ(call("HttpIsFeatureSupported", args().p(4)), 0u);   // HttpFeatureHttp3
}

} // namespace
