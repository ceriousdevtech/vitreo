// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_remote.cpp
/// @brief The installer processes' wire and the remote session API
/// (msi_remote.hpp): custom-action server proxies relay msi.dll calls on
/// engine handles to the engine process, which runs the same export handler
/// a local caller reaches.

#include "winapi/system/msi/msi_remote.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_customaction.hpp"
#include "winapi/system/msi/msi_database.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/unix_pipe.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <unordered_map>

#include <poll.h>

namespace vitreo::winapi::msi {

// ---- Wire codec -----------------------------------------------------------------

void WireWriter::u32_(u32 v) {
    for (int i = 0; i < 4; ++i) m_bytes.push_back(static_cast<u8>(v >> (8 * i)));
}
void WireWriter::u64_(u64 v) {
    for (int i = 0; i < 8; ++i) m_bytes.push_back(static_cast<u8>(v >> (8 * i)));
}
void WireWriter::bytes(std::span<const u8> data) {
    u32_(static_cast<u32>(data.size()));
    m_bytes.insert(m_bytes.end(), data.begin(), data.end());
}
void WireWriter::str(const std::string& text) {
    bytes(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}
void WireWriter::u16str(const std::u16string& text) {
    u32_(static_cast<u32>(text.size()));
    for (const char16_t c : text) {
        m_bytes.push_back(static_cast<u8>(c));
        m_bytes.push_back(static_cast<u8>(c >> 8));
    }
}
void WireWriter::record(const Record& record) {
    u32_(record.count());
    for (const Record::Field& f : record.fields) {
        u8_(static_cast<u8>(f.kind));
        switch (f.kind) {
            case Record::Kind::Null: break;
            case Record::Kind::Integer: u32_(static_cast<u32>(f.integer)); break;
            case Record::Kind::String: u16str(f.text); break;
            case Record::Kind::Stream: bytes(f.stream); break;
        }
    }
}
void WireWriter::string_map(const std::map<std::string, std::string>& map) {
    u32_(static_cast<u32>(map.size()));
    for (const auto& [k, v] : map) {
        str(k);
        str(v);
    }
}

bool WireReader::need(usize n) {
    if (!m_ok || m_data.size() - m_pos < n) {
        m_ok = false;
        return false;
    }
    return true;
}
u8 WireReader::u8_() {
    if (!need(1)) return 0;
    return m_data[m_pos++];
}
u32 WireReader::u32_() {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(m_data[m_pos + i]) << (8 * i);
    m_pos += 4;
    return v;
}
u64 WireReader::u64_() {
    if (!need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(m_data[m_pos + i]) << (8 * i);
    m_pos += 8;
    return v;
}
std::vector<u8> WireReader::bytes() {
    const u32 n = u32_();
    if (!need(n)) return {};
    std::vector<u8> out(m_data.begin() + static_cast<std::ptrdiff_t>(m_pos),
                        m_data.begin() + static_cast<std::ptrdiff_t>(m_pos + n));
    m_pos += n;
    return out;
}
std::string WireReader::str() {
    const std::vector<u8> b = bytes();
    return std::string(b.begin(), b.end());
}
std::u16string WireReader::u16str() {
    const u32 n = u32_();
    if (n > (m_data.size() - m_pos) / 2 || !need(static_cast<usize>(n) * 2)) {
        m_ok = false;
        return {};
    }
    std::u16string out(n, u'\0');
    for (u32 i = 0; i < n; ++i) {
        out[i] = static_cast<char16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
    }
    return out;
}
std::optional<Record> WireReader::record() {
    const u32 count = u32_();
    if (!m_ok || count > 65535) {
        m_ok = false;
        return std::nullopt;
    }
    Record r(count);
    for (Record::Field& f : r.fields) {
        const u8 kind = u8_();
        if (kind > static_cast<u8>(Record::Kind::Stream)) m_ok = false;
        if (!m_ok) return std::nullopt;
        f.kind = static_cast<Record::Kind>(kind);
        switch (f.kind) {
            case Record::Kind::Null: break;
            case Record::Kind::Integer: f.integer = static_cast<i32>(u32_()); break;
            case Record::Kind::String: f.text = u16str(); break;
            case Record::Kind::Stream: f.stream = bytes(); break;
        }
    }
    if (!m_ok) return std::nullopt;
    return r;
}
std::map<std::string, std::string> WireReader::string_map() {
    std::map<std::string, std::string> out;
    const u32 n = u32_();
    for (u32 i = 0; i < n && m_ok; ++i) {
        std::string k = str();
        std::string v = str();
        if (m_ok) out[std::move(k)] = std::move(v);
    }
    return out;
}

// ---- Channel ----------------------------------------------------------------------

namespace {
constexpr u32 kFrameMagic = 0x4D534952;  // "RISM": installer relay frame
constexpr u32 kMaxFrame = 256u << 20;
}  // namespace

RemoteChannel::RemoteChannel(std::shared_ptr<ipc::UnixPipe> pipe) : m_pipe(std::move(pipe)) {
    if (!m_pipe) m_broken = true;
}

RemoteChannel::~RemoteChannel() = default;

std::pair<std::shared_ptr<RemoteChannel>, std::shared_ptr<RemoteChannel>> RemoteChannel::pair() {
    auto [a, b] = ipc::UnixPipe::create_pair();
    return {std::make_shared<RemoteChannel>(a), std::make_shared<RemoteChannel>(b)};
}

int RemoteChannel::fd() const { return m_pipe ? m_pipe->fd() : -1; }

void RemoteChannel::close() {
    std::lock_guard lock(m_send_mutex);
    m_broken = true;
    m_pipe.reset();
}

bool RemoteChannel::send(Wire kind, const std::vector<u8>& body) {
    std::lock_guard lock(m_send_mutex);
    if (m_broken || !m_pipe) return false;
    u8 header[12];
    const u32 fields[3] = {kFrameMagic, static_cast<u32>(kind), static_cast<u32>(body.size())};
    for (int f = 0; f < 3; ++f)
        for (int i = 0; i < 4; ++i) header[f * 4 + i] = static_cast<u8>(fields[f] >> (8 * i));
    if (m_pipe->write(header, sizeof(header)) != static_cast<isize>(sizeof(header)) ||
        (!body.empty() && m_pipe->write(body.data(), body.size()) != static_cast<isize>(body.size()))) {
        m_broken = true;
        return false;
    }
    return true;
}

bool RemoteChannel::read_exact(void* out, usize n) {
    auto* p = static_cast<u8*>(out);
    usize got = 0;
    while (got < n) {
        const isize r = m_pipe->read(p + got, n - got);
        if (r <= 0) return false;
        got += static_cast<usize>(r);
    }
    return true;
}

std::optional<Frame> RemoteChannel::receive(int timeout_ms) {
    m_timed_out = false;
    if (m_broken || !m_pipe) return std::nullopt;
    if (timeout_ms >= 0 && !m_pipe->wait_readable(timeout_ms)) {
        m_timed_out = true;
        return std::nullopt;
    }
    u8 header[12];
    if (!read_exact(header, sizeof(header))) {
        m_broken = true;
        return std::nullopt;
    }
    u32 fields[3] = {};
    for (int f = 0; f < 3; ++f)
        for (int i = 0; i < 4; ++i) fields[f] |= static_cast<u32>(header[f * 4 + i]) << (8 * i);
    if (fields[0] != kFrameMagic || fields[2] > kMaxFrame) {
        m_broken = true;
        return std::nullopt;
    }
    Frame frame;
    frame.kind = static_cast<Wire>(fields[1]);
    frame.body.resize(fields[2]);
    if (!frame.body.empty() && !read_exact(frame.body.data(), frame.body.size())) {
        m_broken = true;
        return std::nullopt;
    }
    return frame;
}

std::optional<std::vector<u8>> RemoteChannel::transact(Wire kind, const std::vector<u8>& body, Wire reply) {
    std::lock_guard lock(m_transact_mutex);
    if (!send(kind, body)) return std::nullopt;
    auto frame = receive();
    if (!frame || frame->kind != reply) {
        m_broken = true;
        return std::nullopt;
    }
    return std::move(frame->body);
}

// ---- Relay signatures -------------------------------------------------------------------

const std::vector<RelaySignature>& relay_signatures() {
    // The MsiQuery.h prototypes of every export that takes an install,
    // database, view or summary-information handle as its first argument.
    // Record functions (MsiRecord*) act on the server's own records.
    static const std::vector<RelaySignature> table = [] {
        std::vector<RelaySignature> t;
        const auto one = [&t](const char* name, const char* args, char ret = 'u') {
            t.push_back(RelaySignature{name, args, false, ret});
        };
        const auto aw = [&t](const char* base, const char* args, char ret = 'u') {
            t.push_back(RelaySignature{std::string(base) + "A", args, false, ret});
            t.push_back(RelaySignature{std::string(base) + "W", args, true, ret});
        };
        // Database, view, summary information.
        aw("MsiDatabaseOpenView", "hsH");
        aw("MsiViewGetError", "hBn");
        one("MsiViewExecute", "hr");
        one("MsiViewFetch", "hH");
        one("MsiViewModify", "hiR");
        one("MsiViewGetColumnInfo", "hiH");
        one("MsiViewClose", "h");
        aw("MsiDatabaseGetPrimaryKeys", "hsH");
        aw("MsiDatabaseIsTablePersistent", "hs");
        aw("MsiGetSummaryInformation", "hsiH");
        one("MsiSummaryInfoGetPropertyCount", "ho");
        aw("MsiSummaryInfoSetProperty", "hiiiFs");
        aw("MsiSummaryInfoGetProperty", "hioofBn");
        one("MsiSummaryInfoPersist", "h");
        one("MsiDatabaseCommit", "h");
        one("MsiGetDatabaseState", "h");
        aw("MsiDatabaseExport", "hsss");
        aw("MsiDatabaseImport", "hss");
        aw("MsiDatabaseApplyTransform", "hsi");
        // Install session.
        one("MsiGetActiveDatabase", "h", 'H');
        one("MsiGetLanguage", "h");
        one("MsiGetMode", "hi");
        one("MsiSetMode", "hii");
        one("MsiProcessMessage", "hir");
        aw("MsiFormatRecord", "hrBn");
        aw("MsiGetProperty", "hsBn");
        aw("MsiSetProperty", "hss");
        aw("MsiDoAction", "hs");
        aw("MsiSequence", "hsi");
        aw("MsiEvaluateCondition", "hs");
        aw("MsiGetFeatureState", "hsoo");
        aw("MsiSetFeatureState", "hsi");
        aw("MsiSetFeatureAttributes", "hsi");
        aw("MsiGetComponentState", "hsoo");
        aw("MsiSetComponentState", "hsi");
        aw("MsiGetFeatureCost", "hsiio");
        aw("MsiEnumComponentCosts", "hsiiBnoo");
        one("MsiSetInstallLevel", "hi");
        one("MsiVerifyDiskSpace", "h");
        aw("MsiGetFeatureValidStates", "hso");
        aw("MsiGetSourcePath", "hsBn");
        aw("MsiGetTargetPath", "hsBn");
        aw("MsiSetTargetPath", "hss");
        return t;
    }();
    return table;
}

namespace {

// Handle encodings in a reply.
constexpr u8 kHandleNull = 0;      ///< the value 0
constexpr u8 kHandleRecord = 1;    ///< a record, by value
constexpr u8 kHandleRemote = 2;    ///< an engine handle
constexpr u8 kHandleUnchanged = 3; ///< the call did not write the output

// Record-argument encodings in a call.
constexpr u8 kRecordNull = 0;
constexpr u8 kRecordValue = 1;
constexpr u8 kRecordInvalid = 2;
constexpr u8 kRecordRemote = 3;

constexpr u32 kMaxStringUnits = 1u << 22;
constexpr u32 kUnwritten = 0xFFFFFFFFu;  ///< an MSIHANDLE output not yet written (handles never reach it)

std::atomic<bool> g_ca_server_process{false};

struct RemoteRegistry {
    std::mutex mutex;
    std::unordered_map<MsiManager*, std::shared_ptr<HandlerMap>> local;
};
RemoteRegistry& registry() {
    static RemoteRegistry r;
    return r;
}

std::shared_ptr<HandlerMap> local_handlers(MsiManager& mgr) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.local.find(&mgr);
    return it == r.local.end() ? nullptr : it->second;
}

const RelaySignature* signature_of(const std::string& name) {
    static const std::unordered_map<std::string, const RelaySignature*> index = [] {
        std::unordered_map<std::string, const RelaySignature*> m;
        for (const RelaySignature& s : relay_signatures()) m[s.name] = &s;
        return m;
    }();
    const auto it = index.find(name);
    return it == index.end() ? nullptr : it->second;
}

/// Raw bytes of a NUL-terminated guest string, terminator excluded.
std::vector<u8> read_guest_string(Marshaler& m, GuestAddr p, bool wide) {
    std::vector<u8> out;
    if (wide) {
        for (u32 i = 0; i < kMaxStringUnits; ++i) {
            auto c = m.read_u16(p + i * 2);
            if (c.is_err() || c.value() == 0) break;
            out.push_back(static_cast<u8>(c.value()));
            out.push_back(static_cast<u8>(c.value() >> 8));
        }
    } else {
        for (u32 i = 0; i < kMaxStringUnits; ++i) {
            auto c = m.read_u8(p + i);
            if (c.is_err() || c.value() == 0) break;
            out.push_back(c.value());
        }
    }
    return out;
}

void write_guest_bytes(Marshaler& m, GuestAddr at, std::span<const u8> bytes) {
    for (usize i = 0; i < bytes.size(); ++i) (void)m.write_u8(at + i, bytes[i]);
}

/// Log a relay that broke once (the engine process went away).
void relay_broken(const std::string& api) {
    log_unimplemented(api, "custom-action server lost its connection to the installer engine");
}

/// The CA-server side of one relayed call.
void relay_call(MsiManager& mgr, const RelaySignature& sig, const RemoteHandle& target, Marshaler& m) {
    WireWriter w;
    w.str(sig.name);
    for (usize i = 0; i < sig.args.size(); ++i) {
        const char k = sig.args[i];
        switch (k) {
            case 'h': w.u32_(target.remote); break;
            case 'i': w.u32_(m.arg32(i)); break;
            case 's': {
                const GuestAddr p = m.arg_ptr(i);
                w.u8_(p != 0 ? 1 : 0);
                if (p != 0) w.bytes(read_guest_string(m, p, sig.wide));
                break;
            }
            case 'r':
            case 'R': {
                const u32 h = m.arg32(i);
                if (h == 0) { w.u8_(kRecordNull); break; }
                if (auto rec = mgr.handles().get<Record>(h)) { w.u8_(kRecordValue); w.record(*rec); break; }
                if (auto proxy = mgr.handles().get<RemoteHandle>(h)) { w.u8_(kRecordRemote); w.u32_(proxy->remote); break; }
                w.u8_(kRecordInvalid);
                break;
            }
            case 'B': {
                const GuestAddr p = m.arg_ptr(i);
                w.u8_(p != 0 ? 1 : 0);
                break;
            }
            case 'n':
            case 'o':
            case 'H': {
                const GuestAddr p = m.arg_ptr(i);
                w.u8_(p != 0 ? 1 : 0);
                if (p != 0) w.u32_(m.read_u32(p).value_or(0));
                break;
            }
            case 'f':
            case 'F': {
                const GuestAddr p = m.arg_ptr(i);
                w.u8_(p != 0 ? 1 : 0);
                if (p != 0) w.u64_(m.read_u64(p).value_or(0));
                break;
            }
            default: break;
        }
    }
    auto reply = target.channel ? target.channel->transact(Wire::Call, w.data(), Wire::CallReply) : std::nullopt;
    if (!reply) {
        relay_broken(sig.name);
        m.set_return(sig.ret == 'H' ? 0u : MSI_ERROR_INVALID_HANDLE);
        return;
    }
    WireReader r(*reply);
    const auto read_handle = [&](u32& out) -> bool {
        const u8 how = r.u8_();
        switch (how) {
            case kHandleNull: out = 0; return true;
            case kHandleRecord: {
                auto rec = r.record();
                out = rec ? mgr.handles().add(std::make_shared<Record>(std::move(*rec))) : 0;
                return true;
            }
            case kHandleRemote: out = add_remote_handle(mgr, target.channel, r.u32_()); return true;
            default: return false;  // unchanged
        }
    };
    u32 ret = r.u32_();
    if (sig.ret == 'H' && !read_handle(ret)) ret = 0;
    for (usize i = 0; i < sig.args.size() && r.ok(); ++i) {
        const char k = sig.args[i];
        const GuestAddr p = (k == 'h' || k == 'i' || k == 's' || k == 'r') ? 0 : m.arg_ptr(i);
        switch (k) {
            case 'R': {
                if (r.u8_() == 0) break;
                auto rec = r.record();
                if (auto local = mgr.handles().get<Record>(m.arg32(i)); local && rec) *local = std::move(*rec);
                break;
            }
            case 'B': {
                if (r.u8_() == 0) break;
                const std::vector<u8> bytes = r.bytes();
                if (p != 0) write_guest_bytes(m, p, bytes);
                break;
            }
            case 'n':
            case 'o': {
                if (r.u8_() == 0) break;
                const u32 v = r.u32_();
                if (p != 0) (void)m.write_u32(p, v);
                break;
            }
            case 'f': {
                if (r.u8_() == 0) break;
                const u64 v = r.u64_();
                if (p != 0) (void)m.write_u64(p, v);
                break;
            }
            case 'H': {
                if (r.u8_() == 0) break;
                u32 h = 0;
                if (read_handle(h) && p != 0) (void)m.write_u32(p, h);
                break;
            }
            default: break;
        }
    }
    if (r.u8_() != 0) {
        if (auto rec = r.record()) put_last_error_record(std::make_shared<Record>(std::move(*rec)));
    }
    m.set_return(ret);
}

u32 close_remote(MsiManager& mgr, u32 h, const RemoteHandle& proxy) {
    (void)mgr.handles().close(h);
    if (!proxy.channel) return MSI_ERROR_SUCCESS;
    WireWriter w;
    w.u32_(proxy.remote);
    auto reply = proxy.channel->transact(Wire::CloseHandle, w.data(), Wire::CloseReply);
    if (!reply) return MSI_ERROR_SUCCESS;  // the engine is gone: nothing is left to close
    WireReader r(*reply);
    return r.u32_();
}

}  // namespace

void set_custom_action_server_process(bool on) { g_ca_server_process.store(on); }
bool custom_action_server_process() { return g_ca_server_process.load(); }

u32 add_remote_handle(MsiManager& mgr, std::shared_ptr<RemoteChannel> channel, u32 remote) {
    auto proxy = std::make_shared<RemoteHandle>();
    proxy->channel = std::move(channel);
    proxy->remote = remote;
    return mgr.handles().add(std::move(proxy));
}

void register_remote_session_api(MsiManager& mgr, HandlerMap& map) {
    auto originals = std::make_shared<HandlerMap>();
    MsiManager* M = &mgr;
    for (const RelaySignature& sig : relay_signatures()) {
        auto it = map.find(sig.name);
        if (it == map.end()) continue;
        (*originals)[sig.name] = it->second;
        Handler local = it->second;
        const RelaySignature* s = &sig;
        it->second = [M, s, local](Marshaler& m) {
            if (auto proxy = M->handles().get<RemoteHandle>(m.arg32(0))) {
                relay_call(*M, *s, *proxy, m);
                return;
            }
            local(m);
        };
    }
    if (auto it = map.find("MsiCloseHandle"); it != map.end()) {
        Handler local = it->second;
        it->second = [M, local](Marshaler& m) {
            const u32 h = m.arg32(0);
            if (auto proxy = M->handles().get<RemoteHandle>(h)) {
                m.set_return(close_remote(*M, h, *proxy));
                return;
            }
            local(m);
        };
    }
    if (auto it = map.find("MsiCloseAllHandles"); it != map.end()) {
        Handler local = it->second;
        it->second = [M, local](Marshaler& m) {
            // The calling thread's proxies close on the engine too; the count
            // the local handler returns includes them.
            for (const u32 h : M->handles().of_current_thread<RemoteHandle>()) {
                if (auto proxy = M->handles().get<RemoteHandle>(h)) {
                    RemoteHandle copy = *proxy;
                    (void)close_remote(*M, 0, copy);
                }
            }
            local(m);
        };
    }
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.local[&mgr] = std::move(originals);
}

// ---- Engine side ---------------------------------------------------------------------

struct RelayExecutor::Impl {
    MsiManager& mgr;
    CustomActionProcess& host;
    std::shared_ptr<HandlerMap> handlers;
    std::set<u32> created;  ///< engine handles handed to the action

    void encode_handle(WireWriter& w, u32 h) {
        if (h == 0) { w.u8_(kHandleNull); return; }
        if (auto rec = mgr.handles().get<Record>(h)) {
            w.u8_(kHandleRecord);
            w.record(*rec);
            (void)mgr.handles().close(h);
            return;
        }
        if (!mgr.handles().contains(h)) { w.u8_(kHandleNull); return; }
        created.insert(h);
        w.u8_(kHandleRemote);
        w.u32_(h);
    }
};

RelayExecutor::RelayExecutor(MsiManager& mgr, CustomActionProcess& host)
    : m(std::make_unique<Impl>(Impl{mgr, host, local_handlers(mgr), {}})) {
    if (!m->handlers) m->handlers = std::make_shared<HandlerMap>(msi_handlers(mgr));
}

RelayExecutor::~RelayExecutor() { close_all(); }

void RelayExecutor::close_all() {
    for (const u32 h : m->created) (void)m->mgr.handles().close(h);
    m->created.clear();
}

std::vector<u8> RelayExecutor::close(std::span<const u8> body) {
    WireReader r(body);
    const u32 h = r.u32_();
    WireWriter w;
    // Only handles handed to the action close here; the hInstall and other
    // engine-owned handles stay with the engine (closing them is a no-op).
    if (m->created.erase(h) != 0) {
        w.u32_(m->mgr.handles().close(h) ? MSI_ERROR_SUCCESS : MSI_ERROR_INVALID_HANDLE);
    } else {
        w.u32_(m->mgr.handles().contains(h) ? MSI_ERROR_SUCCESS : MSI_ERROR_INVALID_HANDLE);
    }
    return std::move(w.data());
}

std::vector<u8> RelayExecutor::call(std::span<const u8> body) {
    WireReader r(body);
    const std::string name = r.str();
    const RelaySignature* sig = signature_of(name);
    const auto handler = sig != nullptr ? m->handlers->find(name) : m->handlers->end();
    WireWriter w;
    if (sig == nullptr || handler == m->handlers->end()) {
        log_unimplemented(name, "relayed call of an export outside the remote session API");
        w.u32_(sig != nullptr && sig->ret == 'H' ? 0u : MSI_ERROR_CALL_NOT_IMPLEMENTED);
        if (sig != nullptr && sig->ret == 'H') w.u8_(kHandleNull);
        w.u8_(0);
        return std::move(w.data());
    }
    const usize unit = sig->wide ? 2 : 1;

    // Decode the arguments and size the scratch block.
    struct Arg {
        char kind{0};
        bool present{false};
        u32 value{0};
        u64 value64{0};
        std::vector<u8> bytes;
        std::optional<Record> record;
        u32 cap{0};          ///< 'B': capacity in units (from the following 'n')
        GuestAddr at{0};     ///< staged address
        u32 temp_record{0};  ///< engine record handle for 'r'/'R'
    };
    std::vector<Arg> args(sig->args.size());
    for (usize i = 0; i < sig->args.size() && r.ok(); ++i) {
        Arg& a = args[i];
        a.kind = sig->args[i];
        switch (a.kind) {
            case 'h':
            case 'i': a.value = r.u32_(); a.present = true; break;
            case 's': a.present = r.u8_() != 0; if (a.present) a.bytes = r.bytes(); break;
            case 'r':
            case 'R': {
                const u8 how = r.u8_();
                if (how == kRecordValue) { a.record = r.record(); a.present = true; }
                else if (how == kRecordRemote) a.value = r.u32_();
                else if (how == kRecordInvalid) a.value = kUnwritten;
                break;
            }
            case 'B': a.present = r.u8_() != 0; break;
            case 'n':
            case 'o':
            case 'H': a.present = r.u8_() != 0; if (a.present) a.value = r.u32_(); break;
            case 'f':
            case 'F': a.present = r.u8_() != 0; if (a.present) a.value64 = r.u64_(); break;
            default: break;
        }
    }
    if (!r.ok()) {
        w.u32_(sig->ret == 'H' ? 0u : MSI_ERROR_INVALID_PARAMETER);
        if (sig->ret == 'H') w.u8_(kHandleNull);
        w.u8_(0);
        return std::move(w.data());
    }
    for (usize i = 0; i + 1 < args.size(); ++i)
        if (args[i].kind == 'B' && args[i + 1].kind == 'n' && args[i + 1].present)
            args[i].cap = std::min<u32>(args[i + 1].value, kMaxStringUnits);

    constexpr u32 kStack = 0x200;
    u64 size = kStack;
    for (const Arg& a : args) {
        if (a.kind == 's' && a.present) size += a.bytes.size() + unit + 16;
        if (a.kind == 'B' && a.present) size += (static_cast<u64>(a.cap) + 1) * unit + 16;
        size += 16;
    }
    const u32 block = static_cast<u32>(std::min<u64>(size, 0x7FFFFFFF));
    const GuestAddr base = m->host.alloc_scratch(block);
    if (base == 0) {
        w.u32_(sig->ret == 'H' ? 0u : 8u /* ERROR_NOT_ENOUGH_MEMORY */);
        if (sig->ret == 'H') w.u8_(kHandleNull);
        w.u8_(0);
        return std::move(w.data());
    }
    GuestMemory& mem = m->host.guest_memory();
    GuestAddr cursor = base + kStack;
    const auto take = [&](u64 bytes) {
        const GuestAddr at = cursor;
        cursor += (bytes + 15) & ~u64{15};
        return at;
    };
    std::vector<u64> values(args.size(), 0);
    for (usize i = 0; i < args.size(); ++i) {
        Arg& a = args[i];
        switch (a.kind) {
            case 'h':
            case 'i': values[i] = a.value; break;
            case 's':
                if (!a.present) break;
                a.at = take(a.bytes.size() + unit);
                (void)mem.write_bytes(a.at, reinterpret_cast<const Byte*>(a.bytes.data()), a.bytes.size());
                for (usize z = 0; z < unit; ++z) (void)mem.write_u8(a.at + a.bytes.size() + z, 0);
                values[i] = a.at;
                break;
            case 'r':
            case 'R':
                if (a.record) {
                    a.temp_record = m->mgr.handles().add(std::make_shared<Record>(*a.record));
                    values[i] = a.temp_record;
                } else {
                    values[i] = a.value;
                }
                break;
            case 'B': {
                if (!a.present) break;
                const u64 bytes = (static_cast<u64>(a.cap) + 1) * unit;
                a.at = take(bytes);
                const std::vector<u8> fill(static_cast<usize>(bytes), 0xFF);
                (void)mem.write_bytes(a.at, reinterpret_cast<const Byte*>(fill.data()), fill.size());
                values[i] = a.at;
                break;
            }
            case 'n':
            case 'o':
            case 'H':
                if (!a.present) break;
                a.at = take(4);
                (void)mem.write_u32(a.at, a.kind == 'H' ? kUnwritten : a.value);
                values[i] = a.at;
                break;
            case 'f':
            case 'F':
                if (!a.present) break;
                a.at = take(8);
                (void)mem.write_u64(a.at, a.value64);
                values[i] = a.at;
                break;
            default: break;
        }
    }

    // The call, through the same handler a local caller reaches (x64 ABI:
    // RCX, RDX, R8, R9, then stack slots above the 32-byte home area).
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = base + 0x100;
    (void)mem.write_u64(cpu.gpr.rsp, 0);
    u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
    for (usize i = 0; i < values.size(); ++i) {
        if (i < 4) *regs[i] = values[i];
        else (void)mem.write_u64(cpu.gpr.rsp + 8 + i * 8, values[i]);
    }
    (void)take_last_error_record();
    {
        Marshaler marshaler(mem);
        marshaler.attach_cpu(cpu);
        handler->second(marshaler);
    }
    const u32 ret = static_cast<u32>(cpu.gpr.rax);
    std::shared_ptr<Record> error = take_last_error_record();

    w.u32_(ret);
    if (sig->ret == 'H') m->encode_handle(w, ret);
    for (usize i = 0; i < args.size(); ++i) {
        Arg& a = args[i];
        switch (a.kind) {
            case 'R': {
                auto rec = a.temp_record != 0 ? m->mgr.handles().get<Record>(a.temp_record) : nullptr;
                w.u8_(rec ? 1 : 0);
                if (rec) w.record(*rec);
                break;
            }
            case 'B': {
                // What the handler wrote: up to and including the terminator,
                // within the capacity. The block was filled with 0xFF, so no
                // terminator means the handler wrote nothing (a size query
                // or a failure) and the caller's buffer stays untouched.
                std::vector<u8> out;
                bool terminated = false;
                for (u32 k = 0; a.present && k < a.cap; ++k) {
                    const u8 lo = mem.read_u8(a.at + k * unit).value_or(0);
                    const u8 hi = unit == 2 ? mem.read_u8(a.at + k * unit + 1).value_or(0) : 0;
                    out.push_back(lo);
                    if (unit == 2) out.push_back(hi);
                    if (lo == 0 && hi == 0) { terminated = true; break; }
                }
                w.u8_(terminated ? 1 : 0);
                if (terminated) w.bytes(out);
                break;
            }
            case 'n':
            case 'o':
                w.u8_(a.present ? 1 : 0);
                if (a.present) w.u32_(mem.read_u32(a.at).value_or(0));
                break;
            case 'f':
                w.u8_(a.present ? 1 : 0);
                if (a.present) w.u64_(mem.read_u64(a.at).value_or(0));
                break;
            case 'H': {
                w.u8_(a.present ? 1 : 0);
                if (!a.present) break;
                const u32 v = mem.read_u32(a.at).value_or(kUnwritten);
                if (v == kUnwritten) w.u8_(kHandleUnchanged);
                else m->encode_handle(w, v);
                break;
            }
            default: break;
        }
    }
    for (const Arg& a : args)
        if (a.temp_record != 0) (void)m->mgr.handles().close(a.temp_record);
    w.u8_(error ? 1 : 0);
    if (error) w.record(*error);
    m->host.free_scratch(base);
    return std::move(w.data());
}

std::optional<std::vector<u8>> serve_custom_action(RemoteChannel& channel, RelayExecutor& executor,
                                                   const std::function<void()>& idle, int idle_ms) {
    for (;;) {
        auto frame = channel.receive(idle ? idle_ms : -1);
        if (!frame) {
            if (channel.timed_out()) {
                idle();
                continue;
            }
            return std::nullopt;
        }
        switch (frame->kind) {
            case Wire::Call:
                if (!channel.send(Wire::CallReply, executor.call(frame->body))) return std::nullopt;
                break;
            case Wire::CloseHandle:
                if (!channel.send(Wire::CloseReply, executor.close(frame->body))) return std::nullopt;
                break;
            case Wire::Done:
                return std::move(frame->body);
            default:
                return std::nullopt;  // protocol violation: the server is not ours
        }
    }
}

}  // namespace vitreo::winapi::msi
