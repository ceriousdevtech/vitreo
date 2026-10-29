// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_s5_tokens.cpp
/// @brief advapi32 token, AccessCheck, ETW and well-known-SID contracts
///        (S5 net/crypto audit, 2026-09-30).
///
/// Every expected value is Windows' own output from tools/oracle_probe/
/// (advapi_token2_probe, advapi_etw_probe, advapi_wksid_probe; .expected for
/// x64, .x86.expected for x86), replayed through the exported thunks at the
/// matching bitness. Lines that depend on the SSH logon session of the
/// measuring box (privileges it had enabled, the token object's own DACL) are
/// named where they are skipped.

#include "advapi_thunk_harness.hpp"

#include "winapi/kernel32/synchronization/sync.hpp"

#include <map>
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

/// The probe line whose first word is `tag`.
std::string line_for(const std::vector<std::string>& lines, const std::string& tag) {
    for (const auto& l : lines) {
        if (l.rfind(tag + " ", 0) == 0) return l;
    }
    ADD_FAILURE() << "no probe line " << tag;
    return {};
}

/// Standard SID string -> bytes (test-side, independent of Vitreo's parser).
std::vector<u8> sid_bytes(const std::string& text) {
    std::vector<u64> parts;
    usize at = 2;
    while (at <= text.size()) {
        const usize dash = text.find('-', at);
        const std::string f = text.substr(at, dash == std::string::npos ? std::string::npos : dash - at);
        parts.push_back(f.rfind("0x", 0) == 0 ? std::strtoull(f.c_str() + 2, nullptr, 16)
                                              : std::strtoull(f.c_str(), nullptr, 10));
        if (dash == std::string::npos) break;
        at = dash + 1;
    }
    std::vector<u8> sid(8 + (parts.size() - 2) * 4, 0);
    sid[0] = static_cast<u8>(parts[0]);
    sid[1] = static_cast<u8>(parts.size() - 2);
    for (int i = 0; i < 6; ++i) sid[2 + i] = static_cast<u8>(parts[1] >> ((5 - i) * 8));
    for (usize i = 2; i < parts.size(); ++i) {
        for (int b = 0; b < 4; ++b) sid[8 + (i - 2) * 4 + b] = static_cast<u8>(parts[i] >> (b * 8));
    }
    return sid;
}

class S5TokenTest : public advapi_test::AdvapiThunkHarness {
protected:
    std::vector<std::string> lines;
    GuestAddr scratch = kData + 0x8000;
    std::vector<u8> user_sid;   ///< the token user (the probe's g_user)
    std::string user_text;

    /// Adopts the process token's user, as the probe does with TokenUser.
    void init_user() {
        const GuestAddr out = kData + 0x7F00;
        write_ptr(out, 0);
        ASSERT_EQ(call("OpenProcessToken", {pseudo(-1), 0x8, out}), 1u);
        user_sid = stubs.token_identity(static_cast<Handle>(read_ptr(out))).user;
        ASSERT_GE(user_sid.size(), 8u);
        u64 authority = 0;
        for (int i = 0; i < 6; ++i) authority = (authority << 8) | user_sid[2 + i];
        user_text = "S-" + std::to_string(user_sid[0]) + "-" + std::to_string(authority);
        for (u32 i = 0; i < user_sid[1]; ++i) {
            u32 sub = 0;
            for (int b = 3; b >= 0; --b) sub = (sub << 8) | user_sid[8 + i * 4 + b];
            user_text += "-" + std::to_string(sub);
        }
    }

    void load(const char* probe) { lines = read_expected(expected_file(probe).c_str()); }

    u64 pseudo(i64 v) const { return is64 ? static_cast<u64>(v) : static_cast<u64>(v) & 0xFFFFFFFFull; }
    GuestAddr put_sid(const std::string& text) {
        const auto b = sid_bytes(text);
        const GuestAddr at = scratch;
        (void)memory.write_bytes(at, b.data(), b.size());
        scratch += 0x50;
        return at;
    }
    u64 open_process_token(u32 access) {
        const GuestAddr out = kData + 0x7F00;
        write_ptr(out, 0);
        EXPECT_EQ(call("OpenProcessToken", {pseudo(-1), access, out}), 1u);
        return read_ptr(out);
    }
    u64 dup_ex(u64 src, u32 access, u32 level, u32 type) {
        const GuestAddr out = kData + 0x7F10;
        write_ptr(out, 0);
        (void)call("DuplicateTokenEx", {src, access, 0, level, type, out});
        return read_ptr(out);
    }
    /// SID_AND_ATTRIBUTES array of one entry at the guest's pointer width.
    GuestAddr one_saa(GuestAddr sid) {
        const GuestAddr at = scratch;
        scratch += 0x20;
        write_ptr(at, sid);
        (void)memory.write_u32(at + (is64 ? 8 : 4), 0);
        return at;
    }
    GuestAddr two_saa(GuestAddr a, GuestAddr b) {
        const GuestAddr at = scratch;
        scratch += 0x40;
        const u32 stride = is64 ? 16 : 8;
        write_ptr(at, a);
        (void)memory.write_u32(at + (is64 ? 8 : 4), 0);
        write_ptr(at + stride, b);
        (void)memory.write_u32(at + stride + (is64 ? 8 : 4), 0);
        return at;
    }
    u64 restricted(u64 src, GuestAddr disable, u32 ndisable, GuestAddr restrict_saa, u32 nrestrict) {
        const GuestAddr out = kData + 0x7F20;
        write_ptr(out, 0);
        EXPECT_EQ(call("CreateRestrictedToken",
                       {src, 0, ndisable, disable, 0, 0, nrestrict, restrict_saa, out}), 1u);
        return read_ptr(out);
    }
    /// CheckTokenMembership against its probe line.
    void expect_ctm(const std::string& tag, u64 tok, GuestAddr sid) {
        const std::string l = line_for(lines, tag);
        const GuestAddr member = kData + 0x7F30;
        (void)memory.write_u32(member, 0x5555);
        set_last_error(0xDEAD);
        const u64 ok = call("CheckTokenMembership", {tok, sid, member});
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(memory.read_u32(member).value(), *field(l, "member")) << tag;
    }
};

// ---------------------------------------------------------------------------
// CheckTokenMembership
// ---------------------------------------------------------------------------

TEST_P(S5TokenTest, CheckTokenMembershipMatchesWindows) {
    load("advapi_token2_probe");
    init_user();
    EXPECT_EQ(stdcall_bytes("CheckTokenMembership"), 12u);
    const GuestAddr wd = put_sid("S-1-1-0"), au = put_sid("S-1-5-11"), sy = put_sid("S-1-5-18");
    const GuestAddr rnd = put_sid("S-1-5-21-1-2-3-4"), user = put_sid(user_text);
    const GuestAddr ba = put_sid("S-1-5-32-544");
    expect_ctm("ctm_null_everyone", 0, wd);
    expect_ctm("ctm_null_user", 0, user);
    expect_ctm("ctm_null_system", 0, sy);
    expect_ctm("ctm_null_random", 0, rnd);
    expect_ctm("ctm_null_au", 0, au);
    const u64 all = open_process_token(0xF01FF);
    expect_ctm("ctm_primary", all, wd);
    expect_ctm("ctm_pseudo_process", pseudo(-4), wd);
    expect_ctm("ctm_pseudo_thread", pseudo(-5), wd);
    expect_ctm("ctm_pseudo_effective", pseudo(-6), wd);
    expect_ctm("ctm_primary_noquery", open_process_token(0x2), wd);
    expect_ctm("ctm_badhandle", 0x1234, wd);
    SyncEvent event_object;
    const Handle ev = handles.allocate(HandleType::Event, &event_object);
    expect_ctm("ctm_event", ev, wd);
    expect_ctm("ctm_imp_noquery", dup_ex(all, 0x4, 2, 2), wd);
    const u64 imp = dup_ex(all, 0x8, 2, 2);
    expect_ctm("ctm_imp_everyone", imp, wd);
    expect_ctm("ctm_imp_user", imp, user);
    expect_ctm("ctm_imp_random", imp, rnd);
    expect_ctm("ctm_ident_everyone", dup_ex(all, 0x8, 1, 2), wd);
    expect_ctm("ctm_anon_everyone", dup_ex(all, 0x8, 0, 2), wd);
    expect_ctm("ctm_deleg_everyone", dup_ex(all, 0x8, 3, 2), wd);

    // Everyone deny-only, restricting SIDs = { user }.
    const u64 r = restricted(all, one_saa(wd), 1, one_saa(user), 1);
    const u64 ri = dup_ex(r, 0xC, 2, 2);
    expect_ctm("ctm_restr_everyone", ri, wd);
    expect_ctm("ctm_restr_user", ri, user);
    expect_ctm("ctm_restr_au", ri, au);
    expect_ctm("ctm_restr_primary", r, user);
    // ctm_thread_restr_*: Windows denies opening the thread's restricted
    // token against the token object's own DACL; token objects carry no DACL
    // here (report: OPEN).
    const u64 r2 = restricted(all, 0, 0, two_saa(wd, user), 2);
    const u64 ri2 = dup_ex(r2, 0xC, 2, 2);
    expect_ctm("ctm_restr2_everyone", ri2, wd);
    expect_ctm("ctm_restr2_au", ri2, au);
    const u64 r3 = restricted(all, one_saa(ba), 1, 0, 0);
    expect_ctm("ctm_denyonly_admin", dup_ex(r3, 0xC, 2, 2), ba);
    ASSERT_EQ(call("ImpersonateSelf", {2}), 1u);
    expect_ctm("ctm_thread_self_everyone", 0, wd);
    (void)call("RevertToSelf", {});
}

// ---------------------------------------------------------------------------
// OpenProcessToken / DuplicateTokenEx / DuplicateToken
// ---------------------------------------------------------------------------

TEST_P(S5TokenTest, TokenGenericMappingAndDuplicateRulesMatchWindows) {
    load("advapi_token2_probe");
    EXPECT_EQ(stdcall_bytes("DuplicateTokenEx"), 24u);
    const GuestAddr out = kData + 0x7F40;
    for (const auto& l : lines) {
        if (l.rfind("opt_access ", 0) != 0) continue;
        const u32 req = static_cast<u32>(*field(l, "req"));
        write_ptr(out, 0x1234);
        set_last_error(0xDEAD);
        const u64 ok = call("OpenProcessToken", {pseudo(-1), req, out});
        EXPECT_EQ(ok, *field(l, "ok")) << l;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << l;
        if (ok) EXPECT_EQ(handles.get_access(static_cast<Handle>(read_ptr(out))), *field(l, "granted")) << l;
        else EXPECT_EQ(read_ptr(out), *field(l, "out")) << l;
    }

    const u64 all = open_process_token(0xF01FF);
    const u64 src = open_process_token(0x2 | 0x8);
    const u64 nodup = open_process_token(0x8);
    const u64 ident = dup_ex(all, 0xF01FF, 1, 2);
    const u64 imp = dup_ex(all, 0xF01FF, 2, 2);
    struct Case { const char* tag; u64 src; u32 access, level, type; };
    const Case cases[] = {
        {"dte_zero", src, 0, 2, 2}, {"dte_max", src, 0x02000000, 2, 2},
        {"dte_generic_all", src, 0x10000000, 2, 2}, {"dte_generic_read", src, 0x80000000, 2, 2},
        {"dte_generic_write", src, 0x40000000, 2, 2}, {"dte_generic_exec", src, 0x20000000, 2, 2},
        {"dte_query", src, 0x8, 2, 2}, {"dte_primary_zero", src, 0, 0, 1},
        {"dte_primary_max", src, 0x02000000, 2, 1}, {"dte_level4", src, 0x8, 4, 2},
        {"dte_level_ff", src, 0x8, 0xFF, 2}, {"dte_type0", src, 0x8, 2, 0},
        {"dte_type3", src, 0x8, 2, 3}, {"dte_primary_level9", src, 0x8, 9, 1},
        {"dte_all_access", src, 0xF01FF, 2, 2}, {"dte_bogus_bits", src, 0x200, 2, 2},
        {"dte_src_nodup", nodup, 0x8, 2, 2}, {"dte_badhandle", 0x1234, 0x8, 2, 2},
        {"dte_pseudo_process_token", pseudo(-4), 0x8, 2, 2},
        {"dte_ident_to_primary", ident, 0x8, 2, 1}, {"dte_ident_to_impers", ident, 0x8, 2, 2},
        {"dte_ident_to_ident", ident, 0x8, 1, 2}, {"dte_ident_to_anon", ident, 0x8, 0, 2},
        {"dte_imp_to_primary", imp, 0x8, 0, 1}, {"dte_imp_to_deleg", imp, 0x8, 3, 2},
    };
    // dte_access_system_security needs SeSecurityPrivilege enabled in the
    // caller's token, which the measuring SSH session had (priv_state=3).
    for (const auto& c : cases) {
        const std::string l = line_for(lines, c.tag);
        write_ptr(out, 0x1234);
        set_last_error(0xDEAD);
        const u64 ok = call("DuplicateTokenEx", {c.src, c.access, 0, c.level, c.type, out});
        EXPECT_EQ(ok, *field(l, "ok")) << c.tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << c.tag;
        if (!ok) {
            EXPECT_EQ(read_ptr(out), *field(l, "out")) << c.tag;
            continue;
        }
        // The "  kind" line that follows reports type/level/granted.
        const auto it = std::find(lines.begin(), lines.end(), l);
        ASSERT_NE(it + 1, lines.end());
        const std::string kind = *(it + 1);
        const Handle h = static_cast<Handle>(read_ptr(out));
        EXPECT_EQ(handles.get_access(h), *field(kind, "granted")) << c.tag;
        // type/level are only readable with TOKEN_QUERY (else the probe
        // prints type=0).
        if (*field(kind, "type") != 0) {
            EXPECT_EQ(stubs.token_type(h), *field(kind, "type")) << c.tag;
            if (*field(kind, "type") == 2) {
                EXPECT_EQ(stubs.token_identity(h).impersonation_level, *field(kind, "level")) << c.tag;
            }
        }
    }
    for (const char* tag : {"dt_nodup", "dt_badlevel"}) {
        const std::string l = line_for(lines, tag);
        write_ptr(out, 0x1234);
        set_last_error(0xDEAD);
        const u64 ok = call("DuplicateToken", {std::string(tag) == "dt_nodup" ? nodup : all,
                                               std::string(tag) == "dt_nodup" ? 2u : 7u, out});
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(read_ptr(out), *field(l, "out")) << tag;
    }
}

// ---------------------------------------------------------------------------
// AccessCheck
// ---------------------------------------------------------------------------

TEST_P(S5TokenTest, AccessCheckMatchesWindows) {
    load("advapi_token2_probe");
    init_user();
    EXPECT_EQ(stdcall_bytes("AccessCheck"), 32u);
    const GuestAddr mapping = kData + 0x7E00;
    for (u32 i = 0; i < 4; ++i) {
        const u32 v[4] = {0x120089, 0x120116, 0x1200A0, 0x1F01FF};
        (void)memory.write_u32(mapping + i * 4, v[i]);
    }
    const u64 all = open_process_token(0xF01FF);
    const u64 imp = dup_ex(all, 0xC, 2, 2);
    const u64 impnq = dup_ex(all, 0x4, 2, 2);
    const GuestAddr wd = put_sid("S-1-1-0"), user = put_sid(user_text);
    const u64 r = restricted(all, one_saa(wd), 1, one_saa(user), 1);
    const u64 ri = dup_ex(r, 0x8, 2, 2);
    const u64 low = dup_ex(all, 0xF01FF, 2, 2);
    {
        const GuestAddr lw = put_sid("S-1-16-4096");
        const GuestAddr tml = scratch;
        scratch += 0x20;
        write_ptr(tml, lw);
        (void)memory.write_u32(tml + (is64 ? 8 : 4), 0x20);
        ASSERT_EQ(call("SetTokenInformation", {low, 25, tml, is64 ? 16u : 8u}), 1u);
    }
    const u64 privs = dup_ex(all, 0xF01FF, 2, 2);
    auto set_priv = [&](u32 luid, u32 attrs) {
        const GuestAddr tp = kData + 0x7D00;
        (void)memory.write_u32(tp, 1);
        (void)memory.write_u64(tp + 4, luid);
        (void)memory.write_u32(tp + 12, attrs);
        (void)call("AdjustTokenPrivileges", {privs, 0, tp, 0, 0, 0});
    };
    auto with_user = [&](std::string t) {
        for (usize p; (p = t.find('@')) != std::string::npos;) t.replace(p, 1, user_text);
        return t;
    };
    struct Case { const char* tag; std::string sddl; u64 tok; u32 desired; u32 pslen; };
    std::vector<Case> cases = {
        {"ac_primary", "O:SYG:SYD:(A;;FA;;;WD)", all, 1, 64},
        {"ac_noquery", "O:SYG:SYD:(A;;FA;;;WD)", impnq, 1, 64},
        {"ac_badtoken", "O:SYG:SYD:(A;;FA;;;WD)", 0x1234, 1, 64},
        {"ac_nulldacl", "O:SYG:SYD:NO_ACCESS_CONTROL", imp, 1, 64},
        {"ac_nulldacl_max", "O:SYG:SYD:NO_ACCESS_CONTROL", imp, 0x02000000, 64},
        {"ac_nodacl", "O:SYG:SY", imp, 1, 64},
        {"ac_emptydacl", "O:SYG:SYD:", imp, 1, 64},
        {"ac_allow_wd", "O:SYG:SYD:(A;;FR;;;WD)", imp, 1, 64},
        {"ac_allow_wd_partial", "O:SYG:SYD:(A;;FR;;;WD)", imp, 3, 64},
        {"ac_deny_first", "O:SYG:SYD:(D;;FR;;;WD)(A;;FA;;;WD)", imp, 1, 64},
        {"ac_deny_other_bits", "O:SYG:SYD:(D;;FW;;;WD)(A;;FA;;;WD)", imp, 1, 64},
        {"ac_allow_then_deny", "O:SYG:SYD:(A;;FA;;;WD)(D;;FR;;;WD)", imp, 1, 64},
        {"ac_generic_desired", "O:SYG:SYD:(A;;FA;;;WD)", imp, 0x80000000, 64},
        {"ac_generic_ace", "O:SYG:SYD:(A;;GR;;;WD)", imp, 1, 64},
        {"ac_max_fr", "O:SYG:SYD:(A;;FR;;;WD)", imp, 0x02000000, 64},
        {"ac_max_deny", "O:SYG:SYD:(D;;FW;;;WD)(A;;FA;;;WD)", imp, 0x02000000, 64},
        {"ac_max_empty", "O:SYG:SYD:", imp, 0x02000000, 64},
        {"ac_max_plus_read", "O:SYG:SYD:(A;;FR;;;WD)", imp, 0x02000002, 64},
        {"ac_inherit_only", "O:SYG:SYD:(A;IO;FA;;;WD)", imp, 1, 64},
        {"ac_random_sid", "O:SYG:SYD:(A;;FA;;;S-1-5-21-1-2-3-4)", imp, 1, 64},
        {"ac_zero_desired", "O:SYG:SYD:(A;;FR;;;WD)", imp, 0, 64},
        {"ac_zero_desired_empty", "O:SYG:SYD:", imp, 0, 64},
        {"ac_owner_implicit", with_user("O:@G:SYD:(A;;FR;;;WD)"), imp, 0x60000, 64},
        {"ac_owner_max", with_user("O:@G:SYD:(A;;FR;;;WD)"), imp, 0x02000000, 64},
        {"ac_owner_rights_ace", with_user("O:@G:SYD:(A;;FR;;;WD)(A;;0x1;;;OW)"), imp, 0x40000, 64},
        {"ac_user_ace", with_user("O:SYG:SYD:(A;;FW;;;@)"), imp, 2, 64},
        {"ac_no_owner", "G:SYD:(A;;FA;;;WD)", imp, 1, 64},
        {"ac_no_group", "O:SYD:(A;;FA;;;WD)", imp, 1, 64},
        {"ac_label_nw_ok", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;LW)", imp, 2, 64},
        {"ac_label_sys_nw", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;SI)", imp, 2, 64},
        {"ac_label_sys_read", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;SI)", imp, 1, 64},
        {"ac_label_sys_nr", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NR;;;SI)", imp, 1, 64},
        {"ac_label_sys_max", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;SI)", imp, 0x02000000, 64},
        {"ac_pslen0", "O:SYG:SYD:(A;;FA;;;WD)", imp, 1, 0},
        {"ac_pslen7", "O:SYG:SYD:(A;;FA;;;WD)", imp, 1, 7},
        {"ac_pslen8", "O:SYG:SYD:(A;;FA;;;WD)", imp, 1, 8},
        {"ac_pslen20", "O:SYG:SYD:(A;;FA;;;WD)", imp, 1, 20},
        {"ac_restr_wd_allow", "O:SYG:SYD:(A;;FR;;;WD)", ri, 1, 64},
        {"ac_restr_wd_deny", "O:SYG:SYD:(D;;FR;;;WD)(A;;FA;;;AU)", ri, 1, 64},
        {"ac_restr_user", with_user("O:SYG:SYD:(A;;FR;;;@)"), ri, 1, 64},
        {"ac_restr_au", "O:SYG:SYD:(A;;FR;;;AU)", ri, 1, 64},
        {"ac_restr_split_max", with_user("O:SYG:SYD:(A;;FR;;;AU)(A;;FW;;;@)"), ri, 0x02000000, 64},
        {"ac_low_vs_me_write", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;ME)", low, 2, 64},
        {"ac_low_vs_me_read", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;ME)", low, 1, 64},
        {"ac_low_vs_me_max", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;ME)", low, 0x02000000, 64},
        {"ac_low_nolabel_write", "O:SYG:SYD:(A;;FA;;;WD)", low, 2, 64},
        {"ac_low_vs_low_write", "O:SYG:SYD:(A;;FA;;;WD)S:(ML;;NW;;;LW)", low, 2, 64},
    };
    // ac_system_security / ac_writeowner_no_ace depend on the privileges the
    // SSH session had enabled; the ac_sacl_priv_* / ac_writeowner_takeown*
    // lines set them explicitly first and are replayed below.
    auto run = [&](const std::string& tag, const std::string& sddl, u64 tok, u32 desired, u32 pslen) {
        const std::string l = line_for(lines, tag);
        const GuestAddr text = wstr(kData + 0x6000, sddl);
        const GuestAddr sd_out = kData + 0x6F00;
        ASSERT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorW", {text, 1, sd_out, 0}), 1u) << tag;
        const GuestAddr sd = static_cast<GuestAddr>(read_ptr(sd_out));
        const GuestAddr ps = kData + 0x6F40, len = kData + 0x6F90, granted = kData + 0x6F94,
                        status = kData + 0x6F98;
        for (u32 i = 0; i < 64; ++i) (void)memory.write_u8(ps + i, 0xCC);
        (void)memory.write_u32(len, pslen);
        (void)memory.write_u32(granted, 0x5555);
        (void)memory.write_u32(status, 0x5555);
        set_last_error(0xDEAD);
        const u64 ok = call("AccessCheck", {sd, tok, desired, mapping, ps, len, granted, status});
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(memory.read_u32(granted).value(), *field(l, "granted")) << tag;
        EXPECT_EQ(memory.read_u32(status).value(), *field(l, "status")) << tag;
        EXPECT_EQ(memory.read_u32(len).value(), *field(l, "pslen")) << tag;
        EXPECT_EQ(memory.read_u32(ps).value(), *field(l, "privCount")) << tag;
    };
    for (const auto& c : cases) run(c.tag, c.sddl, c.tok, c.desired, c.pslen);
    set_priv(8, 0);
    run("ac_sacl_priv_disabled", "O:SYG:SYD:(A;;FA;;;WD)", privs, 0x01000000, 64);
    set_priv(8, 2);
    run("ac_sacl_priv_enabled", "O:SYG:SYD:(A;;FA;;;WD)", privs, 0x01000000, 64);
    run("ac_sacl_priv_enabled_max", "O:SYG:SYD:(A;;FR;;;WD)", privs, 0x03000000, 64);
    set_priv(9, 2);
    run("ac_writeowner_takeown", "O:SYG:SYD:(A;;FR;;;WD)", privs, 0x80000, 64);
    run("ac_writeowner_takeown_max", "O:SYG:SYD:(A;;FR;;;WD)", privs, 0x02000000, 64);
    set_priv(8, 4);   // SE_PRIVILEGE_REMOVED
    run("ac_sacl_priv_removed", "O:SYG:SYD:(A;;FA;;;WD)", privs, 0x01000000, 64);
}

// ---------------------------------------------------------------------------
// ImpersonateAnonymousToken / ImpersonateNamedPipeClient
// ---------------------------------------------------------------------------

TEST_P(S5TokenTest, AnonymousAndPipeImpersonation) {
    load("advapi_token2_probe");
    init_user();
    EXPECT_EQ(stdcall_bytes("ImpersonateAnonymousToken"), 4u);
    EXPECT_EQ(stdcall_bytes("ImpersonateNamedPipeClient"), 4u);
    auto expect_line = [&](const char* tag, u64 ok) {
        const std::string l = line_for(lines, tag);
        EXPECT_EQ(ok, *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
    };
    set_last_error(0xDEAD);
    expect_line("anon_impersonate", call("ImpersonateAnonymousToken", {pseudo(-2)}));
    const GuestAddr t = kData + 0x7F50;
    set_last_error(0xDEAD);
    expect_line("anon_open_thread_token", call("OpenThreadToken", {pseudo(-2), 0x8, 1, t}));
    // anon_user: S-1-5-7, one group S-1-16-0 (0x60), impersonation level 2,
    // no privileges, untrusted integrity.
    const auto id = stubs.token_identity(static_cast<Handle>(read_ptr(t)));
    EXPECT_EQ(id.user, sid_bytes("S-1-5-7"));
    ASSERT_EQ(id.groups.size(), 1u);
    EXPECT_EQ(id.groups[0].sid, sid_bytes("S-1-16-0"));
    EXPECT_EQ(id.groups[0].attributes, 0x60u);
    EXPECT_EQ(id.impersonation_level, 2u);
    EXPECT_TRUE(id.privileges.empty());
    EXPECT_EQ(id.integrity_rid, 0u);
    expect_ctm("anon_ctm_everyone", 0, put_sid("S-1-1-0"));
    const GuestAddr anon = put_sid("S-1-5-7");
    set_last_error(0);   // the probe's ConvertStringSidToSidW left 0 here
    const GuestAddr member = kData + 0x7F30;
    EXPECT_EQ(call("CheckTokenMembership", {0, anon, member}), 1u);
    EXPECT_EQ(memory.read_u32(member).value(), *field(line_for(lines, "anon_ctm_anonymous"), "member"));
    (void)call("RevertToSelf", {});
    set_last_error(0xDEAD);
    expect_line("anon_badhandle", call("ImpersonateAnonymousToken", {0x1234}));
    SyncEvent event_object;
    const Handle ev = handles.allocate(HandleType::Event, &event_object);
    set_last_error(0xDEAD);
    expect_line("anon_event", call("ImpersonateAnonymousToken", {ev}));
    set_last_error(0xDEAD);
    expect_line("inpc_event", call("ImpersonateNamedPipeClient", {ev}));
    set_last_error(0xDEAD);
    expect_line("inpc_badhandle", call("ImpersonateNamedPipeClient", {0x1234}));
    // After the server has read (inpc_default_after_read) the thread holds the
    // client's identity — the capsule user — at SecurityImpersonation.
    const Handle pipe = handles.allocate(HandleType::Pipe, nullptr);
    set_last_error(0xDEAD);
    expect_line("inpc_default_after_read", call("ImpersonateNamedPipeClient", {pipe}));
    ASSERT_EQ(call("OpenThreadToken", {pseudo(-2), 0x8, 1, t}), 1u);
    const auto pid = stubs.token_identity(static_cast<Handle>(read_ptr(t)));
    EXPECT_EQ(pid.user, user_sid);   // sameUser=1
    EXPECT_EQ(pid.impersonation_level, 2u);
    (void)call("RevertToSelf", {});
}

// ---------------------------------------------------------------------------
// Classic ETW with no session
// ---------------------------------------------------------------------------

TEST_P(S5TokenTest, EtwControllerAndConsumerMatchWindows) {
    load("advapi_etw_probe");
    EXPECT_EQ(stdcall_bytes("ControlTraceW"), 20u);
    EXPECT_EQ(stdcall_bytes("ControlTraceA"), 20u);
    EXPECT_EQ(stdcall_bytes("StopTraceW"), 16u);
    EXPECT_EQ(stdcall_bytes("OpenTraceW"), 4u);
    EXPECT_EQ(stdcall_bytes("OpenTraceA"), 4u);
    EXPECT_EQ(stdcall_bytes("EnableTrace"), 24u);
    EXPECT_EQ(stdcall_bytes("TraceMessage"), 0u);
    EXPECT_EQ(stdcall_bytes("TraceMessageVa"), 24u);
    EXPECT_EQ(stdcall_bytes("TraceEvent"), 12u);
    EXPECT_EQ(stdcall_bytes("GetTraceLoggerHandle"), 4u);
    const GuestAddr props = kData + 0x10000;
    const GuestAddr name = wstr(kData + 0x9000, "VitreoProbeNoSuchSession");
    const u32 base = 0x78, full = 0x78 + 4096;
    auto make_props = [&](u32 size, u32 name_off, u32 file_off) {
        for (u32 i = 0; i < full; i += 4) (void)memory.write_u32(props + i, 0);
        (void)memory.write_u32(props, size);
        (void)memory.write_u32(props + 44, 0x20000);   // WNODE_FLAG_TRACED_GUID
        (void)memory.write_u32(props + (is64 ? 116 : 112), name_off);
        (void)memory.write_u32(props + (is64 ? 112 : 108), file_off);
    };
    // A 64-bit TRACEHANDLE is one register on x64, two stack slots on x86.
    auto ctw = [&](const char* tag, u64 id, GuestAddr n, bool with_props, u32 code) {
        const std::string l = line_for(lines, tag);
        set_last_error(0xDEAD);
        const u64 r = is64 ? call("ControlTraceW", {id, n, with_props ? props : 0, code})
                           : call("ControlTraceW", {id & 0xFFFFFFFF, id >> 32, n, with_props ? props : 0, code});
        EXPECT_EQ(r, *field(l, "ret")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
    };
    make_props(full, base, 0);
    ctw("ctw_query_name", 0, name, true, 0);
    ctw("ctw_stop_name", 0, name, true, 1);
    ctw("ctw_flush_name", 0, name, true, 3);
    ctw("ctw_update_name", 0, name, true, 2);
    ctw("ctw_code_99", 0, name, true, 99);
    ctw("ctw_code_5", 0, name, true, 5);
    ctw("ctw_null_props", 0, name, false, 0);
    ctw("ctw_null_both", 0, 0, true, 0);
    ctw("ctw_bad_handle", 0x1234, 0, true, 0);
    ctw("ctw_bad_handle_and_name", 0x1234, name, true, 0);
    make_props(base - 1, base, 0);
    ctw("ctw_size_small", 0, name, true, 0);
    make_props(0, base, 0);
    ctw("ctw_size_zero", 0, name, true, 0);
    make_props(base, 0, 0);
    ctw("ctw_size_exact_noname", 0, name, true, 0);
    make_props(base, base, 0);
    ctw("ctw_size_exact_nameoff", 0, name, true, 0);
    make_props(full, full + 8, 0);
    ctw("ctw_nameoff_beyond", 0, name, true, 0);
    make_props(full, 8, 0);
    ctw("ctw_nameoff_inside", 0, name, true, 0);
    make_props(full, base, full + 8);
    ctw("ctw_fileoff_beyond", 0, name, true, 0);
    make_props(full, base, 0);
    const u8 system_trace[16] = {0xad, 0x4a, 0x81, 0x9e, 0x04, 0x32, 0xd2, 0x11,
                                 0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39};
    (void)memory.write_bytes(props + 24, system_trace, 16);
    ctw("ctw_systemguid_wrongname", 0, name, true, 0);

    auto stop = [&](const char* tag, u64 id, GuestAddr n, bool with_props) {
        make_props(full, base, 0);
        const std::string l = line_for(lines, tag);
        set_last_error(0xDEAD);
        const u64 r = is64 ? call("StopTraceW", {id, n, with_props ? props : 0})
                           : call("StopTraceW", {id & 0xFFFFFFFF, id >> 32, n, with_props ? props : 0});
        EXPECT_EQ(r, *field(l, "ret")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
    };
    stop("stopw_name", 0, name, true);
    stop("stopw_handle", 0x1234, 0, true);
    stop("stopw_none", 0, 0, true);
    stop("stopw_nullprops", 0, name, false);

    // OpenTraceW: the result is 64 bits (EDX:EAX on x86).
    auto open_trace = [&](GuestAddr logfile) {
        set_last_error(0xDEAD);
        const u64 lo = call("OpenTraceW", {logfile});
        return is64 ? lo : (lo | ((cpu.gpr.rdx & 0xFFFFFFFFull) << 32));
    };
    EXPECT_EQ(open_trace(0), *field(line_for(lines, "openw_null"), "ret"));
    EXPECT_EQ(get_last_error(), *field(line_for(lines, "openw_null"), "gle"));
    const GuestAddr lf = kData + 0xA000;
    for (u32 i = 0; i < 0x200; i += 4) (void)memory.write_u32(lf + i, 0);
    EXPECT_EQ(open_trace(lf), *field(line_for(lines, "openw_empty"), "ret"));
    EXPECT_EQ(get_last_error(), *field(line_for(lines, "openw_empty"), "gle"));
    write_ptr(lf + (is64 ? 8 : 4), name);                     // LoggerName
    (void)memory.write_u32(lf + (is64 ? 28 : 20), 0x10000100); // REAL_TIME | EVENT_RECORD
    const u64 h = open_trace(lf);
    const std::string rt = line_for(lines, "openw_realtime_missing");
    EXPECT_EQ(h != 0 && h != (is64 ? ~0ull : 0xFFFFFFFFull), *field(rt, "handleValid") != 0);
    EXPECT_EQ(get_last_error(), *field(rt, "gle"));
    const GuestAddr hs = kData + 0xA400;
    (void)memory.write_u64(hs, h);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("ProcessTrace", {hs, 1, 0, 0}), *field(line_for(lines, "  processtrace_realtime_missing"), "ret"));
    EXPECT_EQ(get_last_error(), *field(line_for(lines, "  processtrace_realtime_missing"), "gle"));
    auto close_trace = [&](u64 v) {
        return is64 ? call("CloseTrace", {v}) : call("CloseTrace", {v & 0xFFFFFFFF, v >> 32});
    };
    EXPECT_EQ(close_trace(h), *field(line_for(lines, "  closetrace_realtime"), "ret"));
    EXPECT_EQ(close_trace(h), *field(line_for(lines, "  closetrace_again"), "ret"));
    set_last_error(0xDEAD);
    EXPECT_EQ(call("ProcessTrace", {0, 0, 0, 0}), *field(line_for(lines, "processtrace_null"), "ret"));
    (void)memory.write_u64(hs, 0x1234);
    EXPECT_EQ(call("ProcessTrace", {hs, 1, 0, 0}), *field(line_for(lines, "processtrace_bad"), "ret"));
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // EnableTrace / TraceMessage / TraceEvent / GetTraceLoggerHandle.
    const GuestAddr guid = kData + 0xA800;
    auto enable = [&](const char* tag, u32 en, GuestAddr g, u64 th) {
        const std::string l = line_for(lines, tag);
        set_last_error(0xDEAD);
        const u64 r = is64 ? call("EnableTrace", {en, 0, 5, g, th})
                           : call("EnableTrace", {en, 0, 5, g, th & 0xFFFFFFFF, th >> 32});
        EXPECT_EQ(r, *field(l, "ret")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
    };
    enable("enabletrace_badhandle", 1, guid, 0x1234);
    enable("enabletrace_nullguid", 1, 0, 0x1234);
    enable("enabletrace_zerohandle", 1, guid, 0);
    enable("enabletrace_disable_bad", 0, guid, 0x1234);
    enable("enabletrace_minus1", 1, guid, ~0ull);
    set_last_error(0xDEAD);
    // TRACE_MESSAGE_SEQUENCE; cdecl, the TRACEHANDLE is two slots on x86.
    EXPECT_EQ(is64 ? call("TraceMessage", {0x1234, 1, guid, 1}) : call("TraceMessage", {0x1234, 0, 1, guid, 1}),
              *field(line_for(lines, "tracemessage_bad"), "ret"));
    EXPECT_EQ(get_last_error(), 0xDEADu);
    const GuestAddr header = kData + 0xAC00;
    EXPECT_EQ(is64 ? call("TraceEvent", {0x1234, header}) : call("TraceEvent", {0x1234, 0, header}),
              *field(line_for(lines, "traceevent_bad"), "ret"));
    EXPECT_EQ(is64 ? call("TraceEvent", {0x1234, 0}) : call("TraceEvent", {0x1234, 0, 0}),
              *field(line_for(lines, "traceevent_nullheader"), "ret"));
    EXPECT_EQ(get_last_error(), 0xDEADu);
    const GuestAddr wnode = kData + 0xB000;
    (void)memory.write_u64(wnode + 8, 0x1234);
    u64 lh = call("GetTraceLoggerHandle", {wnode});
    if (!is64) lh |= (cpu.gpr.rdx & 0xFFFFFFFFull) << 32;
    EXPECT_EQ(lh, *field(line_for(lines, "gettraceloggerhandle_bad"), "ret"));
    EXPECT_EQ(get_last_error(), *field(line_for(lines, "gettraceloggerhandle_bad"), "gle"));
    (void)memory.write_u64(wnode + 8, 0);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("GetTraceLoggerHandle", {wnode}), *field(line_for(lines, "gettraceloggerhandle_zero"), "ret"));
    EXPECT_EQ(get_last_error(), 0xDEADu);
    set_last_error(0xDEAD);
    EXPECT_EQ(is64 ? call("GetTraceEnableLevel", {0x1234}) : call("GetTraceEnableLevel", {0x1234, 0}), 0u);
    EXPECT_EQ(get_last_error(), *field(line_for(lines, "gettraceenablelevel_bad"), "gle"));
}

// ---------------------------------------------------------------------------
// CreateWellKnownSid / IsWellKnownSid / LookupPrivilegeValue / Name
// ---------------------------------------------------------------------------

TEST_P(S5TokenTest, WellKnownSidTableMatchesWindows) {
    load("advapi_wksid_probe");
    const GuestAddr dom = put_sid("S-1-5-21-111-222-333");
    const GuestAddr sid = kData + 0x9000, cb = kData + 0x9100;
    usize checked = 0;
    for (usize i = 0; i + 2 < lines.size(); ++i) {
        if (lines[i].rfind("wks t=", 0) != 0) continue;
        const u32 t = static_cast<u32>(*field(lines[i], "t"));
        const std::string rows[3] = {lines[i], lines[i + 1], lines[i + 2]};
        const GuestAddr domains[3] = {0, dom, dom};
        for (int k = 0; k < 3; ++k) {
            const std::string& l = rows[k];
            (void)memory.write_u32(cb, k == 2 ? 0 : 0x44);
            set_last_error(0xDEAD);
            const u64 ok = call("CreateWellKnownSid", {t, domains[k], k == 2 ? 0 : sid, cb});
            EXPECT_EQ(ok, *field(l, "ok")) << "type " << t << " row " << k;
            EXPECT_EQ(get_last_error(), *field(l, "gle")) << "type " << t << " row " << k;
            EXPECT_EQ(memory.read_u32(cb).value(), *field(l, "cb")) << "type " << t << " row " << k;
            if (ok) {
                const auto want = sid_bytes(*token(l, "sid"));
                std::vector<u8> got(want.size());
                (void)memory.read_bytes(sid, got.data(), got.size());
                EXPECT_EQ(got, want) << "type " << t << " row " << k;
                EXPECT_EQ(call("IsWellKnownSid", {sid, t}), *field(l, "isWk")) << "type " << t;
            }
        }
        ++checked;
    }
    EXPECT_EQ(checked, 126u);
    // world_cb11 / world_nullsid_cb100 / world_nullcb / admin_domain*.
    (void)memory.write_u32(cb, 11);
    EXPECT_EQ(call("CreateWellKnownSid", {1, 0, sid, cb}), 0u);
    EXPECT_EQ(get_last_error(), 0x7Au);
    EXPECT_EQ(memory.read_u32(cb).value(), 12u);
    (void)memory.write_u32(cb, 100);
    EXPECT_EQ(call("CreateWellKnownSid", {1, 0, 0, cb}), 0u);
    EXPECT_EQ(get_last_error(), 0x57u);
    EXPECT_EQ(memory.read_u32(cb).value(), 12u);
    EXPECT_EQ(call("CreateWellKnownSid", {1, 0, sid, 0}), 0u);
    EXPECT_EQ(get_last_error(), 0x57u);
    auto admin = [&](const char* tag, GuestAddr domain) {
        const std::string l = line_for(lines, tag);
        (void)memory.write_u32(cb, 0x44);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("CreateWellKnownSid", {38, domain, sid, cb}), *field(l, "ok")) << tag;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << tag;
        EXPECT_EQ(memory.read_u32(cb).value(), *field(l, "cb")) << tag;
    };
    admin("admin_notdomain", put_sid("S-1-5-32"));
    const u8 bad[12] = {9, 1, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0};   // revision 9
    (void)memory.write_bytes(kData + 0x9800, bad, sizeof(bad));
    admin("admin_baddomain", kData + 0x9800);
    admin("admin_domain14", put_sid("S-1-5-21-1-2-3-4-5-6-7-8-9-10-11-12-13-14"));
    admin("admin_domain15", put_sid("S-1-5-21-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15"));
    for (const auto& l : lines) {
        if (l.rfind("admin_domain \"", 0) != 0) continue;
        const std::string d = l.substr(14, l.find('"', 14) - 14);
        (void)memory.write_u32(cb, 0x44);
        const u64 ok = call("CreateWellKnownSid", {38, put_sid(d), sid, cb});
        EXPECT_EQ(ok, *field(l, "ok")) << d;
        if (ok) EXPECT_EQ(memory.read_u32(cb).value(), *field(l, "cb")) << d;
    }
    EXPECT_EQ(call("IsWellKnownSid", {put_sid("S-1-5-21-111-222-333-500"), 38}), 1u);
    EXPECT_EQ(call("IsWellKnownSid", {put_sid("S-1-5-21-111-222-333-500"), 1}), 0u);
}

TEST_P(S5TokenTest, PrivilegeTableMatchesWindows) {
    load("advapi_wksid_probe");
    const GuestAddr luid = kData + 0x9200;
    usize checked = 0;
    for (const auto& l : lines) {
        if (l.rfind("lpv \"", 0) != 0) continue;
        const std::string name = l.substr(5, l.rfind("\" ok=") - 5);
        (void)memory.write_u64(luid, 0x0000555500005555ull);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("LookupPrivilegeValueW", {0, wstr(kData + 0x9300, name), luid}), *field(l, "ok")) << name;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << name;
        EXPECT_EQ(memory.read_u32(luid).value(), *field(l, "lo")) << name;
        EXPECT_EQ(memory.read_u32(luid + 4).value(), *field(l, "hi")) << name;
        (void)memory.write_u64(luid, 0x0000555500005555ull);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("LookupPrivilegeValueA", {0, astr(kData + 0x9300, name), luid}), *field(l, "aOk")) << name;
        EXPECT_EQ(get_last_error(), *field(l, "aGle")) << name;
        EXPECT_EQ(memory.read_u32(luid).value(), *field(l, "aLo")) << name;
        ++checked;
    }
    EXPECT_EQ(checked, 45u);
    for (const auto& l : lines) {
        if (l.rfind("lpv_system \"", 0) != 0) continue;
        std::string sys = l.substr(12, l.find('"', 12) - 12);
        (void)memory.write_u64(luid, 0x14);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("LookupPrivilegeValueW", {wstr(kData + 0x9400, sys), wstr(kData + 0x9300, "SeDebugPrivilege"), luid}),
                  *field(l, "ok")) << sys;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << sys;
    }
    const GuestAddr name = kData + 0x9500, cch = kData + 0x9600;
    for (const auto& l : lines) {
        if (l.rfind("lpn luid=", 0) != 0) continue;
        (void)memory.write_u64(luid, *field(l, "luid"));
        (void)memory.write_u32(cch, 64);
        set_last_error(0xDEAD);
        const u64 ok = call("LookupPrivilegeNameW", {0, luid, name, cch});
        EXPECT_EQ(ok, *field(l, "ok")) << l;
        EXPECT_EQ(get_last_error(), *field(l, "gle")) << l;
        EXPECT_EQ(memory.read_u32(cch).value(), *field(l, "cch")) << l;
        if (ok) EXPECT_EQ(read_w(name), *token(l, "name")) << l;
    }
    (void)memory.write_u64(luid, 20);
    (void)memory.write_u32(cch, 16);
    EXPECT_EQ(call("LookupPrivilegeNameW", {0, luid, name, cch}), 0u);   // lpn_exact
    EXPECT_EQ(get_last_error(), 0x7Au);
    EXPECT_EQ(memory.read_u32(cch).value(), 0x11u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, S5TokenTest, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("x64") : std::string("x86");
                         });

} // namespace
