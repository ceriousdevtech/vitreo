// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_engine_area.cpp
/// @brief Contract tests for the Windows Installer engine area (sequencer,
/// execution script, session functions), both bitnesses through
/// msi_fixture.hpp.
///
/// Expected values are Windows 11's, from
/// tools/oracle_probe/msi_engine_session_probe.c (msi_engine32/64.expected:
/// x86 and x64 are identical). The package is the probe's own
/// (tests/winapi/msi/fixtures/engine_probe_x86.msi, written by
/// `msi_engine32.exe <path>` on winoracle).

#include "msi_fixture.hpp"

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/system/msi/msi_engine.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

using msi::Engine;
using msi::Session;

std::filesystem::path fixture_path() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "engine_probe_x86.msi";
}

std::vector<u8> fixture_bytes() {
    std::ifstream in(fixture_path(), std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class MsiEngineArea : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        root = std::filesystem::temp_directory_path() /
               ("vitreo_msi_engine_" + std::to_string(::getpid()) + (x64() ? "_64" : "_32"));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::copy_file(fixture_path(), root / "pkg.msi");
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
        write_wide(BUF, u"C:\\pkg.msi");
        ASSERT_EQ(call("MsiOpenPackageW", {I(BUF), I(OUT)}), 0u);
        h = static_cast<u32>(read_int(OUT));
        ASSERT_NE(h, 0u);
    }
    void TearDown() override {
        if (h != 0) (void)mgr.handles().close(h);
        std::filesystem::remove_all(root);
    }

    static constexpr GuestAddr NAME = DATA + 0x8000;
    static constexpr GuestAddr VAL = DATA + 0x9000;
    static constexpr GuestAddr OUTBUF = DATA + 0xA000;
    static constexpr GuestAddr PCCH = DATA + 0xB000;

    std::string get(const std::u16string& name) {
        write_wide(NAME, name);
        write_int(PCCH, 1024);
        EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
        std::u16string out;
        for (u32 i = 0; i < 1024; ++i) {
            const u16 c = memory.read_u16(OUTBUF + i * 2).value();
            if (c == 0) break;
            out.push_back(static_cast<char16_t>(c));
        }
        return std::string(out.begin(), out.end());
    }
    void set(const std::u16string& name, const std::u16string& value) {
        write_wide(NAME, name);
        write_wide(VAL, value);
        EXPECT_EQ(call("MsiSetPropertyW", {I(h), I(NAME), I(VAL)}), 0u);
    }
    u32 cond(const std::u16string& e) {
        write_wide(VAL, e);
        return static_cast<u32>(call("MsiEvaluateConditionW", {I(h), I(VAL)}));
    }
    u32 do_action(const std::u16string& a) {
        write_wide(NAME, a);
        return static_cast<u32>(call("MsiDoActionW", {I(h), I(NAME)}));
    }
    u32 sequence(const std::u16string& t, u32 mode = 0) {
        write_wide(NAME, t);
        return static_cast<u32>(call("MsiSequenceW", {I(h), I(NAME), I(mode)}));
    }
    /// MsiFormatRecordW of a record built in the HandleTable.
    std::string fmt(const std::u16string& tmpl, std::vector<std::optional<std::u16string>> fields = {}) {
        auto rec = std::make_shared<msi::Record>(static_cast<u32>(fields.size()));
        rec->fields[0].kind = msi::Record::Kind::String;
        rec->fields[0].text = tmpl;
        for (usize i = 0; i < fields.size(); ++i) {
            if (!fields[i]) continue;
            rec->fields[i + 1].kind = msi::Record::Kind::String;
            rec->fields[i + 1].text = *fields[i];
        }
        const u32 r = mgr.handles().add(rec);
        write_int(PCCH, 1024);
        EXPECT_EQ(call("MsiFormatRecordW", {I(h), I(r), I(OUTBUF), I(PCCH)}), 0u);
        const u32 n = static_cast<u32>(read_int(PCCH));
        std::string out;
        for (u32 i = 0; i < n; ++i) {
            const u16 c = memory.read_u16(OUTBUF + i * 2).value();
            if (c == 0) out += "\\0";
            else out.push_back(static_cast<char>(c));
        }
        (void)mgr.handles().close(r);
        return out;
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
    u32 h{0};
};

TEST_P(MsiEngineArea, GetPropertyBufferConvention) {
    write_wide(NAME, u"S");
    // probe: "gp nullbuf n0 rc=0 n=3", "gp buf n0 rc=234 n=3 b0=122",
    // "gp buf n3 rc=234 n=3 b=ab", "gp buf n4 rc=0 n=3 b=abc".
    write_int(PCCH, 0);
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(0), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 3);
    write_int(PCCH, 0);
    write_wide(OUTBUF, u"zz");
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(read_int(PCCH), 3);
    EXPECT_EQ(memory.read_u16(OUTBUF).value(), u'z');
    write_int(PCCH, 3);
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(read_int(PCCH), 3);
    EXPECT_EQ(memory.read_u16(OUTBUF + 2).value(), u'b');
    EXPECT_EQ(memory.read_u16(OUTBUF + 4).value(), 0);
    write_int(PCCH, 4);
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 3);
    // "gp missing rc=0 n=0", "gp nullname rc=87", "gp emptyname rc=0 n=0",
    // "gp nullpcch rc=87", "gp h0 rc=6", "gp hbad rc=6".
    write_wide(NAME, u"NoSuch");
    write_int(PCCH, 8);
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 0);
    write_int(PCCH, 8);
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(0), I(OUTBUF), I(PCCH)}), 87u);
    EXPECT_EQ(call("MsiGetPropertyW", {I(h), I(NAME), I(OUTBUF), I(0)}), 87u);
    EXPECT_EQ(call("MsiGetPropertyW", {I(0), I(NAME), I(OUTBUF), I(PCCH)}), 6u);
    EXPECT_EQ(call("MsiGetPropertyW", {I(0x7777), I(NAME), I(OUTBUF), I(PCCH)}), 6u);
    // "gpA n2 rc=234 n=3 b=a".
    ASSERT_TRUE(memory.write_u16(NAME, 0x0053).is_ok());
    write_int(PCCH, 2);
    EXPECT_EQ(call("MsiGetPropertyA", {I(h), I(NAME), I(OUTBUF), I(PCCH)}), 234u);
    EXPECT_EQ(read_int(PCCH), 3);
    EXPECT_EQ(memory.read_u8(OUTBUF).value(), 'a');
    EXPECT_EQ(memory.read_u8(OUTBUF + 1).value(), 0);
}

TEST_P(MsiEngineArea, SetPropertyRules) {
    set(u"NewP", u"v");
    EXPECT_EQ(get(u"NewP"), "v");
    set(u"NewP", u"");
    EXPECT_EQ(get(u"NewP"), "");
    write_wide(NAME, u"S");
    EXPECT_EQ(call("MsiSetPropertyW", {I(h), I(NAME), I(0)}), 0u);  // "sp nullval rc=0" deletes
    EXPECT_EQ(get(u"S"), "");
    write_wide(VAL, u"v");
    EXPECT_EQ(call("MsiSetPropertyW", {I(h), I(0), I(VAL)}), 87u);   // "sp nullname rc=87"
    write_wide(NAME, u"");
    EXPECT_EQ(call("MsiSetPropertyW", {I(h), I(NAME), I(VAL)}), 1627u);  // "sp emptyname rc=1627"
    EXPECT_EQ(call("MsiSetPropertyW", {I(0), I(NAME), I(VAL)}), 6u);
    set(u"s", u"lowercase-s");  // names are case-sensitive
    EXPECT_EQ(get(u"s"), "lowercase-s");
}

TEST_P(MsiEngineArea, ModesAndLanguage) {
    // probe "mode N": 3 ROLLBACKENABLED, 8 CABINET, 13 ZAWENABLED are set.
    for (u32 mode = 0; mode <= 19; ++mode) {
        const bool want = mode == 3 || mode == 8 || mode == 13;
        EXPECT_EQ(call("MsiGetMode", {I(h), I(mode)}), want ? 1u : 0u) << mode;
    }
    EXPECT_EQ(call("MsiGetMode", {I(0), I(0)}), 0u);
    for (u32 mode = 0; mode <= 19; ++mode) {
        const u32 want = (mode == 6 || mode == 7) ? 0u : 5u;
        EXPECT_EQ(call("MsiSetMode", {I(h), I(mode), I(1)}), want) << mode;
    }
    EXPECT_EQ(call("MsiGetMode", {I(h), I(6)}), 1u);
    EXPECT_EQ(call("MsiSetMode", {I(0), I(6), I(1)}), 6u);
    EXPECT_EQ(call("MsiGetLanguage", {I(h)}), 1033u);
    EXPECT_EQ(call("MsiGetLanguage", {I(0)}), 6u);
}

TEST_P(MsiEngineArea, ConditionsMatchWindows) {
    const std::pair<const char16_t*, u32> cases[] = {
        {u"", 2}, {u"   ", 2}, {u"A", 1}, {u"B", 1}, {u"S", 1}, {u"NoSuch", 0}, {u"NOT A", 0},
        {u"not A", 0}, {u"A AND B", 1}, {u"A and B", 1}, {u"A OR B", 1}, {u"A XOR A", 0},
        {u"A EQV B", 1}, {u"A IMP B", 1}, {u"B IMP A", 1}, {u"N > N2", 1}, {u"N > \"9\"", 0},
        {u"N = 10", 1}, {u"N = \"10\"", 1}, {u"N > 9", 1}, {u"S = \"abc\"", 1}, {u"S = abc", 0},
        {u"S ~= \"ABC\"", 1}, {u"S = \"ABC\"", 0}, {u"S < \"abd\"", 1}, {u"S > 5", 0}, {u"S <> 5", 1},
        {u"S = 5", 0}, {u"S >< \"b\"", 1}, {u"S << \"a\"", 1}, {u"S >> \"bc\"", 1}, {u"S ~>< \"B\"", 1},
        {u"S ~<< \"A\"", 1}, {u"S ~>> \"BC\"", 1}, {u"Bits >< 2", 1}, {u"Bits >< 4", 0},
        {u"Bits << 3", 1}, {u"Bits >> 3", 1}, {u"Neg < 0", 1}, {u"Neg = -5", 1}, {u"Big > 1", 1},
        {u"Big = 99999999999", 1}, {u"Hex = 16", 0}, {u"Sp = 5", 0}, {u"Plus = 5", 0},
        {u"V > \"1.2\"", 1}, {u"V >= 1", 0}, {u"NoSuch = \"\"", 1}, {u"NoSuch = 0", 0},
        {u"NoSuch <> 0", 1}, {u"NoSuch < 1", 0}, {u"\"\" = NoSuch", 1}, {u"(A)", 1},
        {u"((A OR B) AND NOT B)", 0}, {u"A = ", 3}, {u"(A", 3}, {u"A)", 3}, {u"A AND", 3},
        {u"\"unterminated", 3}, {u"A == 1", 3}, {u"A !", 3}, {u"1", 1}, {u"0", 0}, {u"-1", 1},
        {u"\"0\"", 1}, {u"\"\"", 0}, {u"\"x\"", 1}, {u"%NOSUCHENVVAR", 0}, {u"$C1", 0},
        {u"$C1 = -1", 0}, {u"?C1", 0}, {u"?C1 = -1", 0}, {u"&F1", 0}, {u"&F1 = -1", 0}, {u"!F1", 0},
        {u"!F1 = -1", 0}, {u"$NoComp = -1", 0}, {u"&NoFeat", 0}, {u"&NoFeat = -1", 0},
        {u"&NoFeat = \"\"", 1}, {u"$C1 = 3", 0}, {u"Installed", 0}, {u"NOT Installed", 1},
        {u"A AND B OR A", 1}, {u"B OR A AND B", 1}, {u"NOT B AND A", 0}, {u"NOT NOT A", 1},
        {u"A=1", 1}, {u"A<>1", 0}, {u"A>=1", 1}, {u"A<=1", 1}, {u"lower", 1}, {u"Lower", 0},
        {u"S><\"\"", 1}, {u"S<<\"\"", 1}, {u"2147483647 > 2147483646", 1}, {u"2147483648 > 1", 1},
        {u"A ~= 1", 1}, {u"U ~= S", 1}, {u"N ~> \"9\"", 0}, {u"A AND (B", 3}, {u"Ref", 1},
        {u"[S]", 3}, {u"S = \"a\"\"b\"", 3}, {u"A or B and B", 1}, {u"A Or B", 1},
    };
    for (const auto& [expr, want] : cases) {
        EXPECT_EQ(cond(expr), want) << std::string(expr, expr + std::char_traits<char16_t>::length(expr));
    }
    EXPECT_EQ(call("MsiEvaluateConditionW", {I(h), I(0)}), 2u);  // "cond null -> 2"
    write_wide(VAL, u"1");
    EXPECT_EQ(call("MsiEvaluateConditionW", {I(0), I(VAL)}), 3u);  // "cond h0 -> 3"
}

TEST_P(MsiEngineArea, FormatRecordMatchesWindows) {
    set(u"s", u"lowercase-s");
    EXPECT_EQ(fmt(u"[S]"), "abc");
    EXPECT_EQ(fmt(u"[NoSuch]x"), "x");
    EXPECT_EQ(fmt(u"[[Ref]]"), "abc");
    EXPECT_EQ(fmt(u"[[Ref2]]"), "S");
    EXPECT_EQ(fmt(u"[[NoSuch]]"), "");
    EXPECT_EQ(fmt(u"[\\[]S[\\]]"), "[S]");
    EXPECT_EQ(fmt(u"[\\abc]"), "a");
    EXPECT_EQ(fmt(u"[\\]"), "[\\]");
    EXPECT_EQ(fmt(u"a[~]b"), "a\\0b");
    EXPECT_EQ(fmt(u"{abc}"), "{abc}");
    EXPECT_EQ(fmt(u"{[S]x}"), "abcx");
    EXPECT_EQ(fmt(u"{[NoSuch]x}"), "");
    EXPECT_EQ(fmt(u"{[S][NoSuch]}"), "");
    EXPECT_EQ(fmt(u"{{[S]}}"), "");
    EXPECT_EQ(fmt(u"{{abc}}"), "");
    EXPECT_EQ(fmt(u"[1]-[2]", {u"one", u"two"}), "one-two");
    EXPECT_EQ(fmt(u"[1]-[2]", {u"one", std::nullopt}), "one-");
    EXPECT_EQ(fmt(u"{[1]-[2]}", {u"one", std::nullopt}), "");
    EXPECT_EQ(fmt(u"{[1]-[2]}", {u"one", u"two"}), "one-two");
    EXPECT_EQ(fmt(u"[3]", {u"one", u"two"}), "");
    EXPECT_EQ(fmt(u"[1]", {u"[S]"}), "abc");
    EXPECT_EQ(fmt(u"[1]", {u"[1]"}), "[1]");
    EXPECT_EQ(fmt(u"[0]", {u"x"}), "[0]");
    EXPECT_EQ(fmt(u"[%NOSUCHENV]"), "");
    EXPECT_EQ(fmt(u"[#NoFile]"), "");
    EXPECT_EQ(fmt(u"[$C1]"), "");
    EXPECT_EQ(fmt(u"[Br]"), "[S]");
    EXPECT_EQ(fmt(u"[Self]"), "[Self]x");
    EXPECT_EQ(fmt(u"a[b"), "a[b");
    EXPECT_EQ(fmt(u"a]b"), "a]b");
    EXPECT_EQ(fmt(u"a{b"), "a{b");
    EXPECT_EQ(fmt(u"a}b"), "a}b");
    EXPECT_EQ(fmt(u"[]"), "");
    EXPECT_EQ(fmt(u"[ S ]"), "");
    EXPECT_EQ(fmt(u"[s]"), "lowercase-s");
    EXPECT_EQ(fmt(u"[[S]"), "[[S]");
    EXPECT_EQ(fmt(u"[S]]"), "abc]");
    EXPECT_EQ(fmt(u"[1]", {u""}), "");
    EXPECT_EQ(fmt(u"{[1]}", {u""}), "");
    EXPECT_EQ(fmt(u"[S][\\[][S]"), "abc[abc");
    EXPECT_EQ(fmt(u"[ProductCode]"), "{6C1F0E4B-5A7D-4E21-9B3C-0D8E2F4A6B19}");

    // A null template: "1: a 2: 7 3: c ", with or without a session.
    auto rec = std::make_shared<msi::Record>(3);
    rec->fields[1].kind = msi::Record::Kind::String;
    rec->fields[1].text = u"a";
    rec->fields[2].kind = msi::Record::Kind::Integer;
    rec->fields[2].integer = 7;
    rec->fields[3].kind = msi::Record::Kind::String;
    rec->fields[3].text = u"c";
    const u32 r = mgr.handles().add(rec);
    write_int(PCCH, 256);
    EXPECT_EQ(call("MsiFormatRecordW", {I(h), I(r), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 15);
    // hInstall 0 leaves properties in place: "[1][S]" -> "a[S]".
    rec->fields[0].kind = msi::Record::Kind::String;
    rec->fields[0].text = u"[1][S]";
    write_int(PCCH, 256);
    EXPECT_EQ(call("MsiFormatRecordW", {I(0), I(r), I(OUTBUF), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 4);
    // Size queries: "fmt sizequery rc=234 n=4", "fmt small rc=234 n=4",
    // "fmt nullbuf rc=0 n=4"; bad handles are 6.
    write_wide(VAL, u"");
    write_int(PCCH, 0);
    EXPECT_EQ(call("MsiFormatRecordW", {I(h), I(r), I(VAL), I(PCCH)}), 234u);
    EXPECT_EQ(read_int(PCCH), 4);
    write_int(PCCH, 0);
    EXPECT_EQ(call("MsiFormatRecordW", {I(h), I(r), I(0), I(PCCH)}), 0u);
    EXPECT_EQ(read_int(PCCH), 4);
    EXPECT_EQ(call("MsiFormatRecordW", {I(h), I(0x7777), I(OUTBUF), I(PCCH)}), 6u);
    EXPECT_EQ(call("MsiFormatRecordW", {I(0x7777), I(r), I(OUTBUF), I(PCCH)}), 6u);
    (void)mgr.handles().close(r);
}

TEST_P(MsiEngineArea, DoActionDispatch) {
    EXPECT_EQ(do_action(u"NoSuchAction"), 1626u);
    EXPECT_EQ(do_action(u""), 1603u);
    EXPECT_EQ(do_action(u"setx"), 1626u);  // names are case-sensitive
    EXPECT_EQ(call("MsiDoActionW", {I(0), I(NAME)}), 6u);
    // System changes need the install script (MsiOpenPackage session).
    EXPECT_EQ(do_action(u"InstallInitialize"), 1603u);
    EXPECT_EQ(do_action(u"InstallFiles"), 1603u);
}

TEST_P(MsiEngineArea, SequenceArgumentsAndMissingTable) {
    EXPECT_EQ(sequence(u"NoSuchTable"), 0u);
    EXPECT_EQ(sequence(u"ProbeSeq", 1), 87u);
    EXPECT_EQ(call("MsiSequenceW", {I(h), I(0), I(0)}), 87u);
    write_wide(NAME, u"ProbeSeq");
    EXPECT_EQ(call("MsiSequenceW", {I(0), I(NAME), I(0)}), 6u);
    // A bad condition ends the sequence with 1603 before later rows run.
    EXPECT_EQ(sequence(u"ProbeSeqBad"), 1603u);
    EXPECT_EQ(get(u"W"), "");
}

TEST_P(MsiEngineArea, SequenceRunsRowsAndTerminationActions) {
    // These rows are type 51 custom actions (the custom-action area).
    if (do_action(u"SetX") != 0) GTEST_SKIP() << "type 51 custom actions not available";
    EXPECT_EQ(get(u"X"), "1-set");
    set(u"X", u"");
    EXPECT_EQ(sequence(u"ProbeSeq"), 0u);
    EXPECT_EQ(get(u"X"), "1-set");
    EXPECT_EQ(get(u"Y"), "1-set+y");
    EXPECT_EQ(get(u"Z"), "");   // condition 0
    EXPECT_EQ(get(u"W"), "w");  // -1 runs on success
    EXPECT_EQ(get(u"T"), "");   // sequence 0 never runs
    EXPECT_EQ(get(u"Q"), "");   // -3 only on failure
    set(u"W", u"");
    EXPECT_EQ(sequence(u"ProbeSeqFail"), 1603u);
    EXPECT_EQ(get(u"W"), "");
    EXPECT_EQ(get(u"Q"), "q");
    EXPECT_EQ(get(u"T"), "");
    EXPECT_EQ(sequence(u"ProbeSeqStop"), 1603u);  // type 19 + continue still fails
}

TEST_P(MsiEngineArea, ProcessMessageResults) {
    auto rec = std::make_shared<msi::Record>(3);
    rec->fields[1].kind = msi::Record::Kind::Integer;
    rec->fields[1].integer = 25000;
    rec->fields[2].kind = msi::Record::Kind::String;
    rec->fields[2].text = u"two";
    const u32 r = mgr.handles().add(rec);
    const std::pair<u32, i32> cases[] = {
        {0x01000000, 0}, {0x01000001, 0}, {0x02000000, 0}, {0x03000000, 0}, {0x04000000, 0},
        {0x00000000, 0}, {0x08000000, 0}, {0x09000000, 0}, {0x0A000000, 0}, {0x0B000000, -1},
        {0x05000000, 0}, {0x06000000, 0}, {0x07000000, 0}, {0x19000000, -1}, {0x0C000000, -1},
        {0x0D000000, -1}, {0x0E000000, -1}, {0x10000000, -1}, {0x1A000000, -1},
    };
    for (const auto& [type, want] : cases) {
        EXPECT_EQ(static_cast<i32>(call("MsiProcessMessage", {I(h), I(type), I(r)})), want) << std::hex << type;
    }
    EXPECT_EQ(static_cast<i32>(call("MsiProcessMessage", {I(0), I(0x04000000), I(r)})), -1);
    EXPECT_EQ(static_cast<i32>(call("MsiProcessMessage", {I(h), I(0x04000000), I(0)})), -1);
    (void)mgr.handles().close(r);
}

TEST_P(MsiEngineArea, OpenPackageArgumentsAndOneSessionAtATime) {
    write_wide(BUF, u"C:\\pkg.msi");
    EXPECT_EQ(call("MsiOpenPackageExW", {I(BUF), I(2), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiOpenPackageW", {I(0), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiOpenPackageW", {I(BUF), I(0)}), 87u);
    // probe "open_missing rc=1618" with a session still open.
    write_wide(BUF, u"C:\\no\\such.msi");
    EXPECT_EQ(call("MsiOpenPackageW", {I(BUF), I(OUT2)}), 1618u);
    (void)mgr.handles().close(h);
    h = 0;
    EXPECT_EQ(call("MsiOpenPackageW", {I(BUF), I(OUT2)}), 1619u);
}

TEST_P(MsiEngineArea, BuiltInPropertiesOfAnOpenPackageSession) {
    // Shape and presence measured by the probe; values are Vitreo's own.
    EXPECT_EQ(get(u"AdminUser"), "1");
    EXPECT_EQ(get(u"Privileged"), "1");
    EXPECT_EQ(get(u"VersionMsi"), "5.00");
    EXPECT_EQ(get(u"VersionDatabase"), "200");
    EXPECT_EQ(get(u"PackageCode"), "{8E3B2A6D-7C9F-4A43-BD5E-2FA04B6C8D3B}");
    EXPECT_EQ(get(u"ProductState"), "-1");
    EXPECT_EQ(get(u"ACTION"), "INSTALL");
    EXPECT_EQ(get(u"OriginalDatabase"), "C:\\pkg.msi");
    EXPECT_EQ(get(u"DATABASE"), "C:\\pkg.msi");
    EXPECT_EQ(get(u"System16Folder"), "");
    EXPECT_EQ(get(u"TARGETDIR"), "");   // set by CostFinalize
    EXPECT_EQ(get(u"SourceDir"), "");   // set by ResolveSource
    EXPECT_EQ(get(u"Installed"), "");
    EXPECT_EQ(get(u"ProgramFilesFolder"), "C:\\Program Files (x86)\\");
    EXPECT_EQ(get(u"ProgramFiles64Folder"), "C:\\Program Files\\");
    EXPECT_EQ(get(u"SystemFolder"), "C:\\Windows\\SysWOW64\\");
    EXPECT_EQ(get(u"System64Folder"), "C:\\Windows\\System32\\");
    EXPECT_EQ(get(u"WindowsVolume"), "C:\\");
    EXPECT_EQ(get(u"VersionNT"), get(u"VersionNT64"));
    EXPECT_FALSE(get(u"Date").empty());
    EXPECT_FALSE(get(u"Time").empty());
}

INSTANTIATE_MSI_BITNESS(MsiEngineArea);

// ---- execution script (Engine API) ----

class MsiEngineScript : public ::testing::Test {
protected:
    std::shared_ptr<Engine> open(bool allow) {
        auto bytes = fixture_bytes();
        auto pkg = msi::Package::open(std::span<const u8>(bytes.data(), bytes.size()));
        EXPECT_TRUE(pkg.has_value());
        Engine::Options o;
        o.package_path = "C:\\pkg.msi";
        o.ui_level = 2;
        o.allow_system_changes = allow;
        o.server = true;
        u32 error = 0;
        auto e = Engine::open(mgr, std::make_shared<msi::Package>(std::move(*pkg)), o, msi::Services{}, error);
        EXPECT_EQ(error, 0u);
        return e;
    }
    MsiManager mgr;
};

TEST_F(MsiEngineScript, OpsRunAtFinalizeAndRollBackNewestFirst) {
    auto e = open(true);
    ASSERT_TRUE(e);
    std::vector<std::string> trace;
    ASSERT_TRUE(e->begin_script());
    EXPECT_FALSE(e->begin_script());  // already open
    for (const char* n : {"a", "b"}) {
        msi::ScriptOp op;
        op.action = n;
        op.run = [&trace, n]() -> u32 { trace.push_back(std::string("run ") + n); return 0; };
        op.rollback = [&trace, n]() { trace.push_back(std::string("undo ") + n); };
        e->schedule(std::move(op));
    }
    msi::ScriptOp rb;
    rb.action = "rb";
    rb.rollback_only = true;
    rb.run = [&trace, &e]() -> u32 {
        trace.push_back(e->script_kind() == msi::kRunRollback ? "rollback-ca" : "wrong");
        return 0;
    };
    e->schedule(std::move(rb));
    msi::ScriptOp commit;
    commit.action = "commit";
    commit.commit_only = true;
    commit.run = [&trace]() -> u32 { trace.push_back("commit"); return 0; };
    e->schedule(std::move(commit));
    msi::ScriptOp fail;
    fail.action = "fail";
    fail.run = [&trace, &e]() -> u32 {
        trace.push_back(e->script_kind() == msi::kRunScheduled ? "run fail" : "wrong");
        return 1603;
    };
    e->schedule(std::move(fail));
    EXPECT_TRUE(trace.empty());  // nothing runs before the script executes
    EXPECT_EQ(e->execute_script(true), 1603u);
    const std::vector<std::string> want = {"run a", "run b", "run fail", "rollback-ca", "undo b", "undo a"};
    EXPECT_EQ(trace, want);
    EXPECT_FALSE(e->scripting());
}

TEST_F(MsiEngineScript, CommitOpsRunAfterSuccessAndDeferredHandleIsRestricted) {
    auto e = open(true);
    ASSERT_TRUE(e);
    ASSERT_TRUE(e->begin_script());
    std::vector<std::string> trace;
    msi::ScriptOp op;
    op.action = "deferred";
    op.run = [&]() -> u32 {
        e->set_custom_action_data("payload");
        const u32 h = e->open_install_handle();
        auto ih = mgr.handles().get<msi::InstallHandle>(h);
        trace.push_back(ih && ih->deferred && ih->custom_action_data == "payload" ? "deferred-ok" : "wrong");
        (void)mgr.handles().close(h);
        return 0;
    };
    e->schedule(std::move(op));
    msi::ScriptOp commit;
    commit.commit_only = true;
    commit.run = [&]() -> u32 { trace.push_back(e->get_mode(msi::kRunCommit) ? "commit" : "wrong"); return 0; };
    e->schedule(std::move(commit));
    EXPECT_EQ(e->execute_script(true), 0u);
    EXPECT_EQ(trace, (std::vector<std::string>{"deferred-ok", "commit"}));
}

TEST_F(MsiEngineScript, SessionWithoutSystemChangesCannotOpenTheScript) {
    auto e = open(false);
    ASSERT_TRUE(e);
    EXPECT_FALSE(e->begin_script());
    EXPECT_EQ(e->do_action("InstallInitialize"), 1603u);
}

TEST_F(MsiEngineScript, CommandLineFeatureRequestSetsPreselected) {
    // Measured by the features area on winoracle: a feature request on the
    // command line sets Preselected=1; MsiSetProperty of one does not.
    auto bytes = fixture_bytes();
    auto pkg = msi::Package::open(std::span<const u8>(bytes.data(), bytes.size()));
    ASSERT_TRUE(pkg.has_value());
    Engine::Options o;
    o.package_path = "C:\\pkg.msi";
    o.ui_level = 2;
    o.command_line["ADDLOCAL"] = "F1";
    u32 error = 0;
    auto e = Engine::open(mgr, std::make_shared<msi::Package>(std::move(*pkg)), o, msi::Services{}, error);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->session().property("Preselected"), "1");
    auto plain = open(false);
    ASSERT_TRUE(plain);
    plain->session().set_property("ADDLOCAL", "F1");
    EXPECT_EQ(plain->session().property("Preselected"), "");
}

// ---- msiexec command line (Learn "Command-Line Options") ----

TEST(MsiexecCommandLineArea, UiLevelsAndModifiers) {
    auto level = [](const char* sw) {
        auto c = msi::parse_msiexec_command_line(std::string("msiexec /i p.msi ") + sw);
        return c ? c->ui_level : 0u;
    };
    EXPECT_EQ(level(""), 5u);
    EXPECT_EQ(level("/qn"), 2u);
    EXPECT_EQ(level("/q"), 2u);
    EXPECT_EQ(level("/quiet"), 2u);
    EXPECT_EQ(level("/qb"), 3u);
    EXPECT_EQ(level("/qr"), 4u);
    EXPECT_EQ(level("/qf"), 5u);
    EXPECT_EQ(level("/qb-"), 3u | 0x40u);
    EXPECT_EQ(level("/qb!"), 3u | 0x20u);
    EXPECT_EQ(level("/qn+"), 2u | 0x80u);
    EXPECT_EQ(level("/passive"), 3u | 0x40u | 0x20u);
}

TEST(MsiexecCommandLineArea, ModesSwitchesAndQuotes) {
    auto c = msi::parse_msiexec_command_line(
        "msiexec /fvomus {5DB65884-C963-4454-AABA-4CA3089281FA} /norestart /l*v+ C:\\x.log P=\"a \"\"b\"\" c\"");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->mode, msi::MsiexecCommand::Mode::Repair);
    EXPECT_EQ(c->repair_modes, "vomus");
    EXPECT_EQ(c->package, "{5DB65884-C963-4454-AABA-4CA3089281FA}");
    EXPECT_EQ(c->restart, msi::MsiexecCommand::Restart::No);
    EXPECT_EQ(c->log_modes, "*v+");
    EXPECT_EQ(c->log_file, "C:\\x.log");
    EXPECT_EQ(c->properties.at("P"), "a \"b\" c");
    EXPECT_TRUE(c->invalid.empty());

    auto f = msi::parse_msiexec_command_line("msiexec /f p.msi");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->repair_modes, "pecms");
    auto ju = msi::parse_msiexec_command_line("msiexec /ju p.msi /t a.mst");
    ASSERT_TRUE(ju);
    EXPECT_EQ(ju->mode, msi::MsiexecCommand::Mode::Advertise);
    EXPECT_EQ(ju->advertise_target, 'u');
    EXPECT_EQ(ju->transforms, "a.mst");
    auto bad = msi::parse_msiexec_command_line("msiexec /i p.msi /bogus");
    ASSERT_TRUE(bad);
    EXPECT_FALSE(bad->invalid.empty());
    EXPECT_EQ(MsiManager{}.run_msiexec("msiexec /i p.msi /bogus"), 1639u);
    EXPECT_EQ(MsiManager{}.run_msiexec("msiexec /i C:\\missing.msi /qn"), 1619u);
}

}  // namespace
}  // namespace vitreo::winapi::msi_test
