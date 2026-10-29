// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_contract.cpp
/// @brief advapi32 SID / token / ACL / named-security / credential contract
///        tests (PEAK audit 08ab and missing exports, 2026-09-23).
///
/// Expected values are Windows 10 measurements from tools/oracle_probe/
/// (advapi_token_probe, advapi_acl_probe, cred_privilege_probe .expected),
/// never Vitreo output.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/security/credentials.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <unistd.h>
#include <initializer_list>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kData = 0x100000;
constexpr GuestAddr kStack = 0x180000;

class SecurityContractTest : public ::testing::Test {
protected:
    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_mgr{vmm, memory, handles};
    GlobalLocalManager local{heap_mgr};
    SecurityStubs stubs{handles, &local};
    capsule::Vfs vfs;
    cpu::CpuState cpu;
    DllExports exports;
    std::filesystem::path root;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        root = std::filesystem::temp_directory_path() /
               ("vitreo-security-contract-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        ASSERT_TRUE(vfs.mount_drive('C',
            std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        exports = create_advapi32_security_thunks(stubs, &vfs);
        set_credential_store_root({});
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    /// Win64 call: rcx, rdx, r8, r9, then stack slots from rsp+0x28.
    u64 call(const char* name, std::initializer_list<u64> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries) {
            if (e.name == name) entry = &e;
        }
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return ~0ull;
        cpu.gpr.rsp = kStack + 0x800;
        u64 regs[4]{};
        usize i = 0;
        for (u64 a : args) {
            if (i < 4) regs[i] = a;
            else (void)memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, a);
            ++i;
        }
        cpu.gpr.rcx = regs[0];
        cpu.gpr.rdx = regs[1];
        cpu.gpr.r8 = regs[2];
        cpu.gpr.r9 = regs[3];
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return cpu.gpr.rax;
    }
    u32 stdcall_bytes(const char* name) {
        for (const auto& e : exports.entries) {
            if (e.name == name) return e.stdcall_bytes;
        }
        return ~0u;
    }
    GuestAddr wstr(GuestAddr at, std::u16string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u16(at + i * 2, s[i]);
        (void)memory.write_u16(at + s.size() * 2, 0);
        return at;
    }
    GuestAddr sid(GuestAddr at, u8 authority, const std::vector<u32>& subs) {
        (void)memory.write_u8(at, 1);
        (void)memory.write_u8(at + 1, static_cast<u8>(subs.size()));
        for (int i = 0; i < 6; ++i) (void)memory.write_u8(at + 2 + i, i == 5 ? authority : 0);
        usize k = 0;
        for (u32 v : subs) (void)memory.write_u32(at + 8 + 4 * k++, v);
        return at;
    }
    Handle process_token(u32 access) {
        const GuestAddr out = kData + 0x3F00;
        EXPECT_EQ(call("OpenProcessToken", {0xFFFFFFFFFFFFFFFFull, access, out}), 1u);
        return static_cast<Handle>(memory.read_u64(out).value());
    }
};

// ---------------------------------------------------------------------------
// SIDs
// ---------------------------------------------------------------------------

TEST_F(SecurityContractTest, AllocateAndInitializeSidWritesFullPointerAndFreeSidFrees) {
    const GuestAddr auth = kData;
    for (int i = 0; i < 6; ++i) (void)memory.write_u8(auth + i, i == 5 ? 5 : 0);
    const GuestAddr psid = kData + 0x40;
    ASSERT_TRUE(memory.write_u64(psid, 0xAAAAAAAAAAAAAAAAull).is_ok());
    set_last_error(0xDEAD);
    EXPECT_EQ(call("AllocateAndInitializeSid",
                   {auth, 2, 32, 544, 0, 0, 0, 0, 0, 0, psid}), 1u);
    const u64 sid_ptr = memory.read_u64(psid).value();
    EXPECT_EQ(sid_ptr >> 32, 0u) << "upper half of PSID must be written";
    EXPECT_EQ(memory.read_u32(static_cast<GuestAddr>(sid_ptr) + 12).value(), 544u);
    EXPECT_EQ(get_last_error(), 0xDEADu); // probe aais_2: untouched
    set_last_error(0xDEAD);
    EXPECT_EQ(call("FreeSid", {sid_ptr}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu); // probe freesid
    // Nine sub-authorities: ERROR_INVALID_SID (probe aais_9: 0x539).
    EXPECT_EQ(call("AllocateAndInitializeSid",
                   {auth, 9, 1, 2, 3, 4, 5, 6, 7, 8, psid}), 0u);
    EXPECT_EQ(get_last_error(), 1337u);
}

TEST_F(SecurityContractTest, CopySidReportsTheRightError) {
    const GuestAddr s = sid(kData, 5, {32, 544});
    set_last_error(0xDEAD);
    EXPECT_EQ(call("CopySid", {8, kData + 0x100, s}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("CopySid", {64, kData + 0x100, s}), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call("CopySid", {64, kData + 0x100, 0x7FFF0000}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
}

TEST_F(SecurityContractTest, LookupAccountSidCoversWellKnownSids) {
    struct Case { u8 auth; std::vector<u32> subs; std::u16string name, dom; u32 use; };
    const std::vector<Case> cases = {
        {16, {8192}, u"Medium Mandatory Level", u"Mandatory Label", 10},
        {5, {19}, u"LOCAL SERVICE", u"NT AUTHORITY", 5},
        {5, {32, 546}, u"Guests", u"BUILTIN", 4},
        {3, {0}, u"CREATOR OWNER", u"", 5},
        {3, {1}, u"CREATOR GROUP", u"", 5},
        {15, {2, 1}, u"ALL APPLICATION PACKAGES", u"APPLICATION PACKAGE AUTHORITY", 5},
        {5, {5, 0, 12345}, u"LogonSessionId_0_12345", u"NT AUTHORITY", 11},
    };
    for (const auto& c : cases) {
        const GuestAddr s = sid(kData, c.auth, c.subs);
        const GuestAddr name = kData + 0x200, dom = kData + 0x400;
        const GuestAddr nl = kData + 0x600, dl = kData + 0x608, use = kData + 0x610;
        (void)memory.write_u32(nl, 128);
        (void)memory.write_u32(dl, 128);
        ASSERT_EQ(call("LookupAccountSidW", {0, s, name, nl, dom, dl, use}), 1u);
        std::u16string got;
        for (GuestAddr p = name; memory.read_u16(p).value() != 0; p += 2) {
            got.push_back(memory.read_u16(p).value());
        }
        std::u16string got_dom;
        for (GuestAddr p = dom; memory.read_u16(p).value() != 0; p += 2) {
            got_dom.push_back(memory.read_u16(p).value());
        }
        EXPECT_EQ(got, c.name);
        EXPECT_EQ(got_dom, c.dom);
        EXPECT_EQ(memory.read_u32(use).value(), c.use);
    }
    // A remote lpSystemName is an unreachable server (probe las_remote).
    const GuestAddr s = sid(kData, 1, {0});
    (void)memory.write_u32(kData + 0x600, 128);
    (void)memory.write_u32(kData + 0x608, 128);
    EXPECT_EQ(call("LookupAccountSidW", {wstr(kData + 0x800, u"\\\\no-such-host"), s,
                                         kData + 0x200, kData + 0x600, kData + 0x400,
                                         kData + 0x608, kData + 0x610}), 0u);
    EXPECT_EQ(get_last_error(), 1722u);
}

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

TEST_F(SecurityContractTest, GetTokenInformationClassesMatchWindows) {
    const Handle tok = process_token(0x0008 | 0x0010);
    const GuestAddr buf = kData + 0x1000, len = kData + 0x3000;
    auto gti = [&](u32 cls, u32 size) {
        (void)memory.write_u32(len, 0x5555);
        return call("GetTokenInformation", {tok, cls, buf, size, len});
    };
    // probe gti class=0xc/0x1a/0x1b/0x1d/0x20: DWORDs 0, 0, 3, 0, 0 -- but
    // that probe ran over ssh, in services session 0. A desktop process
    // reads the same non-zero session ProcessIdToSessionId and WTS report
    // (s7_misc_probe, interactive run), which for Vitreo is console 1.
    for (auto [cls, val] : {std::pair<u32, u32>{12, 1}, {26, 0}, {27, 3}, {29, 0}, {32, 0}}) {
        ASSERT_EQ(gti(cls, 256), 1u) << cls;
        EXPECT_EQ(memory.read_u32(len).value(), 4u) << cls;
        EXPECT_EQ(memory.read_u32(buf).value(), val) << cls;
    }
    // TokenHasRestrictions reports ReturnLength 1 (class 0x15).
    ASSERT_EQ(gti(21, 256), 1u);
    EXPECT_EQ(memory.read_u32(len).value(), 1u);
    // TokenElevation needs the exact size (elev_8: ERROR_BAD_LENGTH, len 4).
    EXPECT_EQ(gti(20, 8), 0u);
    EXPECT_EQ(get_last_error(), 24u);
    EXPECT_EQ(memory.read_u32(len).value(), 4u);
    EXPECT_EQ(gti(20, 4), 1u);
    // An unsplit token has no linked token (linked_exact: 0x520).
    EXPECT_EQ(gti(19, 8), 0u);
    EXPECT_EQ(get_last_error(), 1312u);
    // TokenIntegrityLevel attributes 0x60 (integrity_attrs).
    ASSERT_EQ(gti(25, 256), 1u);
    EXPECT_EQ(memory.read_u32(buf + 8).value(), 0x60u);
    // TokenStatistics.GroupCount equals TokenGroups.GroupCount.
    ASSERT_EQ(gti(10, 256), 1u);
    const u32 stat_groups = memory.read_u32(buf + 40).value();
    ASSERT_EQ(gti(2, 1024), 1u);
    EXPECT_EQ(memory.read_u32(buf).value(), stat_groups);
    // Claim attributes: version 1, no attributes, 16 bytes.
    ASSERT_EQ(gti(33, 256), 1u);
    EXPECT_EQ(memory.read_u32(len).value(), 16u);
    EXPECT_EQ(memory.read_u16(buf).value(), 1u);
    // TokenGroupsAndPrivileges: user + groups, pointers inside the buffer.
    ASSERT_EQ(gti(13, 4096), 1u);
    const u32 sid_count = memory.read_u32(buf).value();
    EXPECT_EQ(sid_count, stat_groups + 1);
    const u64 sids = memory.read_u64(buf + 8).value();
    EXPECT_GE(sids, buf);
    EXPECT_LT(sids, buf + memory.read_u32(len).value());
    EXPECT_EQ(memory.read_u64(buf + 24).value(), 0u); // RestrictedSids NULL
    // Undocumented-hash TokenAccessInformation and absent classes fail.
    EXPECT_EQ(gti(22, 4096), 0u);
    EXPECT_EQ(gti(14, 4096), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // A handle without TOKEN_QUERY is refused (gti_noquery).
    const Handle adjust_only = process_token(0x0020);
    (void)memory.write_u32(len, 0);
    EXPECT_EQ(call("GetTokenInformation", {adjust_only, 1, buf, 256, len}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

TEST_F(SecurityContractTest, ProcessTokenIsOneObjectAndHandlesCarryAccess) {
    // opt_null / opt_garbage: not a process handle -> ERROR_INVALID_HANDLE.
    EXPECT_EQ(call("OpenProcessToken", {0, 8, kData}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(call("OpenProcessToken", {0x7ff0, 8, kData}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    const Handle a = process_token(0x0020 | 0x0008);
    const Handle b = process_token(0x0008);
    const GuestAddr tp = kData + 0x100;
    (void)memory.write_u32(tp, 1);
    (void)memory.write_u64(tp + 4, 19);             // SeShutdownPrivilege
    (void)memory.write_u32(tp + 12, 2);             // SE_PRIVILEGE_ENABLED
    ASSERT_EQ(call("AdjustTokenPrivileges", {a, 0, tp, 0, 0, 0}), 1u);
    // adjust_seen_by_second_handle: one token object.
    EXPECT_TRUE(stubs.privilege_enabled(b, 19));
    // dup_adjust: DuplicateToken handles carry only IMPERSONATE|QUERY.
    const Handle dupsrc = process_token(0x0002 | 0x0008);
    ASSERT_EQ(call("DuplicateToken", {dupsrc, 2, kData + 0x200}), 1u);
    const Handle dup = static_cast<Handle>(memory.read_u64(kData + 0x200).value());
    EXPECT_EQ(call("AdjustTokenPrivileges", {dup, 0, tp, 0, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    // dup_noaccess: the source needs TOKEN_DUPLICATE.
    EXPECT_EQ(call("DuplicateToken", {b, 2, kData + 0x200}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

TEST_F(SecurityContractTest, OpenThreadTokenRefusesAnonymousLevel) {
    u32 error = 0;
    ASSERT_TRUE(stubs.impersonate_self(0 /*SecurityAnonymous*/, error));
    Handle token = 0;
    EXPECT_FALSE(stubs.open_thread_token(0xFFFFFFFEu, 8, token, error));
    EXPECT_EQ(error, 1347u); // ott_anon: ERROR_CANT_OPEN_ANONYMOUS
    EXPECT_TRUE(stubs.revert_to_self());
}

TEST_F(SecurityContractTest, PrivilegeCheckMatchesWindows) {
    const Handle tok = process_token(0x0008);
    const GuestAddr set = kData + 0x100, res = kData + 0x180;
    (void)memory.write_u32(set, 2);
    (void)memory.write_u32(set + 4, 0);           // any
    (void)memory.write_u64(set + 8, 23);          // SeChangeNotifyPrivilege
    (void)memory.write_u32(set + 16, 0);
    (void)memory.write_u64(set + 20, 7);          // SeTcbPrivilege (not held)
    (void)memory.write_u32(set + 28, 0);
    ASSERT_EQ(call("PrivilegeCheck", {tok, set, res}), 1u);
    EXPECT_EQ(memory.read_u32(res).value(), 1u);             // pc_any
    EXPECT_EQ(memory.read_u32(set + 16).value(), 0x80000000u);
    EXPECT_EQ(memory.read_u32(set + 28).value(), 0u);
    (void)memory.write_u32(set + 4, 1);           // PRIVILEGE_SET_ALL_NECESSARY
    ASSERT_EQ(call("PrivilegeCheck", {tok, set, res}), 1u);
    EXPECT_EQ(memory.read_u32(res).value(), 0u);             // pc_all
    const Handle noquery = process_token(0x0004);
    EXPECT_EQ(call("PrivilegeCheck", {noquery, set, res}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED); // pc_noquery
}

TEST_F(SecurityContractTest, EtwStdcallByteCountsFollowEvntprov) {
    EXPECT_EQ(stdcall_bytes("EventWrite"), 20u);
    EXPECT_EQ(stdcall_bytes("EventWriteTransfer"), 28u);
    EXPECT_EQ(stdcall_bytes("EventProviderEnabled"), 20u);
    // evreg_null: RegHandle untouched when ProviderId is NULL.
    (void)memory.write_u64(kData, 0x1122334455667788ull);
    EXPECT_EQ(call("EventRegister", {0, 0, 0, kData}), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(memory.read_u64(kData).value(), 0x1122334455667788ull);
}

// ---------------------------------------------------------------------------
// ACLs
// ---------------------------------------------------------------------------

namespace acl {
std::vector<u8> ace(u8 type, u8 flags, u32 mask, std::vector<u8> sid) {
    std::vector<u8> a(8, 0);
    a[0] = type; a[1] = flags;
    a.insert(a.end(), sid.begin(), sid.end());
    a[2] = static_cast<u8>(a.size());
    std::memcpy(a.data() + 4, &mask, 4);
    return a;
}
std::vector<u8> sid(u8 auth, std::initializer_list<u32> subs) {
    std::vector<u8> s(8 + 4 * subs.size(), 0);
    s[0] = 1; s[1] = static_cast<u8>(subs.size()); s[7] = auth;
    usize i = 0;
    for (u32 v : subs) std::memcpy(s.data() + 8 + 4 * i++, &v, 4);
    return s;
}
} // namespace acl

TEST_F(SecurityContractTest, SetEntriesInAclMergesLikeWindows) {
    const auto users = acl::sid(5, {32, 545});
    const auto guests = acl::sid(5, {32, 546});
    const auto world = acl::sid(1, {0});
    // Old ACL: deny users 2, allow users 1, inherited deny world 0x20,
    // inherited allow world 8 (advapi_acl_probe sea_* input).
    std::vector<std::vector<u8>> aces = {
        acl::ace(1, 0, 2, users), acl::ace(0, 0, 1, users),
        acl::ace(1, 0x10, 0x20, world), acl::ace(0, 0x10, 8, world)};
    const GuestAddr old_acl = kData + 0x100;
    std::vector<u8> bytes(8, 0);
    bytes[0] = 2;
    for (const auto& a : aces) bytes.insert(bytes.end(), a.begin(), a.end());
    bytes[2] = static_cast<u8>(bytes.size()); bytes[4] = 4;
    ASSERT_TRUE(memory.write_bytes(old_acl, bytes.data(), bytes.size()).is_ok());
    const GuestAddr users_p = kData + 0x300, guests_p = kData + 0x340;
    (void)memory.write_bytes(users_p, users.data(), users.size());
    (void)memory.write_bytes(guests_p, guests.data(), guests.size());
    // EXPLICIT_ACCESS_W (x64: 48 bytes): mask, mode, inheritance, TRUSTEE_W
    // at +16 (form +28, name +40).
    auto run = [&](u32 mode, u32 mask, GuestAddr sidp) {
        const GuestAddr ea = kData + 0x400;
        std::vector<u8> z(48, 0);
        (void)memory.write_bytes(ea, z.data(), z.size());
        (void)memory.write_u32(ea, mask);
        (void)memory.write_u32(ea + 4, mode);
        (void)memory.write_u64(ea + 40, sidp);
        const GuestAddr out = kData + 0x500;
        EXPECT_EQ(call("SetEntriesInAclW", {1, ea, old_acl, out}), 0u);
        const GuestAddr n = static_cast<GuestAddr>(memory.read_u64(out).value());
        std::vector<std::tuple<u8, u8, u32>> got;
        const u16 count = memory.read_u16(n + 4).value();
        GuestAddr p = n + 8;
        for (u16 i = 0; i < count; ++i) {
            got.emplace_back(memory.read_u8(p).value(), memory.read_u8(p + 1).value(),
                             memory.read_u32(p + 4).value());
            p += memory.read_u16(p + 2).value();
        }
        return got;
    };
    using T = std::vector<std::tuple<u8, u8, u32>>;
    EXPECT_EQ(run(4 /*REVOKE*/, 0, users_p), (T{{1, 0, 2}, {1, 0x10, 0x20}, {0, 0x10, 8}}));
    EXPECT_EQ(run(1 /*GRANT*/, 0x10, guests_p),
              (T{{1, 0, 2}, {0, 0, 0x10}, {0, 0, 1}, {1, 0x10, 0x20}, {0, 0x10, 8}}));
    EXPECT_EQ(run(1 /*GRANT*/, 0x10, users_p),
              (T{{1, 0, 2}, {0, 0, 0x11}, {1, 0x10, 0x20}, {0, 0x10, 8}}));
    EXPECT_EQ(run(3 /*DENY*/, 4, users_p),
              (T{{1, 0, 6}, {0, 0, 1}, {1, 0x10, 0x20}, {0, 0x10, 8}}));
    EXPECT_EQ(run(2 /*SET*/, 0x10, users_p),
              (T{{0, 0, 0x10}, {1, 0x10, 0x20}, {0, 0x10, 8}}));
}

TEST_F(SecurityContractTest, SetEntriesInAclNameTrustees) {
    auto by_name = [&](std::u16string_view name) {
        const GuestAddr ea = kData + 0x400;
        std::vector<u8> z(48, 0);
        (void)memory.write_bytes(ea, z.data(), z.size());
        (void)memory.write_u32(ea, 0x80000000u);
        (void)memory.write_u32(ea + 4, 1);
        (void)memory.write_u32(ea + 28, 1); // TRUSTEE_IS_NAME
        (void)memory.write_u64(ea + 40, wstr(kData + 0x600, name));
        return call("SetEntriesInAclW", {1, ea, 0, kData + 0x500});
    };
    EXPECT_EQ(by_name(u"CREATOR GROUP"), 0u);
    EXPECT_EQ(by_name(u"NT AUTHORITY\\NETWORK SERVICE"), 0u);
    EXPECT_EQ(by_name(u"OTHERDOM\\Users"), error_codes::ERROR_NONE_MAPPED);
    EXPECT_EQ(by_name(u"BA"), error_codes::ERROR_NONE_MAPPED);
}

TEST_F(SecurityContractTest, GetEffectiveRightsFromAclRules) {
    const GuestAddr trustee = kData + 0x100;  // TRUSTEE_W, x64
    std::vector<u8> z(32, 0);
    (void)memory.write_bytes(trustee, z.data(), z.size());
    const auto user = SecurityStubs::synthetic_sid();
    (void)memory.write_bytes(kData + 0x200, user.data(), user.size());
    (void)memory.write_u64(trustee + 24, kData + 0x200);
    const GuestAddr mask = kData + 0x180;
    // ger_null_acl
    EXPECT_EQ(call("GetEffectiveRightsFromAclW", {0, trustee, mask}), 0u);
    EXPECT_EQ(memory.read_u32(mask).value(), 0x03FFFFFFu);
    // Inherit-only ignored; inherited deny applies; Everyone counts;
    // INTERACTIVE (token-only group) does not.
    std::vector<std::vector<u8>> aces = {
        acl::ace(1, 0x10, 1, acl::sid(1, {0})),
        acl::ace(0, 0, 0x1F01FF, acl::sid(1, {0})),
        acl::ace(0, 0x08 | 0x01, 0x10000000, acl::sid(1, {0})),
        acl::ace(0, 0, 0x20000000, acl::sid(5, {4}))};
    std::vector<u8> bytes(8, 0);
    bytes[0] = 2;
    for (const auto& a : aces) bytes.insert(bytes.end(), a.begin(), a.end());
    bytes[2] = static_cast<u8>(bytes.size()); bytes[4] = static_cast<u8>(aces.size());
    const GuestAddr a = kData + 0x800;
    (void)memory.write_bytes(a, bytes.data(), bytes.size());
    EXPECT_EQ(call("GetEffectiveRightsFromAclW", {a, trustee, mask}), 0u);
    EXPECT_EQ(memory.read_u32(mask).value(), 0x1F01FEu); // ger_inherited_deny
}

// ---------------------------------------------------------------------------
// Named security info (file objects)
// ---------------------------------------------------------------------------

TEST_F(SecurityContractTest, NamedSecurityInfoSetsOnlyTheNamedParts) {
    std::filesystem::create_directories(root / "dir");
    std::ofstream(root / "dir" / "f.txt") << "x";
    const GuestAddr path = wstr(kData + 0x100, u"C:\\dir\\f.txt");
    const GuestAddr dacl_out = kData + 0x600, sd_out = kData + 0x608;
    const GuestAddr owner_out = kData + 0x610;
    auto get_dacl = [&](u32 info) -> std::pair<u32, u16> {
        (void)memory.write_u64(owner_out, 1);
        EXPECT_EQ(call("GetNamedSecurityInfoW", {path, 1, info, owner_out, 0, dacl_out, 0, sd_out}), 0u);
        const GuestAddr sd = static_cast<GuestAddr>(memory.read_u64(sd_out).value());
        const GuestAddr d = static_cast<GuestAddr>(memory.read_u64(dacl_out).value());
        return {d == 0 ? 0xFFFFu : memory.read_u16(d + 4).value(), memory.read_u16(sd + 2).value()};
    };
    // A fresh file carries its inherited DACL (get_fresh: 3 inherited ACEs).
    auto fresh = get_dacl(4);
    EXPECT_EQ(fresh.first, 3u);
    EXPECT_EQ(memory.read_u64(owner_out).value(), 1u); // unrequested out untouched
    // Protected empty DACL is stored as-is: deny all, SE_DACL_PROTECTED.
    const GuestAddr empty = kData + 0x800;
    (void)memory.write_u8(empty, 2);
    (void)memory.write_u16(empty + 2, 8);
    (void)memory.write_u16(empty + 4, 0);
    EXPECT_EQ(call("SetNamedSecurityInfoW", {path, 1, 4 | 0x80000000u, 0, 0, empty, 0}), 0u);
    auto after = get_dacl(4);
    EXPECT_EQ(after.first, 0u);
    EXPECT_EQ(after.second, 0x9404u); // after_empty control
    // Setting only the owner keeps the DACL (after_owner).
    const auto user = SecurityStubs::synthetic_sid();
    (void)memory.write_bytes(kData + 0x900, user.data(), user.size());
    EXPECT_EQ(call("SetNamedSecurityInfoW", {path, 1, 1, kData + 0x900, 0, 0, 0}), 0u);
    EXPECT_EQ(get_dacl(4).first, 0u);
    // Unprotecting merges the parent's inheritable ACEs back
    // (after_unprotected: 3 inherited, control 0x8404).
    EXPECT_EQ(call("SetNamedSecurityInfoW", {path, 1, 4 | 0x20000000u, 0, 0, empty, 0}), 0u);
    auto merged = get_dacl(4);
    EXPECT_EQ(merged.first, 3u);
    EXPECT_EQ(merged.second, 0x8404u);
}

// ---------------------------------------------------------------------------
// Credential Manager
// ---------------------------------------------------------------------------

TEST_F(SecurityContractTest, CredentialRoundTripAndErrors) {
    // CREDENTIALW (x64): Flags 0, Type 4, TargetName 8, Comment 16,
    // LastWritten 24, BlobSize 32, Blob 40, Persist 48, AttrCount 52,
    // Attributes 56, TargetAlias 64, UserName 72.
    const GuestAddr c = kData + 0x100;
    auto reset = [&]() {
        std::vector<u8> z(80, 0);
        (void)memory.write_bytes(c, z.data(), z.size());
        (void)memory.write_u32(c + 4, 1);                                   // GENERIC
        (void)memory.write_u64(c + 8, wstr(kData + 0x200, u"vitreo-probe/generic"));
        (void)memory.write_u32(c + 32, 5);
        const u8 blob[5] = {0, 1, 2, 3, 4};
        (void)memory.write_bytes(kData + 0x300, blob, 5);
        (void)memory.write_u64(c + 40, kData + 0x300);
        (void)memory.write_u32(c + 48, 2);                                  // LOCAL_MACHINE
        (void)memory.write_u64(c + 72, wstr(kData + 0x340, u"someone"));
    };
    reset();
    ASSERT_EQ(call("CredWriteW", {c, 0}), 1u);
    const GuestAddr out = kData + 0x500;
    ASSERT_EQ(call("CredReadW", {wstr(kData + 0x400, u"VITREO-PROBE/GENERIC"), 1, 0, out}), 1u);
    const GuestAddr r = static_cast<GuestAddr>(memory.read_u64(out).value());
    EXPECT_EQ(memory.read_u32(r + 4).value(), 1u);
    EXPECT_EQ(memory.read_u32(r + 48).value(), 2u);
    EXPECT_EQ(memory.read_u32(r + 32).value(), 5u);
    EXPECT_EQ(memory.read_u8(static_cast<GuestAddr>(memory.read_u64(r + 40).value()) + 4).value(), 4u);
    EXPECT_NE(memory.read_u64(r + 24).value(), 0u); // LastWritten
    EXPECT_EQ(call("CredFree", {r}), 0u);
    // Preserve keeps the blob; preserving something absent is ERROR_NOT_FOUND.
    (void)memory.write_u32(c + 32, 0);
    (void)memory.write_u64(c + 40, 0);
    EXPECT_EQ(call("CredWriteW", {c, 1}), 1u);
    ASSERT_EQ(call("CredReadW", {kData + 0x200, 1, 0, out}), 1u);
    EXPECT_EQ(memory.read_u32(static_cast<GuestAddr>(memory.read_u64(out).value()) + 32).value(), 5u);
    (void)memory.write_u64(c + 8, wstr(kData + 0x200, u"vitreo-probe/none"));
    EXPECT_EQ(call("CredWriteW", {c, 1}), 0u);
    EXPECT_EQ(get_last_error(), 1168u);
    // Invalid inputs (probe write_*).
    reset();
    (void)memory.write_u32(c + 32, 2561);
    EXPECT_EQ(call("CredWriteW", {c, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1783u);
    reset();
    (void)memory.write_u32(c + 48, 4);
    EXPECT_EQ(call("CredWriteW", {c, 0}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    reset();
    (void)memory.write_u32(c + 4, 7);
    EXPECT_EQ(call("CredWriteW", {c, 0}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    reset();
    EXPECT_EQ(call("CredWriteW", {c, 2}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // Domain credentials need a user and never return their secret.
    reset();
    (void)memory.write_u32(c + 4, 2);
    (void)memory.write_u64(c + 8, wstr(kData + 0x200, u"vitreo-probe-domain"));
    (void)memory.write_u64(c + 72, 0);
    EXPECT_EQ(call("CredWriteW", {c, 0}), 0u);
    EXPECT_EQ(get_last_error(), 2202u);
    (void)memory.write_u64(c + 72, wstr(kData + 0x340, u"DOM\\user"));
    ASSERT_EQ(call("CredWriteW", {c, 0}), 1u);
    ASSERT_EQ(call("CredReadW", {kData + 0x200, 2, 0, out}), 1u);
    const GuestAddr d = static_cast<GuestAddr>(memory.read_u64(out).value());
    EXPECT_EQ(memory.read_u32(d + 32).value(), 0u);
    EXPECT_EQ(memory.read_u64(d + 40).value(), 0u);
    // Delete then delete again: ERROR_NOT_FOUND.
    EXPECT_EQ(call("CredDeleteW", {wstr(kData + 0x400, u"vitreo-probe/generic"), 1, 0}), 1u);
    EXPECT_EQ(call("CredDeleteW", {kData + 0x400, 1, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1168u);
    EXPECT_EQ(call("CredReadW", {0, 1, 0, out}), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(SecurityContractTest, PersistentCredentialsSurviveAStoreReload) {
    set_credential_store_root((root / "creds").string());
    const GuestAddr c = kData + 0x100;
    std::vector<u8> z(80, 0);
    (void)memory.write_bytes(c, z.data(), z.size());
    (void)memory.write_u32(c + 4, 1);
    (void)memory.write_u64(c + 8, wstr(kData + 0x200, u"persisted"));
    (void)memory.write_u32(c + 48, 2);
    ASSERT_EQ(call("CredWriteW", {c, 0}), 1u);
    (void)memory.write_u64(c + 8, wstr(kData + 0x240, u"session"));
    (void)memory.write_u32(c + 48, 1);
    ASSERT_EQ(call("CredWriteW", {c, 0}), 1u);
    EXPECT_TRUE(std::filesystem::exists(root / "creds" / "persisted.bin"));
    // A different process reads the same files.
    ASSERT_EQ(call("CredReadW", {kData + 0x200, 1, 0, kData + 0x500}), 1u);
    ASSERT_EQ(call("CredReadW", {kData + 0x240, 1, 0, kData + 0x500}), 1u);
    set_credential_store_root({});
}

} // namespace

// ETW controller/consumer answers for sessions and files that do not exist
// (tools/oracle_probe/s4_etw_probe64.expected; x64 fixture).
TEST_F(SecurityContractTest, EtwControlAndConsumerFailuresMatchWindows) {
    EXPECT_EQ(stdcall_bytes("ControlTraceW"), 20u);
    EXPECT_EQ(stdcall_bytes("OpenTraceW"), 4u);
    EXPECT_EQ(stdcall_bytes("TraceMessage"), 0u);    // __cdecl, variadic
    const GuestAddr props = kData + 0x400;
    const GuestAddr name = kData + 0x800;
    const char16_t session[] = u"VitreoNoSuchSession";
    for (usize i = 0; i < sizeof(session) / 2; ++i) {
        (void)memory.write_u16(name + i * 2, static_cast<u16>(session[i]));
    }
    (void)memory.write_u32(props, 1144);             // Wnode.BufferSize
    EXPECT_EQ(call("ControlTraceW", {0, name, props, 0}), 4201u);   // control_query_missing
    EXPECT_EQ(call("ControlTraceW", {0, name, props, 1}), 4201u);   // control_stop_missing
    EXPECT_EQ(call("ControlTraceW", {0, name, 0, 0}), 87u);         // control_null_props
    EXPECT_EQ(call("ControlTraceW", {0, 0, props, 0}), 87u);        // control_null_name_null_handle
    EXPECT_EQ(call("ControlTraceW", {0, name, props, 77}), 87u);    // control_bad_code
    EXPECT_EQ(call("ControlTraceW", {0x12345, 0, props, 0}), 6u);   // control_bad_handle
    (void)memory.write_u32(props, 8);
    EXPECT_EQ(call("ControlTraceW", {0, name, props, 0}), 24u);     // control_small_props

    // Real-time OpenTraceW succeeds without looking the session up;
    // ProcessTrace then reports ERROR_WMI_INSTANCE_NOT_FOUND.
    const GuestAddr logfile = kData + 0x1000;
    for (u32 i = 0; i < 0x200; i += 4) (void)memory.write_u32(logfile + i, 0);
    (void)memory.write_u64(logfile + 8, name);                // LoggerName
    (void)memory.write_u32(logfile + 28, 0x10000100u);        // REAL_TIME | EVENT_RECORD
    set_last_error(0xDEAD);
    const u64 h = call("OpenTraceW", {logfile});
    EXPECT_NE(h, ~0ull);
    EXPECT_EQ(get_last_error(), 0u);                          // open_realtime_missing gle=0
    const GuestAddr harray = kData + 0x1400;
    (void)memory.write_u64(harray, h);
    EXPECT_EQ(call("ProcessTrace", {harray, 1, 0, 0}), 4201u); // process_realtime_missing
    EXPECT_EQ(call("CloseTrace", {h}), 0u);                    // close_realtime_missing
    EXPECT_EQ(call("CloseTrace", {h}), 6u);                    // close_again
    set_last_error(0xDEAD);
    EXPECT_EQ(call("OpenTraceW", {0}), ~0ull);                 // open_null
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(call("TraceMessage", {0x12345, 1, name, 1}), 6u); // tracemessage_bad_handle
}

// =============================================================================
// RegGetKeySecurity / RegSetKeySecurity
//
// Windows 11 measurements from tools/oracle_probe/shlwapi_regsec_shell_probe
// {,.x86}.expected (getsec_* / setsec_* lines, identical for x86 and x64,
// interactive non-elevated session). Run in both calling conventions.
// =============================================================================

#include "capsule/registry/registry.hpp"
#include "winapi/advapi32/registry/registry.hpp"

class RegKeySecurityTest : public ::testing::TestWithParam<bool> {
protected:
    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_mgr{vmm, memory, handles};
    GlobalLocalManager local{heap_mgr};
    SecurityStubs stubs{handles, &local};
    capsule::Registry registry;
    RegistryThunks reg{registry, handles};
    cpu::CpuState cpu;
    DllExports exports;
    u64 key = 0;

    static constexpr GuestAddr kBuf = kData + 0x1000, kSize = kData + 0x0F00,
                               kSd = kData + 0x3000, kAcl = kData + 0x3400,
                               kOwner = kData + 0x3600;
    bool x64() const { return GetParam(); }

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x2000, MemoryProtection::ReadWrite).is_ok());
        add_registry_key_security_exports(exports, stubs, reg);
        ASSERT_EQ(reg.reg_create_key_ex_w(capsule::HKEY_CURRENT_USER,
                                          L"Software\\VitreoProbe\\sec", &key, nullptr), 0u);
        // Everyone (S-1-1-0) and an ACL granting it KEY_ALL_ACCESS.
        const u8 everyone[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        (void)memory.write_bytes(kOwner, everyone, sizeof everyone);
        const u8 acl[28] = {2, 0, 28, 0, 1, 0, 0, 0,
                            0, 0, 20, 0, 0x3F, 0x00, 0x0F, 0x00,
                            1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        (void)memory.write_bytes(kAcl, acl, sizeof acl);
    }
    u32 call(const char* name, std::initializer_list<u64> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries)
            if (e.name == name) entry = &e;
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return ~0u;
        EXPECT_EQ(entry->stdcall_bytes, args.size() * 4) << name;
        cpu = cpu::CpuState{};
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack + 0x800;
        usize i = 0;
        for (u64 a : args) {
            if (x64()) {
                (i == 0 ? cpu.gpr.rcx : i == 1 ? cpu.gpr.rdx : i == 2 ? cpu.gpr.r8 : cpu.gpr.r9) = a;
            } else {
                (void)memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(a));
            }
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    /// An absolute SECURITY_DESCRIPTOR (pointer layout of the bitness).
    GuestAddr absolute_sd(u16 control, GuestAddr owner, GuestAddr dacl) {
        for (u32 i = 0; i < 40; ++i) (void)memory.write_u8(kSd + i, 0);
        (void)memory.write_u8(kSd, 1);
        (void)memory.write_u16(kSd + 2, control);
        const u32 w = x64() ? 8 : 4;
        if (x64()) {
            (void)memory.write_u64(kSd + w, owner);
            (void)memory.write_u64(kSd + 4 * w, dacl);
        } else {
            (void)memory.write_u32(kSd + w, static_cast<u32>(owner));
            (void)memory.write_u32(kSd + 4 * w, static_cast<u32>(dacl));
        }
        return kSd;
    }
    u32 get(u32 info, u32 cap) {
        (void)memory.write_u32(kSize, cap);
        return call("RegGetKeySecurity", {key, info, kBuf, kSize});
    }
};

TEST_P(RegKeySecurityTest, GetReportsSizesAndRefusals) {
    set_last_error(0xDEAD);
    (void)memory.write_u32(kSize, 0);
    EXPECT_EQ(call("RegGetKeySecurity", {key, 5, 0, kSize}), 122u);       // getsec_null
    EXPECT_NE(memory.read_u32(kSize).value_or(0), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(get(4, 4), 122u);                                           // getsec_small
    EXPECT_GT(memory.read_u32(kSize).value_or(0), 4u);
    EXPECT_EQ(get(5, 4096), 0u);                                          // getsec
    const u16 control = memory.read_u16(kBuf + 2).value_or(0);
    EXPECT_NE(control & 0x8000, 0);                                       // self_relative
    EXPECT_NE(control & 0x0004, 0);                                       // dacl_present
    EXPECT_EQ(get(0, 4096), 0u);                                          // getsec_zero_info
    EXPECT_EQ(get(8, 4096), 5u);                                          // getsec_sacl
    (void)memory.write_u32(kSize, 4096);
    EXPECT_EQ(call("RegGetKeySecurity", {0x1234, 4, kBuf, kSize}), 6u);   // getsec_badkey
    const u64 hkcu = x64() ? 0xFFFFFFFF80000001ull : 0x80000001ull;
    EXPECT_EQ(call("RegGetKeySecurity", {hkcu, 4, kBuf, kSize}), 0u);     // getsec_hkcu
    EXPECT_EQ(call("RegGetKeySecurity", {key, 4, 0, 0}), 87u);            // getsec_nullcb
}

TEST_P(RegKeySecurityTest, SetAppliesTheNamedPartsOfEitherLayout) {
    EXPECT_EQ(call("RegSetKeySecurity", {key, 4, absolute_sd(0x0004, 0, kAcl)}), 0u);  // setsec_abs
    ASSERT_EQ(get(4, 4096), 0u);                                          // readback
    const u32 dacl_off = memory.read_u32(kBuf + 16).value_or(0);
    ASSERT_NE(dacl_off, 0u);
    EXPECT_EQ(memory.read_u16(kBuf + dacl_off + 4).value_or(0), 1u);      // aces=1
    const GuestAddr ace = kBuf + dacl_off + 8;
    EXPECT_EQ(memory.read_u8(ace).value_or(9), 0u);                       // type=0
    EXPECT_EQ(memory.read_u8(ace + 1).value_or(9), 0u);                   // flags=0
    EXPECT_EQ(memory.read_u32(ace + 4).value_or(0), 0xF003Fu);            // mask
    EXPECT_EQ(memory.read_u8(ace + 15).value_or(0), 1u);                  // S-1-1-...
    EXPECT_EQ(memory.read_u32(ace + 16).value_or(9), 0u);                 // ...-0

    // The descriptor belongs to the key: a second handle sees it.
    u64 again = 0;
    ASSERT_EQ(reg.reg_open_key_ex_w(capsule::HKEY_CURRENT_USER, L"Software\\VitreoProbe\\sec",
                                    0, 0, &again), 0u);
    (void)memory.write_u32(kSize, 4096);
    ASSERT_EQ(call("RegGetKeySecurity", {again, 4, kBuf, kSize}), 0u);
    EXPECT_EQ(memory.read_u16(kBuf + memory.read_u32(kBuf + 16).value_or(0) + 4).value_or(0), 1u);

    // Self-relative form (setsec_rel).
    u8 rel[48] = {1, 0, 0x04, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0};
    std::vector<u8> acl(28);
    (void)memory.read_bytes(kAcl, acl.data(), acl.size());
    std::memcpy(rel + 20, acl.data(), acl.size());
    (void)memory.write_bytes(kSd + 0x100, rel, sizeof rel);
    EXPECT_EQ(call("RegSetKeySecurity", {key, 4, kSd + 0x100}), 0u);

    EXPECT_EQ(call("RegSetKeySecurity", {key, 4, 0}), 87u);               // setsec_null_sd
    for (u32 i = 0; i < 64; ++i) (void)memory.write_u8(kSd + 0x200 + i, 0);
    EXPECT_EQ(call("RegSetKeySecurity", {key, 4, kSd + 0x200}), 87u);     // setsec_invalid_sd
    EXPECT_EQ(call("RegSetKeySecurity", {key, 0, absolute_sd(0x0004, 0, kAcl)}), 0u);  // setsec_zero_info
    EXPECT_EQ(call("RegSetKeySecurity", {0x1234, 4, kSd}), 6u);           // setsec_badkey
    EXPECT_EQ(call("RegSetKeySecurity", {key, 1, absolute_sd(0, kOwner, 0)}), 1307u);  // setsec_foreign_owner

    // DACL_SECURITY_INFORMATION with a descriptor carrying no DACL sets a
    // NULL DACL (setsec_no_dacl_in_sd, after_no_dacl present=1 nulldacl=1).
    EXPECT_EQ(call("RegSetKeySecurity", {key, 4, absolute_sd(0, 0, 0)}), 0u);
    ASSERT_EQ(get(4, 4096), 0u);
    EXPECT_NE(memory.read_u16(kBuf + 2).value_or(0) & 0x0004, 0);
    EXPECT_EQ(memory.read_u32(kBuf + 16).value_or(1), 0u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, RegKeySecurityTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });
