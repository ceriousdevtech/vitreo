/// @file test_sspicli.cpp
/// @brief SspiCli.dll / secur32.dll GetUserNameEx and untrusted-LSA contract.
///
/// Expected values are Windows 11 (workgroup machine) measurements from
/// tools/oracle_probe/sspi_secur32_probe.c (sspi_secur32_probe.expected,
/// .x86.expected, .nullh.expected), for both bitnesses and both DLLs.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/networking/sspicli/sspicli.hpp>
#include <winapi/networking/secur32/secur32.hpp>
#include <winapi/advapi32/security/security.hpp>
#include <winapi/kernel32/system/sysinfo.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 kSent = 0x5A5A5A5Au;
constexpr u32 ERROR_MORE_DATA_ = 234;
constexpr u32 ERROR_INVALID_PARAMETER_ = 87;
constexpr u32 ERROR_NONE_MAPPED_ = 1332;

// Param: (x64, dll). Windows' secur32 forwards these exports to sspicli, so
// both must behave identically.
class SspiCliContractTest
    : public ::testing::TestWithParam<std::tuple<bool, const char*>> {
protected:
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kData = 0x00400000;
    static constexpr GuestAddr kBuf = kData + 0x100;
    static constexpr GuestAddr kSize = kData + 0x800;
    static constexpr GuestAddr kOut = kData + 0x900;
    static constexpr GuestAddr kStr = kData + 0xA00;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
        is64 = std::get<0>(GetParam());
        dll = std::get<1>(GetParam());
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        exports = std::string(dll) == "sspicli.dll" ? create_sspicli_thunks(sspicli)
                                                     : create_secur32_thunks(secur32);
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
        const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                     [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << dll << "!" << name;
        if (it == exports.entries.end()) return ~0ULL;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        cpu.gpr.rax = 0xDEADDEADDEADDEADULL;
        it->handler(m);
        return is64 ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }
    u32 ps() const { return is64 ? 8 : 4; }
    u64 rptr(GuestAddr a) { return is64 ? memory.read_u64(a).value_or(0) : memory.read_u32(a).value_or(0); }
    void wptr(GuestAddr a, u64 v) {
        if (is64) (void)memory.write_u64(a, v); else (void)memory.write_u32(a, static_cast<u32>(v));
    }
    // LSA_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
    GuestAddr lsa_string(GuestAddr at, std::string_view s) {
        const GuestAddr text = at + 16;
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(text + i, static_cast<u8>(s[i]));
        (void)memory.write_u16(at, static_cast<u16>(s.size()));
        (void)memory.write_u16(at + 2, static_cast<u16>(s.size()));
        wptr(at + (is64 ? 8 : 4), text);
        return at;
    }
    void fill(u8 v, u32 n = 0x600) { for (u32 i = 0; i < n; ++i) (void)memory.write_u8(kBuf + i, v); }
    u32 touched_w() {
        u32 n = 0;
        for (u32 i = 0; i < 0x300; ++i) n += memory.read_u16(kBuf + i * 2).value_or(0) != 0x5A5A;
        return n;
    }
    u32 touched_a() {
        u32 n = 0;
        for (u32 i = 0; i < 0x600; ++i) n += memory.read_u8(kBuf + i).value_or(0) != 0x5A;
        return n;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    SspiCliManager sspicli;
    Secur32Manager secur32;
    DllExports exports;
    bool is64 = false;
    const char* dll = "";
};

INSTANTIATE_TEST_SUITE_P(
    BitnessAndDll, SspiCliContractTest,
    ::testing::Combine(::testing::Values(false, true),
                       ::testing::Values("sspicli.dll", "secur32.dll")),
    [](const ::testing::TestParamInfo<std::tuple<bool, const char*>>& info) {
        return std::string(std::get<0>(info.param) ? "x64_" : "x86_") +
               (std::string(std::get<1>(info.param)) == "sspicli.dll" ? "sspicli" : "secur32");
    });

// gunex_w: NameSamCompatible is "<computer>\<user>", nSize = chars without
// the NUL, last error untouched; every other format of a non-domain account
// is ERROR_NONE_MAPPED (buffer and nSize untouched); > NameSurname is
// ERROR_INVALID_PARAMETER.
TEST_P(SspiCliContractTest, GetUserNameExWFormats) {
    const std::string sam = sspi_sam_compatible_name();
    const u32 len = static_cast<u32>(sam.size());
    for (u32 fmt = 0; fmt <= 15; ++fmt) {
        fill(0x5A);
        (void)memory.write_u32(kSize, 600);
        set_last_error(0xDEAD);
        const u64 ok = call("GetUserNameExW", {fmt, kBuf, kSize});
        if (fmt == 2) {
            EXPECT_EQ(ok & 0xFF, 1u);
            EXPECT_EQ(get_last_error(), 0xDEADu);
            EXPECT_EQ(memory.read_u32(kSize).value_or(0), len);
            for (u32 i = 0; i < len; ++i)
                EXPECT_EQ(memory.read_u16(kBuf + i * 2).value_or(0), static_cast<u8>(sam[i]));
            EXPECT_EQ(memory.read_u16(kBuf + len * 2).value_or(1), 0u);
        } else {
            EXPECT_EQ(ok & 0xFF, 0u) << fmt;
            EXPECT_EQ(get_last_error(), fmt == 15 ? ERROR_INVALID_PARAMETER_ : ERROR_NONE_MAPPED_) << fmt;
            EXPECT_EQ(memory.read_u32(kSize).value_or(0), 600u) << fmt;
            EXPECT_EQ(touched_w(), 0u) << fmt;
        }
    }
}

// gunex_w_size: ERROR_MORE_DATA with nSize = len + 1; an exactly-len buffer
// receives the name without its NUL.
TEST_P(SspiCliContractTest, GetUserNameExWSizing) {
    const u32 len = static_cast<u32>(std::string(sspi_sam_compatible_name()).size());
    const struct { bool null_buf; u32 n; u32 written; } cases[] = {
        {true, 0, 0}, {false, 0, 0}, {false, 1, 0}, {false, len, len},
    };
    for (const auto& c : cases) {
        fill(0x5A);
        (void)memory.write_u32(kSize, c.n);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetUserNameExW", {2, c.null_buf ? 0 : kBuf, kSize}) & 0xFF, 0u);
        EXPECT_EQ(get_last_error(), ERROR_MORE_DATA_);
        EXPECT_EQ(memory.read_u32(kSize).value_or(0), len + 1);
        EXPECT_EQ(touched_w(), c.written) << c.n;
    }
    fill(0x5A);
    (void)memory.write_u32(kSize, len + 1);
    EXPECT_EQ(call("GetUserNameExW", {2, kBuf, kSize}) & 0xFF, 1u);
    EXPECT_EQ(memory.read_u32(kSize).value_or(0), len);
    EXPECT_EQ(touched_w(), len + 1);
}

// gunex_a / gunex_a_size: the ANSI entry reports every failure size in bytes
// of the UTF-16 name (2 * the wide value) and never writes on failure.
TEST_P(SspiCliContractTest, GetUserNameExA) {
    const std::string sam = sspi_sam_compatible_name();
    const u32 len = static_cast<u32>(sam.size());
    for (u32 fmt : {0u, 1u, 3u, 14u, 15u}) {
        fill(0x5A);
        (void)memory.write_u32(kSize, 600);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetUserNameExA", {fmt, kBuf, kSize}) & 0xFF, 0u);
        EXPECT_EQ(get_last_error(), fmt == 15 ? ERROR_INVALID_PARAMETER_ : ERROR_NONE_MAPPED_);
        EXPECT_EQ(memory.read_u32(kSize).value_or(0), 1200u);
        EXPECT_EQ(touched_a(), 0u);
    }
    for (u32 n : {0u, 1u, len}) {
        fill(0x5A);
        (void)memory.write_u32(kSize, n);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetUserNameExA", {2, n ? kBuf : 0, kSize}) & 0xFF, 0u);
        EXPECT_EQ(get_last_error(), ERROR_MORE_DATA_);
        EXPECT_EQ(memory.read_u32(kSize).value_or(0), 2 * (len + 1));
        EXPECT_EQ(touched_a(), 0u);
    }
    fill(0x5A);
    (void)memory.write_u32(kSize, len + 1);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("GetUserNameExA", {2, kBuf, kSize}) & 0xFF, 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(memory.read_u32(kSize).value_or(0), len);
    for (u32 i = 0; i < len; ++i) EXPECT_EQ(memory.read_u8(kBuf + i).value_or(0), static_cast<u8>(sam[i]));
    EXPECT_EQ(memory.read_u8(kBuf + len).value_or(1), 0u);
}

// lsa_*: an untrusted connection to an LSA without authentication packages.
TEST_P(SspiCliContractTest, UntrustedLsaConnection) {
    // lsa_null:connect_nullptr
    EXPECT_EQ(call("LsaConnectUntrusted", {0}), 0xC0030009u);
    // HANDLE is pointer-sized: all 8 bytes are written on x64.
    (void)memory.write_u64(kOut, 0x5A5A5A5A5A5A5A5AULL);
    ASSERT_EQ(call("LsaConnectUntrusted", {kOut}), 0u);
    const u64 h = rptr(kOut);
    EXPECT_NE(h, 0u);
    if (is64) EXPECT_EQ(h >> 32, 0u);

    // Lookup: no package exists; the id is set to 0xFFFFFFFF.
    for (const char* pkg : {"Negotiate", "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0", "Kerberos", "Bogus", ""}) {
        (void)memory.write_u32(kOut + 0x10, kSent);
        EXPECT_EQ(call("LsaLookupAuthenticationPackage", {h, lsa_string(kStr, pkg), kOut + 0x10}),
                  0xC00000FEu) << pkg;
        EXPECT_EQ(memory.read_u32(kOut + 0x10).value_or(0), 0xFFFFFFFFu);
    }
    // lsa_null:lookup_nullh / call_nullh
    EXPECT_EQ(call("LsaLookupAuthenticationPackage", {0, lsa_string(kStr, "Negotiate"), kOut + 0x10}),
              0xC0000140u);
    EXPECT_EQ(call("LsaCallAuthenticationPackage", {0, 0, kStr, 4, kOut + 0x20, kOut + 0x30, kOut + 0x38}),
              0xC0000140u);

    // lsa_call_bogus_pkg: return buffer NULL, length 0, protocol status 0.
    wptr(kOut + 0x20, kSent);
    (void)memory.write_u32(kOut + 0x30, kSent);
    (void)memory.write_u32(kOut + 0x38, kSent);
    EXPECT_EQ(call("LsaCallAuthenticationPackage", {h, 0x7777, kStr, 4, kOut + 0x20, kOut + 0x30, kOut + 0x38}),
              0xC00000FEu);
    EXPECT_EQ(rptr(kOut + 0x20), 0u);
    EXPECT_EQ(memory.read_u32(kOut + 0x30).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(kOut + 0x38).value_or(1), 0u);

    // lsa_logon_bogus_pkg: STATUS_NOT_SUPPORTED; ProfileBufferLength and
    // SubStatus zeroed; ProfileBuffer, LogonId and Token untouched.
    const GuestAddr prof = kOut + 0x40, proflen = kOut + 0x50, luid = kOut + 0x58,
                    token = kOut + 0x60, sub = kOut + 0x70;
    wptr(prof, kSent);
    (void)memory.write_u32(proflen, kSent);
    (void)memory.write_u64(luid, 0x5A5A5A5A5A5A5A5AULL);
    wptr(token, kSent);
    (void)memory.write_u32(sub, kSent);
    EXPECT_EQ(call("LsaLogonUser", {h, lsa_string(kStr, "vitprobe"), 3, 0x7777, kStr, 4, 0, kStr + 0x100,
                                    prof, proflen, luid, token, kOut + 0x80, sub}),
              0xC00000BBu);
    EXPECT_EQ(rptr(prof), kSent);
    EXPECT_EQ(memory.read_u32(proflen).value_or(1), 0u);
    EXPECT_EQ(memory.read_u64(luid).value_or(0), 0x5A5A5A5A5A5A5A5AULL);
    EXPECT_EQ(rptr(token), kSent);
    EXPECT_EQ(memory.read_u32(sub).value_or(1), 0u);
    // lsa_null:logon_nullh
    EXPECT_EQ(call("LsaLogonUser", {0, lsa_string(kStr, "vitprobe"), 3, 0, kStr, 4, 0, kStr + 0x100,
                                    prof, proflen, luid, token, kOut + 0x80, sub}),
              0xC0030004u);

    // lsa_free_null / lsa_dereg.
    EXPECT_EQ(call("LsaFreeReturnBuffer", {0}), 0xC00000A0u);
    EXPECT_EQ(call("LsaDeregisterLogonProcess", {h}), 0u);
    EXPECT_EQ(call("LsaLookupAuthenticationPackage", {h, lsa_string(kStr, "Negotiate"), kOut + 0x10}),
              0xC0000140u);
}

// x86 stdcall byte counts (ntsecapi.h, secext.h).
TEST(SspiCliExportTable, StdcallByteCounts) {
    SspiCliManager mgr;
    const DllExports exports = create_sspicli_thunks(mgr);
    const std::pair<const char*, u32> expected[] = {
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

// NameSamCompatible must name the same account GetComputerNameW and
// GetUserNameW report (Windows: "<C>\<U>" in sspi_secur32_probe.expected).
TEST(SspiGetUserNameExTest, MatchesGetComputerNameAndGetUserName) {
    SysInfoManager sys;
    HandleTable handles;
    SecurityStubs stubs{handles};
    std::wstring user;
    u32 n = 256;
    ASSERT_TRUE(stubs.get_user_name_w(user, n));
    std::string expected = sys.get_computer_name() + "\\";
    for (wchar_t c : user) expected.push_back(static_cast<char>(c));
    EXPECT_EQ(std::string(sspi_sam_compatible_name()), expected);
}

} // namespace

// gcon_w / gcon_w_null / gcon_w_small / gcon_a in
// userenv_computer_object_probe (x64 and x86 identical): a workgroup computer
// has no directory object, so NameUnknown is ERROR_INVALID_PARAMETER and every
// other value (0..15) ERROR_CANT_ACCESS_DOMAIN_INFO; the buffer is never
// written, W leaves nSize alone and A doubles it. secur32 exports it itself;
// sspicli does not.
TEST_P(SspiCliContractTest, GetComputerObjectNameOnWorkgroupComputer) {
    constexpr u32 ERROR_CANT_ACCESS_DOMAIN_INFO_ = 1351;
    const bool own = std::string(dll) == "secur32.dll";
    for (const char* name : {"GetComputerObjectNameW", "GetComputerObjectNameA"}) {
        EXPECT_EQ(std::any_of(exports.entries.begin(), exports.entries.end(),
                              [&](const ExportEntry& e) { return e.name == name; }),
                  own) << dll << "!" << name;
    }
    if (!own) return;

    for (u32 format = 0; format <= 15; ++format) {
        const u32 err = format == 0 ? ERROR_INVALID_PARAMETER_ : ERROR_CANT_ACCESS_DOMAIN_INFO_;
        for (u32 n_in : {300u, 2u}) {
            fill(0x5A);
            (void)memory.write_u32(kSize, n_in);
            set_last_error(0xDEAD);
            EXPECT_EQ(call("GetComputerObjectNameW", {format, kBuf, kSize}) & 0xFF, 0u);
            EXPECT_EQ(get_last_error(), err) << format;
            EXPECT_EQ(memory.read_u32(kSize).value(), n_in);
            EXPECT_EQ(touched_w(), 0u);
        }
        (void)memory.write_u32(kSize, 0);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetComputerObjectNameW", {format, 0, kSize}) & 0xFF, 0u);
        EXPECT_EQ(get_last_error(), err);
        EXPECT_EQ(memory.read_u32(kSize).value(), 0u);

        fill(0x5A);
        (void)memory.write_u32(kSize, 300);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("GetComputerObjectNameA", {format, kBuf, kSize}) & 0xFF, 0u);
        EXPECT_EQ(get_last_error(), err);
        EXPECT_EQ(memory.read_u32(kSize).value(), 600u);
        EXPECT_EQ(touched_a(), 0u);
    }
}
