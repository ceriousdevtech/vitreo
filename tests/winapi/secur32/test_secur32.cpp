// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>
#include <winapi/networking/secur32/schannel_tls.hpp>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/networking/secur32/secur32.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class Secur32FunctionTableTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kStackBase = 0x80000;
    static constexpr GuestAddr kSecurityBase = 0x770C0000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kStackBase, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.set_sp(kStackBase + 0xF000);

        manager.set_module_manager(&modules);
        manager.set_heap_manager(&heap);
        ASSERT_TRUE(registry.register_dll(
            "security.dll", create_secur32_thunks(manager)).is_ok());
        modules.register_module("security.dll", kSecurityBase,
                                "C:\\Windows\\System32\\security.dll");
    }

    GuestAddr call_init(const char* name) {
        auto address = registry.get_thunk_address("security.dll", name);
        EXPECT_TRUE(address.is_ok());
        if (!address.is_ok()) return 0;
        auto handler = registry.resolve_handler(address.value());
        EXPECT_TRUE(handler.is_ok());
        if (!handler.is_ok()) return 0;

        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        cpu.gpr.rax = 0;
        (*handler.value())(marshaler);
        return static_cast<GuestAddr>(cpu.gpr.rax);
    }

    GuestMemory memory;
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    ThunkRegistry registry{0x70000000};
    ModuleManager modules{registry};
    Secur32Manager manager;
    cpu::CpuState cpu;
};

TEST_F(Secur32FunctionTableTest, WideTableUsesWideTextEntryPoints) {
    const GuestAddr table = call_init("InitSecurityInterfaceW");
    ASSERT_NE(table, 0u);

    EXPECT_EQ(memory.read_u32(table + 12).value_or(0),
              registry.get_thunk_address(
                  "security.dll", "AcquireCredentialsHandleW").value());
    EXPECT_EQ(memory.read_u32(table + 24).value_or(0),
              registry.get_thunk_address(
                  "security.dll", "InitializeSecurityContextW").value());
    EXPECT_EQ(memory.read_u32(table + 44).value_or(0),
              registry.get_thunk_address(
                  "security.dll", "QueryContextAttributesW").value());
    EXPECT_EQ(memory.read_u32(table + 68).value_or(0),
              registry.get_thunk_address(
                  "security.dll", "QuerySecurityPackageInfoW").value());
}

TEST_F(Secur32FunctionTableTest, X64TablesUseAlignedPointerSizedEntries) {
    cpu.init_long_mode();
    cpu.set_sp(kStackBase + 0xF000);
    for (const auto* name : {"InitSecurityInterfaceA", "InitSecurityInterfaceW"}) {
        const GuestAddr table = call_init(name);
        ASSERT_NE(table, 0u);
        EXPECT_EQ(memory.read_u32(table).value_or(0), 4u);
        EXPECT_EQ(memory.read_u32(table + 4).value_or(1), 0u);
        const bool wide = std::string_view(name).back() == 'W';
        EXPECT_EQ(memory.read_u64(table + 24).value_or(0),
                  registry.get_thunk_address("security.dll", wide ?
                      "AcquireCredentialsHandleW" : "AcquireCredentialsHandleA").value());
        EXPECT_EQ(memory.read_u64(table + 48).value_or(0),
                  registry.get_thunk_address("security.dll", wide ?
                      "InitializeSecurityContextW" : "InitializeSecurityContextA").value());
        EXPECT_EQ(memory.read_u64(table + 208).value_or(0),
                  registry.get_thunk_address("security.dll", "DecryptMessage").value());
        EXPECT_EQ(call_init(name), table);
    }
}

TEST_F(Secur32FunctionTableTest, X64CredentialsAndHandshakeUseFullWidthBuffers) {
    cpu.init_long_mode();
    cpu.set_sp(kStackBase + 0xF000);
    constexpr GuestAddr data = 0x100200000ULL;
    ASSERT_TRUE(memory.map_region(data, 0x10000, MemoryProtection::ReadWrite).is_ok());
    auto call = [&](const char* name, std::initializer_list<u64> args) {
        u32 i = 0;
        for (u64 arg : args) {
            switch (i) {
            case 0: cpu.gpr.rcx = arg; break;
            case 1: cpu.gpr.rdx = arg; break;
            case 2: cpu.gpr.r8 = arg; break;
            case 3: cpu.gpr.r9 = arg; break;
            default: (void)memory.write_u64(cpu.gpr.rsp + 40 + (i - 4) * 8, arg); break;
            }
            ++i;
        }
        return call_init(name);
    };
    const GuestAddr auth = data, cred = data + 0x100, ctx = data + 0x120;
    (void)memory.write_u32(auth, 4);
    (void)memory.write_u32(auth + 72, 8); // manual validation in SCHANNEL_CRED
    (void)memory.write_u64(cred, ~0ULL);
    (void)memory.write_u64(cred + 8, ~0ULL);
    const std::u16string package = u"Microsoft Unified Security Protocol Provider";
    for (usize i = 0; i < package.size(); ++i)
        (void)memory.write_u16(data + 0x200 + i * 2, package[i]);
    (void)memory.write_u16(data + 0x600, 'x');
    EXPECT_EQ(call("AcquireCredentialsHandleW", {0, data + 0x200, 2, 0, auth, 0, 0, cred, 0}), 0u);
    const u64 id = memory.read_u64(cred).value_or(0);
    ASSERT_NE(id, 0u);
    EXPECT_EQ(id >> 32, 0u);
    EXPECT_EQ(memory.read_u64(cred + 8).value_or(1), 0u);
    EXPECT_FALSE(schannel::credential_verify_peer(static_cast<u32>(id)));
    const GuestAddr desc = data + 0x300, buffers = data + 0x400;
    (void)memory.write_u32(desc + 4, 2);
    (void)memory.write_u64(desc + 8, buffers);
    (void)memory.write_u32(buffers + 4, 17); // alert first, token second
    (void)memory.write_u32(buffers + 16 + 4, 2);
    EXPECT_EQ(call("InitializeSecurityContextW", {cred, 0, data + 0x600, 0x100, 0, 0, 0, 0, ctx, desc, 0, 0}), 0x90312u);
    const u32 token_size = memory.read_u32(buffers + 16).value_or(0);
    const GuestAddr token = memory.read_u64(buffers + 24).value_or(0);
    EXPECT_GT(token_size, 5u);
    ASSERT_NE(token, 0u);
    EXPECT_EQ(memory.read_u8(token).value_or(0), 22u); // TLS handshake record
    EXPECT_EQ(memory.read_u64(ctx + 8).value_or(1), 0u);
    EXPECT_EQ(call("FreeContextBuffer", {token}), 0u);
    EXPECT_EQ(call("DeleteSecurityContext", {ctx}), 0u);
    EXPECT_EQ(call("FreeCredentialsHandle", {cred}), 0u);
    (void)memory.write_u64(data + 0x500, ~0ULL);
    EXPECT_EQ(call("QuerySecurityPackageInfoW", {data + 0x200, data + 0x500}), 0u);
    const GuestAddr info = memory.read_u64(data + 0x500).value_or(0);
    ASSERT_NE(info, 0u);
    // Windows 11 Schannel (tools/oracle_probe/sspi_secur32_probe.expected):
    // caps=0x4107b3 ver=1 rpcid=14 maxtok=0x6000, Name/Comment set.
    EXPECT_EQ(memory.read_u32(info + 8).value_or(0), 0x6000u);
    EXPECT_NE(memory.read_u64(info + 16).value_or(0), 0u);
    EXPECT_NE(memory.read_u64(info + 24).value_or(0), 0u);
    EXPECT_EQ(call("FreeContextBuffer", {info}), 0u);
}

TEST_F(Secur32FunctionTableTest, AnsiAndWideTablesAreDistinct) {
    const GuestAddr wide = call_init("InitSecurityInterfaceW");
    const GuestAddr ansi = call_init("InitSecurityInterfaceA");
    ASSERT_NE(wide, 0u);
    ASSERT_NE(ansi, 0u);
    EXPECT_NE(wide, ansi);

    EXPECT_EQ(memory.read_u32(ansi + 24).value_or(0),
              registry.get_thunk_address(
                  "security.dll", "InitializeSecurityContextA").value());
    EXPECT_EQ(memory.read_u32(wide + 24).value_or(0),
              registry.get_thunk_address(
                  "security.dll", "InitializeSecurityContextW").value());
}

} // namespace

// =============================================================================
// SSPI contract (x86 and x64), expected values measured on Windows 11 by
// tools/oracle_probe/sspi_secur32_probe.c (.expected / .x86.expected).
// =============================================================================

namespace {

constexpr u64 SEC_E_OK_ = 0;
constexpr u64 SEC_I_CONTINUE_NEEDED_ = 0x00090312;
constexpr u64 SEC_E_INSUFFICIENT_MEMORY_ = 0x80090300;
constexpr u64 SEC_E_INVALID_HANDLE_ = 0x80090301;
constexpr u64 SEC_E_UNSUPPORTED_FUNCTION_ = 0x80090302;
constexpr u64 SEC_E_INTERNAL_ERROR_ = 0x80090304;
constexpr u64 SEC_E_SECPKG_NOT_FOUND_ = 0x80090305;
constexpr u64 SEC_E_INVALID_TOKEN_ = 0x80090308;
constexpr u64 SEC_E_UNKNOWN_CREDENTIALS_ = 0x8009030D;
constexpr u64 SEC_E_NO_CREDENTIALS_ = 0x8009030E;
constexpr u64 SEC_E_INCOMPLETE_MESSAGE_ = 0x80090318;
constexpr u64 SEC_E_BUFFER_TOO_SMALL_ = 0x80090321;
constexpr u64 SEC_E_ILLEGAL_MESSAGE_ = 0x80090326;
constexpr u64 SEC_E_ALGORITHM_MISMATCH_ = 0x80090331;
constexpr u64 kNeverExpires = 0x7FFFFF36D5969FFFULL;
constexpr u32 kSent = 0x5A5A5A5Au;
// curl's request: SEQUENCE|REPLAY|CONFIDENTIALITY|ALLOCATE_MEMORY|STREAM.
constexpr u32 kReqCurl = 0x811C;
constexpr u32 kReqNoAlloc = 0x801C;

class SspiContractTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kData = 0x00400000;
    // Fixed scratch areas inside kData.
    static constexpr GuestAddr kStr = kData + 0x0000;     // strings
    static constexpr GuestAddr kCred = kData + 0x1000;    // CredHandle
    static constexpr GuestAddr kCtx = kData + 0x1020;     // CtxtHandle
    static constexpr GuestAddr kNewCtx = kData + 0x1040;  // phNewContext
    static constexpr GuestAddr kAttr = kData + 0x1060;
    static constexpr GuestAddr kExpiry = kData + 0x1070;
    static constexpr GuestAddr kOut = kData + 0x1080;     // misc out-params
    static constexpr GuestAddr kAuth = kData + 0x1100;    // SCHANNEL_CRED / SCH_CREDENTIALS
    static constexpr GuestAddr kTls = kData + 0x1200;     // TLS_PARAMETERS
    static constexpr GuestAddr kInDesc = kData + 0x2000;
    static constexpr GuestAddr kInArr = kData + 0x2040;
    static constexpr GuestAddr kOutDesc = kData + 0x2100;
    static constexpr GuestAddr kOutArr = kData + 0x2140;
    static constexpr GuestAddr kInBytes = kData + 0x3000;
    static constexpr GuestAddr kTokBuf = kData + 0x8000;  // 16 KiB + canary

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kData, 0x40000, MemoryProtection::ReadWrite).is_ok());
        is64 = GetParam();
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        manager.set_module_manager(&modules);
        manager.set_heap_manager(&heap);
        ASSERT_TRUE(registry.register_dll("secur32.dll", create_secur32_thunks(manager)).is_ok());
    }

    u64 call(const char* name, std::initializer_list<u64> args) {
        cpu.set_sp(kStack + 0xE000);
        const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
        u32 i = 0;
        for (u64 a : args) {
            if (is64) {
                switch (i) {
                case 0: cpu.gpr.rcx = a; break;
                case 1: cpu.gpr.rdx = a; break;
                case 2: cpu.gpr.r8 = a; break;
                case 3: cpu.gpr.r9 = a; break;
                default: (void)memory.write_u64(sp + 40 + (i - 4) * 8, a); break;
                }
            } else {
                (void)memory.write_u32(sp + 4 + i * 4, static_cast<u32>(a));
            }
            ++i;
        }
        auto address = registry.get_thunk_address("secur32.dll", name);
        EXPECT_TRUE(address.is_ok()) << name;
        if (!address.is_ok()) return ~0ULL;
        auto handler = registry.resolve_handler(address.value());
        EXPECT_TRUE(handler.is_ok());
        if (!handler.is_ok()) return ~0ULL;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        cpu.gpr.rax = 0xDEADDEADDEADDEADULL;
        (*handler.value())(m);
        return is64 ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }

    u32 ps() const { return is64 ? 8 : 4; }
    u64 rptr(GuestAddr a) { return is64 ? memory.read_u64(a).value_or(0) : memory.read_u32(a).value_or(0); }
    void wptr(GuestAddr a, u64 v) {
        if (is64) (void)memory.write_u64(a, v); else (void)memory.write_u32(a, static_cast<u32>(v));
    }
    GuestAddr wstr(GuestAddr at, std::string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u16(at + i * 2, static_cast<u8>(s[i]));
        (void)memory.write_u16(at + s.size() * 2, 0);
        return at;
    }
    GuestAddr astr(GuestAddr at, std::string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(at + i, static_cast<u8>(s[i]));
        (void)memory.write_u8(at + s.size(), 0);
        return at;
    }
    std::string read_w(GuestAddr p) {
        std::string s;
        for (u16 c; (c = memory.read_u16(p).value_or(0)) != 0; p += 2) s.push_back(static_cast<char>(c));
        return s;
    }
    std::string read_a(GuestAddr p) {
        std::string s;
        for (u8 c; (c = memory.read_u8(p).value_or(0)) != 0; ++p) s.push_back(static_cast<char>(c));
        return s;
    }
    void sentinel_handle(GuestAddr h) { wptr(h, kSent); wptr(h + ps(), kSent); }
    bool handle_untouched(GuestAddr h) { return rptr(h) == kSent && rptr(h + ps()) == kSent; }

    // SecBuffer { ULONG cbBuffer; ULONG BufferType; PVOID pvBuffer; }.
    u32 sbsize() const { return is64 ? 16 : 12; }
    void set_buf(GuestAddr arr, u32 i, u32 cb, u32 type, GuestAddr pv) {
        const GuestAddr e = arr + i * sbsize();
        (void)memory.write_u32(e, cb);
        (void)memory.write_u32(e + 4, type);
        wptr(e + 8, pv);
    }
    u32 buf_cb(GuestAddr arr, u32 i) { return memory.read_u32(arr + i * sbsize()).value_or(~0u); }
    u32 buf_type(GuestAddr arr, u32 i) { return memory.read_u32(arr + i * sbsize() + 4).value_or(~0u); }
    u64 buf_pv(GuestAddr arr, u32 i) { return rptr(arr + i * sbsize() + 8); }
    GuestAddr desc(GuestAddr at, GuestAddr arr, u32 count) {
        (void)memory.write_u32(at, 0);
        (void)memory.write_u32(at + 4, count);
        wptr(at + 8, arr);
        return at;
    }

    // SCHANNEL_CRED (v3/v4) or SCH_CREDENTIALS (v5) with dwFlags at the
    // offset schannel.h gives for this bitness.
    GuestAddr sch_cred(u32 version, u32 flags) {
        for (u32 i = 0; i < 0x100; i += 4) (void)memory.write_u32(kAuth + i, 0);
        (void)memory.write_u32(kAuth, version);
        const u32 off = version == 5 ? (is64 ? 52 : 32) : (is64 ? 72 : 48);
        (void)memory.write_u32(kAuth + off, flags);
        return kAuth;
    }

    u64 acquire(std::string_view pkg, u32 use, GuestAddr auth, GuestAddr cred = kCred) {
        sentinel_handle(cred);
        (void)memory.write_u64(kExpiry, 0x5A5A5A5A5A5A5A5AULL);
        return call("AcquireCredentialsHandleW",
                    {0, wstr(kStr, pkg), use, 0, auth, 0, 0, cred, kExpiry});
    }
    // First ISC leg; out descriptor already prepared by the caller.
    u64 isc(GuestAddr cred, GuestAddr ctx, u32 req, GuestAddr in_desc, GuestAddr out_desc) {
        sentinel_handle(kNewCtx);
        (void)memory.write_u32(kAttr, kSent);
        (void)memory.write_u64(kExpiry, 0x5A5A5A5A5A5A5A5AULL);
        return call("InitializeSecurityContextW",
                    {cred, ctx, wstr(kStr + 0x400, "localhost"), req, 0, 0, in_desc, 0,
                     kNewCtx, out_desc, kAttr, kExpiry});
    }
    // A live Schannel credential with manual validation (the probe's shape).
    void make_cred() {
        ASSERT_EQ(acquire("Microsoft Unified Security Protocol Provider", 2,
                          sch_cred(4, 0x8 | 0x10)), SEC_E_OK_);
    }
    // First leg with ALLOCATE_MEMORY; leaves the context in kCtx.
    void make_ctx() {
        set_buf(kOutArr, 0, 0, 2, 0);
        set_buf(kOutArr, 1, 0, 17, 0);
        ASSERT_EQ(isc(kCred, 0, kReqCurl, 0, desc(kOutDesc, kOutArr, 2)), SEC_I_CONTINUE_NEEDED_);
        (void)call("FreeContextBuffer", {buf_pv(kOutArr, 0)});
        wptr(kCtx, rptr(kNewCtx));
        wptr(kCtx + ps(), rptr(kNewCtx + ps()));
    }
    // Second leg feeding `bytes` as the server token.
    u64 isc2(GuestAddr cred, GuestAddr ctx, const std::vector<u8>& bytes) {
        for (usize i = 0; i < bytes.size(); ++i) (void)memory.write_u8(kInBytes + i, bytes[i]);
        set_buf(kInArr, 0, static_cast<u32>(bytes.size()), 2, kInBytes);
        set_buf(kInArr, 1, 0, 0, 0);
        set_buf(kOutArr, 0, 0, 2, 0);
        set_buf(kOutArr, 1, 0, 17, 0);
        return isc(cred, ctx, kReqCurl, desc(kInDesc, kInArr, 2), desc(kOutDesc, kOutArr, 2));
    }

    GuestMemory memory;
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    ThunkRegistry registry{0x70000000};
    ModuleManager modules{registry};
    Secur32Manager manager;
    cpu::CpuState cpu;
    bool is64 = false;
};

INSTANTIATE_TEST_SUITE_P(Bitness, SspiContractTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });

// qspi_w/qspi_a: every Schannel-family name (case-insensitive) answers with
// the canonical name and identical fields.
TEST_P(SspiContractTest, QuerySecurityPackageInfoReportsWindowsSchannel) {
    const struct { const char* query; const char* canonical; } cases[] = {
        {"Microsoft Unified Security Protocol Provider", "Microsoft Unified Security Protocol Provider"},
        {"Schannel", "Schannel"},
        {"Default TLS SSP", "Default TLS SSP"},
        {"schannel", "Schannel"},
        {"MICROSOFT UNIFIED SECURITY PROTOCOL PROVIDER", "Microsoft Unified Security Protocol Provider"},
    };
    for (const auto& c : cases) {
        for (bool wide : {true, false}) {
            wptr(kOut, kSent);
            const GuestAddr name = wide ? wstr(kStr, c.query) : astr(kStr, c.query);
            ASSERT_EQ(call(wide ? "QuerySecurityPackageInfoW" : "QuerySecurityPackageInfoA",
                           {name, kOut}), SEC_E_OK_) << c.query;
            const GuestAddr info = static_cast<GuestAddr>(rptr(kOut));
            ASSERT_NE(info, 0u);
            EXPECT_EQ(memory.read_u32(info).value_or(0), 0x4107B3u);
            EXPECT_EQ(memory.read_u16(info + 4).value_or(0), 1u);
            EXPECT_EQ(memory.read_u16(info + 6).value_or(0), 14u);
            EXPECT_EQ(memory.read_u32(info + 8).value_or(0), 0x6000u);
            const GuestAddr pname = static_cast<GuestAddr>(rptr(info + (is64 ? 16 : 12)));
            const GuestAddr pcomment = static_cast<GuestAddr>(rptr(info + (is64 ? 24 : 16)));
            EXPECT_EQ(wide ? read_w(pname) : read_a(pname), c.canonical);
            EXPECT_EQ(wide ? read_w(pcomment) : read_a(pcomment), "Schannel Security Package");
            EXPECT_EQ(call("FreeContextBuffer", {info}), SEC_E_OK_);
        }
    }
}

// Unknown packages -- and Negotiate/NTLM/Kerberos, which Vitreo does not
// implement -- fail with SEC_E_SECPKG_NOT_FOUND and leave the output alone
// (Windows' value for a package it lacks: qspi_w:bogus).
TEST_P(SspiContractTest, QuerySecurityPackageInfoUnknownPackageNotFound) {
    for (const char* name : {"Bogus", "", "Microsoft Unified Security Protocol Provider ",
                             "Negotiate", "NTLM", "Kerberos"}) {
        wptr(kOut, kSent);
        EXPECT_EQ(call("QuerySecurityPackageInfoW", {wstr(kStr, name), kOut}),
                  SEC_E_SECPKG_NOT_FOUND_) << name;
        EXPECT_EQ(rptr(kOut), kSent);
        EXPECT_EQ(call("QuerySecurityPackageInfoA", {astr(kStr, name), kOut}),
                  SEC_E_SECPKG_NOT_FOUND_) << name;
        EXPECT_EQ(rptr(kOut), kSent);
    }
}

// enum_w_pkg / enum_a_pkg: the three Schannel entries in Windows' order.
TEST_P(SspiContractTest, EnumerateSecurityPackagesListsSchannelFamily) {
    const char* order[] = {"Schannel", "Microsoft Unified Security Protocol Provider", "Default TLS SSP"};
    for (bool wide : {true, false}) {
        (void)memory.write_u32(kOut, kSent);
        wptr(kOut + 8, kSent);
        ASSERT_EQ(call(wide ? "EnumerateSecurityPackagesW" : "EnumerateSecurityPackagesA",
                       {kOut, kOut + 8}), SEC_E_OK_);
        ASSERT_EQ(memory.read_u32(kOut).value_or(0), 3u);
        const GuestAddr arr = static_cast<GuestAddr>(rptr(kOut + 8));
        const u32 entry = is64 ? 32 : 20;
        for (u32 i = 0; i < 3; ++i) {
            EXPECT_EQ(memory.read_u32(arr + i * entry).value_or(0), 0x4107B3u);
            const GuestAddr n = static_cast<GuestAddr>(rptr(arr + i * entry + (is64 ? 16 : 12)));
            EXPECT_EQ(wide ? read_w(n) : read_a(n), order[i]);
        }
        EXPECT_EQ(call("FreeContextBuffer", {arr}), SEC_E_OK_);
    }
}

// ach_w: package dispatch, fCredentialUse, dwVersion and expiry.
TEST_P(SspiContractTest, AcquireCredentialsHandleDispatchAndValidation) {
    const char* unisp = "Microsoft Unified Security Protocol Provider";
    auto expiry = [&] { return memory.read_u64(kExpiry).value_or(0); };
    // Unknown and unimplemented packages: nothing written.
    for (const char* pkg : {"Bogus", "", "Negotiate", "NTLM", "Kerberos"}) {
        EXPECT_EQ(acquire(pkg, 2, 0), SEC_E_SECPKG_NOT_FOUND_) << pkg;
        EXPECT_TRUE(handle_untouched(kCred));
        EXPECT_EQ(expiry(), 0x5A5A5A5A5A5A5A5AULL);
    }
    // All three names, case-insensitive, NULL pAuthData.
    for (const char* pkg : {unisp, "Schannel", "schannel", "Default TLS SSP"}) {
        EXPECT_EQ(acquire(pkg, 2, 0), SEC_E_OK_) << pkg;
        EXPECT_FALSE(handle_untouched(kCred));
        EXPECT_EQ(rptr(kCred + ps()), 0u);
        EXPECT_EQ(expiry(), kNeverExpires);
        EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    }
    // SCH_CRED_V3 / SCHANNEL_CRED / SCH_CREDENTIALS accepted; others are
    // SEC_E_UNKNOWN_CREDENTIALS with a zero expiry.
    for (u32 v : {3u, 4u, 5u}) {
        EXPECT_EQ(acquire(unisp, 2, sch_cred(v, 0)), SEC_E_OK_) << v;
        EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    }
    for (u32 v : {0u, 1u, 2u, 6u, 99u}) {
        EXPECT_EQ(acquire(unisp, 2, sch_cred(v, 0)), SEC_E_UNKNOWN_CREDENTIALS_) << v;
        EXPECT_TRUE(handle_untouched(kCred));
        EXPECT_EQ(expiry(), 0u);
    }
    // fCredentialUse: 0, INBOUND/BOTH without auth data, DEFAULT(4).
    for (u32 use : {0u, 1u, 3u, 4u}) {
        EXPECT_EQ(acquire(unisp, use, 0), SEC_E_NO_CREDENTIALS_) << use;
        EXPECT_TRUE(handle_untouched(kCred));
        EXPECT_EQ(expiry(), 0u);
    }
    EXPECT_EQ(acquire(unisp, 0, sch_cred(4, 0)), SEC_E_NO_CREDENTIALS_);
    EXPECT_EQ(acquire(unisp, 1, sch_cred(4, 0)), SEC_E_OK_);   // inbound_v4_nocert
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    EXPECT_EQ(acquire(unisp, 3, sch_cred(4, 0)), SEC_E_OK_);   // use_both_v4
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    // ANSI entry point.
    sentinel_handle(kCred);
    EXPECT_EQ(call("AcquireCredentialsHandleA", {0, astr(kStr, unisp), 2, 0, 0, 0, 0, kCred, 0}), SEC_E_OK_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    EXPECT_EQ(call("AcquireCredentialsHandleA", {0, astr(kStr, "NTLM"), 2, 0, 0, 0, 0, kCred, 0}),
              SEC_E_SECPKG_NOT_FOUND_);
}

// schannel.h: SCHANNEL_CRED.dwFlags is at 48 (x86) / 72 (x64);
// SCH_CREDENTIALS.dwFlags at 32 / 52. Each version is read at its own offset.
TEST_P(SspiContractTest, AcquireCredentialsHandleReadsFlagsAtVersionOffset) {
    const char* unisp = "Microsoft Unified Security Protocol Provider";
    for (u32 v : {4u, 5u}) {
        ASSERT_EQ(acquire(unisp, 2, sch_cred(v, 0x8 /*MANUAL*/ | 0x4 /*NO_SERVERNAME*/)), SEC_E_OK_);
        schannel::CredentialConfig cfg;
        ASSERT_TRUE(schannel::get_credential(static_cast<u32>(rptr(kCred)), cfg));
        EXPECT_FALSE(cfg.verify_peer) << v;
        EXPECT_FALSE(cfg.check_server_name) << v;
        EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    }
    // A v5 structure with the flag only at the v4 offset is automatic.
    (void)sch_cred(5, 0);
    (void)memory.write_u32(kAuth + (is64 ? 72 : 48), 0x8);
    ASSERT_EQ(acquire(unisp, 2, kAuth), SEC_E_OK_);
    EXPECT_TRUE(schannel::credential_verify_peer(static_cast<u32>(rptr(kCred))));
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// Fields Vitreo's engine cannot honour fail loudly instead of being ignored
// (documented failure value, plus one UNIMPLEMENTED log).
TEST_P(SspiContractTest, AcquireCredentialsHandleUnhonourableFieldsFail) {
    const char* unisp = "Microsoft Unified Security Protocol Provider";
    // Client certificate.
    (void)sch_cred(4, 0);
    (void)memory.write_u32(kAuth + 4, 1);
    EXPECT_EQ(acquire(unisp, 2, kAuth), SEC_E_INTERNAL_ERROR_);
    EXPECT_TRUE(handle_untouched(kCred));
    // grbitEnabledProtocols (x86 32 / x64 56) without SP_PROT_TLS1_2_CLIENT.
    (void)sch_cred(4, 0);
    (void)memory.write_u32(kAuth + (is64 ? 56 : 32), 0x80 /*SP_PROT_TLS1_0_CLIENT*/);
    EXPECT_EQ(acquire(unisp, 2, kAuth), SEC_E_INTERNAL_ERROR_);
    (void)memory.write_u32(kAuth + (is64 ? 56 : 32), 0x800 | 0x2000);
    EXPECT_EQ(acquire(unisp, 2, kAuth), SEC_E_OK_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    // SCH_CREDENTIALS -> TLS_PARAMETERS (24 / 40 bytes): grbitDisabledProtocols
    // at 8 / 16, cDisabledCrypto at 12 / 20.
    auto v5_with = [&](u32 disabled, u32 crypto) {
        (void)sch_cred(5, 0);
        (void)memory.write_u32(kAuth + (is64 ? 56 : 36), 1);
        wptr(kAuth + (is64 ? 64 : 40), kTls);
        for (u32 i = 0; i < 40; i += 4) (void)memory.write_u32(kTls + i, 0);
        (void)memory.write_u32(kTls + (is64 ? 16 : 8), disabled);
        (void)memory.write_u32(kTls + (is64 ? 20 : 12), crypto);
        return acquire(unisp, 2, kAuth);
    };
    EXPECT_EQ(v5_with(0x800, 0), SEC_E_INTERNAL_ERROR_);
    EXPECT_EQ(v5_with(0, 1), SEC_E_INTERNAL_ERROR_);
    EXPECT_EQ(v5_with(~(0x800u | 0x2000u), 0), SEC_E_OK_);   // curl's default
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// isc:bad_cred/null_cred/user_small/user_null/user_empty_type/out_null/
// user_big/alloc_empty/alloc_with_user_buf.
TEST_P(SspiContractTest, InitializeSecurityContextOutputBufferRules) {
    make_cred();
    auto attr = [&] { return memory.read_u32(kAttr).value_or(0); };
    // Invalid / NULL credential on the first leg: nothing written.
    const GuestAddr bogus = kOut;
    wptr(bogus, 0x1234); wptr(bogus + ps(), 0x5678);
    set_buf(kOutArr, 0, 0, 2, 0);
    EXPECT_EQ(isc(bogus, 0, kReqCurl, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_INVALID_HANDLE_);
    EXPECT_TRUE(handle_untouched(kNewCtx));
    EXPECT_EQ(attr(), kSent);
    EXPECT_EQ(isc(0, 0, kReqCurl, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(attr(), kSent);

    // No ALLOCATE_MEMORY and a 10-byte buffer: BUFFER_TOO_SMALL, nothing
    // written into (or past) the buffer, no context.
    for (u32 i = 0; i < 64; ++i) (void)memory.write_u8(kTokBuf + i, 0xCC);
    set_buf(kOutArr, 0, 10, 2, kTokBuf);
    EXPECT_EQ(isc(kCred, 0, kReqNoAlloc, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_BUFFER_TOO_SMALL_);
    for (u32 i = 0; i < 64; ++i) EXPECT_EQ(memory.read_u8(kTokBuf + i).value_or(0), 0xCCu) << i;
    EXPECT_EQ(buf_cb(kOutArr, 0), 10u);
    EXPECT_TRUE(handle_untouched(kNewCtx));
    EXPECT_EQ(attr(), 0x801Cu);
    EXPECT_EQ(memory.read_u64(kExpiry).value_or(1), 0u);

    // No ALLOCATE_MEMORY, NULL pvBuffer: INSUFFICIENT_MEMORY, early mask.
    set_buf(kOutArr, 0, 0, 2, 0);
    EXPECT_EQ(isc(kCred, 0, kReqNoAlloc, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_INSUFFICIENT_MEMORY_);
    EXPECT_EQ(attr(), 0x1Cu);
    EXPECT_TRUE(handle_untouched(kNewCtx));
    // SECBUFFER_EMPTY without ALLOCATE_MEMORY, and no pOutput at all.
    set_buf(kOutArr, 0, 16384, 0, kTokBuf);
    EXPECT_EQ(isc(kCred, 0, kReqNoAlloc, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_INVALID_TOKEN_);
    EXPECT_EQ(attr(), 0x1Cu);
    EXPECT_EQ(isc(kCred, 0, kReqCurl, 0, 0), SEC_E_INVALID_TOKEN_);
    EXPECT_EQ(attr(), 0x1Cu);

    // Caller buffer large enough: token written in place, no allocation.
    set_buf(kOutArr, 0, 16384, 2, kTokBuf);
    EXPECT_EQ(isc(kCred, 0, kReqNoAlloc, 0, desc(kOutDesc, kOutArr, 1)), SEC_I_CONTINUE_NEEDED_);
    EXPECT_EQ(buf_pv(kOutArr, 0), kTokBuf);
    EXPECT_GT(buf_cb(kOutArr, 0), 16u);
    EXPECT_LT(buf_cb(kOutArr, 0), 16384u);
    EXPECT_EQ(memory.read_u8(kTokBuf).value_or(0), 0x16u);
    EXPECT_EQ(attr(), 0x801Cu);
    EXPECT_EQ(memory.read_u64(kExpiry).value_or(1), 0u);
    EXPECT_EQ(call("DeleteSecurityContext", {kNewCtx}), SEC_E_OK_);

    // ALLOCATE_MEMORY into an EMPTY buffer, and over a caller buffer.
    for (u32 type : {0u, 2u}) {
        set_buf(kOutArr, 0, type ? 16384u : 0u, type, type ? kTokBuf : 0);
        EXPECT_EQ(isc(kCred, 0, kReqCurl, 0, desc(kOutDesc, kOutArr, 1)), SEC_I_CONTINUE_NEEDED_);
        EXPECT_EQ(buf_type(kOutArr, 0), 2u);
        const u64 pv = buf_pv(kOutArr, 0);
        EXPECT_NE(pv, 0u);
        EXPECT_NE(pv, kTokBuf);
        EXPECT_EQ(memory.read_u8(static_cast<GuestAddr>(pv)).value_or(0), 0x16u);
        EXPECT_EQ(attr(), 0x811Cu);
        EXPECT_EQ(call("FreeContextBuffer", {pv}), SEC_E_OK_);
        EXPECT_EQ(call("DeleteSecurityContext", {kNewCtx}), SEC_E_OK_);
    }
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// isc_attr: one ISC_REQ bit at a time (plus ALLOCATE_MEMORY).
TEST_P(SspiContractTest, InitializeSecurityContextAttributeMask) {
    make_cred();
    const struct { u32 bit; u32 attr; } cases[] = {
        {0, 0x811C}, {1, 0x811E}, {6, 0x811C}, {7, 0x819C}, {11, 0x91C},
        {14, 0xC11C}, {15, 0x811C}, {16, 0x811C}, {19, 0x8811C}, {31, 0x811C},
    };
    for (const auto& c : cases) {
        set_buf(kOutArr, 0, 0, 2, 0);
        EXPECT_EQ(isc(kCred, 0, (1u << c.bit) | 0x100u, 0, desc(kOutDesc, kOutArr, 1)),
                  SEC_I_CONTINUE_NEEDED_) << c.bit;
        EXPECT_EQ(memory.read_u32(kAttr).value_or(0), c.attr) << c.bit;
        (void)call("FreeContextBuffer", {buf_pv(kOutArr, 0)});
        EXPECT_EQ(call("DeleteSecurityContext", {kNewCtx}), SEC_E_OK_);
    }
    // ISC_REQ_DATAGRAM asks for DTLS, which the engine cannot do: it fails
    // loudly (Windows: SEC_I_CONTINUE_NEEDED) with the mask Windows reports.
    set_buf(kOutArr, 0, 0, 2, 0);
    EXPECT_EQ(isc(kCred, 0, 0x400u | 0x100u, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_UNSUPPORTED_FUNCTION_);
    EXPECT_EQ(memory.read_u32(kAttr).value_or(0), 0x851Cu);
    EXPECT_TRUE(handle_untouched(kNewCtx));
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// isc2:incomplete_* / incomplete_null_cred / bad_ctx / zero_ctx_first.
TEST_P(SspiContractTest, InitializeSecurityContextIncompleteReportsMissing) {
    make_cred();
    make_ctx();
    const struct { std::vector<u8> bytes; u32 missing; } cases[] = {
        {{0x16, 0x03, 0x03, 0x00, 0x64}, 100},
        {{0x16, 0x03, 0x03}, 2},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(isc2(kCred, kCtx, c.bytes), SEC_E_INCOMPLETE_MESSAGE_);
        EXPECT_EQ(buf_type(kInArr, 1), 4u);             // SECBUFFER_MISSING
        EXPECT_EQ(buf_cb(kInArr, 1), c.missing);
        EXPECT_EQ(buf_pv(kInArr, 1), 0u);
        EXPECT_EQ(buf_cb(kInArr, 0), c.bytes.size());
        EXPECT_TRUE(handle_untouched(kNewCtx));
        EXPECT_EQ(memory.read_u32(kAttr).value_or(0), 0x811Cu);
        EXPECT_EQ(buf_type(kOutArr, 0), 2u);
        EXPECT_EQ(buf_cb(kOutArr, 0), 0u);
        EXPECT_EQ(buf_pv(kOutArr, 0), 0u);
    }
    std::vector<u8> body(5 + 50, 0x02);
    body[0] = 0x16; body[1] = 0x03; body[2] = 0x03; body[3] = 0x00; body[4] = 0x64;
    EXPECT_EQ(isc2(kCred, kCtx, body), SEC_E_INCOMPLETE_MESSAGE_);
    EXPECT_EQ(buf_cb(kInArr, 1), 50u);

    // A later leg without the credential: INVALID_HANDLE, early mask.
    EXPECT_EQ(isc2(0, kCtx, {0x16, 0x03, 0x03, 0x00, 0x64}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(memory.read_u32(kAttr).value_or(0), 0x1Cu);
    EXPECT_EQ(buf_type(kInArr, 1), 0u);
    // Unknown and zeroed context handles: nothing written.
    const GuestAddr bad = kOut;
    wptr(bad, 0x1234); wptr(bad + ps(), 0x5678);
    EXPECT_EQ(isc2(kCred, bad, {0x16, 0x03, 0x03, 0x00, 0x64}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(memory.read_u32(kAttr).value_or(0), kSent);
    wptr(bad, 0); wptr(bad + ps(), 0);
    set_buf(kOutArr, 0, 0, 2, 0);
    EXPECT_EQ(isc(kCred, bad, kReqCurl, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(memory.read_u32(kAttr).value_or(0), kSent);
    EXPECT_TRUE(handle_untouched(kNewCtx));

    // isc2:fatal_alert on a fresh context: a server fatal alert is
    // SEC_E_ILLEGAL_MESSAGE and the context handle is not rewritten.
    EXPECT_EQ(call("DeleteSecurityContext", {kCtx}), SEC_E_OK_);
    make_ctx();
    EXPECT_EQ(isc2(kCred, kCtx, {0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28}), SEC_E_ILLEGAL_MESSAGE_);
    EXPECT_TRUE(handle_untouched(kNewCtx));
    EXPECT_EQ(memory.read_u32(kAttr).value_or(0), 0x811Cu);
    EXPECT_EQ(call("DeleteSecurityContext", {kCtx}), SEC_E_OK_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// isc:inbound_cred: a server-only credential has no client protocol.
TEST_P(SspiContractTest, InitializeSecurityContextWithInboundCredential) {
    ASSERT_EQ(acquire("Microsoft Unified Security Protocol Provider", 1, sch_cred(4, 0)), SEC_E_OK_);
    set_buf(kOutArr, 0, 0, 2, 0);
    EXPECT_EQ(isc(kCred, 0, kReqCurl, 0, desc(kOutDesc, kOutArr, 1)), SEC_E_ALGORITHM_MISMATCH_);
    EXPECT_EQ(memory.read_u32(kAttr).value_or(0), 0x811Cu);
    EXPECT_EQ(memory.read_u64(kExpiry).value_or(1), 0u);
    EXPECT_TRUE(handle_untouched(kNewCtx));
    EXPECT_EQ(buf_pv(kOutArr, 0), 0u);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// qca_stream_sizes_incomplete / encrypt_incomplete / decrypt_incomplete_ctx.
TEST_P(SspiContractTest, RecordLayerRejectsUnfinishedHandshake) {
    make_cred();
    make_ctx();
    EXPECT_EQ(call("QueryContextAttributesW", {kCtx, 4, kOut}), SEC_E_INVALID_HANDLE_);
    const GuestAddr msg = kTokBuf;
    set_buf(kInArr, 0, 13, 7, msg);
    set_buf(kInArr, 1, 16, 1, msg + 13);
    set_buf(kInArr, 2, 64, 6, msg + 29);
    set_buf(kInArr, 3, 0, 0, 0);
    EXPECT_EQ(call("EncryptMessage", {kCtx, 0, desc(kInDesc, kInArr, 4), 0}), SEC_E_INVALID_HANDLE_);
    set_buf(kInArr, 0, 8, 1, msg);
    set_buf(kInArr, 1, 0, 0, 0);
    set_buf(kInArr, 2, 0, 0, 0);
    EXPECT_EQ(call("DecryptMessage", {kCtx, desc(kInDesc, kInArr, 4), 0, kOut}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(buf_type(kInArr, 0), 1u);
    EXPECT_EQ(buf_cb(kInArr, 0), 8u);
    EXPECT_EQ(call("DeleteSecurityContext", {kCtx}), SEC_E_OK_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// fch_* / dsc_*: handle validation on release.
TEST_P(SspiContractTest, ReleaseValidatesHandles) {
    make_cred();
    make_ctx();
    const GuestAddr h = kOut;
    wptr(h, 0x1234); wptr(h + ps(), 0x5678);
    EXPECT_EQ(call("FreeCredentialsHandle", {h}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(call("DeleteSecurityContext", {h}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(call("QueryContextAttributesW", {h, 4, kOut + 0x40}), SEC_E_INVALID_HANDLE_);
    wptr(h, 0); wptr(h + ps(), 0);
    EXPECT_EQ(call("FreeCredentialsHandle", {h}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(call("DeleteSecurityContext", {h}), SEC_E_OK_);
    EXPECT_EQ(call("DeleteSecurityContext", {kCtx}), SEC_E_OK_);
    EXPECT_EQ(call("DeleteSecurityContext", {kCtx}), SEC_E_INVALID_HANDLE_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_INVALID_HANDLE_);
}

// ---------------------------------------------------------------------------
// Record layer against a real TLS 1.2 peer (the system `openssl s_server
// -www`). Expected values: tools/oracle_probe/sspi_schannel_stream_probe
// .expected (Windows 11, TLS 1.2 AES-GCM, in-process Schannel server).
// ---------------------------------------------------------------------------

struct TlsPeer {
    pid_t pid = -1;
    int port = 0;
    std::string dir;
    bool start() {
        if (access("/usr/bin/openssl", X_OK) != 0) return false;
        char tmpl[] = "/tmp/vitreo_sspi_peer_XXXXXX";
        if (!mkdtemp(tmpl)) return false;
        dir = tmpl;
        const std::string gen = "/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 2 "
                                "-subj /CN=localhost -keyout " + dir + "/k.pem -out " + dir +
                                "/c.pem >/dev/null 2>&1";
        if (std::system(gen.c_str()) != 0) return false;
        const int probe = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t al = sizeof(a);
        if (bind(probe, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            getsockname(probe, reinterpret_cast<sockaddr*>(&a), &al) != 0) {
            close(probe);
            return false;
        }
        port = ntohs(a.sin_port);
        close(probe);
        const std::string port_s = std::to_string(port), cert = dir + "/c.pem", key = dir + "/k.pem";
        const char* argv[] = {"/usr/bin/openssl", "s_server", "-accept", port_s.c_str(), "-cert",
                              cert.c_str(), "-key", key.c_str(), "-www", "-tls1_2", "-quiet", nullptr};
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        const int rc = posix_spawn(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv), environ);
        posix_spawn_file_actions_destroy(&fa);
        return rc == 0;
    }
    int connect_client() const {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int s = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(static_cast<u16>(port));
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
                timeval tv{5, 0};
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                return s;
            }
            close(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return -1;
    }
    ~TlsPeer() {
        if (pid > 0) { kill(pid, SIGKILL); int st = 0; waitpid(pid, &st, 0); }
        if (!dir.empty()) {
            unlink((dir + "/k.pem").c_str());
            unlink((dir + "/c.pem").c_str());
            rmdir(dir.c_str());
        }
    }
};

TEST_P(SspiContractTest, StreamRecordLayerAgainstTlsPeer) {
    TlsPeer peer;
    if (!peer.start()) GTEST_SKIP() << "/usr/bin/openssl s_server unavailable";
    const int sock = peer.connect_client();
    ASSERT_GE(sock, 0);
    auto send_guest = [&](GuestAddr p, u32 n) {
        std::vector<u8> tmp(n);
        ASSERT_TRUE(memory.read_bytes(p, reinterpret_cast<Byte*>(tmp.data()), n).is_ok());
        ASSERT_EQ(send(sock, tmp.data(), n, 0), static_cast<ssize_t>(n));
    };
    auto recv_some = [&](std::vector<u8>& into) {
        u8 chunk[16384];
        const ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
        if (n > 0) into.insert(into.end(), chunk, chunk + n);
        return n;
    };

    // Handshake through the guest-visible SSPI entry points.
    make_cred();
    set_buf(kOutArr, 0, 0, 2, 0);
    set_buf(kOutArr, 1, 0, 17, 0);
    u64 st = isc(kCred, 0, kReqCurl, 0, desc(kOutDesc, kOutArr, 2));
    ASSERT_EQ(st, SEC_I_CONTINUE_NEEDED_);
    wptr(kCtx, rptr(kNewCtx));
    wptr(kCtx + ps(), 0);
    send_guest(static_cast<GuestAddr>(buf_pv(kOutArr, 0)), buf_cb(kOutArr, 0));
    (void)call("FreeContextBuffer", {buf_pv(kOutArr, 0)});
    std::vector<u8> in;
    int rounds = 0;
    while (st != SEC_E_OK_ && rounds++ < 40) {
        if (in.empty() || st == SEC_E_INCOMPLETE_MESSAGE_) ASSERT_GT(recv_some(in), 0);
        st = isc2(kCred, kCtx, in);
        if (st == SEC_E_INCOMPLETE_MESSAGE_) continue;
        ASSERT_TRUE(st == SEC_E_OK_ || st == SEC_I_CONTINUE_NEEDED_) << std::hex << st;
        if (buf_cb(kOutArr, 0) != 0) {
            send_guest(static_cast<GuestAddr>(buf_pv(kOutArr, 0)), buf_cb(kOutArr, 0));
            (void)call("FreeContextBuffer", {buf_pv(kOutArr, 0)});
        }
        if (buf_type(kInArr, 1) == 5) in.erase(in.begin(), in.end() - buf_cb(kInArr, 1));
        else in.clear();
    }
    ASSERT_EQ(st, SEC_E_OK_);

    // stream_sizes: header=13 trailer=16 max=16384 cbuffers=4 block=16.
    ASSERT_EQ(call("QueryContextAttributesW", {kCtx, 4, kOut}), SEC_E_OK_);
    EXPECT_EQ(memory.read_u32(kOut).value_or(0), 13u);
    EXPECT_EQ(memory.read_u32(kOut + 4).value_or(0), 16u);
    EXPECT_EQ(memory.read_u32(kOut + 8).value_or(0), 16384u);
    EXPECT_EQ(memory.read_u32(kOut + 12).value_or(0), 4u);
    EXPECT_EQ(memory.read_u32(kOut + 16).value_or(0), 16u);

    // EncryptMessage failures (buffers found by type; nothing consumed).
    const GuestAddr H = kData + 0xC000, D = kData + 0xD000, T = kData + 0x10000;
    const std::string request = "GET / HTTP/1.0\r\n\r\n";
    auto encrypt = [&](u32 hcb, u32 dcb, u32 tcb, int omit, u32 count = 4) {
        for (usize i = 0; i < request.size(); ++i) (void)memory.write_u8(D + i, static_cast<u8>(request[i]));
        u32 types[4] = {7, 1, 6, 0};
        if (omit >= 0) types[omit] = 0;
        set_buf(kInArr, 0, hcb, types[0], H);
        set_buf(kInArr, 1, dcb, types[1], D);
        set_buf(kInArr, 2, tcb, types[2], T);
        set_buf(kInArr, 3, 0, types[3], 0);
        return call("EncryptMessage", {kCtx, 0, desc(kInDesc, kInArr, count), 0});
    };
    const u32 len = static_cast<u32>(request.size());
    constexpr u64 SEC_E_ENCRYPT_FAILURE_ = 0x80090329;
    EXPECT_EQ(encrypt(13, len, 16, 1), SEC_E_INVALID_TOKEN_);          // no_data
    EXPECT_EQ(encrypt(13, len, 16, 0), SEC_E_ENCRYPT_FAILURE_);        // no_header
    EXPECT_EQ(encrypt(13, len, 16, 2), SEC_E_ENCRYPT_FAILURE_);        // no_trailer
    EXPECT_EQ(encrypt(12, len, 16, -1), SEC_E_ENCRYPT_FAILURE_);       // header_short
    EXPECT_EQ(encrypt(14, len, 16, -1), SEC_E_ENCRYPT_FAILURE_);       // header_plus1
    EXPECT_EQ(encrypt(13, len, 15, -1), SEC_E_ENCRYPT_FAILURE_);       // trailer_short
    EXPECT_EQ(encrypt(13, 16385, 16, -1), SEC_E_ENCRYPT_FAILURE_);     // data_max_plus1
    EXPECT_EQ(buf_cb(kInArr, 0), 13u);

    // separate16 + trailer_oversized + three_buffers: each part lands in its
    // own buffer; the trailer shrinks to 16.
    ASSERT_EQ(encrypt(13, len, 64, -1, 3), SEC_E_OK_);
    EXPECT_EQ(buf_cb(kInArr, 0), 13u);
    EXPECT_EQ(buf_cb(kInArr, 1), len);
    EXPECT_EQ(buf_cb(kInArr, 2), 16u);
    EXPECT_EQ(memory.read_u8(H).value_or(0), 0x17u);                  // application_data
    EXPECT_EQ(memory.read_u8(H + 3).value_or(0) << 8 | memory.read_u8(H + 4).value_or(0),
              static_cast<int>(8 + len + 16));
    send_guest(H, 13);
    send_guest(D, len);
    send_guest(T, 16);

    // Collect the whole response (the server closes after the page).
    std::vector<u8> stream;
    while (recv_some(stream) > 0) {}
    close(sock);
    ASSERT_GT(stream.size(), 5u);
    const u32 first = 5 + (stream[3] << 8 | stream[4]);

    const GuestAddr base = kData + 0x20000;
    auto decrypt = [&](const u8* bytes, u32 n, u32 count) {
        ASSERT_TRUE(memory.write_bytes(base, reinterpret_cast<const Byte*>(bytes), n).is_ok());
        set_buf(kInArr, 0, n, 1, base);
        for (u32 i = 1; i < 5; ++i) set_buf(kInArr, i, 0, 0, 0);
        (void)memory.write_u32(kOut, kSent);
        st = call("DecryptMessage", {kCtx, desc(kInDesc, kInArr, count), 0, kOut});
        EXPECT_EQ(memory.read_u32(kOut).value_or(0), kSent);           // pfQOP untouched
    };
    // partial3 / partial5 / partial_half: MISSING in the data buffer and in
    // the first empty buffer; the data buffer keeps its pointer.
    for (u32 n : {3u, 5u, first / 2}) {
        decrypt(stream.data(), n, 4);
        EXPECT_EQ(st, SEC_E_INCOMPLETE_MESSAGE_) << n;
        const u32 missing = n < 13 ? 13 - n : first - n;
        EXPECT_EQ(buf_type(kInArr, 0), 4u);
        EXPECT_EQ(buf_cb(kInArr, 0), missing);
        EXPECT_EQ(buf_pv(kInArr, 0), base);
        EXPECT_EQ(buf_type(kInArr, 1), 4u);
        EXPECT_EQ(buf_cb(kInArr, 1), missing);
        EXPECT_EQ(buf_pv(kInArr, 1), 0u);
    }
    // full_2buf / full_3buf: fewer than four buffers, untouched.
    for (u32 count : {1u, 2u, 3u}) {
        decrypt(stream.data(), first, count);
        EXPECT_EQ(st, SEC_E_INVALID_TOKEN_);
        EXPECT_EQ(buf_type(kInArr, 0), 1u);
        EXPECT_EQ(buf_cb(kInArr, 0), first);
    }
    // One record per call, decrypted in place: header | data | trailer (the
    // rest of the input) | EXTRA (the next record).
    std::string plaintext;
    usize pos = 0;
    bool closed = false;
    while (pos < stream.size() && !closed) {
        const u32 n = static_cast<u32>(stream.size() - pos);
        decrypt(stream.data() + pos, n, 5);
        if (st == 0x00090317) {                                         // close_notify
            closed = true;
            EXPECT_EQ(buf_type(kInArr, 0), 1u);                         // untouched
            EXPECT_EQ(buf_cb(kInArr, 0), n);
            break;
        }
        ASSERT_EQ(st, SEC_E_OK_);
        const u32 rec = 5 + (stream[pos + 3] << 8 | stream[pos + 4]);
        const u32 plen = buf_cb(kInArr, 1);
        EXPECT_EQ(buf_type(kInArr, 0), 7u);
        EXPECT_EQ(buf_cb(kInArr, 0), 13u);
        EXPECT_EQ(buf_pv(kInArr, 0), base);
        EXPECT_EQ(buf_type(kInArr, 1), 1u);
        EXPECT_EQ(buf_pv(kInArr, 1), base + 13);
        EXPECT_EQ(plen, rec - 13 - 16);
        EXPECT_EQ(buf_type(kInArr, 2), 6u);
        EXPECT_EQ(buf_pv(kInArr, 2), base + 13 + plen);
        EXPECT_EQ(buf_cb(kInArr, 2), n - 13 - plen);
        if (n > rec) {
            EXPECT_EQ(buf_type(kInArr, 3), 5u);
            EXPECT_EQ(buf_pv(kInArr, 3), base + rec);
            EXPECT_EQ(buf_cb(kInArr, 3), n - rec);
        } else {
            EXPECT_EQ(buf_type(kInArr, 3), 0u);
            EXPECT_EQ(buf_cb(kInArr, 3), 0u);
        }
        EXPECT_EQ(buf_type(kInArr, 4), 0u);                             // full_5buf
        for (u32 i = 0; i < plen; ++i)
            plaintext.push_back(static_cast<char>(memory.read_u8(base + 13 + i).value_or(0)));
        pos += rec;
    }
    EXPECT_EQ(plaintext.rfind("HTTP/1.0 200", 0), 0u) << plaintext.substr(0, 40);
    // LibreSSL's s_server marks its side shut down without sending
    // close_notify, so the SEC_I_CONTEXT_EXPIRED path (buffers untouched,
    // stream probe "close_notify") is only reached when a peer sends one.
    (void)closed;
    EXPECT_EQ(call("DeleteSecurityContext", {kCtx}), SEC_E_OK_);
    EXPECT_EQ(call("FreeCredentialsHandle", {kCred}), SEC_E_OK_);
}

// x86 stdcall byte counts from sspi.h / ntsecapi.h / secext.h prototypes.
TEST(SspiExportTable, StdcallByteCountsMatchPrototypes) {
    Secur32Manager mgr;
    const DllExports exports = create_secur32_thunks(mgr);
    const std::pair<const char*, u32> expected[] = {
        {"AcquireCredentialsHandleA", 36}, {"AcquireCredentialsHandleW", 36},
        {"InitializeSecurityContextA", 48}, {"InitializeSecurityContextW", 48},
        {"QuerySecurityPackageInfoA", 8}, {"QuerySecurityPackageInfoW", 8},
        {"EnumerateSecurityPackagesA", 8}, {"EnumerateSecurityPackagesW", 8},
        {"EncryptMessage", 16}, {"DecryptMessage", 16}, {"DeleteSecurityContext", 4},
        {"FreeCredentialsHandle", 4}, {"FreeContextBuffer", 4}, {"AcceptSecurityContext", 36},
        {"GetUserNameExA", 12}, {"GetUserNameExW", 12}, {"LsaConnectUntrusted", 4},
        {"LsaLookupAuthenticationPackage", 12}, {"LsaCallAuthenticationPackage", 28},
        {"LsaLogonUser", 56}, {"LsaFreeReturnBuffer", 4}, {"LsaDeregisterLogonProcess", 4},
    };
    for (const auto& [name, bytes] : expected) {
        const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                     [&](const ExportEntry& e) { return e.name == name; });
        ASSERT_NE(it, exports.entries.end()) << name;
        EXPECT_EQ(it->stdcall_bytes, bytes) << name;
    }
}

} // namespace
