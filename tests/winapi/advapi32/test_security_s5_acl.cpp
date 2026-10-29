// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_s5_acl.cpp
/// @brief advapi32 security-descriptor / ACL primitives, Get/SetSecurityInfo
///        and CryptoAPI (CAPI) contracts (S5 net/crypto audit, 2026-09-30).
///
/// Expected values are Windows' own output: tools/oracle_probe/
/// advapi_sdapi_probe and advapi_capi_probe (.expected for x64,
/// .x86.expected for x86), replayed through the exported thunks with the
/// probe's inputs at the matching bitness.

#include "advapi_thunk_harness.hpp"

#include "winapi/kernel32/synchronization/sync.hpp"

#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using advapi_test::field;
using advapi_test::kData;
using advapi_test::quoted;
using advapi_test::read_expected;
using advapi_test::token;

std::string line_for(const std::vector<std::string>& lines, const std::string& tag) {
    for (const auto& l : lines) {
        if (l.rfind(tag + " ", 0) == 0) return l;
    }
    ADD_FAILURE() << "no probe line " << tag;
    return {};
}

/// " bytes=<hex>" of a probe line.
std::string bytes_of(const std::string& l) { return token(l, "bytes").value_or("<none>"); }

class S5AclTest : public advapi_test::AdvapiThunkHarness {
protected:
    std::vector<std::string> lines;
    const GuestAddr acl = kData + 0x2000;      // 64-byte ACL work area
    const GuestAddr wd = kData + 0x2100;       // S-1-1-0
    const GuestAddr lw = kData + 0x2120;       // S-1-16-4096
    const GuestAddr an = kData + 0x2140;       // S-1-5-7
    const GuestAddr ba = kData + 0x2160;       // S-1-5-32-544

    void load(const char* probe) {
        lines = read_expected(expected_file(probe).c_str());
        const u8 wd_b[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        const u8 lw_b[12] = {1, 1, 0, 0, 0, 0, 0, 16, 0, 0x10, 0, 0};
        const u8 an_b[12] = {1, 1, 0, 0, 0, 0, 0, 5, 7, 0, 0, 0};
        const u8 ba_b[16] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 2, 0, 0};
        (void)memory.write_bytes(wd, wd_b, 12);
        (void)memory.write_bytes(lw, lw_b, 12);
        (void)memory.write_bytes(an, an_b, 12);
        (void)memory.write_bytes(ba, ba_b, 16);
    }
    void fill(GuestAddr at, u32 n, u8 v) {
        for (u32 i = 0; i < n; ++i) (void)memory.write_u8(at + i, v);
    }
    /// The probe's FRESH(sz, rev): 0xCC-fill then InitializeAcl.
    void fresh(u32 size, u32 rev) {
        fill(acl, 64, 0xCC);
        (void)call("InitializeAcl", {acl, size, rev});
    }
    /// ok + gle (+ bytes over `n`) against the probe line `tag`.
    void expect(const std::string& tag, u64 ok, GuestAddr at = 0, u32 n = 0) {
        const std::string l = line_for(lines, tag);
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        if (n != 0) EXPECT_EQ(hex_bytes(at, n), bytes_of(l)) << tag;
    }
    GuestAddr sd_from(const std::string& sddl, GuestAddr len_out = 0) {
        const GuestAddr out = kData + 0x3F00;
        EXPECT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorW",
                       {wstr(kData + 0x3000, sddl), 1, out, len_out}), 1u) << sddl;
        return static_cast<GuestAddr>(read_ptr(out));
    }
    std::string sddl_of(GuestAddr sd, u32 si) {
        const GuestAddr out = kData + 0x3F10;
        if (call("ConvertSecurityDescriptorToStringSecurityDescriptorW", {sd, 1, si, out, 0}) != 1) {
            return "ERR";
        }
        return read_w(static_cast<GuestAddr>(read_ptr(out)));
    }
    /// The probe's abs_report for an absolute SECURITY_DESCRIPTOR.
    void expect_abs(const std::string& tag, GuestAddr sd) {
        const std::string l = line_for(lines, tag);
        const u32 w = is64 ? 8 : 4;
        auto ptr_class = [&](GuestAddr at) {
            const u64 v = read_ptr(at);
            return v == 0 ? 0u : (v == 1 ? 1u : 2u);
        };
        EXPECT_EQ(memory.read_u8(sd).value(), *field(l, "rev")) << tag;
        EXPECT_EQ(memory.read_u16(sd + 2).value(), *field(l, "control")) << tag;
        EXPECT_EQ(read_ptr(sd + w) != 0, *field(l, "owner") != 0) << tag;
        EXPECT_EQ(read_ptr(sd + 2 * w) != 0, *field(l, "group") != 0) << tag;
        EXPECT_EQ(ptr_class(sd + 3 * w), *field(l, "sacl")) << tag;
        EXPECT_EQ(ptr_class(sd + 4 * w), *field(l, "dacl")) << tag;
    }
};

// ---------------------------------------------------------------------------
// InitializeSecurityDescriptor / InitializeAcl / IsValidAcl
// ---------------------------------------------------------------------------

TEST_P(S5AclTest, InitializeAndValidateMatchWindows) {
    load("advapi_sdapi_probe");
    const GuestAddr sd = kData + 0x2400;
    const u32 sd_size = is64 ? 40 : 20;
    for (u32 rev : {0u, 1u, 2u, 0xFFFFFFFFu}) {
        fill(sd, sd_size, 0xCC);
        set_last_error(0xDEAD);
        const u64 ok = call("InitializeSecurityDescriptor", {sd, rev});
        char tag[32];
        std::snprintf(tag, sizeof(tag), "isd rev=%x", rev);
        const std::string want = [&] {
            for (const auto& l : lines) if (l.rfind(std::string(tag) + " ", 0) == 0) return l;
            return std::string();
        }();
        ASSERT_FALSE(want.empty()) << tag;
        EXPECT_EQ(ok, *field(want, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(want, "gle")) << tag;
        EXPECT_EQ(hex_bytes(sd, sd_size), bytes_of(want)) << tag;
    }
    const GuestAddr big = kData + 0x20000;
    for (u32 size : {0u, 7u, 8u, 9u, 10u, 12u, 0xFFFCu, 0xFFFFu, 0x10000u, 0x10004u}) {
        fill(big, 16, 0xCC);
        set_last_error(0xDEAD);
        const u64 ok = call("InitializeAcl", {big, size, 2});
        char tag[40];
        std::snprintf(tag, sizeof(tag), "iacl size=%x", size);
        for (const auto& l : lines) {
            if (l.rfind(std::string(tag) + " ", 0) != 0) continue;
            EXPECT_EQ(ok, *field(l, "ok")) << tag;
            EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
            EXPECT_EQ(hex_bytes(big, 8), bytes_of(l)) << tag;
        }
    }
    for (u32 rev = 0; rev <= 5; ++rev) {
        fill(acl, 16, 0xCC);
        set_last_error(0xDEAD);
        const u64 ok = call("InitializeAcl", {acl, 16, rev});
        const std::string l = line_for(lines, "iacl_rev rev=" + std::to_string(rev));
        EXPECT_EQ(ok, *field(l, "ok")) << rev;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << rev;
        EXPECT_EQ(hex_bytes(acl, 8), bytes_of(l)) << rev;
    }
    struct Case { const char* tag; std::vector<u8> b; };
    const std::vector<Case> cases = {
        {"ivacl_empty", {2, 0, 8, 0, 0, 0, 0, 0}},
        {"ivacl_rev1", {1, 0, 8, 0, 0, 0, 0, 0}},
        {"ivacl_rev3", {3, 0, 8, 0, 0, 0, 0, 0}},
        {"ivacl_rev5", {5, 0, 8, 0, 0, 0, 0, 0}},
        {"ivacl_size6", {2, 0, 6, 0, 0, 0, 0, 0}},
        {"ivacl_size10", {2, 0, 10, 0, 0, 0, 0, 0}},
        {"ivacl_count_overflow", {2, 0, 8, 0, 1, 0, 0, 0}},
        {"ivacl_sbz1", {2, 1, 8, 0, 0, 0, 0, 0}},
        {"ivacl_one_ace", {2, 0, 28, 0, 1, 0, 0, 0, 0, 0, 20, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"ivacl_ace_badsid", {2, 0, 28, 0, 1, 0, 0, 0, 0, 0, 20, 0, 1, 0, 0, 0, 5, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"ivacl_ace_size_odd", {2, 0, 28, 0, 1, 0, 0, 0, 0, 0, 18, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"ivacl_ace_unknown_type", {2, 0, 28, 0, 1, 0, 0, 0, 0x1F, 0, 20, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"ivacl_obj_ace_in_rev2", {2, 0, 32, 0, 1, 0, 0, 0, 5, 0, 24, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"ivacl_obj_ace_in_rev4", {4, 0, 32, 0, 1, 0, 0, 0, 5, 0, 24, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"ivacl_ml_in_rev2", {2, 0, 28, 0, 1, 0, 0, 0, 0x11, 0, 20, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 16, 0, 16, 0, 0}},
    };
    for (const auto& c : cases) {
        fill(acl, 40, 0);
        (void)memory.write_bytes(acl, c.b.data(), c.b.size());
        set_last_error(0xDEAD);
        const u64 v = call("IsValidAcl", {acl});
        const std::string l = line_for(lines, c.tag);
        EXPECT_EQ(v, *field(l, "v")) << c.tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << c.tag;
    }
}

// ---------------------------------------------------------------------------
// Add*Ace / AddMandatoryAce / Add*ObjectAce / AddAce
// ---------------------------------------------------------------------------

TEST_P(S5AclTest, AddAceFamilyMatchesWindows) {
    load("advapi_sdapi_probe");
    EXPECT_EQ(stdcall_bytes("AddAccessAllowedObjectAce"), 28u);
    EXPECT_EQ(stdcall_bytes("AddAuditAccessObjectAce"), 36u);
    EXPECT_EQ(stdcall_bytes("AddMandatoryAce"), 20u);
    EXPECT_EQ(stdcall_bytes("SetSecurityDescriptorControl"), 12u);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("aaa_ok", call("AddAccessAllowedAce", {acl, 2, 0x10000000, wd}), acl, 28);
    for (u32 rev : {0u, 1u, 3u, 4u, 5u}) {
        fresh(64, 2);
        set_last_error(0xDEAD);
        expect("aaa_rev rev=" + std::to_string(rev), call("AddAccessAllowedAce", {acl, rev, 0x10000000, wd}),
               acl, 8);
    }
    fresh(64, 4);
    set_last_error(0xDEAD);
    expect("aaa_rev2_into_rev4", call("AddAccessAllowedAce", {acl, 2, 0x10000000, wd}), acl, 8);
    fresh(27, 2);
    set_last_error(0xDEAD);
    expect("aaa_full", call("AddAccessAllowedAce", {acl, 2, 0x10000000, wd}), acl, 8);
    fill(acl, 64, 0);
    (void)memory.write_u8(acl, 9);
    (void)memory.write_u8(acl + 2, 64);
    set_last_error(0xDEAD);
    expect("aaa_badacl", call("AddAccessAllowedAce", {acl, 2, 0x10000000, wd}));

    for (u32 flags : {0x1Fu, 0x20u, 0x40u, 0x80u, 0x100u, 0xC0u}) {
        char tag[40];
        std::snprintf(tag, sizeof(tag), "aaaex_flags flags=%x", flags);
        usize at = 0;
        for (; at < lines.size(); ++at) if (lines[at].rfind(std::string(tag) + " ", 0) == 0) break;
        ASSERT_LT(at + 2, lines.size()) << tag;
        auto check = [&](const std::string& l, u64 ok) {
            EXPECT_EQ(ok, *field(l, "ok")) << l;
            EXPECT_EQ(get_last_error(), *field(l, "gle")) << l;
            EXPECT_EQ(hex_bytes(acl + 8, 4), bytes_of(l)) << l;
        };
        fresh(64, 2);
        set_last_error(0xDEAD);
        check(lines[at], call("AddAccessAllowedAceEx", {acl, 2, flags, 0x10000000, wd}));
        fresh(64, 2);
        set_last_error(0xDEAD);
        check(lines[at + 1], call("AddAccessDeniedAceEx", {acl, 2, flags, 0x10000000, wd}));
        fresh(64, 2);
        set_last_error(0xDEAD);
        check(lines[at + 2], call("AddAuditAccessAceEx", {acl, 2, flags, 0x10000000, wd, 1, 1}));
    }
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("ada_ok", call("AddAccessDeniedAce", {acl, 2, 1, wd}), acl, 28);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("audit_success", call("AddAuditAccessAce", {acl, 2, 1, wd, 1, 0}), acl, 28);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("audit_neither", call("AddAuditAccessAce", {acl, 2, 1, wd, 0, 0}), acl, 28);

    for (u32 p : {0u, 1u, 7u, 8u, 0xFFFFFFFFu}) {
        char tag[40];
        std::snprintf(tag, sizeof(tag), "ama_policy p=%x", p);
        fresh(64, 2);
        set_last_error(0xDEAD);
        expect(tag, call("AddMandatoryAce", {acl, 2, 0, p, lw}), acl, 28);
    }
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("ama_non_label_sid", call("AddMandatoryAce", {acl, 2, 0, 1, wd}), acl, 28);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("ama_flag40", call("AddMandatoryAce", {acl, 2, 0x40, 1, lw}));
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("ama_flags1f", call("AddMandatoryAce", {acl, 2, 0x1F, 1, lw}), acl + 8, 4);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("ama_rev4", call("AddMandatoryAce", {acl, 4, 0, 1, lw}), acl, 8);

    const GuestAddr g1 = kData + 0x2200;
    const u8 guid[16] = {0xba, 0x7a, 0x96, 0xbf, 0xe6, 0x0d, 0xd0, 0x11,
                         0xa2, 0x85, 0x00, 0xaa, 0x00, 0x30, 0x49, 0xe2};
    (void)memory.write_bytes(g1, guid, 16);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("aaoa_noguid", call("AddAccessAllowedObjectAce", {acl, 4, 0, 0x10, 0, 0, wd}), acl, 32);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("aaoa_guid", call("AddAccessAllowedObjectAce", {acl, 4, 0, 0x10, g1, 0, wd}), acl, 48);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("aaoa_rev2", call("AddAccessAllowedObjectAce", {acl, 2, 0, 0x10, g1, 0, wd}), acl, 8);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("adoa_inh", call("AddAccessDeniedObjectAce", {acl, 4, 2, 0x10, 0, g1, wd}), acl, 48);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("auoa_both", call("AddAuditAccessObjectAce", {acl, 4, 0, 0x10, g1, g1, wd, 1, 1}), acl, 64);
    fresh(64, 2);
    set_last_error(0xDEAD);
    expect("aaoa_flag40", call("AddAccessAllowedObjectAce", {acl, 4, 0x40, 0x10, 0, 0, wd}));

    const GuestAddr ace = kData + 0x2300;
    const u8 ace_b[20] = {0, 0, 20, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    auto set_ace = [&](u8 mask) {
        (void)memory.write_bytes(ace, ace_b, 20);
        (void)memory.write_u8(ace + 4, mask);
    };
    auto one_ace = [&] {
        fresh(64, 2);
        set_ace(1);
        (void)call("AddAce", {acl, 2, 0xFFFFFFFF, ace, 20});
        set_ace(2);
    };
    fresh(64, 2);
    set_ace(1);
    set_last_error(0xDEAD);
    expect("addace_append", call("AddAce", {acl, 2, 0xFFFFFFFF, ace, 20}), acl, 8);
    set_ace(2);
    set_last_error(0xDEAD);
    expect("addace_front", call("AddAce", {acl, 2, 0, ace, 20}), acl, 48);
    one_ace();
    set_last_error(0xDEAD);
    expect("addace_index_beyond", call("AddAce", {acl, 2, 7, ace, 20}), acl, 48);
    struct AddCase { const char* tag; u32 rev; u32 index; u32 len; };
    for (const AddCase c : {AddCase{"addace_index_count", 2, 1, 20}, AddCase{"addace_rev4", 4, 0xFFFFFFFF, 20},
                            AddCase{"addace_rev1", 1, 0xFFFFFFFF, 20}, AddCase{"addace_rev5", 5, 0xFFFFFFFF, 20},
                            AddCase{"addace_len19", 2, 0xFFFFFFFF, 19}}) {
        one_ace();
        set_last_error(0xDEAD);
        expect(c.tag, call("AddAce", {acl, c.rev, c.index, ace, c.len}), acl, 8);
    }
    one_ace();
    const GuestAddr two = kData + 0x2340;
    (void)memory.write_bytes(two, ace_b, 20);
    (void)memory.write_u8(two + 4, 2);
    (void)memory.write_bytes(two + 20, ace_b, 20);
    (void)memory.write_u8(two + 24, 2);
    set_last_error(0xDEAD);
    expect("addace_two", call("AddAce", {acl, 2, 0xFFFFFFFF, two, 40}), acl, 8);
    fresh(24, 2);
    set_last_error(0xDEAD);
    expect("addace_full", call("AddAce", {acl, 2, 0xFFFFFFFF, ace, 20}), acl, 8);
}

// ---------------------------------------------------------------------------
// SetSecurityDescriptorControl / Get/SetSecurityDescriptorDacl/Sacl
// ---------------------------------------------------------------------------

TEST_P(S5AclTest, DescriptorControlDaclAndSaclMatchWindows) {
    load("advapi_sdapi_probe");
    const GuestAddr sd = kData + 0x2400;
    for (u32 bit : {0x1000u, 0x400u, 0x100u, 0x2000u, 0x800u, 0x200u, 0x4u, 0x1u, 0x8000u, 0x4000u, 0x40u,
                    0x80u}) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "ssdc bit=%x", bit);
        const std::string l = line_for(lines, tag);
        (void)call("InitializeSecurityDescriptor", {sd, 1});
        set_last_error(0xDEAD);
        EXPECT_EQ(call("SetSecurityDescriptorControl", {sd, bit, bit}), *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(memory.read_u16(sd + 2).value(), *field(l, "control")) << tag;
        set_last_error(0xDEAD);
        EXPECT_EQ(call("SetSecurityDescriptorControl", {sd, bit, 0}), *field(l, "clearOk")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "clearGle")) << tag;
        EXPECT_EQ(memory.read_u16(sd + 2).value(), *field(l, "control2")) << tag;
    }
    (void)call("InitializeSecurityDescriptor", {sd, 1});
    (void)call("SetSecurityDescriptorControl", {sd, 0x3000, 0x1000});
    EXPECT_EQ(memory.read_u16(sd + 2).value(), *field(line_for(lines, "ssdc_mask_subset"), "control"));
    const GuestAddr rel = sd_from("D:(A;;GA;;;WD)");
    set_last_error(0xDEAD);
    expect("ssdc_selfrel", call("SetSecurityDescriptorControl", {rel, 0x1000, 0x1000}));
    EXPECT_EQ(memory.read_u16(rel + 2).value(), *field(line_for(lines, "ssdc_selfrel"), "control"));

    // GetSecurityDescriptorSacl and the DACL/SACL setters.
    const GuestAddr a = kData + 0x2500;
    (void)call("InitializeAcl", {a, 64, 2});
    (void)call("InitializeSecurityDescriptor", {sd, 1});
    const GuestAddr present = kData + 0x2600, got = kData + 0x2608, def = kData + 0x2610;
    auto get_sacl = [&](GuestAddr desc) {
        (void)memory.write_u32(present, 0x55);
        (void)memory.write_u32(def, 0x55);
        write_ptr(got, 1);
        set_last_error(0xDEAD);
        return call("GetSecurityDescriptorSacl", {desc, present, got, def});
    };
    auto acl_class = [&] {
        const u64 v = read_ptr(got);
        return v == 1 ? 1u : (v != 0 ? 2u : 0u);
    };
    std::string l = line_for(lines, "gsds_fresh");
    EXPECT_EQ(get_sacl(sd), *field(l, "ok"));
    EXPECT_EQ(get_last_error(), *field(l, "gle"));
    EXPECT_EQ(memory.read_u32(present).value(), *field(l, "present"));
    EXPECT_EQ(acl_class(), *field(l, "acl"));
    EXPECT_EQ(memory.read_u32(def).value(), *field(l, "def"));
    (void)call("SetSecurityDescriptorSacl", {sd, 1, a, 1});
    expect_abs("ssds_set", sd);
    l = line_for(lines, "gsds_set");
    EXPECT_EQ(get_sacl(sd), *field(l, "ok"));
    EXPECT_EQ(read_ptr(got) == a, *field(l, "aclIsOurs") != 0);
    EXPECT_EQ(memory.read_u32(def).value(), *field(l, "def"));
    (void)call("SetSecurityDescriptorSacl", {sd, 1, 0, 0});
    l = line_for(lines, "gsds_null");
    EXPECT_EQ(get_sacl(sd), *field(l, "ok"));
    EXPECT_EQ(acl_class(), *field(l, "acl"));
    (void)call("SetSecurityDescriptorSacl", {sd, 0, a, 1});
    expect_abs("ssds_notpresent", sd);
    l = line_for(lines, "gsds_notpresent");
    EXPECT_EQ(get_sacl(sd), *field(l, "ok"));
    EXPECT_EQ(memory.read_u32(present).value(), *field(l, "present"));
    EXPECT_EQ(acl_class(), *field(l, "acl"));
    set_last_error(0xDEAD);
    const u64 set_ok = call("SetSecurityDescriptorDacl", {sd, 1, a, 1});
    expect_abs("ssdd_set", sd);
    EXPECT_EQ(set_ok, 1u);
    (void)call("SetSecurityDescriptorDacl", {sd, 0, a, 1});
    expect_abs("ssdd_notpresent", sd);
    const GuestAddr f = kData + 0x2700;
    (void)call("InitializeSecurityDescriptor", {f, 1});
    (void)call("SetSecurityDescriptorDacl", {f, 0, a, 1});
    expect_abs("ssdd_fresh_false_def", f);
    (void)call("InitializeSecurityDescriptor", {f, 1});
    (void)call("SetSecurityDescriptorDacl", {f, 1, a, 0});
    (void)call("SetSecurityDescriptorDacl", {f, 0, 0, 0});
    expect_abs("ssdd_set_then_false_null", f);
    (void)memory.write_u32(present, 0x55);
    (void)memory.write_u32(def, 0x55);
    write_ptr(got, 1);
    set_last_error(0xDEAD);
    l = line_for(lines, "gsdd_after_false");
    EXPECT_EQ(call("GetSecurityDescriptorDacl", {f, present, got, def}), *field(l, "ok"));
    EXPECT_EQ(memory.read_u32(present).value(), *field(l, "present"));
    EXPECT_EQ(acl_class(), *field(l, "acl"));
    EXPECT_EQ(memory.read_u32(def).value(), *field(l, "def"));   // written even when absent
    (void)call("InitializeSecurityDescriptor", {f, 1});
    (void)call("SetSecurityDescriptorSacl", {f, 1, a, 0});
    (void)call("SetSecurityDescriptorSacl", {f, 0, 0, 1});
    expect_abs("ssds_set_then_false", f);
    (void)memory.write_u8(sd, 2);
    set_last_error(0xDEAD);
    expect("ssdd_rev2", call("SetSecurityDescriptorDacl", {sd, 1, 0, 0}));
    set_last_error(0xDEAD);
    expect("ssds_rev2", call("SetSecurityDescriptorSacl", {sd, 1, 0, 0}));
    const GuestAddr rel_len = kData + 0x2800;
    const GuestAddr rel2 = sd_from("D:(A;;GA;;;WD)", rel_len);
    set_last_error(0xDEAD);
    expect("ssdd_selfrel", call("SetSecurityDescriptorDacl", {rel2, 1, 0, 0}), rel2,
           memory.read_u32(rel_len).value());
    set_last_error(0xDEAD);
    expect("ssds_selfrel", call("SetSecurityDescriptorSacl", {rel2, 1, 0, 0}));
    const GuestAddr label = sd_from("S:P(ML;;NW;;;LW)");
    l = line_for(lines, "gsds_selfrel");
    EXPECT_EQ(get_sacl(label), *field(l, "ok"));
    EXPECT_EQ(memory.read_u32(present).value(), *field(l, "present"));
    EXPECT_EQ(read_ptr(got) - label, *field(l, "aclOffset"));
    const GuestAddr bad = kData + 0x2900;
    fill(bad, 40, 0);
    (void)memory.write_u8(bad, 3);
    l = line_for(lines, "gsds_rev3");
    EXPECT_EQ(get_sacl(bad), *field(l, "ok"));
    EXPECT_EQ(get_last_error(), *field(l, "gle"));
    EXPECT_EQ(memory.read_u32(present).value(), *field(l, "present"));
}

// ---------------------------------------------------------------------------
// BuildSecurityDescriptorW / GetExplicitEntriesFromAclA/W
// ---------------------------------------------------------------------------

TEST_P(S5AclTest, BuildDescriptorAndExplicitEntriesMatchWindows) {
    load("advapi_sdapi_probe");
    EXPECT_EQ(stdcall_bytes("BuildSecurityDescriptorW"), 36u);
    EXPECT_EQ(stdcall_bytes("GetExplicitEntriesFromAclA"), 12u);
    const GuestAddr size = kData + 0x2A00, out = kData + 0x2A08;
    auto build = [&](GuestAddr owner, GuestAddr group, u32 n_access, GuestAddr access, u32 n_audit,
                     GuestAddr audit, GuestAddr old) {
        (void)memory.write_u32(size, 0x5555);
        write_ptr(out, 1);
        set_last_error(0xDEAD);
        return call("BuildSecurityDescriptorW", {owner, group, n_access, access, n_audit, audit, old, size, out});
    };
    std::string l = line_for(lines, "bsd_empty");
    EXPECT_EQ(build(0, 0, 0, 0, 0, 0, 0), *field(l, "ret"));
    EXPECT_EQ(get_last_error(), *field(l, "gle"));
    EXPECT_EQ(memory.read_u32(size).value(), *field(l, "size"));
    EXPECT_EQ(hex_bytes(static_cast<GuestAddr>(read_ptr(out)), memory.read_u32(size).value()), bytes_of(l));
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(out)), 0x1F), *quoted(l, "sddl"));

    // TRUSTEE_W and EXPLICIT_ACCESS_W at the guest's pointer width.
    const u32 tw = is64 ? 32 : 20, trustee_at = is64 ? 16 : 12, ew = trustee_at + tw;
    auto trustee = [&](GuestAddr at, u32 form, u32 type, GuestAddr name) {
        fill(at, tw, 0);
        (void)memory.write_u32(at + (is64 ? 12 : 8), form);
        (void)memory.write_u32(at + (is64 ? 16 : 12), type);
        write_ptr(at + (is64 ? 24 : 16), name);
    };
    auto access = [&](GuestAddr at, u32 perm, u32 mode, u32 inh, u32 form, u32 type, GuestAddr name) {
        fill(at, ew, 0);
        (void)memory.write_u32(at, perm);
        (void)memory.write_u32(at + 4, mode);
        (void)memory.write_u32(at + 8, inh);
        trustee(at + trustee_at, form, type, name);
    };
    const GuestAddr owner = kData + 0x2B00, ea = kData + 0x2C00;
    trustee(owner, 0, 5, ba);                          // TRUSTEE_IS_SID, WELL_KNOWN_GROUP
    access(ea, 0x10000000, 1, 0, 0, 5, wd);            // GRANT GA
    access(ea + ew, 2, 3, 3, 0, 5, an);                // DENY FILE_WRITE_DATA, OI|CI
    l = line_for(lines, "bsd_owner_two_aces");
    EXPECT_EQ(build(owner, owner, 2, ea, 0, 0, 0), *field(l, "ret"));
    EXPECT_EQ(get_last_error(), *field(l, "gle"));
    EXPECT_EQ(memory.read_u32(size).value(), *field(l, "size"));
    const GuestAddr built = static_cast<GuestAddr>(read_ptr(out));
    EXPECT_EQ(hex_bytes(built, memory.read_u32(size).value()), bytes_of(l));
    l = line_for(lines, "bsd_copy_old");
    EXPECT_EQ(build(0, 0, 0, 0, 0, 0, built), *field(l, "ret"));
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(out)), 0x1F), *quoted(l, "sddl"));
    const GuestAddr bogus_name = wstr(kData + 0x2D00, "NoSuchAccountVitreo");
    trustee(owner, 1, 0, bogus_name);                  // TRUSTEE_IS_NAME
    l = line_for(lines, "bsd_bogus_owner");
    EXPECT_EQ(build(owner, 0, 0, 0, 0, 0, 0), *field(l, "ret"));
    EXPECT_EQ(get_last_error(), *field(l, "gle"));
    EXPECT_EQ(read_ptr(out), *field(l, "sdOut"));
    EXPECT_EQ(memory.read_u32(size).value(), *field(l, "size"));
    access(ea, 0x10000000, 5, 0, 0, 0, wd);            // SET_AUDIT_SUCCESS
    l = line_for(lines, "bsd_audit");
    EXPECT_EQ(build(0, 0, 0, 0, 1, ea, 0), *field(l, "ret"));
    EXPECT_EQ(memory.read_u32(size).value(), *field(l, "size"));
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(out)), 0x1F), *quoted(l, "sddl"));

    // GetExplicitEntriesFromAclA/W: inherited ACEs are not explicit entries.
    const GuestAddr sd = sd_from("D:(A;OICI;GA;;;WD)(D;;FR;;;AN)(A;ID;FA;;;SY)");
    const GuestAddr present = kData + 0x2E00, dacl = kData + 0x2E08, def = kData + 0x2E10;
    (void)call("GetSecurityDescriptorDacl", {sd, present, dacl, def});
    const GuestAddr d = static_cast<GuestAddr>(read_ptr(dacl));
    const GuestAddr n = kData + 0x2E20, list = kData + 0x2E28;
    for (const char* api : {"GetExplicitEntriesFromAclW", "GetExplicitEntriesFromAclA"}) {
        const bool wide = api[25] == 'W';
        (void)memory.write_u32(n, 0x5555);
        write_ptr(list, 1);
        set_last_error(0xDEAD);
        const u64 r = call(api, {d, n, list});
        const std::string tag = wide ? "geea_w" : "geea_a";
        usize at = 0;
        for (; at < lines.size(); ++at) if (lines[at].rfind(tag + " ", 0) == 0) break;
        ASSERT_LT(at, lines.size());
        EXPECT_EQ(r, *field(lines[at], "ret")) << api;
        EXPECT_EQ(get_last_error(), *field(lines[at], "gle")) << api;
        const u32 count = memory.read_u32(n).value();
        ASSERT_EQ(count, *field(lines[at], "n")) << api;
        const GuestAddr entries = static_cast<GuestAddr>(read_ptr(list));
        for (u32 i = 0; i < count; ++i) {
            const std::string& e = lines[at + 1 + i];
            const GuestAddr en = entries + i * ew;
            EXPECT_EQ(memory.read_u32(en).value(), *field(e, "perm")) << api << i;
            EXPECT_EQ(memory.read_u32(en + 4).value(), *field(e, "mode")) << api << i;
            EXPECT_EQ(memory.read_u32(en + 8).value(), *field(e, "inh")) << api << i;
            EXPECT_EQ(memory.read_u32(en + trustee_at + (is64 ? 12 : 8)).value(), *field(e, "form")) << api << i;
            EXPECT_EQ(memory.read_u32(en + trustee_at + (is64 ? 16 : 12)).value(), *field(e, "type")) << api << i;
        }
    }
    (void)memory.write_u32(n, 0x5555);
    write_ptr(list, 1);
    set_last_error(0xDEAD);
    l = line_for(lines, "geea_a_null");
    EXPECT_EQ(call("GetExplicitEntriesFromAclA", {0, n, list}), *field(l, "ret"));
    EXPECT_EQ(get_last_error(), *field(l, "gle"));
    EXPECT_EQ(memory.read_u32(n).value(), *field(l, "n"));
    EXPECT_EQ(read_ptr(list) == 0, *field(l, "outNull") != 0);
}

// ---------------------------------------------------------------------------
// GetSecurityInfo / SetSecurityInfo on event objects
// ---------------------------------------------------------------------------

TEST_P(S5AclTest, ObjectSecurityOnEventsMatchesWindows) {
    load("advapi_sdapi_probe");
    EXPECT_EQ(stdcall_bytes("GetSecurityInfo"), 32u);
    EXPECT_EQ(stdcall_bytes("SetSecurityInfo"), 28u);
    constexpr u32 kKernel = 6;
    SyncEvent unnamed_object;
    const Handle anon = handles.allocate(HandleType::Event, &unnamed_object);
    const GuestAddr o = kData + 0x3100, g = kData + 0x3108, d = kData + 0x3110, s = kData + 0x3118,
                    sd = kData + 0x3120;
    auto ptr_class = [&](GuestAddr at) {
        const u64 v = read_ptr(at);
        return v == 1 ? 1u : (v != 0 ? 2u : 0u);
    };
    auto get = [&](u64 h, u32 info) {
        for (GuestAddr at : {o, g, d, s}) write_ptr(at, 1);
        write_ptr(sd, 0);
        set_last_error(0xDEAD);
        return call("GetSecurityInfo", {h, kKernel, info, o, g, d, s, sd});
    };
    auto expect_ret = [&](const char* tag, u64 r) {
        const std::string l = line_for(lines, tag);
        EXPECT_EQ(r, *field(l, "ret")) << tag;
        if (field(l, "gle")) EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
    };
    std::string l = line_for(lines, "gsi_unnamed_event");
    expect_ret("gsi_unnamed_event", get(anon, 4));
    EXPECT_EQ(ptr_class(d), *field(l, "d"));
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 0x1F), *quoted(l, "sddl"));
    const GuestAddr e8 = kData + 0x3200;
    (void)call("InitializeAcl", {e8, 8, 2});
    set_last_error(0xDEAD);
    expect_ret("ssi_unnamed_event", call("SetSecurityInfo", {anon, kKernel, 4, 0, 0, e8, 0}));

    // Every kernel32 event has its own ntsync object id; distinct ids per
    // run keep objects that reuse an address apart, as in production.
    const u64 id_base = is64 ? 0x6400 : 0x8600;
    unnamed_object.nt_id = id_base + 1;
    SyncEvent named_object;
    named_object.nt_id = id_base + 2;
    named_object.name = "Local\\VitreoSdapiProbeEvent";
    const Handle ev = handles.allocate(HandleType::Event, &named_object);
    expect_ret("gsi_badhandle", get(0x1234, 4));
    expect_ret("gsi_null", get(0, 4));
    l = line_for(lines, "gsi_event_dacl");
    expect_ret("gsi_event_dacl", get(ev, 4));
    EXPECT_EQ(ptr_class(o), *field(l, "o"));
    EXPECT_EQ(ptr_class(g), *field(l, "g"));
    EXPECT_EQ(ptr_class(d), *field(l, "d"));
    EXPECT_EQ(ptr_class(s), *field(l, "s"));
    expect_ret("gsi_event_sacl", get(ev, 8));
    l = line_for(lines, "gsi_event_label");
    expect_ret("gsi_event_label", get(ev, 0x10));
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 0x10), *quoted(l, "sddl"));
    const GuestAddr want = sd_from("D:(A;;GA;;;WD)(D;;0x1;;;AN)");
    const GuestAddr present = kData + 0x3300, want_dacl = kData + 0x3308, def = kData + 0x3310;
    (void)call("GetSecurityDescriptorDacl", {want, present, want_dacl, def});
    const u64 wacl = read_ptr(want_dacl);
    set_last_error(0xDEAD);
    expect_ret("ssi_event_dacl", call("SetSecurityInfo", {ev, kKernel, 4, 0, 0, wacl, 0}));
    (void)get(ev, 4);
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 4), *quoted(line_for(lines, "gsi_after_set"), "sddl"));
    set_last_error(0xDEAD);
    expect_ret("ssi_event_empty", call("SetSecurityInfo", {ev, kKernel, 0x80000004u, 0, 0, e8, 0}));
    (void)get(ev, 4);
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 4), *quoted(line_for(lines, "gsi_after_empty"), "sddl"));
    set_last_error(0xDEAD);
    expect_ret("ssi_badhandle", call("SetSecurityInfo", {0x1234, kKernel, 4, 0, 0, wacl, 0}));
    set_last_error(0xDEAD);
    expect_ret("ssi_event_sacl", call("SetSecurityInfo", {ev, kKernel, 8, 0, 0, 0, e8}));
    set_last_error(0xDEAD);
    expect_ret("ssi_event_zero_info", call("SetSecurityInfo", {ev, kKernel, 0, 0, 0, 0, 0}));
    set_last_error(0xDEAD);
    expect_ret("ssi_event_null_dacl", call("SetSecurityInfo", {ev, kKernel, 4, 0, 0, 0, 0}));
    (void)get(ev, 4);
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 4), *quoted(line_for(lines, "gsi_after_null"), "sddl"));
    SyncEvent second_object;
    second_object.nt_id = id_base + 3;
    second_object.name = "Local\\VitreoSdapiProbeEvent2";
    const Handle ev2 = handles.allocate(HandleType::Event, &second_object);
    EXPECT_EQ(get(ev2, 4), 0u);
    EXPECT_EQ(read_ptr(d) != 0, *field(line_for(lines, "gsi_second_event_unaffected"), "hasDacl") != 0);
    const Handle dup = handles.allocate(HandleType::Event, &named_object);
    (void)get(dup, 4);
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 4),
              *quoted(line_for(lines, "gsi_dup_handle_sees_object"), "sddl"));

    // Objects outside the handle table keep working: the current-process
    // pseudo handle and a user32 desktop handle.
    EXPECT_EQ(get(is64 ? ~0ull : 0xFFFFFFFFull, 4), 0u);
    EXPECT_NE(read_ptr(d), 0u);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("SetSecurityInfo", {0xABCD1100u, 7, 4, 0, 0, e8, 0}), 0u);
    for (GuestAddr at : {o, g, d, s}) write_ptr(at, 1);
    EXPECT_EQ(call("GetSecurityInfo", {0xABCD1100u, 7, 4, o, g, d, s, sd}), 0u);
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 4), "D:");

    // An object whose creation captured security (kernel32 records owner +
    // DACL for its OpenEvent access checks): Get reports that DACL and Set
    // replaces it, so the new DACL governs later opens.
    SyncEvent captured_object;
    captured_object.nt_id = id_base + 4;
    captured_object.name = "Local\\VitreoCapturedEvent";
    captured_object.security.owner = SecurityStubs::synthetic_sid();
    captured_object.security.dacl = std::vector<u8>{2, 0, 0x1C, 0, 1, 0, 0, 0, 0, 0, 0x14, 0, 3, 0, 0x1F, 0,
                                                    1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    const Handle cap = handles.allocate(HandleType::Event, &captured_object);
    EXPECT_EQ(get(cap, 4), 0u);
    EXPECT_EQ(sddl_of(static_cast<GuestAddr>(read_ptr(sd)), 4), "D:(A;;0x1f0003;;;WD)");
    EXPECT_EQ(call("SetSecurityInfo", {cap, kKernel, 4, 0, 0, e8, 0}), 0u);
    ASSERT_TRUE(captured_object.security.dacl.has_value());
    EXPECT_EQ(*captured_object.security.dacl, (std::vector<u8>{2, 0, 8, 0, 0, 0, 0, 0}));
    EXPECT_EQ(call("SetSecurityInfo", {cap, kKernel, 4, 0, 0, 0, 0}), 0u);
    EXPECT_FALSE(captured_object.security.dacl.has_value());   // NULL DACL: unrestricted
}

// ---------------------------------------------------------------------------
// CryptoAPI: CryptAcquireContext / hashes / CryptGetProvParam / release
// ---------------------------------------------------------------------------

class S5CapiTest : public S5AclTest {
protected:
    const GuestAddr prov_out = kData + 0x4000;
    u64 acquire(const char* container, const char* provider, u32 type, u32 flags) {
        write_ptr(prov_out, 0x1234);
        const GuestAddr c = container ? wstr(kData + 0x4100, container) : 0;
        const GuestAddr p = provider ? wstr(kData + 0x4200, provider) : 0;
        set_last_error(0xDEAD);
        return call("CryptAcquireContextW", {prov_out, c, p, type, flags});
    }
    void expect_acquire(const std::string& tag, u64 ok) {
        const std::string l = line_for(lines, tag);
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(read_ptr(prov_out) == 0, *field(l, "provIsNull") != 0) << tag;
        EXPECT_EQ(read_ptr(prov_out) == 0x1234, *field(l, "provUntouched") != 0) << tag;
    }
    /// The probe's prov_param: 512-byte buffer, compared as string or bytes.
    void expect_param(const std::string& line, u64 prov, u32 param, u32 flags, bool as_string) {
        const GuestAddr buf = kData + 0x5000, len = kData + 0x4F00;
        fill(buf, 512, 0);
        (void)memory.write_u32(len, 512);
        set_last_error(0xDEAD);
        const u64 ok = call("CryptGetProvParam", {prov, param, buf, len, flags});
        EXPECT_EQ(ok, *field(line, "ok")) << line;
        EXPECT_EQ(get_last_error(), *field(line, "gle")) << line;
        EXPECT_EQ(memory.read_u32(len).value(), *field(line, "len")) << line;
        if (!ok) return;
        if (as_string) {
            std::string s;
            for (GuestAddr p = buf; memory.read_u8(p).value() != 0; ++p) s.push_back(static_cast<char>(memory.read_u8(p).value()));
            EXPECT_EQ(s, *quoted(line, "str")) << line;
        } else {
            const u32 n = memory.read_u32(len).value();
            EXPECT_EQ(hex_bytes(buf, n > 32 ? 32 : n), bytes_of(line)) << line;
        }
    }
};

TEST_P(S5CapiTest, AcquireContextAndProviderParamsMatchWindows) {
    load("advapi_capi_probe");
    EXPECT_EQ(stdcall_bytes("CryptAcquireContextW"), 20u);
    EXPECT_EQ(stdcall_bytes("CryptGetProvParam"), 20u);
    EXPECT_EQ(stdcall_bytes("CryptContextAddRef"), 12u);
    constexpr u32 kVerify = 0xF0000000u;
    // DSS / DH providers (types 3, 13, 18) fail loudly: not replayed here.
    for (u32 type : {1u, 2u, 4u, 5u, 6u, 12u, 24u, 0u, 99u, 25u}) {
        const std::string tag = "acq_type_" + std::to_string(type);
        const u64 ok = acquire(nullptr, nullptr, type, kVerify);
        expect_acquire(tag, ok);
        if (!ok) continue;
        usize at = 0;
        for (; at < lines.size(); ++at) if (lines[at].rfind(tag + " ", 0) == 0) break;
        const u64 prov = read_ptr(prov_out);
        expect_param(lines[at + 1].substr(2), prov, 4, 0, true);     // PP_NAME
        expect_param(lines[at + 2].substr(2), prov, 16, 0, false);   // PP_PROVTYPE
        expect_param(lines[at + 3].substr(2), prov, 3, 0, false);    // PP_IMPTYPE
        expect_param(lines[at + 4].substr(2), prov, 5, 0, false);    // PP_VERSION
        EXPECT_EQ(call("CryptReleaseContext", {prov, 0}), 1u);
    }
    struct Named { const char* tag; const char* name; u32 type; };
    for (const Named c : {Named{"acq_name_base", "Microsoft Base Cryptographic Provider v1.0", 1},
                          Named{"acq_name_enhanced", "Microsoft Enhanced Cryptographic Provider v1.0", 1},
                          Named{"acq_name_aes", "Microsoft Enhanced RSA and AES Cryptographic Provider", 24},
                          Named{"acq_name_aes_xp",
                                "Microsoft Enhanced RSA and AES Cryptographic Provider (Prototype)", 24},
                          Named{"acq_name_aes_as_full", "Microsoft Enhanced RSA and AES Cryptographic Provider", 1},
                          Named{"acq_name_base_as_aes", "Microsoft Base Cryptographic Provider v1.0", 24},
                          Named{"acq_name_bogus", "Vitreo Bogus Provider", 1},
                          Named{"acq_name_lower", "microsoft strong cryptographic provider", 1}}) {
        const u64 ok = acquire(nullptr, c.name, c.type, kVerify);
        expect_acquire(c.tag, ok);
        if (ok) {
            usize at = 0;
            for (; at < lines.size(); ++at) if (lines[at].rfind(std::string(c.tag) + " ", 0) == 0) break;
            expect_param(lines[at + 1].substr(2), read_ptr(prov_out), 4, 0, true);
            (void)call("CryptReleaseContext", {read_ptr(prov_out), 0});
        }
    }
    struct Flags { const char* tag; const char* container; u32 flags; };
    for (const Flags c : {Flags{"acq_flags_verify_delete", nullptr, kVerify | 0x10},
                          Flags{"acq_flags_bogus_1", nullptr, kVerify | 1},
                          Flags{"acq_flags_bogus_100", nullptr, kVerify | 0x100},
                          Flags{"acq_verify_named_container", "VitreoProbeNoSuchContainer", kVerify}}) {
        expect_acquire(c.tag, acquire(c.container, nullptr, 1, c.flags));
    }
    // Key-container lifecycle (per process).
    expect_acquire("acq_open_missing", acquire("VitreoProbeContainerQ7", nullptr, 1, 0));
    expect_acquire("acq_new", acquire("VitreoProbeContainerQ7", nullptr, 1, 8));
    (void)call("CryptReleaseContext", {read_ptr(prov_out), 0});
    expect_acquire("acq_new_again", acquire("VitreoProbeContainerQ7", nullptr, 1, 8));
    expect_acquire("acq_open_existing", acquire("VitreoProbeContainerQ7", nullptr, 1, 0));
    {
        usize at = 0;
        for (; at < lines.size(); ++at) if (lines[at].rfind("acq_open_existing ", 0) == 0) break;
        expect_param(lines[at + 5].substr(2), read_ptr(prov_out), 6, 0, true);   // PP_CONTAINER
        (void)call("CryptReleaseContext", {read_ptr(prov_out), 0});
    }
    expect_acquire("acq_delete", acquire("VitreoProbeContainerQ7", nullptr, 1, 0x10));
    expect_acquire("acq_open_after_delete", acquire("VitreoProbeContainerQ7", nullptr, 1, 0));
    expect_acquire("acq_delete_missing", acquire("VitreoProbeContainerQ7", nullptr, 1, 0x10));

    // CryptGetProvParam on a verify context.
    ASSERT_EQ(acquire(nullptr, nullptr, 1, kVerify), 1u);
    const u64 p = read_ptr(prov_out);
    expect_param(line_for(lines, "  PP_CONTAINER_verify").substr(2), p, 6, 0, true);
    expect_param(line_for(lines, "  PP_KEYSET_TYPE").substr(2), p, 27, 0, false);
    expect_param(line_for(lines, "  PP_ENUMALGS_first").substr(2), p, 1, 1, false);
    expect_param(line_for(lines, "  PP_ENUMALGS_next").substr(2), p, 1, 0, false);
    expect_param(line_for(lines, "  PP_ENUMALGS_EX_first").substr(2), p, 22, 1, false);
    expect_param(line_for(lines, "  bad_param_999").substr(2), p, 999, 0, false);
    expect_param(line_for(lines, "  bad_flags").substr(2), p, 4, 0x10, true);
    const GuestAddr b = kData + 0x5000, len = kData + 0x4F00;
    (void)memory.write_u32(len, 64);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("CryptGetProvParam", {0x1234, 4, b, len, 0}), 0u);
    EXPECT_EQ(get_last_error(), *field(line_for(lines, "gpp_badprov"), "gle"));
    EXPECT_EQ(call("CryptReleaseContext", {p, 0}), 1u);
}

TEST_P(S5CapiTest, HashesAndHandleLifetimesMatchWindows) {
    load("advapi_capi_probe");
    EXPECT_EQ(stdcall_bytes("CryptCreateHash"), 20u);
    EXPECT_EQ(stdcall_bytes("CryptHashData"), 16u);
    EXPECT_EQ(stdcall_bytes("CryptGetHashParam"), 20u);
    EXPECT_EQ(stdcall_bytes("CryptDestroyHash"), 4u);
    EXPECT_EQ(stdcall_bytes("CryptReleaseContext"), 8u);
    ASSERT_EQ(acquire(nullptr, nullptr, 1, 0xF0000000u), 1u);
    const u64 p = read_ptr(prov_out);
    const GuestAddr h_out = kData + 0x4300, abc = astr(kData + 0x4400, "abc");
    const GuestAddr v = kData + 0x4500, len = kData + 0x4600;
    auto hash_case = [&](const std::string& tag, u32 alg) {
        const std::string l = line_for(lines, tag);
        write_ptr(h_out, 0x1234);
        set_last_error(0xDEAD);
        const u64 ok = call("CryptCreateHash", {p, alg, 0, 0, h_out});
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(read_ptr(h_out) == 0x1234, *field(l, "hUntouched") != 0) << tag;
        if (!ok) return;
        const u64 h = read_ptr(h_out);
        (void)call("CryptHashData", {h, abc, 3, 0});
        (void)memory.write_u32(len, 64);
        const u64 got = call("CryptGetHashParam", {h, 2, v, len, 0});
        EXPECT_EQ(got, *field(l, "getOk")) << tag;
        if (!got) EXPECT_EQ(get_last_error(), *field(l, "getGle")) << tag;
        EXPECT_EQ(memory.read_u32(len).value(), *field(l, "len")) << tag;
        if (got) EXPECT_EQ(hex_bytes(v, memory.read_u32(len).value()), bytes_of(l)) << tag;
        (void)memory.write_u32(len, 4);
        (void)call("CryptGetHashParam", {h, 1, v, len, 0});
        EXPECT_EQ(memory.read_u32(v).value(), *field(l, "algid")) << tag;
        EXPECT_EQ(call("CryptDestroyHash", {h}), 1u) << tag;
    };
    hash_case("hash_md5", 0x8003);
    hash_case("hash_sha1", 0x8004);
    hash_case("hash_sha256", 0x800C);
    hash_case("hash_sha384", 0x800D);
    hash_case("hash_sha512", 0x800E);
    hash_case("hash_md2", 0x8001);
    hash_case("hash_md4", 0x8002);
    hash_case("hash_ssl3_shamd5", 0x8008);
    hash_case("hash_rc4_notahash", 0x6801);
    hash_case("hash_zero", 0);
    hash_case("hash_hmac_nokey", 0x8009);
    hash_case("hash_mac_nokey", 0x8005);

    auto expect_line = [&](const char* tag, u64 ok) {
        const std::string l = line_for(lines, tag);
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
    };
    write_ptr(h_out, 0x1234);
    set_last_error(0xDEAD);
    expect_line("hash_flags1", call("CryptCreateHash", {p, 0x8004, 0, 1, h_out}));
    set_last_error(0xDEAD);
    expect_line("hash_badprov", call("CryptCreateHash", {0x1234, 0x8004, 0, 0, h_out}));
    set_last_error(0xDEAD);
    expect_line("hash_badkey", call("CryptCreateHash", {p, 0x8004, 0x1234, 0, h_out}));
    set_last_error(0xDEAD);
    expect_line("hash_nullout", call("CryptCreateHash", {p, 0x8004, 0, 0, 0}));

    ASSERT_EQ(call("CryptCreateHash", {p, 0x8004, 0, 0, h_out}), 1u);
    const u64 h = read_ptr(h_out);
    (void)call("CryptHashData", {h, abc, 3, 0});
    (void)memory.write_u32(len, 64);
    (void)call("CryptGetHashParam", {h, 2, v, len, 0});
    set_last_error(0xDEAD);
    expect_line("hashdata_after_final", call("CryptHashData", {h, abc, 1, 0}));
    (void)memory.write_u32(len, 4);
    set_last_error(0xDEAD);
    expect_line("gethash_small", call("CryptGetHashParam", {h, 2, v, len, 0}));
    EXPECT_EQ(memory.read_u32(len).value(), *field(line_for(lines, "gethash_small"), "len"));
    (void)memory.write_u32(len, 0);
    set_last_error(0xDEAD);
    expect_line("gethash_query", call("CryptGetHashParam", {h, 2, 0, len, 0}));
    EXPECT_EQ(memory.read_u32(len).value(), *field(line_for(lines, "gethash_query"), "len"));
    (void)memory.write_u32(len, 64);
    set_last_error(0xDEAD);
    expect_line("gethash_badparam", call("CryptGetHashParam", {h, 99, v, len, 0}));
    set_last_error(0xDEAD);
    expect_line("gethash_badflags", call("CryptGetHashParam", {h, 2, v, len, 1}));
    set_last_error(0xDEAD);
    expect_line("hashdata_badflags", call("CryptHashData", {h, abc, 1, 2}));
    EXPECT_EQ(call("CryptDestroyHash", {h}), 1u);
    set_last_error(0xDEAD);
    expect_line("destroy_twice", call("CryptDestroyHash", {h}));
    set_last_error(0xDEAD);
    expect_line("destroy_bogus", call("CryptDestroyHash", {0x1234}));
    set_last_error(0xDEAD);
    expect_line("destroy_zero", call("CryptDestroyHash", {0}));
    set_last_error(0xDEAD);
    expect_line("hashdata_bogus", call("CryptHashData", {0x1234, abc, 1, 0}));

    // CryptReleaseContext / CryptContextAddRef.
    ASSERT_EQ(acquire(nullptr, nullptr, 1, 0xF0000000u), 1u);
    u64 q = read_ptr(prov_out);
    set_last_error(0xDEAD);
    expect_line("release_flags1", call("CryptReleaseContext", {q, 1}));
    set_last_error(0xDEAD);
    expect_line("release_after_flagged", call("CryptReleaseContext", {q, 0}));
    set_last_error(0xDEAD);
    expect_line("release_bogus", call("CryptReleaseContext", {0x1234, 0}));
    set_last_error(0xDEAD);
    expect_line("release_zero", call("CryptReleaseContext", {0, 0}));
    ASSERT_EQ(acquire(nullptr, nullptr, 1, 0xF0000000u), 1u);
    q = read_ptr(prov_out);
    set_last_error(0xDEAD);
    expect_line("addref_flags1", call("CryptContextAddRef", {q, 0, 1}));
    set_last_error(0xDEAD);
    expect_line("addref_reserved", call("CryptContextAddRef", {q, kData + 0x4700, 0}));
    set_last_error(0xDEAD);
    expect_line("addref_bogus", call("CryptContextAddRef", {0x1234, 0, 0}));
    set_last_error(0xDEAD);
    expect_line("addref", call("CryptContextAddRef", {q, 0, 0}));
    set_last_error(0xDEAD);
    expect_line("release_1_of_2", call("CryptReleaseContext", {q, 0}));
    set_last_error(0xDEAD);
    expect_line("release_2_of_2", call("CryptReleaseContext", {q, 0}));
    set_last_error(0xDEAD);
    expect_line("release_3_of_2", call("CryptReleaseContext", {q, 0}));
    EXPECT_EQ(call("CryptReleaseContext", {p, 0}), 1u);
}

auto bitness_name = [](const ::testing::TestParamInfo<bool>& info) {
    return info.param ? std::string("x64") : std::string("x86");
};
// ---------------------------------------------------------------------------
// DeleteAce (advapi_deleteace_probe: three ACEs in a 96-byte ACL)
// ---------------------------------------------------------------------------

TEST_P(S5AclTest, DeleteAceMatchesWindows) {
    load("advapi_deleteace_probe");
    // This probe prints decimal fields.
    auto dec = [](const std::string& l, const char* key) { return std::stoull(*token(l, key)); };
    auto hex_line = [&](const std::string& tag) {
        for (const auto& l : lines)
            if (l.rfind(tag + ":", 0) == 0) return l.substr(tag.size() + 1);
        ADD_FAILURE() << "no probe line " << tag << ":";
        return std::string();
    };
    const std::string before = hex_line("before");
    ASSERT_EQ(before.size(), 192u);
    auto reset = [&] {
        for (u32 i = 0; i < 96; ++i)
            (void)memory.write_u8(acl + i, static_cast<u8>(std::stoul(before.substr(i * 2, 2), nullptr, 16)));
    };
    for (const char* tag : {"first", "middle", "last", "past-end", "huge"}) {
        reset();
        const std::string l = line_for(lines, tag);
        set_last_error(0xDEADBEEF);
        const u64 ok = call("DeleteAce", {acl, dec(l, "index")});
        EXPECT_EQ(ok, dec(l, "ok")) << tag;
        if (ok == 0) EXPECT_EQ(get_last_error(), dec(l, "err")) << tag;
        EXPECT_EQ(hex_bytes(acl, 96), hex_line(tag)) << tag;
    }
    reset();
    (void)memory.write_u8(acl, 0);
    set_last_error(0xDEADBEEF);
    EXPECT_EQ(call("DeleteAce", {acl, 0}), dec(line_for(lines, "bad-revision"), "ok"));
    EXPECT_EQ(get_last_error(), dec(line_for(lines, "bad-revision"), "err"));
    (void)call("InitializeAcl", {acl, 96, 2});
    set_last_error(0xDEADBEEF);
    EXPECT_EQ(call("DeleteAce", {acl, 0}), dec(line_for(lines, "empty"), "ok"));
    EXPECT_EQ(get_last_error(), dec(line_for(lines, "empty"), "err"));
    reset();
    (void)memory.write_u16(acl + 4, 9);
    set_last_error(0xDEADBEEF);
    EXPECT_EQ(call("DeleteAce", {acl, 5}), dec(line_for(lines, "corrupt-count"), "ok"));
    EXPECT_EQ(get_last_error(), dec(line_for(lines, "corrupt-count"), "err"));
    EXPECT_EQ(memory.read_u16(acl + 4).value(), dec(line_for(lines, "corrupt-count"), "count"));
}

INSTANTIATE_TEST_SUITE_P(Bitness, S5AclTest, ::testing::Values(true, false), bitness_name);
INSTANTIATE_TEST_SUITE_P(Bitness, S5CapiTest, ::testing::Values(true, false), bitness_name);

} // namespace
