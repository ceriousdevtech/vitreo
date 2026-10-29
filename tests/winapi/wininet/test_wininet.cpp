/// @file test_wininet.cpp
/// @brief Focused WinINet contract tests.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/networking/wininet/wininet.hpp"
#include "winapi/networking/winhttp/url_components.hpp"

#include <set>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

class WinInetTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kBase = 0x10000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kBase, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        marshaler.attach_cpu(cpu);
        exports = create_wininet_thunks(manager);
    }

    u64 call_args(const char* name, bool wide, std::initializer_list<u64> args) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x1F000;
        u64* registers[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
        unsigned index = 0;
        for (u64 value : args) {
            if (wide && index < 4) *registers[index] = value;
            else if (wide) (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (index - 4) * 8, value);
            else (void)memory.write_u32(cpu.gpr.rsp + 4 + index * 4, static_cast<u32>(value));
            ++index;
        }
        find_export(exports, name)->handler(marshaler);
        return cpu.gpr.rax;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    WinInetManager manager;
    DllExports exports;
};

} // namespace

TEST_F(WinInetTest, SetOptionNeverAcceptsUnappliedSettingsBothModes) {
    for (bool wide : {false,true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x1f000;
        auto call = [&](const char* name,std::array<u64,4> values) {
            cpu.gpr.rcx=values[0];cpu.gpr.rdx=values[1];cpu.gpr.r8=values[2];cpu.gpr.r9=values[3];
            if (!wide) for (unsigned i=0;i<4;++i)
                (void)memory.write_u32(cpu.gpr.rsp+4+i*4,static_cast<u32>(values[i]));
            find_export(exports,name)->handler(marshaler);return cpu.gpr.rax;
        };
        const u64 session=call("InternetOpenA",{0,1,0,0});
        ASSERT_TRUE(memory.write_u32(0x10200,100).is_ok());
        for (const char* name : {"InternetSetOptionA","InternetSetOptionW"}) {
            EXPECT_EQ(call(name,{session,0x7fffffff,0x10200,4}),0u);
            EXPECT_EQ(get_last_error(),12009u);
            EXPECT_EQ(call(name,{session,9,0x10200,4}),0u);
            EXPECT_EQ(get_last_error(),12011u);
            EXPECT_EQ(call(name,{session,6,0x10200,1}),0u);
            EXPECT_EQ(get_last_error(),12010u);
            EXPECT_EQ(call(name,{session,6,0,4}),0u);
            EXPECT_EQ(get_last_error(),87u);
            // Transport deadline remains unsupported; it must not report success.
            EXPECT_EQ(call(name,{session,6,0x10200,4}),0u);
            EXPECT_EQ(get_last_error(),12009u);
        }
    }
}

TEST_F(WinInetTest, RequiredBattleNetExportsAreRegisteredOnce) {
    std::set<std::string> names;
    for (const auto& entry : exports.entries) {
        EXPECT_TRUE(names.insert(entry.name).second)
            << "duplicate WinINet export: " << entry.name;
    }
    EXPECT_NE(find_export(exports, "InternetReadFileExA"), nullptr);
    EXPECT_NE(find_export(exports, "InternetSetCookieW"), nullptr);
    EXPECT_NE(find_export(exports, "InternetSetStatusCallbackA"), nullptr);
}

TEST_F(WinInetTest, StatusCallbackReturnsPreviousCallbackAcrossAliases) {
    auto* open = find_export(exports, "InternetOpenA");
    auto* set_a = find_export(exports, "InternetSetStatusCallbackA");
    auto* set_w = find_export(exports, "InternetSetStatusCallbackW");
    ASSERT_NE(open, nullptr);
    ASSERT_NE(set_a, nullptr);
    ASSERT_NE(set_w, nullptr);

    open->handler(marshaler);
    const u32 session = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(session, 0u);

    cpu.gpr.rcx = session;
    cpu.gpr.rdx = 0x12345678;
    set_a->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = session;
    cpu.gpr.rdx = 0x87654321;
    set_w->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x12345678u);
}

TEST_F(WinInetTest, SetCookieWAcceptsNamedCookie) {
    auto* set_cookie = find_export(exports, "InternetSetCookieW");
    ASSERT_NE(set_cookie, nullptr);

    constexpr GuestAddr url = kBase + 0x100;
    constexpr GuestAddr name = kBase + 0x200;
    constexpr GuestAddr value = kBase + 0x300;
    ASSERT_TRUE(marshaler.write_wide_string(url, "https://battle.net/").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(name, "session").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(value, "abc; Path=/").is_ok());

    cpu.gpr.rcx = url;
    cpu.gpr.rdx = name;
    cpu.gpr.r8 = value;
    set_cookie->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(WinInetTest, ReadFileExARejectsUnknownHandleAndClearsLength) {
    auto* read_ex = find_export(exports, "InternetReadFileExA");
    ASSERT_NE(read_ex, nullptr);

    constexpr GuestAddr buffers = kBase + 0x400;
    constexpr GuestAddr output = kBase + 0x800;
    ASSERT_TRUE(memory.write_u32(buffers, 56).is_ok());
    ASSERT_TRUE(memory.write_u64(buffers + 32, output).is_ok());
    ASSERT_TRUE(memory.write_u32(buffers + 40, 64).is_ok());

    cpu.gpr.rcx = 0xBAD0;
    cpu.gpr.rdx = buffers;
    cpu.gpr.r8 = 0;
    set_last_error(error_codes::ERROR_SUCCESS);
    read_ex->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(memory.read_u32(buffers + 40).value(), 0u);
}

TEST_F(WinInetTest, HttpEndRequestWRejectsReservedBuffer) {
    auto* end_request = find_export(exports, "HttpEndRequestW");
    ASSERT_NE(end_request, nullptr);
    EXPECT_EQ(end_request->stdcall_bytes, 16u);
    cpu.gpr.rcx = 1;
    cpu.gpr.rdx = 0x2000;  // lpBuffersOut must be NULL
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    set_last_error(0);
    end_request->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(WinInetTest, InternetErrorDlgSucceedsWhenNoDialogIsNeeded) {
    auto* dialog = find_export(exports, "InternetErrorDlg");
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ(dialog->stdcall_bytes, 20u);
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;     // dwError
    cpu.gpr.r9 = 0x08;  // FLAGS_ERROR_UI_FLAGS_NO_UI
    dialog->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // ERROR_SUCCESS
}

// Expected values: tools/oracle_probe/http_crackurl_probe.wininet(.x86).expected
// and http_request_probe.wininet(.x86).expected (Windows 11).

TEST_F(WinInetTest, CrackUrlReadsComponentsFromTheFourthArgumentBothModes) {
    constexpr GuestAddr url = 0x10100, comps = 0x10800, host = 0x10A00;
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const auto l = urlcrack::layout(wide);
        auto reset = [&](u32 len, GuestAddr host_buffer, u32 host_len) {
            std::vector<Byte> zero(l.size, Byte{0});
            ASSERT_TRUE(memory.write_bytes(comps, zero.data(), zero.size()).is_ok());
            ASSERT_TRUE(memory.write_u32(comps, l.size).is_ok());
            for (int c = 0; c < 6; ++c) ASSERT_TRUE(memory.write_u32(comps + l.len[c], len).is_ok());
            if (host_buffer) {
                if (wide) ASSERT_TRUE(memory.write_u64(comps + l.ptr[1], host_buffer).is_ok());
                else ASSERT_TRUE(memory.write_u32(comps + l.ptr[1], static_cast<u32>(host_buffer)).is_ok());
                ASSERT_TRUE(memory.write_u32(comps + l.len[1], host_len).is_ok());
            }
        };
        auto ptr = [&](int c) { return wide ? memory.read_u64(comps + l.ptr[c]).value() : memory.read_u32(comps + l.ptr[c]).value(); };
        // W-inplace#0 / A-inplace#0: http is INTERNET_SCHEME 3 in WinINet.
        ASSERT_TRUE(marshaler.write_wide_string(url, "http://user:pass@www.example.com:8080/path/file.html?query=1#frag").is_ok());
        ASSERT_TRUE(marshaler.write_ansi_string(0x10400, "http://user:pass@www.example.com:8080/path/file.html?query=1#frag").is_ok());
        for (const char* api : {"InternetCrackUrlW", "InternetCrackUrlA"}) {
            SCOPED_TRACE(api);
            const bool w = api[16] == 'W';
            const GuestAddr text = w ? url : 0x10400;
            reset(1, 0, 0);
            set_last_error(0xDEAD);
            EXPECT_EQ(call_args(api, wide, {text, 0, 0, comps}), 1u);
            EXPECT_EQ(get_last_error(), 0u);
            EXPECT_EQ(memory.read_u32(comps + l.nscheme).value(), 3u);
            EXPECT_EQ(memory.read_u16(comps + l.port).value(), 8080u);
            EXPECT_EQ(ptr(1), text + 17 * (w ? 2u : 1u));
            EXPECT_EQ(memory.read_u32(comps + l.len[1]).value(), 15u);
        }
        // W-smallhost: Unicode copies what fits and stops; A-smallhost
        // reports the required size and clears the buffer.
        ASSERT_TRUE(marshaler.write_wide_string(url, "http://abcd/x?y").is_ok());
        reset(0, host, 3);
        ASSERT_TRUE(memory.write_u64(host, 0x2323232323232323ull).is_ok());
        EXPECT_EQ(call_args("InternetCrackUrlW", wide, {url, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 122u);
        EXPECT_EQ(memory.read_u32(comps + l.len[1]).value(), 3u);
        EXPECT_EQ(memory.read_u16(host).value(), u'a');
        EXPECT_EQ(memory.read_u16(host + 6).value(), 0x2323u);
        ASSERT_TRUE(marshaler.write_ansi_string(0x10400, "http://abcd/x?y").is_ok());
        reset(0, host, 3);
        EXPECT_EQ(call_args("InternetCrackUrlA", wide, {0x10400, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 122u);
        EXPECT_EQ(memory.read_u32(comps + l.len[1]).value(), 5u);
        EXPECT_EQ(memory.read_u8(host).value(), 0u);
        // A-size-0 fails, W ignores dwStructSize (W-size0).
        reset(1, 0, 0);
        ASSERT_TRUE(memory.write_u32(comps, 0).is_ok());
        EXPECT_EQ(call_args("InternetCrackUrlA", wide, {0x10400, 0, 0, comps}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
        EXPECT_EQ(call_args("InternetCrackUrlW", wide, {url, 0, 0, comps}), 1u);
        // W-copy#4: a file: URL copied out becomes a DOS path.
        ASSERT_TRUE(marshaler.write_wide_string(url, "file:///C:/dir/file.txt").is_ok());
        reset(0, 0, 0);
        if (wide) ASSERT_TRUE(memory.write_u64(comps + l.ptr[4], host).is_ok());
        else ASSERT_TRUE(memory.write_u32(comps + l.ptr[4], host).is_ok());
        ASSERT_TRUE(memory.write_u32(comps + l.len[4], 64).is_ok());
        EXPECT_EQ(call_args("InternetCrackUrlW", wide, {url, 0, 0, comps}), 1u);
        EXPECT_EQ(memory.read_u32(comps + l.nscheme).value(), 5u);
        EXPECT_EQ(marshaler.read_wide_string(host).value(), "C:\\dir\\file.txt");
    }
}

TEST_F(WinInetTest, OpenRequestFlagsAndContextComeFromArgumentsSevenAndEightBothModes) {
    // http_request_probe.wininet: url-port0-secure / open-request-context.
    ASSERT_TRUE(marshaler.write_ansi_string(0x10100, "127.0.0.1").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(0x10180, "/p").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(0x10190, "probe").is_ok());
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        const u32 session = static_cast<u32>(call_args("InternetOpenA", wide, {0x10190, 1, 0, 0, 0}));
        const u32 conn = static_cast<u32>(call_args("InternetConnectA", wide, {session, 0x10100, 0, 0, 0, 3, 0, 0}));
        ASSERT_NE(conn, 0u);
        // lplpszAcceptTypes (arg 6) is a non-NULL pointer; the flags follow it.
        const u32 request = static_cast<u32>(call_args("HttpOpenRequestA", wide,
            {conn, 0, 0x10180, 0, 0, 0x10300, 0x00800000u, 0x5678}));
        ASSERT_NE(request, 0u);
        ASSERT_TRUE(memory.write_u32(0x10500, 256).is_ok());
        EXPECT_EQ(call_args("InternetQueryOptionA", wide, {request, 34, 0x10600, 0x10500}), 1u);
        EXPECT_EQ(marshaler.read_ansi_string(0x10600).value(), "https://127.0.0.1:80/p");
        ASSERT_TRUE(memory.write_u32(0x10500, 8).is_ok());
        EXPECT_EQ(call_args("InternetQueryOptionA", wide, {request, 45, 0x10600, 0x10500}), 1u);
        EXPECT_EQ(memory.read_u32(0x10600).value(), 0x5678u);
        EXPECT_EQ(memory.read_u32(0x10500).value(), wide ? 8u : 4u);
        ASSERT_TRUE(memory.write_u32(0x10500, 64).is_ok());
        EXPECT_EQ(call_args("InternetQueryOptionA", wide, {request, 41, 0x10600, 0x10500}), 1u);
        EXPECT_EQ(marshaler.read_ansi_string(0x10600).value(), "probe");
    }
}

TEST_F(WinInetTest, AddRequestHeadersModifiersDifferFromWinHttp) {
    // http_request_probe.wininet: REPLACE of a missing name adds it, values
    // are trimmed, ADD_IF_NEW -> 12155, no colon / empty -> 87.
    const u32 session = static_cast<u32>(call_args("InternetOpenA", true, {0, 1, 0, 0, 0}));
    const u32 conn = static_cast<u32>(call_args("InternetConnectA", true, {session, 0, 80, 0, 0, 3, 0, 0}));
    const u32 request = static_cast<u32>(call_args("HttpOpenRequestA", true, {conn, 0, 0, 0, 0, 0, 0, 0}));
    auto add = [&](const char* text, u32 modifiers) {
        (void)marshaler.write_ansi_string(0x10100, text);
        return call_args("HttpAddRequestHeadersA", true, {request, 0x10100, 0xFFFFFFFFu, modifiers});
    };
    auto header = [&](const char* name, u32 index) -> std::string {
        (void)marshaler.write_ansi_string(0x10600, name);
        (void)memory.write_u32(0x10500, 256);
        (void)memory.write_u32(0x10510, index);
        return call_args("HttpQueryInfoA", true, {request, 0x80000000u | 65535u, 0x10600, 0x10500, 0x10510})
            ? marshaler.read_ansi_string(0x10600).value() : std::string("<none>");
    };
    EXPECT_EQ(add("X-J: 2", 0x80000000u), 1u);
    EXPECT_EQ(header("X-J", 0), "2");
    EXPECT_EQ(add("X-Q:    spaced   ", 0x20000000u), 1u);
    EXPECT_EQ(header("X-Q", 0), "spaced");
    EXPECT_EQ(add("X-E: 1", 0x20000000u), 1u);
    EXPECT_EQ(add("X-E: 2", 0x10000000u), 0u);
    EXPECT_EQ(get_last_error(), 12155u);
    EXPECT_EQ(add("X-D: 1", 0x20000000u), 1u);
    EXPECT_EQ(add("X-D: 2", 0x20000000u), 1u);
    EXPECT_EQ(header("X-D", 1), "2");
    EXPECT_EQ(memory.read_u32(0x10510).value(), 2u);
    EXPECT_EQ(add("NoColon", 0x20000000u), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(add("", 0x20000000u), 0u);
    EXPECT_EQ(get_last_error(), 87u);
}

TEST_F(WinInetTest, StatusCallbackSentinelIsAllOnes) {
    // INTERNET_INVALID_STATUS_CALLBACK is (INTERNET_STATUS_CALLBACK)-1L.
    EXPECT_EQ(call_args("InternetSetStatusCallbackA", true, {0xBAD0, 0x1000}), ~0ull);
    EXPECT_EQ(call_args("InternetSetStatusCallbackA", false, {0xBAD0, 0x1000}) & 0xFFFFFFFFull, 0xFFFFFFFFull);
}
