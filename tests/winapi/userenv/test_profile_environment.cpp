/// @file test_profile_environment.cpp
/// @brief userenv ExpandEnvironmentStringsForUser and the logon blocks behind
/// it (and CreateEnvironmentBlock(bInherit=FALSE)).
///
/// Expected values are Windows 11 measurements from
/// tools/oracle_probe/userenv_computer_object_probe.c (default and "block"
/// modes), identical for x64 and x86. Machine-specific values are replaced by
/// this capsule's equivalents: USERPROFILE, SystemRoot, COMPUTERNAME.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/userenv/profile_environment.hpp>
#include <winapi/kernel32/environment/environment.hpp>
#include <winapi/advapi32/security/security.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 kTokenQuery = 0x0008;
constexpr u32 kTokenImpersonateQueryDuplicate = 0x0004 | 0x0008 | 0x0002;
constexpr u64 kCurrentProcess = 0xFFFFFFFFFFFFFFFFull;

class ProfileEnvironmentTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kData = 0x00400000;
    static constexpr GuestAddr kSrc = kData + 0x100;
    static constexpr GuestAddr kDst = kData + 0x1000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
        is64 = GetParam();
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        // The capsule's logon variables, plus one per-process addition that
        // no logon block carries.
        for (const auto& [k, v] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"SystemDrive", "C:"}, {"SystemRoot", "C:\\Windows"},
                 {"windir", "C:\\Windows"}, {"COMPUTERNAME", "VITREO"},
                 {"USERNAME", "Default"}, {"USERPROFILE", "C:\\Users\\Default"},
                 {"APPDATA", "C:\\Users\\Default\\AppData\\Roaming"},
                 {"LOCALAPPDATA", "C:\\Users\\Default\\AppData\\Local"},
                 {"HOMEDRIVE", "C:"}, {"HOMEPATH", "\\Users\\Default"},
                 {"LOGONSERVER", "\\\\VITREO"}, {"USERDOMAIN", "VITREO"},
                 {"USERDOMAIN_ROAMINGPROFILE", "VITREO"},
                 {"TEMP", "C:\\Users\\Default\\AppData\\Local\\Temp"},
                 {"TMP", "C:\\Users\\Default\\AppData\\Local\\Temp"},
                 {"ProgramFiles", "C:\\Program Files"}, {"OS", "Windows_NT"},
                 {"SteamAppId", "265930"}}) {
            env.set_variable(k, v);
        }
        u32 error = 0;
        ASSERT_TRUE(security.open_process_token(static_cast<Handle>(kCurrentProcess),
                                                kTokenImpersonateQueryDuplicate, full_token, error));
        ASSERT_TRUE(security.open_process_token(static_cast<Handle>(kCurrentProcess),
                                                kTokenQuery, query_token, error));
    }

    u64 call(bool wide, u64 token, std::u16string_view src, u32 size) {
        for (usize i = 0; i < src.size(); ++i) {
            if (wide) (void)memory.write_u16(kSrc + i * 2, src[i]);
            else (void)memory.write_u8(kSrc + i, static_cast<u8>(src[i]));
        }
        if (wide) (void)memory.write_u16(kSrc + src.size() * 2, 0);
        else (void)memory.write_u8(kSrc + src.size(), 0);
        for (u32 i = 0; i < 0x400; ++i) (void)memory.write_u8(kDst + i, 0x5A);

        const u64 args[] = {token, kSrc, size ? kDst : 0, size};
        cpu.set_sp(kStack + 0xE000);
        const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
        if (is64) {
            cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1];
            cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
        } else {
            for (u32 i = 0; i < 4; ++i)
                (void)memory.write_u32(sp + 4 + i * 4, static_cast<u32>(args[i]));
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        cpu.gpr.rax = 0xDEADDEADDEADDEADULL;
        set_last_error(0xDEAD);
        impl_ExpandEnvironmentStringsForUser(m, env, security, handles, wide);
        return is64 ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }
    std::string out_w(u32 max = 0x200) {
        std::string s;
        for (u32 i = 0; i < max; ++i) {
            const u16 c = memory.read_u16(kDst + i * 2).value_or(0);
            if (c == 0 || c == 0x5A5A) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    bool terminated_w(u32 len) { return memory.read_u16(kDst + len * 2).value_or(1) == 0; }
    u32 touched_a() {
        u32 n = 0;
        for (u32 i = 0; i < 0x400; ++i) n += memory.read_u8(kDst + i).value_or(0) != 0x5A;
        return n;
    }
    u64 token(bool user) const { return user ? full_token : 0; }

    GuestMemory memory;
    cpu::CpuState cpu;
    EnvironmentManager env{"test.exe", memory};
    HandleTable handles;
    SecurityStubs security{handles};
    Handle full_token{};
    Handle query_token{};
    bool is64 = false;
};

INSTANTIATE_TEST_SUITE_P(Bitness, ProfileEnvironmentTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });

} // namespace

// eesfu_w null_tok_* / proc_tok_*: the NULL token expands against the system
// logon block, a user token against the user's, never the process block.
TEST_P(ProfileEnvironmentTest, ExpandsAgainstTheLogonBlockForTheToken) {
    struct Case { std::u16string src; std::string null_tok; std::string proc_tok; };
    const Case cases[] = {
        {u"%USERPROFILE%", "C:\\Users\\Default", "C:\\Users\\Default"},
        {u"%SystemRoot%\\x", "C:\\Windows\\x", "C:\\Windows\\x"},
        {u"%COMPUTERNAME%", "VITREO", "VITREO"},
        {u"%NOPE_VAR%|%%|50%", "%NOPE_VAR%|%%|50%", "%NOPE_VAR%|%%|50%"},
        {u"plain", "plain", "plain"},
        {u"", "", ""},
        {u"%TEMP%", "C:\\Windows\\TEMP", "C:\\Users\\Default\\AppData\\Local\\Temp"},
        {u"%APPDATA%", "%APPDATA%", "C:\\Users\\Default\\AppData\\Roaming"},
        {u"%USERNAME%", "SYSTEM", "Default"},
        {u"%SteamAppId%", "%SteamAppId%", "%SteamAppId%"},
    };
    for (const Case& c : cases) {
        for (bool user : {false, true}) {
            const std::string& want = user ? c.proc_tok : c.null_tok;
            EXPECT_EQ(call(true, token(user), c.src, 512), 1u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_ENVVAR_NOT_FOUND);
            EXPECT_EQ(out_w(), want);
            EXPECT_TRUE(terminated_w(static_cast<u32>(want.size())));

            // eesfu_a: returns the character count including the NUL.
            EXPECT_EQ(call(false, token(user), c.src, 512), want.size() + 1);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_ENVVAR_NOT_FOUND);
            EXPECT_EQ(touched_a(), want.size() + 1);
        }
    }
}

// eesfu_w plain_size_N: below length+1 the call fails with
// ERROR_INSUFFICIENT_BUFFER, W having copied size-1 characters unterminated.
TEST_P(ProfileEnvironmentTest, WideBufferTooSmallCopiesSizeMinusOneCharacters) {
    for (u32 size = 0; size <= 5; ++size) {
        EXPECT_EQ(call(true, 0, u"plain", size), 0u) << size;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
        const u32 copied = size >= 2 ? size - 1 : 0;
        EXPECT_EQ(out_w(), std::string("plain").substr(0, copied)) << size;
        EXPECT_EQ(memory.read_u16(kDst + copied * 2).value(), 0x5A5Au) << size;
    }
    for (u32 size : {6u, 7u}) {
        EXPECT_EQ(call(true, 0, u"plain", size), 1u);
        EXPECT_EQ(out_w(), "plain");
        EXPECT_TRUE(terminated_w(5));
    }
}

// eesfu_a a_plain_size_N: the ANSI call writes nothing when it fails.
TEST_P(ProfileEnvironmentTest, AnsiBufferTooSmallWritesNothing) {
    for (u32 size : {4u, 5u}) {
        EXPECT_EQ(call(false, 0, u"plain", size), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
        EXPECT_EQ(touched_a(), 0u);
    }
    for (u32 size : {6u, 7u}) {
        EXPECT_EQ(call(false, 0, u"plain", size), 6u);
        EXPECT_EQ(touched_a(), 6u);
    }
}

// query_only_tok: still expands as the user, but leaves ERROR_ACCESS_DENIED;
// bogus_tok: FALSE / ERROR_INVALID_HANDLE with nothing written.
TEST_P(ProfileEnvironmentTest, TokenAccessAndValidity) {
    EXPECT_EQ(call(true, query_token, u"%APPDATA%", 512), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(out_w(), "C:\\Users\\Default\\AppData\\Roaming");

    EXPECT_EQ(call(true, 0x1234, u"%USERPROFILE%", 512), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(out_w(), "");
}

// "block null" / "block proc": the system block omits the user's profile
// variables and every per-process addition.
TEST_P(ProfileEnvironmentTest, LogonBlocksCarryOnlyLogonVariables) {
    const auto system = profile_environment_block(env, ProfileEnvironment::System);
    const auto user = profile_environment_block(env, ProfileEnvironment::User);
    for (const char* name : {"APPDATA", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA",
                             "LOGONSERVER", "USERDOMAIN", "USERDOMAIN_ROAMINGPROFILE"}) {
        EXPECT_FALSE(find_environment_variable(system, name)) << name;
        EXPECT_TRUE(find_environment_variable(user, name)) << name;
    }
    EXPECT_FALSE(find_environment_variable(system, "SteamAppId"));
    EXPECT_FALSE(find_environment_variable(user, "SteamAppId"));
    EXPECT_EQ(find_environment_variable(system, "tmp").value(), "C:\\Windows\\TEMP");
    EXPECT_EQ(find_environment_variable(user, "OS").value(), "Windows_NT");
}
