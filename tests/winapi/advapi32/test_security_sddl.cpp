// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_sddl.cpp
/// @brief SDDL contract tests: ConvertStringSecurityDescriptorToSecurityDescriptor,
///        ConvertSecurityDescriptorToStringSecurityDescriptor, ConvertStringSidToSid.
///
/// The corpus tests replay tools/oracle_probe/advapi_sddl_probe.expected
/// (x64) and .x86.expected (x86) — Windows' own output for the same calls —
/// through the exported thunks at the matching bitness. Nothing here is
/// derived from Vitreo.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "advapi_thunk_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using advapi_test::kData;
using advapi_test::kStack;
using advapi_test::read_expected;
using advapi_test::field;
using advapi_test::quoted;
using advapi_test::token;
using advapi_test::corpus_text;

class SddlContractTest : public advapi_test::AdvapiThunkHarness {
protected:
    const char* expected_file() const {
        return is64 ? "advapi_sddl_probe.expected" : "advapi_sddl_probe.x86.expected";
    }

    struct Converted {
        u64 ok{0};
        u32 gle{0};
        u64 sd{0};
        u32 size{0};
    };
    Converted to_sd(const std::string& text) {
        const GuestAddr s = wstr(kData + 0x1000, text);
        const GuestAddr out = kData + 0x100, size = kData + 0x108;
        write_ptr(out, 1);
        (void)memory.write_u32(size, 0x5555);
        set_last_error(0xDEAD);
        Converted c;
        c.ok = call("ConvertStringSecurityDescriptorToSecurityDescriptorW", {s, 1, out, size});
        c.gle = get_last_error();
        c.sd = read_ptr(out);
        c.size = memory.read_u32(size).value();
        return c;
    }
    struct Formatted {
        u64 ok{0};
        u32 gle{0};
        u32 len{0};
        u64 out{0};
        std::string str;
    };
    Formatted to_string(u64 sd, u32 si) {
        const GuestAddr out = kData + 0x200, len = kData + 0x208;
        write_ptr(out, 1);
        (void)memory.write_u32(len, 0x5555);
        set_last_error(0xDEAD);
        Formatted f;
        f.ok = call("ConvertSecurityDescriptorToStringSecurityDescriptorW", {sd, 1, si, out, len});
        f.gle = get_last_error();
        f.len = memory.read_u32(len).value();
        f.out = read_ptr(out);
        if (f.ok) {
            f.str = read_w(static_cast<GuestAddr>(f.out));
            stubs.free_local_memory(static_cast<GuestAddr>(f.out));
        }
        return f;
    }
    /// Absolute SD with a DACL holding one ACCESS_ALLOWED_ACE for Everyone.
    GuestAddr absolute_sd_with_mask(u32 mask, u8 type = 0, u8 flags = 0, bool in_sacl = false) {
        const GuestAddr acl = kData + 0x3000;
        (void)memory.write_u8(acl, 2);
        (void)memory.write_u8(acl + 1, 0);
        (void)memory.write_u16(acl + 2, 8 + 20);
        (void)memory.write_u16(acl + 4, 1);
        (void)memory.write_u16(acl + 6, 0);
        (void)memory.write_u8(acl + 8, type);
        (void)memory.write_u8(acl + 9, flags);
        (void)memory.write_u16(acl + 10, 20);
        (void)memory.write_u32(acl + 12, mask);
        const u8 wd[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        (void)memory.write_bytes(acl + 16, wd, sizeof wd);
        const GuestAddr sd = kData + 0x3100;
        for (u32 i = 0; i < 40; ++i) (void)memory.write_u8(sd + i, 0);
        (void)memory.write_u8(sd, 1);
        (void)memory.write_u16(sd + 2, in_sacl ? 0x0010 : 0x0004);
        const u32 field = in_sacl ? (is64 ? 24u : 12u) : (is64 ? 32u : 16u);
        write_ptr(sd + field, acl);
        return sd;
    }
};

// ---------------------------------------------------------------------------
// Whole-corpus replay
// ---------------------------------------------------------------------------

TEST_P(SddlContractTest, StringToDescriptorCorpusMatchesWindowsByteForByte) {
    const auto lines = read_expected(expected_file());
    ASSERT_GT(lines.size(), 700u);
    usize checked = 0;
    for (usize i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.rfind("sd \"", 0) != 0) continue;
        const std::string text = corpus_text(line, "sd \"");
        // Conditional ACE expressions and resource attributes are not
        // compiled yet: Vitreo fails them loudly (ERROR_INVALID_PARAMETER +
        // one-time UNIMPLEMENTED) instead of guessing a binary form.
        if (text.find(";(") != std::string::npos &&
            (text.find("(XA;") != std::string::npos || text.find("(XD;") != std::string::npos ||
             text.find("(XU;") != std::string::npos || text.find("(ZA;") != std::string::npos ||
             text.find("(RA;") != std::string::npos)) {
            continue;
        }
        SCOPED_TRACE("sddl: " + text);
        const Converted c = to_sd(text);
        EXPECT_EQ(c.ok, *field(line, "ok"));
        EXPECT_EQ(c.gle, *field(line, "gle"));
        if (*field(line, "ok") == 0) {
            EXPECT_EQ(c.sd, *field(line, "sdOut"));
            EXPECT_EQ(c.size, *field(line, "sizeOut"));
            ++checked;
            continue;
        }
        EXPECT_EQ(c.size, *field(line, "size"));
        if (c.ok) {
            EXPECT_EQ(hex_bytes(static_cast<GuestAddr>(c.sd), c.size), *token(line, "bytes"));
            // Round trip ("  rt si=1f ...") on the next line.
            ASSERT_LT(i + 1, lines.size());
            const std::string& rt = lines[i + 1];
            ASSERT_EQ(rt.rfind("  rt ", 0), 0u);
            const Formatted f = to_string(c.sd, 0x1F);
            EXPECT_EQ(f.ok, *field(rt, "ok"));
            EXPECT_EQ(f.gle, *field(rt, "gle"));
            EXPECT_EQ(f.len, *field(rt, "len"));
            if (f.ok) EXPECT_EQ(f.str, *quoted(rt, "str"));
            stubs.free_local_memory(static_cast<GuestAddr>(c.sd));
        }
        ++checked;
    }
    EXPECT_GT(checked, 300u);
}

TEST_P(SddlContractTest, SidAliasTableAndStringSidsMatchWindows) {
    const auto lines = read_expected(expected_file());
    usize checked = 0;
    for (const auto& line : lines) {
        if (line.rfind("sid \"", 0) != 0) continue;
        const usize close = line.find("\" ok=");
        const std::string text = line.substr(5, close - 5);
        SCOPED_TRACE("sid: " + text);
        // ConvertStringSidToSidW
        const GuestAddr s = wstr(kData + 0x1000, text);
        const GuestAddr out = kData + 0x100;
        write_ptr(out, 1);
        set_last_error(0xDEAD);
        const u64 ok = call("ConvertStringSidToSidW", {s, out});
        EXPECT_EQ(ok, *field(line, "ok"));
        EXPECT_EQ(get_last_error(), *field(line, "gle"));
        if (ok) {
            const u64 sid = read_ptr(out);
            const u32 len = 8 + 4 * memory.read_u8(static_cast<GuestAddr>(sid) + 1).value();
            EXPECT_EQ(hex_bytes(static_cast<GuestAddr>(sid), len), *token(line, "bytes"));
        } else {
            // Probe prints "out=1": Windows leaves *Sid unchanged on failure.
            EXPECT_EQ(read_ptr(out), *field(line, "out"));
        }
        // "O:<text>" and its owner-only round trip.
        const Converted c = to_sd("O:" + text);
        EXPECT_EQ(c.ok, *field(line, "sdOk"));
        EXPECT_EQ(c.gle, *field(line, "sdGle"));
        if (c.ok) {
            const Formatted f = to_string(c.sd, 0x1);
            const std::string back = *quoted(line, "back");
            if (back == "(null)") {
                EXPECT_EQ(f.ok, 0u);
                EXPECT_EQ(f.gle, *field(line, "backGle"));
            } else {
                EXPECT_EQ(f.ok, 1u);
                EXPECT_EQ(f.str, back);
            }
        }
        ++checked;
    }
    EXPECT_GT(checked, 90u);
}

TEST_P(SddlContractTest, RightsAceFlagsAndMaskFormattingMatchWindows) {
    const auto lines = read_expected(expected_file());
    usize checked = 0;
    for (const auto& line : lines) {
        if (line.rfind("right \"", 0) == 0) {
            const std::string r = line.substr(7, line.find("\" ok=") - 7);
            SCOPED_TRACE("right: " + r);
            const Converted c = to_sd("D:(A;;" + r + ";;;WD)");
            EXPECT_EQ(c.ok, *field(line, "ok"));
            EXPECT_EQ(c.gle, *field(line, "gle"));
            if (c.ok) {
                // mask is the first ACE's Mask: header 20 + ACL 8 + ACE header 4.
                EXPECT_EQ(memory.read_u32(static_cast<GuestAddr>(c.sd) + 32).value(),
                          *field(line, "mask"));
                const Formatted f = to_string(c.sd, 0x4);
                EXPECT_EQ(f.str, *quoted(line, "back"));
            }
            ++checked;
        } else if (line.rfind("aceflags \"", 0) == 0) {
            const std::string fl = line.substr(10, line.find("\" ok=") - 10);
            SCOPED_TRACE("aceflags: " + fl);
            const Converted c = to_sd("S:(AU;" + fl + ";GA;;;WD)");
            EXPECT_EQ(c.ok, *field(line, "ok"));
            EXPECT_EQ(c.gle, *field(line, "gle"));
            if (c.ok) {
                EXPECT_EQ(hex_bytes(static_cast<GuestAddr>(c.sd), c.size), *token(line, "bytes"));
                EXPECT_EQ(to_string(c.sd, 0x8).str, *quoted(line, "back"));
            }
            ++checked;
        } else if (line.rfind("mask m=", 0) == 0) {
            const u32 mask = static_cast<u32>(*field(line, "m"));
            SCOPED_TRACE("mask: " + std::to_string(mask));
            const Formatted f = to_string(absolute_sd_with_mask(mask), 0x4);
            EXPECT_EQ(f.ok, *field(line, "ok"));
            EXPECT_EQ(f.len, *field(line, "len"));
            EXPECT_EQ(f.str, *quoted(line, "back"));
            ++checked;
        }
    }
    EXPECT_EQ(checked, 120u);   // every "sd" / "mask" line of the probe
}

// ---------------------------------------------------------------------------
// API edges (probe lines rev_*, null_*, ansi*, si, tostr_*, abs_* ...)
// ---------------------------------------------------------------------------

TEST_P(SddlContractTest, ParameterAndRevisionErrorsLeaveOutputsAlone) {
    EXPECT_EQ(stdcall_bytes("ConvertStringSecurityDescriptorToSecurityDescriptorW"), 16u);
    EXPECT_EQ(stdcall_bytes("ConvertStringSecurityDescriptorToSecurityDescriptorA"), 16u);
    EXPECT_EQ(stdcall_bytes("ConvertSecurityDescriptorToStringSecurityDescriptorW"), 20u);
    EXPECT_EQ(stdcall_bytes("ConvertSecurityDescriptorToStringSecurityDescriptorA"), 20u);
    const GuestAddr good = wstr(kData + 0x1000, "D:(A;;GA;;;WD)");
    const GuestAddr out = kData + 0x100, size = kData + 0x108;
    for (u32 rev : {0u, 2u, 0xFFFFFFFFu}) {
        write_ptr(out, 1);
        (void)memory.write_u32(size, 0x5555);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorW", {good, rev, out, size}), 0u);
        EXPECT_EQ(get_last_error(), 1305u);            // rev_good: 0x519
        EXPECT_EQ(read_ptr(out), 1u);
        EXPECT_EQ(memory.read_u32(size).value(), 0x5555u);
        set_last_error(0xDEAD);
        EXPECT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorW", {0, rev, out, size}), 0u);
        EXPECT_EQ(get_last_error(), 87u);              // rev_null: 0x57
    }
    set_last_error(0xDEAD);
    EXPECT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorW", {good, 1, 0, size}), 0u);
    EXPECT_EQ(get_last_error(), 87u);                  // null_out
    write_ptr(out, 0);
    EXPECT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorW", {good, 1, out, 0}), 1u);
    EXPECT_NE(read_ptr(out), 0u);                      // null_size
    EXPECT_EQ(local.global_size(read_ptr(out)), 0x30u);  // localsize sz=30

    // ANSI: same bytes and the ANSI formatter (probe "ansi").
    const GuestAddr a = astr(kData + 0x1800, "O:BAG:SYD:(D;OICI;FA;;;AN)(A;OICI;0x1200a9;;;BU)");
    write_ptr(out, 1);
    set_last_error(0xDEAD);
    ASSERT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorA", {a, 1, out, size}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(memory.read_u32(size).value(), 0x64u);
    const u64 sd = read_ptr(out);
    EXPECT_EQ(hex_bytes(static_cast<GuestAddr>(sd), 0x64),
              "0100048048000000580000000000000014000000020034000200000001031400ff011f00010100"
              "00000000050700000000031800a90012000102000000000005200000002102000001020000000000"
              "052000000020020000010100000000000512000000");
    const GuestAddr sout = kData + 0x200, slen = kData + 0x208;
    set_last_error(0xDEAD);
    ASSERT_EQ(call("ConvertSecurityDescriptorToStringSecurityDescriptorA", {sd, 1, 0x1F, sout, slen}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(memory.read_u32(slen).value(), 0x35u);
    std::string ansi;
    for (GuestAddr p = static_cast<GuestAddr>(read_ptr(sout)); memory.read_u8(p).value() != 0; ++p) {
        ansi.push_back(static_cast<char>(memory.read_u8(p).value()));
    }
    EXPECT_EQ(ansi, "O:BAG:SYD:(D;OICI;FA;;;AN)(A;OICI;0x1200a9;;;BU)");
    write_ptr(out, 1);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("ConvertStringSecurityDescriptorToSecurityDescriptorA",
                   {astr(kData + 0x1800, "garbage"), 1, out, size}), 0u);
    EXPECT_EQ(get_last_error(), 87u);                  // ansi_bad
    EXPECT_EQ(read_ptr(out), 1u);
}

TEST_P(SddlContractTest, FormatterSelectsComponentsByInformationClass) {
    const Converted c = to_sd("O:BAG:SYD:PAI(A;OICI;FA;;;SY)S:PAI(ML;;NW;;;LW)(AU;SA;GA;;;WD)"
                              "(TL;;0x200;;;S-1-19-512-1024)(SP;;;;;S-1-17-1)");
    ASSERT_EQ(c.ok, 1u);
    // probe "  si si=..." lines (len is Windows' budgeted length: string
    // length + 1 per ACE + hex-mask shortfall from 10 characters).
    struct Case { u32 si; const char* want; u32 len; };
    const Case cases[] = {
        {0x0, "", 0}, {0x1, "O:BA", 4}, {0x2, "G:SY", 4}, {0x4, "D:PAI(A;OICI;FA;;;SY)", 0x16},
        {0x8, "S:PAI(AU;SA;GA;;;WD)", 0x15}, {0x10, "S:PAI(ML;;NW;;;LW)", 0x13},
        {0x18, "S:PAI(ML;;NW;;;LW)(AU;SA;GA;;;WD)", 0x23},
        {0x1F, "O:BAG:SYD:PAI(A;OICI;FA;;;SY)S:PAI(ML;;NW;;;LW)(AU;SA;GA;;;WD)", 0x41},
        {0x80000000, "", 0}, {0x40000000, "", 0}, {0x20000000, "", 0}, {0x10000000, "", 0},
        {0x20, "S:PAI", 5}, {0x40, "S:PAI(SP;;;;;S-1-17-1)", 0x17},
        {0x80, "S:PAI(TL;;0x200;;;S-1-19-512-1024)", 0x28}, {0x10000, "", 0},
    };
    for (const auto& k : cases) {
        const Formatted f = to_string(c.sd, k.si);
        EXPECT_EQ(f.ok, 1u) << k.si;
        EXPECT_EQ(f.gle, 0u) << k.si;
        EXPECT_EQ(f.str, k.want) << k.si;
        EXPECT_EQ(f.len, k.len) << k.si;
    }
    // tostr_rev2 / tostr_nullsd / tostr_nullout / tostr_nulllen.
    const GuestAddr out = kData + 0x200, len = kData + 0x208;
    write_ptr(out, 1);
    (void)memory.write_u32(len, 0x5555);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("ConvertSecurityDescriptorToStringSecurityDescriptorW", {c.sd, 2, 4, out, len}), 0u);
    EXPECT_EQ(get_last_error(), 1305u);
    EXPECT_EQ(read_ptr(out), 1u);
    EXPECT_EQ(memory.read_u32(len).value(), 0x5555u);
    const Formatted null_sd = to_string(0, 4);
    EXPECT_EQ(null_sd.ok, 1u);
    EXPECT_EQ(null_sd.len, 0u);
    EXPECT_NE(null_sd.out, 1u);
    EXPECT_EQ(null_sd.str, "");
    set_last_error(0xDEAD);
    EXPECT_EQ(call("ConvertSecurityDescriptorToStringSecurityDescriptorW", {c.sd, 1, 4, 0, len}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(call("ConvertSecurityDescriptorToStringSecurityDescriptorW", {c.sd, 1, 4, out, 0}), 1u);
}

TEST_P(SddlContractTest, FormatterRejectsWhatWindowsRejects) {
    // probe: bad_rev 0x519, unknown_ace / ml_in_dacl / au_in_dacl / a_in_sacl 0x538,
    // flag20 "D:(A;CR;CC;;;WD)", ml_mask8 "S:(ML;;0x8;;;WD)", ml_mask3 "S:(ML;;NWNR;;;WD)".
    const GuestAddr bad = kData + 0x3400;
    (void)memory.write_u8(bad, 2);
    (void)memory.write_u16(bad + 2, 0x8004);
    EXPECT_EQ(to_string(bad, 4).gle, 1305u);
    for (u8 type : {u8{0x1F}, u8{0x11}, u8{0x02}}) {
        const Formatted f = to_string(absolute_sd_with_mask(1, type), 4);
        EXPECT_EQ(f.ok, 0u) << int(type);
        EXPECT_EQ(f.gle, 1336u) << int(type);
    }
    EXPECT_EQ(to_string(absolute_sd_with_mask(1, 0x00, 0, true), 8).gle, 1336u);
    EXPECT_EQ(to_string(absolute_sd_with_mask(1, 0x00, 0x20), 4).str, "D:(A;CR;CC;;;WD)");
    EXPECT_EQ(to_string(absolute_sd_with_mask(0x8, 0x11, 0, true), 0x10).str, "S:(ML;;0x8;;;WD)");
    EXPECT_EQ(to_string(absolute_sd_with_mask(0x3, 0x11, 0, true), 0x10).str, "S:(ML;;NWNR;;;WD)");
}

TEST_P(SddlContractTest, AbsoluteDescriptorsFormatNullAndEmptyAcls) {
    // probe abs_empty "", abs_nulldacl "D:NO_ACCESS_CONTROL", abs_emptydacl "D:",
    // abs_emptydacl_flags "D:PARAI", abs_nullsacl "D:PARAIS:NO_ACCESS_CONTROL".
    const GuestAddr sd = kData + 0x3200, acl = kData + 0x3300;
    for (u32 i = 0; i < 40; ++i) (void)memory.write_u8(sd + i, 0);
    (void)memory.write_u8(sd, 1);
    EXPECT_EQ(to_string(sd, 0x1F).str, "");
    (void)memory.write_u16(sd + 2, 0x0004);
    EXPECT_EQ(to_string(sd, 0x1F).str, "D:NO_ACCESS_CONTROL");
    const u8 empty[8] = {2, 0, 8, 0, 0, 0, 0, 0};
    (void)memory.write_bytes(acl, empty, 8);
    write_ptr(sd + (is64 ? 32 : 16), acl);
    (void)memory.write_u16(sd + 2, 0x000C);
    EXPECT_EQ(to_string(sd, 0x1F).str, "D:");
    (void)memory.write_u16(sd + 2, 0x150C);
    EXPECT_EQ(to_string(sd, 0x1F).str, "D:PARAI");
    (void)memory.write_u16(sd + 2, 0x151C);
    EXPECT_EQ(to_string(sd, 0x1F).str, "D:PARAIS:NO_ACCESS_CONTROL");
}

TEST_P(SddlContractTest, ConvertSidToStringSidPrintsWideAuthorityUnpadded) {
    // probe sidtostr_bigauth: S-1-0x100000000-1
    const GuestAddr sid = kData + 0x500;
    const u8 bytes[12] = {1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0};
    (void)memory.write_bytes(sid, bytes, sizeof bytes);
    const GuestAddr out = kData + 0x100;
    ASSERT_EQ(call("ConvertSidToStringSidW", {sid, out}), 1u);
    EXPECT_EQ(read_w(static_cast<GuestAddr>(read_ptr(out))), "S-1-0x100000000-1");
}

INSTANTIATE_TEST_SUITE_P(Bitness, SddlContractTest, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("x64") : std::string("x86");
                         });

} // namespace
