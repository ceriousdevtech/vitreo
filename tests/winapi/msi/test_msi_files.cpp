// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_files.cpp
/// @brief Contract tests for the Windows Installer files area: the file
/// versioning rules, MsiFileHash, wildcards, the Word Count source mode, and
/// the file actions run against a package built in memory and a capsule VFS
/// on a temporary directory. Expected values come from Microsoft Learn and
/// tools/oracle_probe/msi_files_probe{32,64}.s{1,2,3}.expected (Windows 11).
/// The rules are bitness-independent (the probe's x86 and x64 runs are
/// identical), so these are plain tests.

#include "msi_fixture.hpp"

#include <winapi/system/msi/msi_engine.hpp>
#include <winapi/system/msi/msi_features.hpp>
#include <winapi/system/msi/msi_files.hpp>
#include <winapi/system/msi/msi_package.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

using namespace vitreo::winapi::msi;

VersionedFile vf(const char* version, std::vector<u16> langs = {}) {
    VersionedFile v;
    v.version = parse_file_version(version);
    v.languages = std::move(langs);
    return v;
}

ExistingFile disk(const char* version, std::vector<u16> langs = {}, u64 created = 100, u64 modified = 100,
                  std::string bytes = {}) {
    ExistingFile f;
    f.exists = true;
    f.info = vf(version, std::move(langs));
    f.created = created;
    f.modified = modified;
    f.bytes.assign(bytes.begin(), bytes.end());
    return f;
}

const ReinstallMode kDefault = parse_reinstall_mode("");

TEST(MsiFilesVersion, ParsesDottedVersionsAndRejectsCompanionKeys) {
    EXPECT_EQ(parse_file_version("2.0.0.0"), (u64{2} << 48));
    EXPECT_EQ(parse_file_version("1.2"), (u64{1} << 48) | (u64{2} << 32));
    EXPECT_EQ(parse_file_version("1.2.3.4"), (u64{1} << 48) | (u64{2} << 32) | (u64{3} << 16) | 4);
    EXPECT_FALSE(parse_file_version("f_vup").has_value());   // a companion parent key
    EXPECT_FALSE(parse_file_version("").has_value());
    EXPECT_FALSE(parse_file_version("1.2.3.4.5").has_value());
    EXPECT_FALSE(parse_file_version("70000.0").has_value());
}

TEST(MsiFilesHash, IsMd5AsLittleEndianDwords) {
    // Measured: Installer.FileHash("MARK:heq_old;") on Windows 11.
    const std::string text = "MARK:heq_old;";
    const auto h = compute_file_hash(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    EXPECT_EQ(h[0], 0x2b26589fu);
    EXPECT_EQ(h[1], 0xce950772u);
    EXPECT_EQ(h[2], 0x1ee01dabu);
    EXPECT_EQ(h[3], 0xfa434cb6u);
}

TEST(MsiFilesWildcard, StarAnyRunQuestionExactlyOneCaseInsensitive) {
    EXPECT_TRUE(wildcard_match("*.tmp", "a.tmp"));
    EXPECT_TRUE(wildcard_match("*.TMP", "b.tmp"));
    EXPECT_FALSE(wildcard_match("*.tmp", "keep.dat"));
    EXPECT_TRUE(wildcard_match("?.log", "1.log"));    // measured: removed
    EXPECT_FALSE(wildcard_match("?.log", "12.log"));  // measured: kept
    EXPECT_TRUE(wildcard_match("*", "anything"));
    EXPECT_TRUE(wildcard_match("a*b*c", "aXXbYYc"));
    EXPECT_FALSE(wildcard_match("a*b*c", "aXXbYY"));
}

TEST(MsiFilesReinstallMode, DefaultIsOmusAndLettersAreCaseInsensitive) {
    EXPECT_TRUE(kDefault.o);
    EXPECT_FALSE(kDefault.e || kDefault.a || kDefault.d || kDefault.p);
    const ReinstallMode m = parse_reinstall_mode("PECMS");
    EXPECT_TRUE(m.p && m.e && m.c);
    EXPECT_FALSE(m.o);
}

// The probe's InstallFiles matrix (msi_files_probe*.s1.expected, tree after
// install) with the default REINSTALLMODE.
TEST(MsiFilesVersioning, MatchesTheMeasuredDefaultMatrix) {
    ExistingFile missing;
    EXPECT_TRUE(should_install_file(vf(""), missing, std::nullopt, false, kDefault));             // f_new
    ExistingFile e = disk("1.0.0.0", {1033});
    EXPECT_TRUE(should_install_file(vf("2.0.0.0", {1033}), e, std::nullopt, false, kDefault));    // vup
    e = disk("2.0.0.0", {1033});
    EXPECT_FALSE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, kDefault));   // vdown
    e = disk("1.0.0.0", {1033});
    EXPECT_FALSE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, kDefault));   // veq
    e = disk("1.0.0.0", {1033});
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {1031}), e, std::nullopt, false, kDefault));    // lkeep
    e = disk("1.0.0.0", {1031});
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, kDefault));    // lrepl
    e = disk("", {}, 100, 200);
    EXPECT_FALSE(should_install_file(vf(""), e, std::nullopt, false, kDefault));                  // umod
    e = disk("", {}, 100, 100);
    EXPECT_TRUE(should_install_file(vf(""), e, std::nullopt, false, kDefault));                   // usame
    e = disk("", {}, 200, 100);
    EXPECT_TRUE(should_install_file(vf(""), e, std::nullopt, false, kDefault));                   // uold
    e = disk("", {}, 100, 100);
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, kDefault));    // vu
    e = disk("1.0.0.0", {1033});
    EXPECT_FALSE(should_install_file(vf(""), e, std::nullopt, false, kDefault));                  // uv
}

TEST(MsiFilesVersioning, MsiFileHashSkipsIdenticalBytesElseTheDateRuleDecides) {
    const std::string old_text = "MARK:heq_old;";
    const auto same = compute_file_hash(std::span<const u8>(reinterpret_cast<const u8*>(old_text.data()), old_text.size()));
    std::array<u32, 4> other = same;
    other[0] ^= 1;
    ExistingFile e = disk("", {}, 100, 200, old_text);
    EXPECT_FALSE(should_install_file(vf(""), e, same, false, kDefault));   // heq
    e = disk("", {}, 100, 200, old_text);
    EXPECT_FALSE(should_install_file(vf(""), e, other, false, kDefault));  // hne: user-modified, kept
    e = disk("", {}, 100, 100, old_text);
    EXPECT_TRUE(should_install_file(vf(""), e, other, false, kDefault));   // hsame
    e = disk("", {}, 100, 100, old_text);
    EXPECT_FALSE(should_install_file(vf(""), e, same, false, kDefault));   // hash match beats the date rule
}

TEST(MsiFilesVersioning, LearnReplacingExistingFilesLanguageExamples) {
    constexpr u16 ENG = 1033, FRN = 1036, SPN = 1034, GER = 1031, ITN = 1040;
    auto sorted = [](std::vector<u16> v) { std::sort(v.begin(), v.end()); return v; };
    ExistingFile e = disk("1.0.0.0", sorted({ENG, FRN, SPN}));
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", sorted({ITN, ENG, GER})), e, std::nullopt, false, kDefault));  // FileH
    e = disk("1.0.0.0", sorted({ENG, FRN}));
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", sorted({ENG, FRN, SPN})), e, std::nullopt, false, kDefault));  // FileI
    e = disk("1.0.0.0", sorted({ENG, GER, ITN}));
    EXPECT_FALSE(should_install_file(vf("1.0.0.0", {GER}), e, std::nullopt, false, kDefault));                   // FileJ
    // "Both Files Have a Version": a language replaces no language; none does
    // not replace none.
    e = disk("1.0.0.0", {});
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {ENG}), e, std::nullopt, false, kDefault));
    e = disk("1.0.0.0", {});
    EXPECT_FALSE(should_install_file(vf("1.0.0.0", {}), e, std::nullopt, false, kDefault));
}

TEST(MsiFilesVersioning, ReinstallModeLettersOverrideTheDefaults) {
    ExistingFile e = disk("1.0.0.0", {1033});
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, parse_reinstall_mode("emus")));
    e = disk("2.0.0.0", {1033});
    EXPECT_FALSE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, parse_reinstall_mode("emus")));
    e = disk("2.0.0.0", {1033});
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, parse_reinstall_mode("dmus")));
    e = disk("2.0.0.0", {1033});
    EXPECT_TRUE(should_install_file(vf("1.0.0.0", {1033}), e, std::nullopt, false, parse_reinstall_mode("amus")));
    e = disk("1.0.0.0", {1033});
    EXPECT_FALSE(should_install_file(vf("2.0.0.0", {1033}), e, std::nullopt, false, parse_reinstall_mode("pmus")));
    ExistingFile missing;
    EXPECT_TRUE(should_install_file(vf("2.0.0.0"), missing, std::nullopt, false, parse_reinstall_mode("pmus")));
}

TEST(MsiFilesVersioning, CompanionFollowsItsParent) {
    // Parent replaced (measured compup: the user-modified companion is
    // replaced with it).
    ExistingFile parent = disk("1.0.0.0", {1033});
    ExistingFile comp = disk("", {}, 100, 200);
    EXPECT_TRUE(should_install_companion(vf("2.0.0.0", {1033}), parent, comp, kDefault));
    // Parent on disk is newer: skipped.
    parent = disk("3.0.0.0", {1033});
    ExistingFile none;
    EXPECT_FALSE(should_install_companion(vf("2.0.0.0", {1033}), parent, none, kDefault));
    // Same parent version and language, companion missing: installed.
    parent = disk("2.0.0.0", {1033});
    EXPECT_TRUE(should_install_companion(vf("2.0.0.0", {1033}), parent, none, kDefault));
    // Same parent version, companion present, REINSTALLMODE o: installed.
    parent = disk("2.0.0.0", {1033});
    EXPECT_TRUE(should_install_companion(vf("2.0.0.0", {1033}), parent, comp, kDefault));
    parent = disk("2.0.0.0", {1033});
    EXPECT_FALSE(should_install_companion(vf("2.0.0.0", {1033}), parent, comp, parse_reinstall_mode("pmus")));
}

TEST(MsiFilesVersioning, PeChecksumOfNonPeIsUnknown) {
    const std::vector<u8> text = {'h', 'i'};
    EXPECT_FALSE(pe_checksum_valid(text).has_value());
}

// ---- actions against a package built in memory --------------------------------

/// A cabinet with one uncompressed folder (typeCompress 0, [MS-CAB]).
std::vector<u8> make_cabinet(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<u8> out;
    auto u16le = [&](u32 v) { out.push_back(static_cast<u8>(v)); out.push_back(static_cast<u8>(v >> 8)); };
    auto u32le = [&](u32 v) { u16le(v & 0xFFFF); u16le(v >> 16); };
    std::string data;
    for (const auto& f : files) data += f.second;
    const u32 blocks = static_cast<u32>((data.size() + 32767) / 32768);
    u32 files_size = 0;
    for (const auto& f : files) files_size += 16 + static_cast<u32>(f.first.size()) + 1;
    const u32 header = 36, folder = 8, files_at = header + folder, data_at = files_at + files_size;
    const u32 total = data_at + blocks * 8 + static_cast<u32>(data.size());
    out.insert(out.end(), {'M', 'S', 'C', 'F'});
    u32le(0); u32le(total); u32le(0); u32le(files_at); u32le(0);
    out.push_back(3); out.push_back(1);
    u16le(1); u16le(static_cast<u32>(files.size())); u16le(0); u16le(0x1234); u16le(0);
    u32le(data_at); u16le(blocks); u16le(0);
    u32 offset = 0;
    for (const auto& f : files) {
        u32le(static_cast<u32>(f.second.size())); u32le(offset); u16le(0); u16le(0x5B21); u16le(0x6000); u16le(0x20);
        out.insert(out.end(), f.first.begin(), f.first.end());
        out.push_back(0);
        offset += static_cast<u32>(f.second.size());
    }
    for (u32 b = 0; b < blocks; ++b) {
        const usize at = static_cast<usize>(b) * 32768;
        const u32 n = static_cast<u32>(std::min<usize>(32768, data.size() - at));
        u32le(0); u16le(n); u16le(n);
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
    return out;
}

/// Summary information stream holding only PID_WORDCOUNT ([MS-OLEPS]).
std::vector<u8> make_summary(i32 word_count) {
    std::vector<u8> s(28, 0);
    s[0] = 0xFE; s[1] = 0xFF;
    s[24] = 1;
    auto u32le = [&](u32 v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<u8>(v >> (8 * i))); };
    const u8 fmtid[16] = {0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10, 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9};
    s.insert(s.end(), fmtid, fmtid + 16);
    u32le(48);       // section offset
    u32le(8 + 8 + 8);  // section size
    u32le(1);        // one property
    u32le(15); u32le(16);
    u32le(3); u32le(static_cast<u32>(word_count));
    return s;
}

struct RecordingUi : UiHost {
    struct Msg { u32 type; std::vector<std::string> fields; };
    std::vector<Msg> messages;
    i32 error_answer{0};
    [[nodiscard]] u32 level() const override { return 2; }
    u32 run_dialog(Session&, const std::string&) override { return 0; }
    i32 message(Session&, u32 type, const Record& r) override {
        Msg m{type, {}};
        for (u32 f = 0; f <= r.count(); ++f) {
            const auto& fld = r.fields[f];
            if (fld.kind == Record::Kind::Integer) m.fields.push_back(std::to_string(fld.integer));
            else if (fld.kind == Record::Kind::String) m.fields.push_back(std::string(fld.text.begin(), fld.text.end()));
            else m.fields.push_back("<null>");
        }
        messages.push_back(std::move(m));
        return (type & 0xFF000000u) == kMsgError ? error_answer : 0;
    }
    [[nodiscard]] std::vector<Msg> of(u32 kind) const {
        std::vector<Msg> out;
        for (const Msg& m : messages) if ((m.type & 0xFF000000u) == kind) out.push_back(m);
        return out;
    }
};

class MsiFilesActions : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / ("vitreo_msi_files_" + std::to_string(::getpid()) + "_" +
                                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        services.vfs = &vfs;
        services.ui = &ui;
        // These are first installs: no product is registered. An earlier
        // test's process-wide provider may point at a destroyed registry.
        set_installed_state_provider(InstalledStateProvider{});
        pkg = Package::create();
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    void table(const std::string& name, std::vector<ColumnDef> cols) { ASSERT_EQ(pkg.create_table(name, std::move(cols), false), 0u); }
    void row(const std::string& name, std::vector<std::string> cells) {
        Table* t = pkg.find_table(name);
        ASSERT_NE(t, nullptr) << name;
        ASSERT_EQ(pkg.insert_row(*t, std::move(cells), false), 0u) << name;
    }
    static ColumnDef s(const char* n, u32 size, bool nullable = false, bool key = false) { return make_column(n, 's', size, nullable, key, false, false); }
    static ColumnDef i(const char* n, u32 size, bool nullable = false, bool key = false) { return make_column(n, 'i', size, nullable, key, false, false); }

    /// The standard schema plus one feature and the directories the tests use.
    void base_package(i32 word_count) {
        table("Property", {s("Property", 72, false, true), s("Value", 0)});
        table("Directory", {s("Directory", 72, false, true), s("Directory_Parent", 72, true), s("DefaultDir", 255)});
        table("Feature", {s("Feature", 38, false, true), s("Feature_Parent", 38, true), s("Title", 64, true),
                          s("Description", 255, true), i("Display", 2, true), i("Level", 2), s("Directory_", 72, true),
                          i("Attributes", 2)});
        table("FeatureComponents", {s("Feature_", 38, false, true), s("Component_", 72, false, true)});
        table("Component", {s("Component", 72, false, true), s("ComponentId", 38, true), s("Directory_", 72),
                            i("Attributes", 2), s("Condition", 255, true), s("KeyPath", 72, true)});
        table("File", {s("File", 72, false, true), s("Component_", 72), s("FileName", 255), i("FileSize", 4),
                       s("Version", 72, true), s("Language", 20, true), i("Attributes", 2, true), i("Sequence", 4)});
        table("Media", {i("DiskId", 2, false, true), i("LastSequence", 4), s("DiskPrompt", 64, true),
                        s("Cabinet", 255, true), s("VolumeLabel", 32, true), s("Source", 72, true)});
        row("Property", {"ProductCode", "{5D3A1F0E-7C42-4B8E-9A61-2F0B7E9C4D11}"});
        row("Property", {"ProductLanguage", "1033"});
        row("Directory", {"TARGETDIR", "", "SourceDir"});
        row("Directory", {"INSTALLDIR", "TARGETDIR", "app|AppDir:srcapp"});
        row("Directory", {"MADE", "INSTALLDIR", "made"});
        row("Directory", {"DUPDIR", "TARGETDIR", "dupd"});
        row("Feature", {"Main", "", "Main", "", "1", "1", "", "0"});
        pkg.set_stream("\x05SummaryInformation", make_summary(word_count));
    }
    void component(const std::string& name, const std::string& key_file, u32 attributes = 0) {
        row("Component", {name, "{6A0E3B10-0000-4000-8000-0000000000" + std::to_string(10 + n_components++) + "}",
                          "INSTALLDIR", std::to_string(attributes), "", key_file});
        row("FeatureComponents", {"Main", name});
    }
    void file(const std::string& key, const std::string& comp, const std::string& name, usize size, u32 seq,
              u32 attributes = 0, const std::string& version = "") {
        row("File", {key, comp, name, std::to_string(size), version, "", std::to_string(attributes), std::to_string(seq)});
    }

    /// Open an install session (an Engine that may change the system), run
    /// the costing actions (features area) and open the execution script.
    void start(const std::string& target = "C:\\t\\") {
        Engine::Options o;
        o.package_path = "C:\\src\\pkg.msi";
        o.ui_level = 2;  // INSTALLUILEVEL_NONE
        o.server = true;
        o.allow_system_changes = true;
        o.command_line["TARGETDIR"] = target;
        u32 error = 0;
        engine = Engine::open(mgr, std::make_shared<Package>(std::move(pkg)), o, services, error);
        ASSERT_TRUE(engine) << error;
        session = &engine->session();
        session->set_property("OriginalDatabase", "C:\\src\\pkg.msi");
        ctx = std::make_unique<ActionContext>(*session, engine->services());
        ASSERT_EQ(action_cost_initialize(*ctx), kActionSuccess);
        ASSERT_EQ(action_file_cost(*ctx), kActionSuccess);
        ASSERT_EQ(action_cost_finalize(*ctx), kActionSuccess);
        ASSERT_TRUE(engine->begin_script());
    }
    /// InstallFinalize: run the script (rollback on failure, then commit).
    u32 run_script() { return engine->execute_script(true); }

    void write_host(const std::string& rel, const std::string& text) {
        const auto p = root / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
    [[nodiscard]] std::string read_host(const std::string& rel) const {
        std::ifstream in(root / rel, std::ios::binary);
        if (!in) return "<missing>";
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
    [[nodiscard]] bool host_exists(const std::string& rel) const { return std::filesystem::exists(root / rel); }

    std::filesystem::path root;
    capsule::Vfs vfs;
    RecordingUi ui;
    Services services;
    Package pkg;
    MsiManager mgr;
    std::shared_ptr<Engine> engine;
    Session* session{nullptr};
    std::unique_ptr<ActionContext> ctx;
    int n_components{0};
};

TEST_F(MsiFilesActions, InstallFilesExtractsEmbeddedCabinetAndReportsLikeWindows) {
    base_package(2);  // compressed source (Word Count bit 1)
    component("C_a", "f_a");
    component("C_b", "f_b");
    file("f_a", "C_a", "a.txt", 5, 1);
    file("f_b", "C_b", "bshort.txt|b long.txt", 6, 2, kFileReadOnly | kFileHidden);
    row("Media", {"1", "2", "", "#emb.cab", "", ""});
    pkg.set_stream("emb.cab", make_cabinet({{"f_a", "hello"}, {"f_b", "world!"}}));
    start();
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    EXPECT_FALSE(host_exists("t/AppDir/a.txt"));  // scheduled, not yet run
    ASSERT_EQ(run_script(), 0u);
    EXPECT_EQ(read_host("t/AppDir/a.txt"), "hello");
    EXPECT_EQ(read_host("t/AppDir/b long.txt"), "world!");
    // File.Attributes read-only + hidden are applied (measured attr.txt 0x3).
    auto st = vfs.stat("C:\\t\\AppDir\\b long.txt");
    ASSERT_TRUE(st.is_ok());
    EXPECT_TRUE(capsule::has_attribute(st.value().attributes, capsule::FileAttributes::ReadOnly));

    const auto data = ui.of(kMsgActionData);
    ASSERT_EQ(data.size(), 2u);
    ASSERT_EQ(data[0].fields.size(), 10u);  // fields 0..9
    EXPECT_EQ(data[0].fields[1], "a.txt");
    EXPECT_EQ(data[0].fields[6], "5");
    EXPECT_EQ(data[0].fields[9], "C:\\t\\AppDir\\");
    EXPECT_EQ(data[0].fields[2], "<null>");
    const auto prog = ui.of(kMsgProgress);
    ASSERT_EQ(prog.size(), 3u);
    EXPECT_EQ(prog[0].fields, (std::vector<std::string>{"<null>", "1", "1", "0", "0"}));
    EXPECT_EQ(prog[1].fields, (std::vector<std::string>{"<null>", "2", "5", "0", "0"}));
    EXPECT_EQ(prog[2].fields, (std::vector<std::string>{"<null>", "2", "6", "0", "0"}));
}

TEST_F(MsiFilesActions, LooseFileOfACompressedPackageComesFromTheSourceRoot) {
    base_package(2);
    component("C_l", "f_l");
    file("f_l", "C_l", "loose.txt", 4, 1, kFileNoncompressed);
    row("Media", {"1", "1", "", "", "", ""});
    write_host("src/loose.txt", "root");
    write_host("src/srcapp/loose.txt", "tree");
    start();
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    EXPECT_EQ(read_host("t/AppDir/loose.txt"), "root");
}

TEST_F(MsiFilesActions, UncompressedPackageReadsTheSourceTree) {
    base_package(0);
    component("C_l", "f_l");
    file("f_l", "C_l", "tree.txt", 4, 1);
    row("Media", {"1", "1", "", "", "", ""});
    write_host("src/srcapp/tree.txt", "tree");
    start();
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    EXPECT_EQ(read_host("t/AppDir/tree.txt"), "tree");
}

TEST_F(MsiFilesActions, MissingSourceFailsTheInstallInQuietUi) {
    // Measured (s2 / s3): vital or not, a missing source in a quiet install
    // ends the install with 1603 after error 1308.
    base_package(0);
    component("C_l", "f_l");
    file("f_l", "C_l", "gone.txt", 4, 1);
    row("Media", {"1", "1", "", "", "", ""});
    start();
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    EXPECT_EQ(run_script(), 1603u);
    const auto errors = ui.of(kMsgError);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].fields[1], "1308");
    EXPECT_EQ(errors[0].fields[2], "C:\\src\\srcapp\\gone.txt");
    EXPECT_EQ(errors[0].type & 0xF, 0x2u);  // MB_ABORTRETRYIGNORE for a non-vital file
}

TEST_F(MsiFilesActions, IgnoredNonVitalFileLetsTheRestInstall) {
    base_package(0);
    component("C_a", "f_a");
    component("C_b", "f_b");
    file("f_a", "C_a", "gone.txt", 4, 1);
    file("f_b", "C_b", "here.txt", 4, 2);
    row("Media", {"1", "2", "", "", "", ""});
    write_host("src/srcapp/here.txt", "here");
    ui.error_answer = 5;  // IDIGNORE
    start();
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    EXPECT_FALSE(host_exists("t/AppDir/gone.txt"));
    EXPECT_EQ(read_host("t/AppDir/here.txt"), "here");
}

TEST_F(MsiFilesActions, UserModifiedUnversionedFileIsKept) {
    base_package(0);
    component("C_u", "f_u");
    file("f_u", "C_u", "data.txt", 3, 1);
    row("Media", {"1", "1", "", "", "", ""});
    write_host("src/srcapp/data.txt", "new");
    write_host("t/AppDir/data.txt", "mine");
    // created 2020, modified 2021 (FILETIME seconds * 1e7 since 1601).
    auto opened = vfs.open("C:\\t\\AppDir\\data.txt", capsule::AccessMode::WriteMetadata, capsule::ShareMode::All,
                           capsule::CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());
    const u64 created = 132223104000000000ull, modified = 132539328000000000ull;
    (void)vfs.set_file_times_ft(opened.value(), &created, nullptr, &modified);
    (void)vfs.close(opened.value());
    auto st = vfs.stat("C:\\t\\AppDir\\data.txt");
    ASSERT_TRUE(st.is_ok());
    if (st.value().creation_time >= st.value().last_write_time) GTEST_SKIP() << "host FS keeps no creation time";
    start();
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    EXPECT_EQ(read_host("t/AppDir/data.txt"), "mine");
    EXPECT_TRUE(ui.of(kMsgActionData).empty());  // kept files are not reported
}

TEST_F(MsiFilesActions, VersionedFilesFollowTheOnDiskVersionResource) {
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures";
    ExistingFile v2 = probe_existing_file(vfs, "C:\\nothing");
    EXPECT_FALSE(v2.exists);
    std::filesystem::create_directories(root / "x");
    std::filesystem::copy_file(fixtures / "files_v2_1033.dll", root / "x" / "v2.dll");
    std::filesystem::copy_file(fixtures / "files_v1_1031.dll", root / "x" / "v1.dll");
    v2 = probe_existing_file(vfs, "C:\\x\\v2.dll");
    ASSERT_TRUE(v2.exists);
    EXPECT_EQ(v2.info.version, parse_file_version("2.0.0.0"));
    EXPECT_EQ(v2.info.languages, (std::vector<u16>{1033}));
    ExistingFile v1 = probe_existing_file(vfs, "C:\\x\\v1.dll");
    EXPECT_EQ(v1.info.version, parse_file_version("1.0.0.0"));
    EXPECT_EQ(v1.info.languages, (std::vector<u16>{1031}));
}

TEST_F(MsiFilesActions, RemoveFilesHonoursModesWildcardsLongNamesAndEmptyFolders) {
    base_package(0);
    table("RemoveFile", {s("FileKey", 72, false, true), s("Component_", 72), s("FileName", 255, true),
                         s("DirProperty", 72), i("InstallMode", 2)});
    component("C_new", "");
    row("Directory", {"EMPTYDIR", "INSTALLDIR", "emptyd"});
    row("RemoveFile", {"rf_tmp", "C_new", "*.tmp", "INSTALLDIR", "1"});
    row("RemoveFile", {"rf_q", "C_new", "?.log", "INSTALLDIR", "1"});
    row("RemoveFile", {"rf_onrem", "C_new", "gone.dat", "INSTALLDIR", "2"});
    row("RemoveFile", {"rf_dir", "C_new", "", "EMPTYDIR", "3"});
    row("RemoveFile", {"rf_short", "C_new", "short~1.dat|longname.dat", "INSTALLDIR", "1"});
    for (const char* f : {"a.tmp", "b.tmp", "keep.dat", "1.log", "12.log", "gone.dat", "longname.dat", "short~1.dat"})
        write_host(std::string("t/AppDir/") + f, "x");
    std::filesystem::create_directories(root / "t/AppDir/emptyd");
    start();
    ASSERT_EQ(action_remove_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    // The probe's tree after install.
    for (const char* gone : {"a.tmp", "b.tmp", "1.log", "longname.dat"}) EXPECT_FALSE(host_exists(std::string("t/AppDir/") + gone)) << gone;
    for (const char* kept : {"keep.dat", "12.log", "gone.dat", "short~1.dat"}) EXPECT_TRUE(host_exists(std::string("t/AppDir/") + kept)) << kept;
    EXPECT_FALSE(host_exists("t/AppDir/emptyd"));
    const auto data = ui.of(kMsgActionData);
    ASSERT_EQ(data.size(), 5u);
    EXPECT_EQ(data[0].fields[1], "a.tmp");
    EXPECT_EQ(data[0].fields[9], "C:\\t\\AppDir\\");
    EXPECT_EQ(data[4].fields.size(), 2u);  // the folder: one field
    EXPECT_EQ(data[4].fields[1], "C:\\t\\AppDir\\emptyd\\");
    const auto prog = ui.of(kMsgProgress);
    ASSERT_GE(prog.size(), 2u);
    EXPECT_EQ(prog[0].fields, (std::vector<std::string>{"<null>", "1", "175000", "1", "0"}));
    EXPECT_EQ(prog[1].fields, (std::vector<std::string>{"<null>", "2", "0"}));
}

TEST_F(MsiFilesActions, MoveFilesMovesCopiesAndSkipsUnresolvedDestinations) {
    base_package(0);
    table("MoveFile", {s("FileKey", 72, false, true), s("Component_", 72), s("SourceName", 255, true),
                       s("DestName", 255, true), s("SourceFolder", 72, true), s("DestFolder", 72), i("Options", 2)});
    component("C_new", "");
    row("Directory", {"MVSRC", "TARGETDIR", "mvsrc"});
    row("Directory", {"MVDST", "TARGETDIR", "mvdst"});
    row("MoveFile", {"mv_xls", "C_new", "*.xls", "renamed.xls", "MVSRC", "MVDST", "1"});
    row("MoveFile", {"mv_copy", "C_new", "c.txt", "c2.txt", "MVSRC", "MVDST", "0"});
    row("MoveFile", {"mv_full", "C_new", "", "", "MVFULL", "MVDST", "0"});
    row("MoveFile", {"mv_nodst", "C_new", "d.txt", "", "MVSRC", "NOSUCHPROP", "1"});
    for (const char* f : {"a.xls", "b.xls", "c.txt", "d.txt", "single.dat"}) write_host(std::string("t/mvsrc/") + f, f);
    start();
    session->set_property("MVFULL", "C:\\t\\mvsrc\\single.dat");
    ASSERT_EQ(action_move_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    EXPECT_EQ(read_host("t/mvdst/renamed.xls"), "a.xls");
    EXPECT_EQ(read_host("t/mvdst/b.xls"), "b.xls");
    EXPECT_EQ(read_host("t/mvdst/c2.txt"), "c.txt");
    EXPECT_EQ(read_host("t/mvdst/single.dat"), "single.dat");
    EXPECT_FALSE(host_exists("t/mvsrc/a.xls"));
    EXPECT_FALSE(host_exists("t/mvsrc/b.xls"));
    EXPECT_TRUE(host_exists("t/mvsrc/c.txt"));
    EXPECT_TRUE(host_exists("t/mvsrc/d.txt"));
    EXPECT_TRUE(host_exists("t/mvsrc/single.dat"));
    const auto data = ui.of(kMsgActionData);
    ASSERT_EQ(data.size(), 4u);
    EXPECT_EQ(data[0].fields[1], "renamed.xls");
    EXPECT_EQ(data[0].fields[6], "5");
    EXPECT_EQ(data[0].fields[9], "C:\\t\\mvdst\\");
}

TEST_F(MsiFilesActions, DuplicateFilesAndCreateFolders) {
    base_package(0);
    table("DuplicateFile", {s("FileKey", 72, false, true), s("Component_", 72), s("File_", 72),
                            s("DestName", 255, true), s("DestFolder", 72, true)});
    table("CreateFolder", {s("Directory_", 72, false, true), s("Component_", 72, false, true)});
    component("C_a", "f_a");
    file("f_a", "C_a", "a.txt", 5, 1);
    row("Media", {"1", "1", "", "", "", ""});
    row("DuplicateFile", {"dup1", "C_a", "f_a", "dupname.txt", "DUPDIR"});
    row("DuplicateFile", {"dup2", "C_a", "f_a", "", "DUPDIR"});
    row("DuplicateFile", {"dup3", "C_a", "f_a", "", ""});
    row("CreateFolder", {"MADE", "C_a"});
    write_host("src/srcapp/a.txt", "hello");
    start();
    ASSERT_EQ(action_create_folders(*ctx), kActionSuccess);
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    ASSERT_EQ(action_duplicate_files(*ctx), kActionSuccess);
    ASSERT_EQ(run_script(), 0u);
    EXPECT_TRUE(std::filesystem::is_directory(root / "t/AppDir/made"));
    EXPECT_EQ(read_host("t/dupd/dupname.txt"), "hello");
    EXPECT_EQ(read_host("t/dupd/a.txt"), "hello");
    const auto data = ui.of(kMsgActionData);
    ASSERT_EQ(data.size(), 4u);  // folder, file, two duplicates
    EXPECT_EQ(data[0].fields, (std::vector<std::string>{"<null>", "C:\\t\\AppDir\\made\\"}));
    EXPECT_EQ(data[2].fields[1], "dupname.txt");
    EXPECT_EQ(data[2].fields[9], "C:\\t\\dupd\\");
}

TEST_F(MsiFilesActions, AFailedScriptRollsBackEveryFileChange) {
    // Measured (s2): after a failed InstallFiles the removed, moved and
    // replaced files are back and the new files are gone.
    base_package(0);
    table("RemoveFile", {s("FileKey", 72, false, true), s("Component_", 72), s("FileName", 255, true),
                         s("DirProperty", 72), i("InstallMode", 2)});
    component("C_a", "f_a");
    component("C_b", "f_b");
    file("f_a", "C_a", "a.txt", 5, 1);
    file("f_b", "C_b", "missing.txt", 4, 2, kFileVital);
    row("Media", {"1", "2", "", "", "", ""});
    row("RemoveFile", {"rf_tmp", "C_a", "*.tmp", "INSTALLDIR", "1"});
    write_host("src/srcapp/a.txt", "hello");
    write_host("t/AppDir/old.tmp", "tmp");
    write_host("t/AppDir/a.txt", "previous");
    start();
    ASSERT_EQ(action_remove_files(*ctx), kActionSuccess);
    ASSERT_EQ(action_install_files(*ctx), kActionSuccess);
    EXPECT_EQ(run_script(), 1603u);
    EXPECT_EQ(read_host("t/AppDir/old.tmp"), "tmp");
    EXPECT_EQ(read_host("t/AppDir/a.txt"), "previous");
    EXPECT_FALSE(host_exists("t/AppDir/missing.txt"));
    const auto errors = ui.of(kMsgError);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].type & 0xF, 0x5u);  // MB_RETRYCANCEL for a vital file
}

TEST_F(MsiFilesActions, OutsideTheScriptASystemChangeIs1603) {
    base_package(0);
    component("C_a", "f_a");
    file("f_a", "C_a", "a.txt", 5, 1);
    row("Media", {"1", "1", "", "", "", ""});
    start();
    ASSERT_EQ(run_script(), 0u);  // closes the script
    EXPECT_EQ(action_install_files(*ctx), kActionFailure);
}

TEST_F(MsiFilesActions, ResolveSourceSetsTheMeasuredProperties) {
    base_package(0);
    start();
    session->set_property("SourceDir", "");
    ASSERT_EQ(action_resolve_source(*ctx), kActionSuccess);
    EXPECT_EQ(session->property("SourceDir"), "C:\\src\\");
    EXPECT_EQ(session->property("SOURCEDIR"), "C:\\src\\");
    EXPECT_EQ(session->property("SourcedirProduct"), "{5D3A1F0E-7C42-4B8E-9A61-2F0B7E9C4D11}");
}

TEST_F(MsiFilesActions, WordCountSelectsTheSourceMode) {
    base_package(3);
    start();
    const SourceMode m = source_mode(*session);
    EXPECT_TRUE(m.short_names);
    EXPECT_TRUE(m.compressed);
    EXPECT_FALSE(m.admin_image);
}

}  // namespace
}  // namespace vitreo::winapi::msi_test
