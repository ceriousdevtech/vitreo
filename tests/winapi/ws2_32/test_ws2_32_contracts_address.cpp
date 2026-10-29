/// @file test_ws2_32_contracts_address.cpp
/// @brief WSAAddressToStringA/W replayed against Windows' own output.
///
/// Every row of tools/oracle_probe/ws2_address_to_string_probe.c is rebuilt
/// here from the same inputs, printed in the probe's format, and compared with
/// the probe's .expected file for this bitness (x64 / x86). The expected text
/// is Windows', never Vitreo's.

#include "ws2_32_contract_fixture.hpp"

#include <array>
#include <cstdio>
#include <fstream>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::ws2_contract_test;

namespace {

constexpr GuestAddr kSockaddr = kData + 0x1000;
constexpr GuestAddr kAnsiBuf = kData + 0x2000;
constexpr GuestAddr kWideBuf = kData + 0x3000;
constexpr GuestAddr kLen = kData + 0x4000;

using Words = std::array<u16, 8>;

}  // namespace

TEST_P(Ws2ContractTest, WSAAddressToStringMatchesWindows) {
    std::vector<std::string> got;
    auto set_wsa_error = [&](u32 e) { (void)call("WSASetLastError", {e}); };
    auto read_text = [&](GuestAddr at, bool wide) {
        std::string out;
        for (u32 i = 0; i < 255; ++i) {
            const u32 c = wide ? mem_.read_u16(at + 2 * i).value_or(0) : mem_.read_u8(at + i).value_or(0);
            if (c == 0) break;
            out += static_cast<char>(c < 0x80 ? c : '?');
        }
        return out;
    };
    // One A call and one W call on the same sockaddr bytes and buffer length,
    // printed as the probe prints them.
    auto run = [&](const std::string& label, const std::vector<u8>& sa, u32 sa_len, u32 buf_len,
                   bool null_buf) {
        for (usize i = 0; i < 32; ++i) (void)mem_.write_u8(kSockaddr + i, i < sa.size() ? sa[i] : 0);
        for (bool wide : {false, true}) {
            const GuestAddr buf = wide ? kWideBuf : kAnsiBuf;
            for (u32 i = 0; i < 256; ++i) {
                if (wide) (void)mem_.write_u16(buf + 2 * i, 0x5A5A);
                else (void)mem_.write_u8(buf + i, 0x5A);
            }
            (void)mem_.write_u32(kLen, buf_len);
            set_wsa_error(0xDEAD);
            const i32 ret = static_cast<i32>(call(wide ? "WSAAddressToStringW" : "WSAAddressToStringA",
                                                  {kSockaddr, sa_len, 0, null_buf ? 0 : buf, kLen}));
            const u32 err = wsa_error();
            char line[512];
            std::snprintf(line, sizeof(line), "%s %c ret=%d err=%u len=%u", label.c_str(),
                          wide ? 'W' : 'A', ret, err, mem_.read_u32(kLen).value_or(0));
            std::string text = line;
            if (ret == 0 && !null_buf) {
                text += " \"" + read_text(buf, wide) + "\"";
            } else if (!null_buf) {
                char b[16];
                if (wide) std::snprintf(b, sizeof(b), " buf0=%04x", mem_.read_u16(buf).value_or(0));
                else std::snprintf(b, sizeof(b), " buf0=%02x", mem_.read_u8(buf).value_or(0));
                text += b;
            }
            got.push_back(text);
        }
    };
    auto v6 = [&](const std::string& label, const Words& w, u32 scope, u16 port, u32 flow,
                  u32 sa_len, u32 buf_len) {
        std::vector<u8> sa(28, 0);
        sa[0] = 23;
        sa[2] = static_cast<u8>(port >> 8);
        sa[3] = static_cast<u8>(port);
        for (int i = 0; i < 4; ++i) sa[4 + i] = static_cast<u8>(flow >> (24 - 8 * i));
        for (int i = 0; i < 8; ++i) {
            sa[8 + 2 * i] = static_cast<u8>(w[i] >> 8);
            sa[9 + 2 * i] = static_cast<u8>(w[i]);
        }
        for (int i = 0; i < 4; ++i) sa[24 + i] = static_cast<u8>(scope >> (8 * i));
        run(label, sa, sa_len, buf_len, false);
    };
    auto v4 = [&](const std::string& label, u32 addr, u16 port, u32 sa_len, u32 buf_len,
                  bool zero_pad, bool null_buf) {
        std::vector<u8> sa(32, zero_pad ? 0 : 0x77);
        sa[0] = 2;
        sa[1] = 0;
        sa[2] = static_cast<u8>(port >> 8);
        sa[3] = static_cast<u8>(port);
        for (int i = 0; i < 4; ++i) sa[4 + i] = static_cast<u8>(addr >> (24 - 8 * i));
        run(label, sa, sa_len, buf_len, null_buf);
    };

    v4("v4 no-startup", 0xC0000201, 80, 16, 64, true, false);
    startup();

    v4("v4 port0", 0xC0000201, 0, 16, 64, true, false);
    v4("v4 port80", 0xC0000201, 80, 16, 64, true, false);
    v4("v4 port65535", 0x0A000001, 65535, 16, 64, true, false);
    v4("v4 any", 0, 0, 16, 64, true, false);
    v4("v4 nonzero-pad", 0xC0000201, 80, 16, 64, false, false);
    v4("v4 len15", 0xC0000201, 80, 15, 64, true, false);
    v4("v4 len17", 0xC0000201, 80, 17, 64, true, false);
    v4("v4 len28", 0xC0000201, 80, 28, 64, true, false);
    v4("v4 len0", 0xC0000201, 80, 0, 64, true, false);
    v4("v4 buf13", 0xC0000201, 80, 16, 13, true, false);
    v4("v4 buf12", 0xC0000201, 80, 16, 12, true, false);
    v4("v4 buf0", 0xC0000201, 80, 16, 0, true, false);
    v4("v4 nullbuf0", 0xC0000201, 80, 16, 0, true, true);
    v4("v4 nullbuf64", 0xC0000201, 80, 16, 64, true, true);

    const Words loopback{0, 0, 0, 0, 0, 0, 0, 1};
    const Words any{};
    const Words link1{0xfe80, 0, 0, 0, 0, 0, 0, 1};
    const Words doc1{0x2001, 0xdb8, 0, 0, 0, 0, 0, 1};
    v6("v6 loopback", loopback, 0, 0, 0, 28, 64);
    v6("v6 any", any, 0, 0, 0, 28, 64);
    v6("v6 any port", any, 0, 443, 0, 28, 64);
    v6("v6 link", link1, 0, 0, 0, 28, 64);
    v6("v6 link scope", link1, 7, 0, 0, 28, 64);
    v6("v6 link scope port", link1, 7, 443, 0, 28, 64);
    v6("v6 doc port", doc1, 0, 8080, 0, 28, 64);
    v6("v6 tworuns", {0x2001, 0xdb8, 0, 0, 1, 0, 0, 1}, 0, 0, 0, 28, 64);
    v6("v6 longer2nd", {0x2001, 0, 0, 1, 0, 0, 0, 1}, 0, 0, 0, 28, 64);
    v6("v6 single0", {0x2001, 0xdb8, 0, 1, 1, 1, 1, 1}, 0, 0, 0, 28, 64);
    v6("v6 trailing", {0x2001, 0xdb8, 0xabcd, 0x12, 0, 0, 0, 0}, 0, 0, 0, 28, 64);
    v6("v6 leading", {0, 0, 0, 0, 0, 0, 0x1234, 0x5678}, 0, 0, 0, 28, 64);
    const Words mapped{0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201};
    v6("v6 mapped", mapped, 0, 0, 0, 28, 64);
    v6("v6 mapped port", mapped, 0, 80, 0, 28, 64);
    v6("v6 compat", {0, 0, 0, 0, 0, 0, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 translated", {0, 0, 0, 0, 0xffff, 0, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 isatap", {0xfe80, 0, 0, 0, 0, 0x5efe, 0xc000, 0x0201}, 3, 0, 0, 28, 64);
    v6("v6 isatap200", {0xfe80, 0, 0, 0, 0x200, 0x5efe, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 nat64", {0x64, 0xff9b, 0, 0, 0, 0, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 full", {0x2001, 0xdb8, 0x85a3, 0x8d3, 0x1319, 0x8a2e, 0x370, 0x7348}, 0, 0, 0, 28, 64);
    v6("v6 teredo", {0x2001, 0, 0x4136, 0xe378, 0x8000, 0x63bf, 0x3fff, 0xfdd2}, 0, 0, 0, 28, 64);
    v6("v6 last2", {0, 0, 0, 0, 0, 0, 0, 2}, 0, 0, 0, 28, 64);
    v6("v6 lastffff", {0, 0, 0, 0, 0, 0, 0, 0xffff}, 0, 0, 0, 28, 64);
    v6("v6 w6only", {0, 0, 0, 0, 0, 0, 1, 0}, 0, 0, 0, 28, 64);
    v6("v6 mapped0", {0, 0, 0, 0, 0, 0xffff, 0, 0}, 0, 0, 0, 28, 64);
    v6("v6 translated0", {0, 0, 0, 0, 0xffff, 0, 0, 0}, 0, 0, 0, 28, 64);
    v6("v6 isatapglobal", {0x2001, 0xdb8, 0, 0, 0, 0x5efe, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 isatap100", {0xfe80, 0, 0, 0, 0x100, 0x5efe, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 isatap300", {0xfe80, 0, 0, 0, 0x300, 0x5efe, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 first1", {1, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 28, 64);
    v6("v6 notmapped", {0, 0, 0, 0, 1, 0xffff, 0xc000, 0x0201}, 0, 0, 0, 28, 64);
    v6("v6 allf", {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff}, 0, 0, 0, 28, 64);
    v6("v6 mappedlow", {0, 0, 0, 0, 0, 0xffff, 0, 5}, 0, 0, 0, 28, 64);
    v6("v6 translatedlow", {0, 0, 0, 0, 0xffff, 0, 0, 5}, 0, 0, 0, 28, 64);
    v6("v6 isataplow", {0xfe80, 0, 0, 0, 0, 0x5efe, 0, 5}, 0, 0, 0, 28, 64);
    v6("v6 isatapzero", {0xfe80, 0, 0, 0, 0, 0x5efe, 0, 0}, 0, 0, 0, 28, 64);
    v6("v6 flow", doc1, 0, 80, 0x12345, 28, 64);
    v6("v6 bigscope", link1, 0xFFFFFFFF, 65535, 0, 28, 64);
    v6("v6 len24 scope", link1, 7, 0, 0, 24, 64);
    v6("v6 len27", link1, 7, 0, 0, 27, 64);
    v6("v6 len23", link1, 7, 0, 0, 23, 64);
    v6("v6 len29", link1, 7, 0, 0, 29, 64);
    v6("v6 len16", link1, 7, 0, 0, 16, 64);
    v6("v6 buf16", link1, 7, 443, 0, 28, 16);
    v6("v6 buf15", link1, 7, 443, 0, 28, 15);

    std::vector<u8> raw(32, 0);
    run("af0 len16", raw, 16, 64, false);
    raw[0] = 99;
    run("af99 len16", raw, 16, 64, false);
    raw[0] = 23;
    run("af23 len16", raw, 16, 64, false);
    raw[0] = 2;
    run("af2 len28", raw, 28, 64, false);

    char line[128];
    (void)mem_.write_u32(kLen, 64);
    set_wsa_error(0xDEAD);
    i32 ret = static_cast<i32>(call("WSAAddressToStringA", {0, 16, 0, kAnsiBuf, kLen}));
    std::snprintf(line, sizeof(line), "null-addr A ret=%d err=%u len=%u", ret, wsa_error(),
                  mem_.read_u32(kLen).value_or(0));
    got.emplace_back(line);
    std::vector<u8> sin(16, 0);
    sin[0] = 2;
    for (usize i = 0; i < sin.size(); ++i) (void)mem_.write_u8(kSockaddr + i, sin[i]);
    for (bool wide : {false, true}) {
        set_wsa_error(0xDEAD);
        ret = static_cast<i32>(call(wide ? "WSAAddressToStringW" : "WSAAddressToStringA",
                                    {kSockaddr, 16, 0, wide ? kWideBuf : kAnsiBuf, 0}));
        std::snprintf(line, sizeof(line), "null-len %c ret=%d err=%u", wide ? 'W' : 'A', ret,
                      wsa_error());
        got.emplace_back(line);
    }

    const std::string path = std::string(VITREO_ORACLE_PROBE_DIR) +
                             (is64() ? "/ws2_address_to_string_probe.expected"
                                     : "/ws2_address_to_string_probe.x86.expected");
    std::ifstream in(path);
    ASSERT_TRUE(in.good()) << path;
    std::vector<std::string> want;
    for (std::string l; std::getline(in, l);) want.push_back(l);
    ASSERT_EQ(got.size(), want.size());
    for (usize i = 0; i < want.size(); ++i) EXPECT_EQ(got[i], want[i]) << "row " << i;
}
