/// @file test_wldap32.cpp
/// @brief Wldap32.dll synchronous LDAP client.
///
/// Expected values: export ordinals, ldap_err2string text and the
/// no-server / NULL-argument results come from
/// tools/oracle_probe/wldap32_contract_probe.{x86,x64}.expected; the search
/// walk is checked against what a loopback LDAP responder (built with
/// liblber, RFC 4511 encodings) sent.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/networking/wldap32/wldap32.hpp>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// libldap is the system LDAP client; its SDK headers mark every symbol
// deprecated. Pushed for the whole file, popped at the end.
#include <lber.h>
#include <ldap.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 LDAP_SUCCESS_W = 0x00;
constexpr u32 LDAP_UNAVAILABLE_W = 0x34;
constexpr u32 LDAP_SERVER_DOWN_W = 0x51;
constexpr u32 LDAP_LOCAL_ERROR_W = 0x52;
constexpr u32 LDAP_PARAM_ERROR_W = 0x59;

std::string expected_path(bool x64) {
    return std::string(VITREO_SOURCE_DIR) + "/tools/oracle_probe/wldap32_contract_probe." +
           (x64 ? "x64" : "x86") + ".expected";
}

std::vector<std::string> expected_lines(bool x64) {
    std::ifstream in(expected_path(x64));
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return lines;
}

/// One loopback LDAP server connection: answers a bind and one search with
/// a single entry, then waits for the unbind.
class FakeLdapServer {
public:
    FakeLdapServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        (void)::listen(fd_, 1);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeLdapServer() {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        if (thread_.joinable()) thread_.join();
    }
    u16 port() const { return port_; }
    std::string last_bind_dn() const { return bind_dn_; }

private:
    static bool read_full(int c, unsigned char* p, size_t n) {
        while (n) {
            const ssize_t r = ::read(c, p, n);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }
    static void send_ber(int c, BerElement* ber) {
        struct berval* bv = nullptr;
        if (ber_flatten(ber, &bv) == 0) {
            (void)::write(c, bv->bv_val, bv->bv_len);
            ber_bvfree(bv);
        }
        ber_free(ber, 1);
    }
    void serve() {
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return;
        for (;;) {
            unsigned char hdr[2];
            if (!read_full(c, hdr, 2)) break;
            std::vector<unsigned char> msg(hdr, hdr + 2);
            size_t len = hdr[1];
            if (len & 0x80) {
                const size_t n = len & 0x7f;
                std::vector<unsigned char> lb(n);
                if (!read_full(c, lb.data(), n)) break;
                msg.insert(msg.end(), lb.begin(), lb.end());
                len = 0;
                for (unsigned char b : lb) len = (len << 8) | b;
            }
            const size_t off = msg.size();
            msg.resize(off + len);
            if (!read_full(c, msg.data() + off, len)) break;
            struct berval in{static_cast<ber_len_t>(msg.size()), reinterpret_cast<char*>(msg.data())};
            BerElement* req = ber_init(&in);
            ber_int_t msgid = 0;
            ber_len_t oplen = 0;
            (void)ber_scanf(req, "{i", &msgid);
            const ber_tag_t op = ber_peek_tag(req, &oplen);
            if (op == LDAP_REQ_BIND) {
                ber_int_t version = 0;
                char* dn = nullptr;
                (void)ber_scanf(req, "{ia", &version, &dn);
                if (dn) { bind_dn_ = dn; ber_memfree(dn); }
            }
            ber_free(req, 1);
            if (op == LDAP_REQ_UNBIND) break;
            if (op == LDAP_REQ_BIND) {
                BerElement* r = ber_alloc_t(LBER_USE_DER);
                (void)ber_printf(r, "{it{ess}}", msgid, LDAP_RES_BIND, 0, "", "");
                send_ber(c, r);
            } else if (op == LDAP_REQ_SEARCH) {
                static const char bin[] = {'x', 0, 'y'};
                BerElement* e = ber_alloc_t(LBER_USE_DER);
                (void)ber_printf(e, "{it{s{{s[s]}{s[so]}}}}", msgid, LDAP_RES_SEARCH_ENTRY,
                                 "cn=cat,dc=stray", "cn", "cat", "mail", "a@b", bin,
                                 static_cast<ber_len_t>(sizeof bin));
                send_ber(c, e);
                BerElement* d = ber_alloc_t(LBER_USE_DER);
                (void)ber_printf(d, "{it{ess}}", msgid, LDAP_RES_SEARCH_RESULT, 0, "", "");
                send_ber(c, d);
            }
        }
        ::close(c);
    }

    int fd_ = -1;
    u16 port_ = 0;
    std::string bind_dn_;
    std::thread thread_;
};

class Wldap32Test : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA = 0x1000;
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        mgr.set_heap_manager(&heap);
        exports = create_wldap32_thunks(mgr);
    }
    bool x64() const { return GetParam(); }
    u32 ptr_size() const { return x64() ? 8 : 4; }

    u64 call(const char* name, std::initializer_list<u64> args) {
        usize i = 0;
        for (u64 v : args) {
            if (x64()) {
                switch (i) {
                case 0: cpu.gpr.rcx = v; break;
                case 1: cpu.gpr.rdx = v; break;
                case 2: cpu.gpr.r8 = v; break;
                case 3: cpu.gpr.r9 = v; break;
                default: (void)memory.write_u64(cpu.gpr.rsp + 8 + 8 * i, v); break;
                }
            } else {
                (void)memory.write_u32(cpu.gpr.rsp + 4 + 4 * i, static_cast<u32>(v));
            }
            ++i;
        }
        for (auto& e : exports.entries) {
            if (e.name == name) { e.handler(marshaler); return x64() ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax); }
        }
        ADD_FAILURE() << name;
        return ~0ull;
    }
    GuestAddr str(GuestAddr at, const std::string& s) {
        EXPECT_TRUE(marshaler.write_ansi_string(at, s).is_ok());
        return at;
    }
    GuestAddr wstr(GuestAddr at, const std::string& s) {
        EXPECT_TRUE(marshaler.write_wide_string(at, s).is_ok());
        return at;
    }
    u64 read_ptr(GuestAddr at) {
        return x64() ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    std::string ansi(GuestAddr p) { return marshaler.read_ansi_string(p).value(); }
    std::string wide(GuestAddr p) { return marshaler.read_wide_string(p).value(); }
    u32 ld_errno(GuestAddr ld) { return memory.read_u32(ld + (x64() ? 116 : 100)).value(); }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    Wldap32Manager mgr;
    DllExports exports;
};

}  // namespace

TEST_P(Wldap32Test, ExportOrdinalsMatchWindows) {
    std::map<std::string, u16> windows;
    for (const auto& l : expected_lines(x64())) {
        std::istringstream is(l);
        std::string tag, name;
        unsigned ord = 0;
        if ((is >> tag >> name >> ord) && tag == "EXPORT") windows[name] = static_cast<u16>(ord);
    }
    ASSERT_GT(windows.size(), 200u) << expected_path(x64());
    for (const auto& e : exports.entries) {
        ASSERT_TRUE(windows.count(e.name)) << e.name;
        EXPECT_EQ(e.ordinal, windows[e.name]) << e.name;
    }
}

TEST_P(Wldap32Test, Err2StringMatchesWindowsTextAndIsStatic) {
    int checked = 0;
    for (const auto& l : expected_lines(x64())) {
        unsigned code = 0;
        char text[128] = {};
        if (std::sscanf(l.c_str(), "ERR2STR 0x%x \"%127[^\"]\"", &code, text) >= 1) {
            const GuestAddr a = static_cast<GuestAddr>(call("ldap_err2string", {code}));
            ASSERT_NE(a, 0u);
            EXPECT_EQ(ansi(a), text) << code;
            EXPECT_EQ(wide(static_cast<GuestAddr>(call("ldap_err2stringW", {code}))), text) << code;
            ++checked;
        }
    }
    EXPECT_EQ(checked, 0x62);
    // ERR2STR_BIG / ERR2STR_NEG / ERR2STR_SAME_PTR
    EXPECT_EQ(ansi(static_cast<GuestAddr>(call("ldap_err2string", {0x62}))), "");
    EXPECT_EQ(ansi(static_cast<GuestAddr>(call("ldap_err2stringA", {0xFFFFFFFFu}))), "");
    EXPECT_EQ(call("ldap_err2string", {0x51}), call("ldap_err2string", {0x51}));
}

TEST_P(Wldap32Test, NoServerSessionMatchesWindows) {
    const GuestAddr host = str(DATA, "127.0.0.1");
    // INIT ld_nonnull=1 ldgle=0; FIELDS_FRESH version=2 refhop=32 options=0x63 host=NULL
    const GuestAddr ld = static_cast<GuestAddr>(call("ldap_initA", {host, 1}));
    ASSERT_NE(ld, 0u);
    EXPECT_EQ(call("LdapGetLastError", {}), 0u);
    EXPECT_EQ(read_ptr(ld + (x64() ? 88 : 76)), 0u);
    EXPECT_EQ(memory.read_u32(ld + (x64() ? 96 : 80)).value(), 2u);
    EXPECT_EQ(memory.read_u32(ld + (x64() ? 176 : 152)).value(), 32u);
    EXPECT_EQ(memory.read_u32(ld + (x64() ? 180 : 156)).value(), 0x63u);

    const GuestAddr v = DATA + 0x100;
    (void)memory.write_u32(v, 0);
    EXPECT_EQ(call("ldap_get_option", {ld, 0x11, v}), 0u);  // GET_OPT_VERSION v=2
    EXPECT_EQ(memory.read_u32(v).value(), 2u);
    (void)memory.write_u32(v, 3);
    EXPECT_EQ(call("ldap_set_option", {ld, 0x11, v}), 0u);  // SET_OPT_VERSION3
    EXPECT_EQ(memory.read_u32(ld + (x64() ? 96 : 80)).value(), 2u) << "public field is not mirrored";

    EXPECT_EQ(call("ldap_simple_bind_sA", {ld, 0, 0}), LDAP_SERVER_DOWN_W);  // BIND_NOSERVER
    EXPECT_EQ(call("LdapGetLastError", {}), LDAP_SERVER_DOWN_W);
    EXPECT_EQ(ld_errno(ld), LDAP_SERVER_DOWN_W);

    const GuestAddr res = DATA + 0x200;
    (void)memory.write_u64(res, 1);
    EXPECT_EQ(call("ldap_search_sA", {ld, str(DATA + 0x300, ""), 0, str(DATA + 0x340, "(objectClass=*)"),
                                      0, 0, res}),
              LDAP_UNAVAILABLE_W);  // SEARCH_NOSERVER r=0x34 res_null=1
    EXPECT_EQ(read_ptr(res), 0u);
    EXPECT_EQ(call("ldap_unbind_s", {ld}), 0u);
}

TEST_P(Wldap32Test, SearchFirstThenBindReportsServerDownThenUnavailable) {
    const GuestAddr ld = static_cast<GuestAddr>(call("ldap_init", {str(DATA, "127.0.0.1"), 1}));
    ASSERT_NE(ld, 0u);
    const GuestAddr res = DATA + 0x200;
    EXPECT_EQ(call("ldap_search_s", {ld, str(DATA + 0x300, ""), 0, str(DATA + 0x340, "(objectClass=*)"),
                                     0, 0, res}),
              LDAP_SERVER_DOWN_W);  // SEARCH_NOBIND
    EXPECT_EQ(read_ptr(res), 0u);
    EXPECT_EQ(ld_errno(ld), LDAP_SERVER_DOWN_W);
    EXPECT_EQ(call("ldap_simple_bind_s", {ld, 0, 0}), LDAP_UNAVAILABLE_W);  // BIND_AFTER_SEARCH
    EXPECT_EQ(ld_errno(ld), LDAP_UNAVAILABLE_W);
    EXPECT_EQ(call("ldap_unbind_s", {ld}), 0u);
}

TEST_P(Wldap32Test, OptionsMatchWindows) {
    const GuestAddr ld = static_cast<GuestAddr>(call("ldap_init", {str(DATA, "127.0.0.1"), 1}));
    ASSERT_NE(ld, 0u);
    (void)call("ldap_simple_bind_s", {ld, 0, 0});  // LdapGetLastError = 0x51
    const GuestAddr v = DATA + 0x100;
    (void)memory.write_u32(v, 5);
    EXPECT_EQ(call("ldap_set_option", {ld, 0x7777, v}), LDAP_PARAM_ERROR_W);  // SET_UNKNOWN
    EXPECT_EQ(call("LdapGetLastError", {}), LDAP_SERVER_DOWN_W) << "option failures leave it";
    EXPECT_EQ(call("ldap_get_option", {ld, 0x7777, v}), LDAP_PARAM_ERROR_W);  // GET_UNKNOWN
    EXPECT_EQ(call("ldap_set_option", {ld, 0x08, 1}), 0u);  // SET_REFERRALS_ON (LDAP_OPT_ON)
    (void)memory.write_u32(v, 0xFFFF);
    (void)call("ldap_get_option", {ld, 0x08, v});
    EXPECT_EQ(memory.read_u32(v).value(), 1u);
    (void)memory.write_u32(v, 0);
    EXPECT_EQ(call("ldap_set_option", {ld, 0x08, v}), 0u);  // SET_REFERRALS_PTR0
    (void)memory.write_u32(v, 0xFFFF);
    (void)call("ldap_get_option", {ld, 0x08, v});
    EXPECT_EQ(memory.read_u32(v).value(), 0u);
    (void)memory.write_u32(v, 7);
    EXPECT_EQ(call("ldap_set_option", {ld, 0x11, v}), LDAP_LOCAL_ERROR_W);  // SET_VERSION7
    (void)memory.write_u32(v, 42);
    EXPECT_EQ(call("ldap_set_option", {ld, 0x03, v}), 0u);  // SET_SIZELIMIT
    EXPECT_EQ(memory.read_u32(ld + (x64() ? 112 : 96)).value(), 0u) << "ld_sizelimit not mirrored";
    // Learn: options 0x05-0x07, 0x09, 0x0B, 0x0D-0x0F are LDAP_LOCAL_ERROR.
    for (u32 opt : {0x05u, 0x06u, 0x07u, 0x09u, 0x0Bu, 0x0Du, 0x0Eu, 0x0Fu}) {
        EXPECT_EQ(call("ldap_set_option", {ld, opt, v}), LDAP_LOCAL_ERROR_W) << opt;
        EXPECT_EQ(call("ldap_get_option", {ld, opt, v}), LDAP_LOCAL_ERROR_W) << opt;
    }
    const GuestAddr out = DATA + 0x180;
    (void)memory.write_u64(out, 0x1234);
    EXPECT_EQ(call("ldap_get_option", {ld, 0x30, out}), 0u);  // GET_HOSTNAME (null)
    EXPECT_EQ(read_ptr(out), 0u);
    EXPECT_EQ(call("ldap_set_option", {0, 0x03, v}), LDAP_PARAM_ERROR_W);  // SET_OPT_NULL_LD
    EXPECT_EQ(call("ldap_unbind_s", {ld}), 0u);

    // SSLINIT_FIELDS ssl=0: not secure until a connection exists.
    const GuestAddr ls = static_cast<GuestAddr>(call("ldap_sslinitA", {str(DATA, "127.0.0.1"), 0, 1}));
    ASSERT_NE(ls, 0u);
    (void)memory.write_u32(v, 9);
    EXPECT_EQ(call("ldap_get_option", {ls, 0x0A, v}), 0u);
    EXPECT_EQ(memory.read_u32(v).value(), 0u);
    EXPECT_EQ(call("ldap_unbind_s", {ls}), 0u);
}

TEST_P(Wldap32Test, NullArgumentsMatchWindows) {
    EXPECT_EQ(call("ldap_unbind_s", {0}), LDAP_PARAM_ERROR_W);    // UNBIND_NULL
    EXPECT_EQ(call("ldap_msgfree", {0}), 0u);                      // MSGFREE_NULL
    EXPECT_EQ(call("ldap_value_free_len", {0}), 0u);               // VALUE_FREE_LEN_NULL
    EXPECT_EQ(call("ldap_first_entry", {0, 0}), 0u);               // FIRST_ENTRY_NULL
    EXPECT_EQ(call("LdapGetLastError", {}), LDAP_PARAM_ERROR_W);
    (void)call("ldap_memfree", {0});                               // FREE_NULL_OK
    (void)call("ber_free", {0, 0});
    EXPECT_EQ(call("ldap_simple_bind_s", {0, 0, 0}), LDAP_PARAM_ERROR_W);  // SIMPLE_BIND_NULL_LD
    const GuestAddr res = DATA + 0x200;
    (void)memory.write_u64(res, 1);
    EXPECT_EQ(call("ldap_search_s", {0, str(DATA + 0x300, ""), 0, str(DATA + 0x340, "(a=*)"), 0, 0, res}),
              LDAP_PARAM_ERROR_W);  // SEARCH_NULL_LD res_null=0
    EXPECT_EQ(read_ptr(res), 1u);
    EXPECT_EQ(call("ldap_get_dnA", {0, 0}), 0u);                   // GET_DN_NULL
    EXPECT_EQ(call("LdapGetLastError", {}), LDAP_PARAM_ERROR_W);
    EXPECT_EQ(call("ldap_first_attributeA", {0, 0, DATA + 0x380}), 0u);  // FIRST_ATTR_NULL
    EXPECT_EQ(call("ldap_get_values_lenA", {0, 0, str(DATA + 0x3C0, "x")}), 0u);  // GET_VALUES_LEN_NULL
    EXPECT_EQ(call("LdapGetLastError", {}), LDAP_PARAM_ERROR_W);
}

TEST_P(Wldap32Test, NullHostNameHasNoDefaultServer) {
    // INIT_NULLHOST ld_nonnull=1, NULLHOST_BIND r=0x51 (not a domain member)
    const GuestAddr ld = static_cast<GuestAddr>(call("ldap_init", {0, 389}));
    ASSERT_NE(ld, 0u);
    EXPECT_EQ(call("ldap_simple_bind_s", {ld, 0, 0}), LDAP_SERVER_DOWN_W);
    EXPECT_EQ(call("ldap_unbind_s", {ld}), 0u);
}

TEST_P(Wldap32Test, SearchWalksEntriesAttributesAndValues) {
    FakeLdapServer server;
    const std::string host = "127.0.0.1:" + std::to_string(server.port());
    // The ":port" in HostName overrides PortNumber (Learn ldap_init).
    const GuestAddr ld = static_cast<GuestAddr>(call("ldap_initW", {wstr(DATA, host), 1}));
    ASSERT_NE(ld, 0u);
    const GuestAddr v = DATA + 0x100;
    (void)memory.write_u32(v, 3);
    ASSERT_EQ(call("ldap_set_option", {ld, 0x11, v}), 0u);
    ASSERT_EQ(call("ldap_simple_bind_sW", {ld, wstr(DATA + 0x400, "cn=reader"), wstr(DATA + 0x480, "pw")}),
              LDAP_SUCCESS_W);
    EXPECT_EQ(server.last_bind_dn(), "cn=reader");

    const GuestAddr res = DATA + 0x200;
    ASSERT_EQ(call("ldap_search_s", {ld, str(DATA + 0x300, "dc=stray"), 2,
                                     str(DATA + 0x340, "(objectClass=*)"), 0, 0, res}),
              LDAP_SUCCESS_W);
    const GuestAddr head = static_cast<GuestAddr>(read_ptr(res));
    ASSERT_NE(head, 0u);

    const GuestAddr entry = static_cast<GuestAddr>(call("ldap_first_entry", {ld, head}));
    ASSERT_EQ(entry, head);
    // LDAPMessage: lm_msgtype, lm_chain -> the SearchResultDone message.
    EXPECT_EQ(memory.read_u32(entry + 4).value(), static_cast<u32>(LDAP_RES_SEARCH_ENTRY));
    const GuestAddr done = static_cast<GuestAddr>(read_ptr(entry + (x64() ? 16 : 12)));
    ASSERT_NE(done, 0u);
    EXPECT_EQ(memory.read_u32(done + 4).value(), static_cast<u32>(LDAP_RES_SEARCH_RESULT));
    EXPECT_EQ(read_ptr(entry + (x64() ? 40 : 24)), ld);

    const GuestAddr dn = static_cast<GuestAddr>(call("ldap_get_dn", {ld, entry}));
    EXPECT_EQ(ansi(dn), "cn=cat,dc=stray");
    (void)call("ldap_memfree", {dn});
    const GuestAddr dnw = static_cast<GuestAddr>(call("ldap_get_dnW", {ld, entry}));
    EXPECT_EQ(wide(dnw), "cn=cat,dc=stray");
    (void)call("ldap_memfreeW", {dnw});

    const GuestAddr ber_slot = DATA + 0x500;
    const GuestAddr a1 = static_cast<GuestAddr>(call("ldap_first_attribute", {ld, entry, ber_slot}));
    EXPECT_EQ(ansi(a1), "cn");
    const GuestAddr ber = static_cast<GuestAddr>(read_ptr(ber_slot));
    ASSERT_NE(ber, 0u);
    const GuestAddr vals = static_cast<GuestAddr>(call("ldap_get_values_len", {ld, entry, a1}));
    ASSERT_NE(vals, 0u);
    const GuestAddr bv0 = static_cast<GuestAddr>(read_ptr(vals));
    EXPECT_EQ(read_ptr(vals + ptr_size()), 0u) << "NULL-terminated";
    EXPECT_EQ(memory.read_u32(bv0).value(), 3u);
    EXPECT_EQ(ansi(static_cast<GuestAddr>(read_ptr(bv0 + ptr_size()))), "cat");
    EXPECT_EQ(call("ldap_value_free_len", {vals}), 0u);
    (void)call("ldap_memfree", {a1});

    const GuestAddr a2 = static_cast<GuestAddr>(call("ldap_next_attributeW", {ld, entry, ber}));
    EXPECT_EQ(wide(a2), "mail");
    const GuestAddr mv = static_cast<GuestAddr>(call("ldap_get_values_lenW", {ld, entry, a2}));
    ASSERT_NE(mv, 0u);
    const GuestAddr m0 = static_cast<GuestAddr>(read_ptr(mv));
    const GuestAddr m1 = static_cast<GuestAddr>(read_ptr(mv + ptr_size()));
    EXPECT_EQ(read_ptr(mv + 2 * ptr_size()), 0u);
    EXPECT_EQ(memory.read_u32(m0).value(), 3u);
    EXPECT_EQ(ansi(static_cast<GuestAddr>(read_ptr(m0 + ptr_size()))), "a@b");
    EXPECT_EQ(memory.read_u32(m1).value(), 3u) << "binary value keeps its embedded NUL";
    const GuestAddr m1v = static_cast<GuestAddr>(read_ptr(m1 + ptr_size()));
    EXPECT_EQ(memory.read_u8(m1v).value(), 'x');
    EXPECT_EQ(memory.read_u8(m1v + 1).value(), 0);
    EXPECT_EQ(memory.read_u8(m1v + 2).value(), 'y');
    EXPECT_EQ(call("ldap_value_free_len", {mv}), 0u);
    (void)call("ldap_memfreeW", {a2});

    EXPECT_EQ(call("ldap_next_attribute", {ld, entry, ber}), 0u);
    EXPECT_EQ(ld_errno(ld), 0u) << "end of attributes is not an error";
    (void)call("ber_free", {ber, 0});

    // Learn: an attribute with no values is NULL with the session error 0.
    EXPECT_EQ(call("ldap_get_values_len", {ld, entry, str(DATA + 0x600, "telephoneNumber")}), 0u);
    EXPECT_EQ(ld_errno(ld), 0u);

    EXPECT_EQ(call("ldap_next_entry", {ld, entry}), 0u);
    EXPECT_EQ(call("ldap_msgfree", {head}), 0u);
    EXPECT_EQ(call("ldap_first_entry", {ld, head}), 0u) << "freed chain is no longer a result";
    EXPECT_EQ(call("ldap_unbind_s", {ld}), 0u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, Wldap32Test, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

#pragma clang diagnostic pop
