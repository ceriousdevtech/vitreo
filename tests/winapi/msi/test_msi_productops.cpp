// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_productops.cpp
/// @brief Contract tests for the Windows Installer productops area, both bitnesses
/// through msi_fixture.hpp.
///
/// Expected values come from Windows 11: tools/oracle_probe/msi_productops_probe.c
/// (msi_productops32/64.expected; the line each test mirrors is named), and
/// the Installer.FileVersion / FileHash of tests/winapi/msi/fixtures/
/// productops_resources.dll measured on winoracle (1.2.3.4, "0,1031,1033",
/// a25e2d98 c95966d2 6a1ac1c8 d04c5ae9). The fixture DLL is built from
/// tools/oracle_probe/msi_productops_probe.rc, the same resources the probe
/// measured MsiLoadString with.

#include "msi_fixture.hpp"

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/system/msi/msi_product.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace vitreo::winapi::msi_test {
namespace {

constexpr u32 kOk = 0, kInvalidParameter = 87, kMoreData = 234;
const char* const kUnknown = "{0D1C2B3A-4E5F-4071-8293-A4B5C6D7E8F9}";
const char* const kUnknownComp = "{1A2B3C4D-5E6F-4A7B-8C9D-0E1F2A3B4C5D}";

std::u16string u(const std::string& s) { return std::u16string(s.begin(), s.end()); }

class Msi_productops_Test : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        root = std::filesystem::temp_directory_path() /
               ("vitreo-msi-productops-" + std::to_string(::getpid()) + (x64() ? "-64" : "-32"));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "Windows");
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
        next = DATA + 0x40000;
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    void file(const std::string& name, const std::vector<u8>& bytes) {
        std::ofstream(root / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                           static_cast<std::streamsize>(bytes.size()));
    }
    void file(const std::string& name, const std::string& text) { file(name, std::vector<u8>(text.begin(), text.end())); }

    /// A NUL-terminated string in guest memory (UTF-16 or bytes).
    GuestAddr wstr(const std::u16string& s) {
        const GuestAddr at = next;
        write_wide(at, s);
        next += (s.size() + 1) * 2 + 16;
        return at;
    }
    GuestAddr wstr(const std::string& s) { return wstr(u(s)); }
    GuestAddr astr(const std::string& s) {
        const GuestAddr at = next;
        EXPECT_TRUE(memory.write_bytes(at, reinterpret_cast<const u8*>(s.data()), s.size()).is_ok());
        EXPECT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
        next += s.size() + 16;
        return at;
    }
    /// A buffer pre-filled with u16 '#' (bytes 0x23 0x00), or with @p fill.
    GuestAddr buffer(usize bytes, int fill = -1) {
        const GuestAddr at = next;
        if (fill >= 0) poison(at, bytes, static_cast<u8>(fill));
        else
            for (usize i = 0; i + 1 < bytes; i += 2) EXPECT_TRUE(memory.write_u16(at + i, u16{'#'}).is_ok());
        next += bytes + 16;
        return at;
    }
    std::u16string read_w(GuestAddr at, usize max = 512) {
        std::u16string s;
        for (usize i = 0; i < max; ++i) {
            const u16 c = memory.read_u16(at + i * 2).value();
            if (c == 0) break;
            s.push_back(static_cast<char16_t>(c));
        }
        return s;
    }
    std::string read_a(GuestAddr at, usize max = 512) {
        std::string s;
        for (usize i = 0; i < max; ++i) {
            const u8 c = memory.read_u8(at + i).value();
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    std::string path(const std::string& name) { return "C:\\" + name; }

    std::filesystem::path root;
    capsule::Vfs vfs;
    GuestAddr next{0};
};

std::vector<u8> fixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "fixtures" / name, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

// ---- MsiGetFileHash (probe hash lines) ---------------------------------------------------------

TEST_P(Msi_productops_Test, FileHashIsMd5AsLittleEndianDwordsAndEmptyIsZero) {
    file("abc.bin", "abc");
    file("empty.bin", "");
    file("text.txt", "hello, installer\r\n");
    file("res.dll", fixture("productops_resources.dll"));
    std::filesystem::create_directories(root / "dir");
    struct Case { std::string path; u32 ret; std::array<u32, 4> parts; };
    const Case cases[] = {
        {path("abc.bin"), 0, {0x98500190, 0xb04fd23c, 0x7d3f96d6, 0x727fe128}},
        {path("empty.bin"), 0, {0, 0, 0, 0}},
        {path("text.txt"), 0, {0xa81d8a17, 0x2c37084f, 0xa58a7684, 0x7808cb87}},
        {path("res.dll"), 0, {0xa25e2d98, 0xc95966d2, 0x6a1ac1c8, 0xd04c5ae9}},
        {path("missing.msi"), 2, {}},
        {"C:\\no_such_dir_x\\a.dll", 3, {}},
        {path("dir"), 5, {}},
        {"", 3, {}},
    };
    for (const Case& c : cases) {
        const GuestAddr info = buffer(20, 0xCC);
        write_int(info, 20);
        EXPECT_EQ(call("MsiGetFileHashW", {I(wstr(c.path)), I(0), I(info)}), c.ret) << c.path;
        if (c.ret == 0)
            for (int i = 0; i < 4; ++i) EXPECT_EQ(static_cast<u32>(read_int(info + 4 + i * 4)), c.parts[i]) << c.path;
        else
            EXPECT_EQ(static_cast<u32>(read_int(info + 4)), 0xCCCCCCCCu) << c.path;
    }
    const GuestAddr info = buffer(20, 0xCC);
    write_int(info, 20);
    EXPECT_EQ(call("MsiGetFileHashA", {I(astr(path("abc.bin"))), I(0), I(info)}), 0u);
    EXPECT_EQ(static_cast<u32>(read_int(info + 4)), 0x98500190u);
    EXPECT_EQ(call("MsiGetFileHashW", {I(wstr(path("abc.bin"))), I(1), I(info)}), kInvalidParameter);   // abc-opt1
    for (i32 size : {0, 16, 24}) {                                                                    // abc-size*
        write_int(info, size);
        EXPECT_EQ(call("MsiGetFileHashW", {I(wstr(path("abc.bin"))), I(0), I(info)}), kInvalidParameter);
    }
    EXPECT_EQ(call("MsiGetFileHashW", {I(wstr(path("abc.bin"))), I(0), I(0)}), kInvalidParameter);   // abc-nullinfo
    write_int(info, 20);
    EXPECT_EQ(call("MsiGetFileHashW", {I(0), I(0), I(info)}), kInvalidParameter);                     // null
}

// ---- MsiGetFileVersion (probe version lines) ------------------------------------------------------

TEST_P(Msi_productops_Test, FileVersionAndLanguagesOfAPeImage) {
    file("res.dll", fixture("productops_resources.dll"));
    const GuestAddr p = wstr(path("res.dll"));
    GuestAddr v = buffer(128), l = buffer(128);
    write_int(OUT, 64);
    write_int(OUT2, 64);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(v), I(OUT), I(l), I(OUT2)}), kOk);
    EXPECT_EQ(read_w(v), u"1.2.3.4");
    EXPECT_EQ(read_w(l), u"0,1031,1033");
    EXPECT_EQ(read_int(OUT), 7);
    EXPECT_EQ(read_int(OUT2), 11);
    // self-lcap3: truncated language, ERROR_MORE_DATA, required length back.
    l = buffer(128);
    write_int(OUT, 64);
    write_int(OUT2, 3);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(v), I(OUT), I(l), I(OUT2)}), kMoreData);
    EXPECT_EQ(read_w(l), u"0,");
    EXPECT_EQ(read_int(OUT2), 11);
    // vcap5 / vcap1 / vcap0
    for (auto [cap, want] : {std::pair<i32, std::u16string>{5, u"1.2."}, {1, u""}}) {
        v = buffer(128);
        write_int(OUT, cap);
        EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(v), I(OUT), I(0), I(0)}), kMoreData);
        EXPECT_EQ(read_w(v), want);
        EXPECT_EQ(read_int(OUT), 7);
    }
    v = buffer(128);
    write_int(OUT, 0);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(v), I(OUT), I(0), I(0)}), kMoreData);
    EXPECT_EQ(memory.read_u16(v).value(), u16{'#'});
    // size queries, version-only, no outputs
    write_int(OUT, 99);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(0), I(OUT), I(0), I(0)}), kOk);
    EXPECT_EQ(read_int(OUT), 7);
    write_int(OUT2, 99);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(0), I(0), I(0), I(OUT2)}), kOk);
    EXPECT_EQ(read_int(OUT2), 11);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(0), I(0), I(0), I(0)}), kOk);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(p), I(v), I(0), I(0), I(0)}), kInvalidParameter);  // buf-nopcch
    // ANSI: k32-cap3 shape
    const GuestAddr va = buffer(64);
    write_int(OUT, 3);
    write_int(OUT2, 64);
    const GuestAddr la = buffer(64);
    EXPECT_EQ(call("MsiGetFileVersionA", {I(astr(path("res.dll"))), I(va), I(OUT), I(la), I(OUT2)}), kMoreData);
    EXPECT_EQ(read_a(va), "1.");
    EXPECT_EQ(read_a(la), "0,1031,1033");
}

TEST_P(Msi_productops_Test, FileVersionErrors) {
    file("text.txt", "hello, installer\r\n");
    file("empty.bin", "");
    std::filesystem::create_directories(root / "dir");
    struct Case { std::string path; u32 ret; };
    for (const Case& c : {Case{path("text.txt"), 1006}, Case{path("empty.bin"), 1006}, Case{path("missing.msi"), 2},
                          Case{"C:\\no_such_dir_x\\a.dll", 2}, Case{path("dir"), 2}, Case{"", 2}}) {
        const GuestAddr v = buffer(64), l = buffer(64);
        write_int(OUT, 64);
        write_int(OUT2, 64);
        EXPECT_EQ(call("MsiGetFileVersionW", {I(wstr(c.path)), I(v), I(OUT), I(l), I(OUT2)}), c.ret) << c.path;
        EXPECT_EQ(read_int(OUT), 64) << c.path;
        EXPECT_EQ(memory.read_u16(v).value(), u16{'#'}) << c.path;
    }
    write_int(OUT, 64);
    EXPECT_EQ(call("MsiGetFileVersionW", {I(0), I(buffer(64)), I(OUT), I(0), I(0)}), kInvalidParameter);
}

/// A minimal sfnt with one `name` record (platform 3, name ID 5).
std::vector<u8> font_with_version(const std::u16string& version) {
    std::vector<u8> f;
    auto be16 = [&](u32 v) { f.push_back(static_cast<u8>(v >> 8)); f.push_back(static_cast<u8>(v)); };
    auto be32 = [&](u32 v) { be16(v >> 16); be16(v & 0xFFFF); };
    be32(0x00010000); be16(1); be16(16); be16(0); be16(0);       // offset table, 1 table
    be32(0x6E616D65); be32(0); be32(28); be32(0);                // 'name' at 28
    be16(0); be16(1); be16(18);                                  // format, count, stringOffset
    be16(3); be16(1); be16(0x409); be16(5); be16(static_cast<u32>(version.size() * 2)); be16(0);
    for (char16_t c : version) be16(c);
    return f;
}

TEST_P(Msi_productops_Test, FontVersionFromTheNameTable) {
    // probe font lines: "Version 2102.025" -> 2102.25.0.0, "Version 2.07;..." -> 2.7.0.0, empty language.
    for (auto [text, want] : {std::pair<std::u16string, std::u16string>{u"Version 2102.025", u"2102.25.0.0"},
                              {u"Version 2.07;210929233650", u"2.7.0.0"}, {u"Version 7.10", u"7.10.0.0"}}) {
        file("f.ttf", font_with_version(text));
        const GuestAddr v = buffer(64), l = buffer(64);
        write_int(OUT, 64);
        write_int(OUT2, 64);
        EXPECT_EQ(call("MsiGetFileVersionW", {I(wstr(path("f.ttf"))), I(v), I(OUT), I(l), I(OUT2)}), kOk);
        EXPECT_EQ(read_w(v), want);
        EXPECT_EQ(read_w(l), u"");
        EXPECT_EQ(read_int(OUT2), 0);
    }
}

// ---- Descriptors (probe decompose lines) -----------------------------------------------------------------

TEST_P(Msi_productops_Test, CompressedGuidsMatchTheMeasuredEncoding) {
    EXPECT_EQ(msi::compress_guid(kUnknown), "bt06'-WRe8C$@HcTN9Cy");
    EXPECT_EQ(msi::compress_guid(kUnknownComp), "LxuJ+gtix?%rM},3Dl}E");
    EXPECT_EQ(msi::compress_guid("{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}"), "!0_?{!0_?{!0_?{!0_?{");
    EXPECT_EQ(msi::decompress_guid(u"~~~~~" + u(msi::compress_guid(kUnknown).substr(5))),
              "{08780EC4-4E5F-4071-8293-A4B5C6D7E8F9}");
    EXPECT_EQ(msi::compose_descriptor(kUnknown, "FeatureOne", kUnknownComp),
              "bt06'-WRe8C$@HcTN9CyFeatureOne>LxuJ+gtix?%rM},3Dl}E");
}

TEST_P(Msi_productops_Test, DecomposeDescriptorMeasuredForms) {
    const std::string p = msi::compress_guid(kUnknown), c = msi::compress_guid(kUnknownComp);
    struct Case {
        std::string desc; u32 ret; i64 off; std::u16string prod, feat, comp;
    };
    const std::u16string P = u(kUnknown), C = u(kUnknownComp), none = u"#";
    const Case cases[] = {
        {p + "FeatureOne>" + c, 0, 51, P, u"FeatureOne", C},
        {p + "FeatureOne>" + c + " trailing args", 0, 51, P, u"FeatureOne", C},
        {p + "FeatureOne<", 0, 31, P, u"FeatureOne", u""},
        {p + "FeatureOne<rest", 0, 31, P, u"FeatureOne", u""},
        {p + ">" + c, 87, -1, P, none, none},
        {p + "<", 87, -1, P, none, none},
        {p + "FeatureOne", 87, -1, P, u"FeatureOne#", none},
        {p + "F>" + c, 0, 42, P, u"F", C},
        {p + "FeatureOne>" + c.substr(0, 10), 87, -1, P, u"FeatureOne", none},
        {"abc", 87, -1, none, none, none},
        {"", 87, -1, none, none, none},
        {"#" + p.substr(1) + "X>" + c, 87, -1, none, none, none},
        {p + "Feat>" + c + "TRAIL", 0, 45, P, u"Feat", C},
        {p + "Fe<at>" + c, 0, 23, P, u"Fe", u""},
        {p + "Fe>at>" + c, 87, -1, P, u"Fe", none},
        {p + std::string(38, 'F') + ">" + c, 0, 79, P, u(std::string(38, 'F')), C},
        {p + std::string(39, 'F') + ">" + c, 87, -1, P, u(std::string(38, 'F') + "#"), none},
        {p + std::string(38, 'F') + "<", 0, 59, P, u(std::string(38, 'F')), u""},
    };
    for (const Case& k : cases) {
        const GuestAddr pb = buffer(128), fb = buffer(128), cb = buffer(128);
        write_int(OUT, static_cast<i32>(0xDEADBEEF));
        EXPECT_EQ(call("MsiDecomposeDescriptorW", {I(wstr(k.desc)), I(pb), I(fb), I(cb), I(OUT)}), k.ret) << k.desc;
        EXPECT_EQ(static_cast<u32>(read_int(OUT)), k.off < 0 ? 0xDEADBEEFu : static_cast<u32>(k.off)) << k.desc;
        auto check = [&](GuestAddr b, const std::u16string& want) {
            if (want == u"#") EXPECT_EQ(memory.read_u16(b).value(), u16{'#'}) << k.desc;
            else EXPECT_EQ(read_w(b).substr(0, want.size()), want) << k.desc;
        };
        check(pb, k.prod);
        check(fb, k.feat);
        check(cb, k.comp);
    }
    // non-ASCII feature (feature-nonascii): one character each.
    const GuestAddr pb = buffer(128), fb = buffer(128), cb = buffer(128);
    EXPECT_EQ(call("MsiDecomposeDescriptorW", {I(wstr(u(p) + u"F\u00e9>" + u(c))), I(pb), I(fb), I(cb), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 43);
    EXPECT_EQ(read_w(fb), u"F\u00e9");
    // outputs are optional (full-nobufs / full-nooffset), NULL descriptor is 87.
    write_int(OUT, 7);
    EXPECT_EQ(call("MsiDecomposeDescriptorW", {I(wstr(p + "FeatureOne>" + c)), I(0), I(0), I(0), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 51);
    EXPECT_EQ(call("MsiDecomposeDescriptorW", {I(wstr(p + "FeatureOne>" + c)), I(0), I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiDecomposeDescriptorW", {I(0), I(pb), I(fb), I(cb), I(OUT)}), kInvalidParameter);
    // ANSI (decomposeA)
    const GuestAddr pa = buffer(64), fa = buffer(64), ca = buffer(64);
    EXPECT_EQ(call("MsiDecomposeDescriptorA", {I(astr(p + "FeatureOne>" + c)), I(pa), I(fa), I(ca), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 51);
    EXPECT_EQ(read_a(pa), kUnknown);
    EXPECT_EQ(read_a(fa), "FeatureOne");
    EXPECT_EQ(read_a(ca), kUnknownComp);
}

// ---- MsiGetShortcutTarget (probe shortcut lines) -----------------------------------------------------------

/// A [MS-SHLLINK] file: header (HasDarwinID | IsUnicode), optional
/// EXP_DARWIN_LINK block, terminal block.
std::vector<u8> shell_link(const std::optional<std::string>& descriptor) {
    std::vector<u8> f(0x4C, 0);
    auto put32 = [](std::vector<u8>& v, usize at, u32 x) { for (int i = 0; i < 4; ++i) v[at + i] = static_cast<u8>(x >> (8 * i)); };
    put32(f, 0, 0x4C);
    const u8 clsid[16] = {0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    std::copy(clsid, clsid + 16, f.begin() + 4);
    put32(f, 0x14, (descriptor ? 0x1000u : 0u) | 0x80u);
    if (descriptor) {
        std::vector<u8> block(0x314, 0);
        put32(block, 0, 0x314);
        put32(block, 4, 0xA0000006u);
        for (usize i = 0; i < descriptor->size(); ++i) {
            block[8 + i] = static_cast<u8>((*descriptor)[i]);
            block[8 + 260 + i * 2] = static_cast<u8>((*descriptor)[i]);
        }
        f.insert(f.end(), block.begin(), block.end());
    }
    f.insert(f.end(), 4, 0);
    return f;
}

TEST_P(Msi_productops_Test, ShortcutTargetOfADarwinLink) {
    const std::string p = msi::compress_guid(kUnknown), c = msi::compress_guid(kUnknownComp);
    file("darwin.lnk", shell_link(p + "FeatureOne>" + c));
    file("nocomp.lnk", shell_link(p + "FeatureOne<"));
    file("plain.lnk", shell_link(std::nullopt));
    file("text.txt", "hello, installer\r\n");
    GuestAddr pb = buffer(128), fb = buffer(128), cb = buffer(128);
    EXPECT_EQ(call("MsiGetShortcutTargetW", {I(wstr(path("darwin.lnk"))), I(pb), I(fb), I(cb)}), 0u);
    EXPECT_EQ(read_w(pb), u(kUnknown));
    EXPECT_EQ(read_w(fb), u"FeatureOne");
    EXPECT_EQ(read_w(cb), u(kUnknownComp));
    pb = buffer(128), fb = buffer(128), cb = buffer(128);
    EXPECT_EQ(call("MsiGetShortcutTargetW", {I(wstr(path("darwin.lnk"))), I(pb), I(0), I(0)}), 0u);  // darwin-prodonly
    EXPECT_EQ(read_w(pb), u(kUnknown));
    EXPECT_EQ(call("MsiGetShortcutTargetW", {I(wstr(path("darwin.lnk"))), I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiGetShortcutTargetW", {I(wstr(path("nocomp.lnk"))), I(pb), I(fb), I(cb)}), 0u);
    EXPECT_EQ(read_w(cb), u"");
    for (const std::string& name : {"plain.lnk", "text.txt", "missing.lnk"}) {
        cb = buffer(128);
        EXPECT_EQ(call("MsiGetShortcutTargetW", {I(wstr(path(name))), I(pb), I(fb), I(cb)}), 1627u) << name;
        EXPECT_EQ(memory.read_u16(cb).value(), u16{'#'});
    }
    EXPECT_EQ(call("MsiGetShortcutTargetW", {I(0), I(pb), I(fb), I(cb)}), 1627u);
    const GuestAddr pa = buffer(64);
    EXPECT_EQ(call("MsiGetShortcutTargetA", {I(astr(path("darwin.lnk"))), I(pa), I(0), I(0)}), 0u);
    EXPECT_EQ(read_a(pa), kUnknown);
}

// ---- MsiLoadString (probe loadstring lines) -----------------------------------------------------------------

TEST_P(Msi_productops_Test, LoadStringFromAModulesStringTable) {
    // Map the fixture's headers and sections at a guest base, as the loader does.
    const std::vector<u8> image = fixture("productops_resources.dll");
    ASSERT_FALSE(image.empty());
    constexpr GuestAddr base = 0x400000;
    ASSERT_TRUE(memory.map_region(base, 0x20000, MemoryProtection::ReadWrite).is_ok());
    auto r16 = [&](usize o) { return static_cast<u32>(image[o] | (image[o + 1] << 8)); };
    auto r32 = [&](usize o) { return r16(o) | (r16(o + 2) << 16); };
    const usize pe = r32(0x3C);
    const usize sections = r16(pe + 6), opt_size = r16(pe + 20);
    const usize headers = r32(pe + 24 + 60);
    ASSERT_TRUE(memory.write_bytes(base, image.data(), headers).is_ok());
    for (usize i = 0; i < sections; ++i) {
        const usize s = pe + 24 + opt_size + i * 40;
        const usize va = r32(s + 12), raw_size = r32(s + 16), raw = r32(s + 20);
        if (raw_size) ASSERT_TRUE(memory.write_bytes(base + va, image.data() + raw, raw_size).is_ok());
    }
    struct Case { u32 id; i32 cap; u32 lang; i32 ret; std::u16string text; };
    const Case cases[] = {
        {1, 64, 0, 1252, u"English one"}, {1, 64, 0x409, 1252, u"English one"}, {1, 64, 0x407, 1252, u"English one"},
        {2, 64, 0x409, 1252, u"English two"}, {2, 64, 0x407, 1252, u"Deutsch zwei"}, {2, 64, 0x40c, 1252, u"English two"},
        {1, 4, 0, 1252, u"Eng"}, {1, 1, 0, 1252, u""}, {1, 0, 0, 0, u"#"}, {3, 64, 0, 0, u"#"},
        {99, 64, 0, 0, u"#"}, {17, 64, 0, 1252, u"Seventeen"},
    };
    for (const Case& c : cases) {
        const GuestAddr b = buffer(128);
        EXPECT_EQ(static_cast<i32>(call("MsiLoadStringW", {I(base), I(c.id), I(b), I(static_cast<u32>(c.cap)), I(c.lang)})),
                  c.ret) << c.id << " " << c.lang;
        if (c.text == u"#") EXPECT_EQ(memory.read_u16(b).value(), u16{'#'});
        else EXPECT_EQ(read_w(b), c.text) << c.id;
    }
    GuestAddr a = buffer(64);
    EXPECT_EQ(call("MsiLoadStringA", {I(base), I(1), I(a), I(64), I(0)}), 1252u);
    EXPECT_EQ(read_a(a), "English one");
    a = buffer(64);
    EXPECT_EQ(call("MsiLoadStringA", {I(base), I(1), I(a), I(4), I(0)}), 1252u);  // loadstringA id1-cap4
    EXPECT_EQ(read_a(a), "");
}

// ---- Product operations against an unknown product (probe configure/... lines) ----------------------------

TEST_P(Msi_productops_Test, ProductOperationParameterRules) {
    const GuestAddr U = wstr(std::string(kUnknown)), Ulower = wstr(std::string("{0d1c2b3a-4e5f-4071-8293-a4b5c6d7e8f9}"));
    const GuestAddr bad = wstr(std::string("notaguid")), braces = wstr(std::string("{0D1C2B3A-4E5F-4071-8293-A4B5C6D7E8F}"));
    const GuestAddr F = wstr(std::string("F")), comp = wstr(std::string(kUnknownComp)), cmd = wstr(std::string("ADDLOCAL=ALL"));
    const GuestAddr longf = wstr(std::string(160, 'F')), dll = wstr(std::string("a.dll"));
    struct Case { const char* api; std::initializer_list<Arg> args; u32 ret; };
    const Case cases[] = {
        {"MsiConfigureProductW", {I(0), I(0), I(3)}, 87}, {"MsiConfigureProductW", {I(bad), I(0), I(3)}, 87},
        {"MsiConfigureProductW", {I(braces), I(0), I(3)}, 87}, {"MsiConfigureProductW", {I(Ulower), I(0), I(3)}, 1605},
        {"MsiConfigureProductW", {I(U), I(0), I(3)}, 1605}, {"MsiConfigureProductW", {I(U), I(0), I(2)}, 1605},
        {"MsiConfigureProductW", {I(U), I(0), I(5)}, 1605}, {"MsiConfigureProductW", {I(U), I(0), I(1)}, 1605},
        {"MsiConfigureProductW", {I(U), I(0), I(4)}, 1605}, {"MsiConfigureProductW", {I(U), I(0), I(0xFFFFFFFF)}, 87},
        {"MsiConfigureProductW", {I(U), I(0), I(99)}, 87}, {"MsiConfigureProductW", {I(U), I(0), I(0)}, 87},
        {"MsiConfigureProductW", {I(U), I(0xFFFFFFFF), I(5)}, 87}, {"MsiConfigureProductW", {I(U), I(65536), I(5)}, 87},
        {"MsiConfigureProductW", {I(U), I(100), I(5)}, 1605},
        {"MsiConfigureProductExW", {I(0), I(0), I(3), I(0)}, 87}, {"MsiConfigureProductExW", {I(U), I(0), I(5), I(cmd)}, 1605},
        {"MsiConfigureProductExW", {I(U), I(0), I(99), I(0)}, 87}, {"MsiConfigureProductExW", {I(U), I(0), I(3), I(cmd)}, 1605},
        {"MsiConfigureFeatureW", {I(0), I(F), I(3)}, 87}, {"MsiConfigureFeatureW", {I(U), I(0), I(3)}, 87},
        {"MsiConfigureFeatureW", {I(bad), I(F), I(3)}, 87}, {"MsiConfigureFeatureW", {I(U), I(F), I(3)}, 1605},
        {"MsiConfigureFeatureW", {I(U), I(F), I(99)}, 87}, {"MsiConfigureFeatureW", {I(U), I(F), I(0xFFFFFFFF)}, 87},
        {"MsiConfigureFeatureW", {I(U), I(F), I(1)}, 1605}, {"MsiConfigureFeatureW", {I(U), I(longf), I(3)}, 87},
        {"MsiReinstallProductW", {I(0), I(2)}, 87}, {"MsiReinstallProductW", {I(bad), I(2)}, 87},
        {"MsiReinstallProductW", {I(U), I(2)}, 1605}, {"MsiReinstallProductW", {I(U), I(0)}, 87},
        {"MsiReinstallProductW", {I(U), I(1)}, 1605}, {"MsiReinstallProductW", {I(U), I(0x800)}, 87},
        {"MsiReinstallProductW", {I(U), I(0x12)}, 1605},
        {"MsiReinstallFeatureW", {I(0), I(F), I(2)}, 87}, {"MsiReinstallFeatureW", {I(U), I(0), I(2)}, 87},
        {"MsiReinstallFeatureW", {I(U), I(F), I(2)}, 1605}, {"MsiReinstallFeatureW", {I(U), I(F), I(0)}, 87},
        {"MsiInstallMissingComponentW", {I(0), I(comp), I(3)}, 87}, {"MsiInstallMissingComponentW", {I(U), I(0), I(3)}, 87},
        {"MsiInstallMissingComponentW", {I(U), I(bad), I(3)}, 87}, {"MsiInstallMissingComponentW", {I(U), I(comp), I(3)}, 1605},
        {"MsiInstallMissingComponentW", {I(U), I(comp), I(2)}, 87}, {"MsiInstallMissingComponentW", {I(U), I(comp), I(5)}, 1605},
        {"MsiInstallMissingComponentW", {I(U), I(comp), I(4)}, 1605}, {"MsiInstallMissingComponentW", {I(U), I(comp), I(99)}, 87},
        {"MsiInstallMissingFileW", {I(0), I(dll)}, 87}, {"MsiInstallMissingFileW", {I(U), I(0)}, 87},
        {"MsiInstallMissingFileW", {I(U), I(dll)}, 1605}, {"MsiInstallMissingFileW", {I(bad), I(dll)}, 87},
        {"MsiCollectUserInfoW", {I(0)}, 87}, {"MsiCollectUserInfoW", {I(bad)}, 87}, {"MsiCollectUserInfoW", {I(U)}, 1605},
    };
    for (const Case& c : cases) EXPECT_EQ(call(c.api, c.args), c.ret) << c.api;
    EXPECT_EQ(call("MsiConfigureProductA", {I(astr(kUnknown)), I(0), I(3)}), 1605u);
    // From descriptors (cffd / rffd lines).
    const GuestAddr desc = wstr(msi::compose_descriptor(kUnknown, "FeatureOne", kUnknownComp));
    const GuestAddr abc = wstr(std::string("abc"));
    for (auto [api, arg, ret] : {std::tuple<const char*, u32, u32>{"MsiConfigureFeatureFromDescriptorW", 3, 1605},
                                 {"MsiConfigureFeatureFromDescriptorW", 99, 87},
                                 {"MsiReinstallFeatureFromDescriptorW", 2, 1605},
                                 {"MsiReinstallFeatureFromDescriptorW", 0, 87}}) {
        EXPECT_EQ(call(api, {I(desc), I(arg)}), ret) << api;
        EXPECT_EQ(call(api, {I(0), I(arg)}), 87u) << api;
        EXPECT_EQ(call(api, {I(abc), I(arg)}), 87u) << api;
    }
}

TEST_P(Msi_productops_Test, InstallVerifyAndAdvertisePackageRules) {
    file("text.txt", "hello, installer\r\n");
    msi::Package empty = msi::Package::create();
    file("empty.msi", empty.serialize());
    std::filesystem::create_directories(root / "dir");
    const GuestAddr text = wstr(path("text.txt")), db = wstr(path("empty.msi")), missing = wstr(path("missing.msi"));
    const GuestAddr script = wstr(path("probe_script.aas"));
    // verify lines
    EXPECT_EQ(call("MsiVerifyPackageW", {I(0)}), 87u);
    EXPECT_EQ(call("MsiVerifyPackageW", {I(wstr(std::string()))}), 1619u);
    EXPECT_EQ(call("MsiVerifyPackageW", {I(missing)}), 1619u);
    EXPECT_EQ(call("MsiVerifyPackageW", {I(text)}), 1619u);
    EXPECT_EQ(call("MsiVerifyPackageW", {I(db)}), 0u);
    EXPECT_EQ(call("MsiVerifyPackageW", {I(wstr(path("dir")))}), 1619u);
    EXPECT_EQ(call("MsiVerifyPackageA", {I(astr(path("empty.msi")))}), 0u);
    // install lines
    EXPECT_EQ(call("MsiInstallProductW", {I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiInstallProductW", {I(wstr(std::string())), I(0)}), 3u);
    EXPECT_EQ(call("MsiInstallProductW", {I(missing), I(0)}), 2u);
    EXPECT_EQ(call("MsiInstallProductW", {I(text), I(0)}), 1620u);
    EXPECT_EQ(call("MsiInstallProductW", {I(db), I(0)}), 1620u);
    EXPECT_EQ(call("MsiInstallProductW", {I(db), I(wstr(std::string("FOO=1")))}), 1620u);
    // advertise lines
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(0), I(0), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(missing), I(0), I(0), I(0)}), 2u);
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(text), I(0), I(0), I(0)}), 1620u);
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(db), I(0), I(0), I(0)}), 1620u);
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(db), I(1), I(0), I(0)}), 1620u);
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(db), I(script), I(0), I(0)}), 1620u);
    EXPECT_EQ(call("MsiAdvertiseProductW", {I(missing), I(script), I(0), I(0)}), 2u);
    struct Ex { GuestAddr script; u32 platform, options, ret; };
    for (const Ex& e : {Ex{script, 0, 0, 2}, Ex{script, 0x10, 0, 87}, Ex{script, 5, 0, 87}, Ex{script, 0, 2, 87},
                        Ex{script, 0, 1, 87}, Ex{0, 1, 0, 87}, Ex{0, 0, 1, 87}, Ex{0, 0, 0, 2}})
        EXPECT_EQ(call("MsiAdvertiseProductExW", {I(missing), I(e.script), I(0), I(0), I(e.platform), I(e.options)}), e.ret);
    EXPECT_EQ(call("MsiAdvertiseProductExW", {I(0), I(0), I(0), I(0), I(0), I(0)}), 87u);
    // advscript / procadv lines
    EXPECT_EQ(call("MsiAdvertiseScriptW", {I(0), I(0), I(0), I(0)}), 5u);
    EXPECT_EQ(call("MsiAdvertiseScriptW", {I(text), I(1), I(0), I(0)}), 5u);
    EXPECT_EQ(call("MsiProcessAdvertiseScriptW", {I(0), I(0), I(0), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiProcessAdvertiseScriptW", {I(missing), I(0), I(0), I(0), I(0)}), 5u);
}

/// A patch package: the patch root CLSID and a SummaryInformation stream
/// whose PID_TEMPLATE lists @p targets (Learn "Template Summary" for patches).
std::vector<u8> patch_package(const std::string& targets) {
    msi::Package p = msi::Package::create();
    p.set_clsid({0x86, 0x10, 0x0C, 0x00, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46});
    std::vector<u8> s(48, 0);
    auto put16 = [&](usize at, u32 v) { s[at] = static_cast<u8>(v); s[at + 1] = static_cast<u8>(v >> 8); };
    auto put32 = [&](usize at, u32 v) { put16(at, v & 0xFFFF); put16(at + 2, v >> 16); };
    put16(0, 0xFFFE);
    put32(24, 1);
    const u8 fmtid[16] = {0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10, 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9};
    std::copy(fmtid, fmtid + 16, s.begin() + 28);
    put32(44, 48);
    std::vector<u8> section(16, 0);
    std::string value = targets;
    value.push_back('\0');
    while (value.size() % 4) value.push_back('\0');
    const u32 size = 16 + 8 + static_cast<u32>(value.size());
    for (int i = 0; i < 4; ++i) section[i] = static_cast<u8>(size >> (8 * i));
    section[4] = 1;
    section[8] = 7;    // PID_TEMPLATE
    section[12] = 16;  // offset
    std::vector<u8> prop = {30, 0, 0, 0};
    const u32 len = static_cast<u32>(targets.size() + 1);
    for (int i = 0; i < 4; ++i) prop.push_back(static_cast<u8>(len >> (8 * i)));
    prop.insert(prop.end(), value.begin(), value.end());
    s.insert(s.end(), section.begin(), section.end());
    s.insert(s.end(), prop.begin(), prop.end());
    p.set_stream("\x05SummaryInformation", s);
    return p.serialize();
}

std::vector<u8> product_database(const std::string& product_code) {
    msi::Package p = msi::Package::create();
    EXPECT_EQ(p.create_table("Property", {msi::make_column("Property", 's', 72, false, true, false, false),
                                           msi::make_column("Value", 's', 0, false, false, true, false)}, false), 0u);
    msi::Table* t = p.find_table("Property");
    EXPECT_NE(t, nullptr);
    if (t) EXPECT_EQ(p.insert_row(*t, {"ProductCode", product_code}, false), 0u);
    return p.serialize();
}

TEST_P(Msi_productops_Test, PatchFunctionParameterRules) {
    file("text.txt", "hello, installer\r\n");
    file("empty.msi", msi::Package::create().serialize());
    const GuestAddr text = wstr(path("text.txt")), db = wstr(path("empty.msi")), missing = wstr(path("missing.msi"));
    const GuestAddr U = wstr(std::string(kUnknown)), bad = wstr(std::string("x")), empty = wstr(std::string());
    // applypatch lines
    EXPECT_EQ(call("MsiApplyPatchW", {I(0), I(0), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(missing), I(0), I(0), I(0)}), 1636u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(text), I(0), I(0), I(0)}), 1636u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(db), I(0), I(0), I(0)}), 1636u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(missing), I(0), I(5), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(missing), I(0), I(1), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(missing), I(0), I(2), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyPatchW", {I(missing), I(U), I(2), I(0)}), 1636u);
    // applymulti lines
    EXPECT_EQ(call("MsiApplyMultiplePatchesW", {I(0), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyMultiplePatchesW", {I(empty), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyMultiplePatchesW", {I(missing), I(0), I(0)}), 2u);
    EXPECT_EQ(call("MsiApplyMultiplePatchesW", {I(missing), I(U), I(0)}), 1605u);
    EXPECT_EQ(call("MsiApplyMultiplePatchesW", {I(missing), I(bad), I(0)}), 87u);
    EXPECT_EQ(call("MsiApplyMultiplePatchesW", {I(text), I(0), I(0)}), 1636u);
    // removepatches lines
    EXPECT_EQ(call("MsiRemovePatchesW", {I(0), I(U), I(2), I(0)}), 87u);
    EXPECT_EQ(call("MsiRemovePatchesW", {I(missing), I(0), I(2), I(0)}), 87u);
    EXPECT_EQ(call("MsiRemovePatchesW", {I(missing), I(U), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiRemovePatchesW", {I(missing), I(U), I(2), I(0)}), 1605u);
    EXPECT_EQ(call("MsiRemovePatchesW", {I(missing), I(bad), I(2), I(0)}), 87u);
    EXPECT_EQ(call("MsiRemovePatchesW", {I(empty), I(U), I(2), I(0)}), 87u);
    // xml lines
    const GuestAddr xml = buffer(512);
    write_int(OUT, 256);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(0), I(0), I(xml), I(OUT)}), 87u);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(missing), I(0), I(xml), I(OUT)}), 2u);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(text), I(0), I(xml), I(OUT)}), 1636u);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(db), I(0), I(xml), I(OUT)}), 1636u);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(db), I(1), I(xml), I(OUT)}), 87u);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(db), I(0), I(xml), I(0)}), 87u);
    EXPECT_EQ(call("MsiExtractPatchXMLDataW", {I(db), I(0), I(0), I(OUT)}), 1636u);
    EXPECT_EQ(read_int(OUT), 256);
    // filelist lines: always success, no records.
    write_int(OUT, 77);
    poison(OUT2, 8, 0x11);
    EXPECT_EQ(call("MsiGetPatchFileListW", {I(U), I(missing), I(OUT), I(OUT2)}), 0u);
    EXPECT_EQ(read_int(OUT), 0);
    EXPECT_EQ(read_handle(OUT2), 0u);
    EXPECT_EQ(call("MsiGetPatchFileListW", {I(0), I(missing), I(OUT), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiGetPatchFileListW", {I(U), I(0), I(0), I(0)}), 0u);
}

TEST_P(Msi_productops_Test, ApplicablePatchesAndSequenceRules) {
    const usize stride = x64() ? 24 : 16, ptr = x64() ? 8 : 4;
    const GuestAddr info = DATA + 0x3000;
    auto entry = [&](u32 i, GuestAddr data, u32 type) {
        if (x64()) ASSERT_TRUE(memory.write_u64(info + i * stride, data).is_ok());
        else ASSERT_TRUE(memory.write_u32(info + i * stride, static_cast<u32>(data)).is_ok());
        write_int(info + i * stride + ptr, static_cast<i32>(type));
        write_int(info + i * stride + ptr + 4, 77);
        write_int(info + i * stride + ptr + 8, 77);
    };
    auto order = [&](u32 i) { return read_int(info + i * stride + ptr + 4); };
    auto status = [&](u32 i) { return read_int(info + i * stride + ptr + 8); };
    const std::string target = "{11111111-2222-3333-4444-555555555555}";
    file("text.txt", "hello, installer\r\n");
    file("empty.msi", msi::Package::create().serialize());
    file("product.msi", product_database(target));
    file("for_target.msp", patch_package(target + ";" + kUnknown));
    file("for_other.msp", patch_package(kUnknown));
    const GuestAddr missing = wstr(path("missing.msi")), text = wstr(path("text.txt"));
    // applicable lines (measured): -1 / 0 in every entry when the package fails.
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(0), I(1), I(info)}), 87u);
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(wstr(path("empty.msi"))), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(wstr(path("empty.msi"))), I(1), I(0)}), 87u);
    entry(0, missing, 0);
    entry(1, text, 0);
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(missing), I(2), I(info)}), 110u);
    EXPECT_EQ(order(0), -1); EXPECT_EQ(status(0), 0); EXPECT_EQ(order(1), -1); EXPECT_EQ(status(1), 0);
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(text), I(2), I(info)}), 110u);
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(wstr(path("empty.msi"))), I(2), I(info)}), 1620u);
    EXPECT_EQ(order(0), -1); EXPECT_EQ(status(0), 0);
    // Learn: applicable patches ERROR_SUCCESS with dwOrder >= 0, inapplicable
    // dwOrder -1 with ERROR_PATCH_TARGET_NOT_FOUND.
    entry(0, wstr(path("for_other.msp")), 0);
    entry(1, wstr(path("for_target.msp")), 0);
    EXPECT_EQ(call("MsiDetermineApplicablePatchesW", {I(wstr(path("product.msi"))), I(2), I(info)}), 0u);
    EXPECT_EQ(order(0), -1); EXPECT_EQ(status(0), 1642);
    EXPECT_EQ(order(1), 0); EXPECT_EQ(status(1), 0);
    // sequence lines
    const GuestAddr U = wstr(std::string(kUnknown));
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(U), I(0), I(4), I(2), I(info)}), 1605u);
    EXPECT_EQ(order(0), -1); EXPECT_EQ(status(0), 0);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(U), I(0), I(2), I(2), I(info)}), 1605u);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(U), I(wstr(std::string("S-1-1-0"))), I(4), I(2), I(info)}), 87u);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(U), I(0), I(7), I(2), I(info)}), 87u);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(U), I(0), I(8), I(2), I(info)}), 87u);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(0), I(0), I(4), I(2), I(info)}), 87u);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(wstr(std::string("x"))), I(0), I(4), I(2), I(info)}), 87u);
    EXPECT_EQ(call("MsiDeterminePatchSequenceW", {I(U), I(0), I(4), I(0), I(0)}), 87u);
}

// ---- Signatures (probe signature lines) -----------------------------------------------------------------------

TEST_P(Msi_productops_Test, FileSignatureInformationUnsignedAndInvalid) {
    file("text.txt", "hello, installer\r\n");
    file("res.dll", fixture("productops_resources.dll"));
    poison(OUT, 8, 0x34);
    const GuestAddr hash = buffer(128);
    write_int(OUT2, 128);
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(wstr(path("text.txt"))), I(0), I(OUT), I(hash), I(OUT2)}), 0x8007065Bu);
    EXPECT_EQ(read_handle(OUT), 0u);
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(wstr(path("missing.msi"))), I(0), I(OUT), I(hash), I(OUT2)}), 0x80092003u);
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(wstr(path("res.dll"))), I(0), I(OUT), I(hash), I(OUT2)}), 0x800B0100u);
    poison(OUT, 8, 0x34);
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(wstr(path("res.dll"))), I(2), I(OUT), I(hash), I(OUT2)}), 0x80070057u);
    EXPECT_NE(read_handle(OUT), 0u);  // untouched
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(0), I(0), I(OUT), I(hash), I(OUT2)}), 0x80070057u);
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(wstr(path("res.dll"))), I(0), I(0), I(0), I(0)}), 0x80070057u);
    EXPECT_EQ(call("MsiGetFileSignatureInformationW", {I(wstr(path("res.dll"))), I(0), I(OUT), I(hash), I(0)}), 0x80070057u);
}

// ---- Transactions (probe begin/join/end lines) ------------------------------------------------------------------

TEST_P(Msi_productops_Test, TransactionLifecycle) {
    u64 events = 0x100;
    msi::ProductOpsPlatform platform;
    platform.create_event = [&](bool, bool) { return events += 4; };
    msi::set_product_ops_platform(mgr, platform);
    const GuestAddr name = wstr(std::string("probe"));
    EXPECT_EQ(call("MsiBeginTransactionW", {I(0), I(0), I(OUT), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiBeginTransactionW", {I(name), I(0), I(0), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiBeginTransactionW", {I(name), I(0), I(OUT), I(0)}), 87u);
    EXPECT_EQ(call("MsiBeginTransactionW", {I(name), I(4), I(OUT), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiEndTransaction", {I(0)}), 5u);
    EXPECT_EQ(call("MsiEndTransaction", {I(1)}), 5u);
    EXPECT_EQ(call("MsiEndTransaction", {I(5)}), 87u);
    EXPECT_EQ(call("MsiBeginTransactionW", {I(name), I(0), I(OUT), I(OUT2)}), 0u);
    const u32 handle = static_cast<u32>(read_int(OUT));
    EXPECT_NE(handle, 0u);
    EXPECT_NE(read_handle(OUT2), 0u);
    EXPECT_TRUE(msi::current_transaction(mgr).active);
    EXPECT_EQ(call("MsiBeginTransactionW", {I(wstr(std::string("probe2"))), I(0), I(OUT3), I(OUT2)}), 1618u);
    EXPECT_EQ(call("MsiJoinTransaction", {I(handle), I(0), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiJoinTransaction", {I(0x7777), I(0), I(OUT2)}), 6u);
    EXPECT_EQ(call("MsiJoinTransaction", {I(0), I(0), I(OUT2)}), 6u);
    EXPECT_EQ(call("MsiJoinTransaction", {I(handle), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiJoinTransaction", {I(handle), I(4), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiEndTransaction", {I(5)}), 87u);
    EXPECT_EQ(call("MsiEndTransaction", {I(0)}), 0u);
    EXPECT_EQ(call("MsiEndTransaction", {I(0)}), 5u);
    EXPECT_EQ(call("MsiCloseHandle", {I(handle)}), 6u);
    EXPECT_EQ(call("MsiBeginTransactionW", {I(name), I(1), I(OUT), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiEndTransaction", {I(1)}), 0u);
    EXPECT_EQ(call("MsiBeginTransactionA", {I(astr("probeA")), I(0), I(OUT), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiEndTransaction", {I(0)}), 0u);
}

// ---- Misc entry points (probe misc lines) ---------------------------------------------------------------------

TEST_P(Msi_productops_Test, DllEntryPointsAndHousekeeping) {
    const GuestAddr v = buffer(32, 0xCC);
    for (auto [size, hr] : {std::pair<i32, u32>{0, 0x80004005u}, {16, 0x80004005u}, {19, 0x80004005u},
                            {20, 0u}, {21, 0u}, {24, 0u}}) {
        poison(v, 32, 0xCC);
        write_int(v, size);
        EXPECT_EQ(call("DllGetVersion", {I(v)}), hr) << size;
        if (hr == 0) {
            EXPECT_EQ(read_int(v + 4), 5); EXPECT_EQ(read_int(v + 8), 0);
            EXPECT_EQ(read_int(v + 12), 10011); EXPECT_EQ(read_int(v + 16), 2);
        } else {
            EXPECT_EQ(static_cast<u32>(read_int(v + 4)), 0xCCCCCCCCu);
        }
    }
    poison(v, 32, 0xCC);
    write_int(v, 32);  // DLLVERSIONINFO2: only the v1 fields
    EXPECT_EQ(call("DllGetVersion", {I(v)}), 0u);
    EXPECT_EQ(static_cast<u32>(read_int(v + 20)), 0xCCCCCCCCu);
    EXPECT_EQ(call("DllCanUnloadNow", {}), 1u);
    const GuestAddr clsid = buffer(16);
    write_ints(clsid, {0x12345678, 0x12341234, 0x04030201, 0x08070605});
    poison(OUT3, 8, 0x11);
    EXPECT_EQ(call("DllGetClassObject", {I(clsid), I(clsid), I(OUT3)}), 0x80040111u);
    EXPECT_EQ(read_handle(OUT3), 0u);
    EXPECT_EQ(call("MsiCreateAndVerifyInstallerDirectory", {I(1)}), 87u);
    EXPECT_EQ(call("MsiCreateAndVerifyInstallerDirectory", {I(0)}), 0u);
    EXPECT_TRUE(std::filesystem::is_directory(root / "Windows" / "Installer"));
    EXPECT_EQ(call("MsiCreateAndVerifyInstallerDirectory", {I(0)}), 0u);
    EXPECT_EQ(call("MsiNotifySidChangeW", {I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiNotifySidChangeW", {I(wstr(std::string("S-1-5-21-1-2-3-1001"))), I(0)}), 87u);
    const GuestAddr U = wstr(std::string(kUnknown));
    EXPECT_EQ(call("Migrate10CachedPackagesW", {I(0), I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(call("Migrate10CachedPackagesW", {I(U), I(wstr(std::string("S-1-1-0"))), I(0), I(0)}), 0u);
}

INSTANTIATE_MSI_BITNESS(Msi_productops_Test);

}  // namespace
}  // namespace vitreo::winapi::msi_test
