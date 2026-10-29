// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_search.cpp
/// @brief Contract tests for the Windows Installer search area: the
/// registry value encodings, Signature filters, .ini editing, AppSearch /
/// CCPSearch / RMCCPSearch over every locator, and the registry, .ini,
/// environment and service script ops.
///
/// Expected values are Windows 11's, measured by
/// tools/oracle_probe/msi_search_probe.c (msi_search32/64.expected) and
/// msi_search_install_probe.c (msi_search_install_*.expected); the case
/// names in comments are the probe's property / value names.

#include "msi_fixture.hpp"

#include <capsule/registry/registry.hpp>
#include <winapi/advapi32/services/scm.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/system/msi/msi_engine.hpp>
#include <winapi/system/msi/msi_package.hpp>
#include <winapi/system/msi/msi_search.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

using namespace std::string_literals;
using capsule::RegType;
using capsule::RegValue;
using msi::EncodedRegistryValue;

std::optional<std::string> raw(const RegValue& v) {
    return msi::format_raw_registry_value(v, [](const std::string& name) -> std::optional<std::string> {
        if (name == "SystemRoot") return "C:\\WINDOWS"s;
        if (name == "SystemDrive") return "C:"s;
        return std::nullopt;
    });
}

// ---- RegLocator raw values (msi_search_probe R64_* lines) -----------------------

TEST(MsiSearchRawValue, StringsEscapeALeadingHashAndEmptyIsUnset) {
    EXPECT_EQ(raw(RegValue{RegType::Sz, L"plain text"s}), "plain text");
    EXPECT_EQ(raw(RegValue{RegType::Sz, L"#hash"s}), "##hash");
    EXPECT_EQ(raw(RegValue{RegType::Sz, L"##two"s}), "###two");
    EXPECT_EQ(raw(RegValue{RegType::Sz, L"a[~]b"s}), "a[~]b");
    EXPECT_EQ(raw(RegValue{RegType::Sz, L""s}), std::nullopt);
}

TEST(MsiSearchRawValue, DwordIsSignedDecimalWithHash) {
    EXPECT_EQ(raw(RegValue{RegType::Dword, u32{42}}), "#42");
    EXPECT_EQ(raw(RegValue{RegType::Dword, u32{0xFFFFFFFE}}), "#-2");
    EXPECT_EQ(raw(RegValue{RegType::Dword, u32{0x80000000}}), "#-2147483648");
    EXPECT_EQ(raw(RegValue{RegType::Dword, u32{0}}), "#0");
    // R_NT64 / R_NT32: CurrentMajorVersionNumber, the LaunchCondition input.
    EXPECT_EQ(raw(RegValue{RegType::Dword, u32{10}}), "#10");
}

TEST(MsiSearchRawValue, ExpandStringIsExpandedWithoutPrefix) {
    EXPECT_EQ(raw(RegValue{RegType::ExpandSz, L"%SystemRoot%\\probe"s}), "C:\\WINDOWS\\probe");
    EXPECT_EQ(raw(RegValue{RegType::ExpandSz, L"%NoSuchVar%\\x"s}), "%NoSuchVar%\\x");
}

TEST(MsiSearchRawValue, MultiStringIsNulPrefixedItemsAndATrailingNul) {
    EXPECT_EQ(raw(RegValue{RegType::MultiSz, std::vector<std::wstring>{L"a", L"b", L"c"}}),
              std::string("\0a\0b\0c\0", 7));
    EXPECT_EQ(raw(RegValue{RegType::MultiSz, std::vector<std::wstring>{L"solo"}}), std::string("\0solo\0", 6));
    EXPECT_EQ(raw(RegValue{RegType::MultiSz, std::vector<std::wstring>{}}), std::string("\0", 1));
}

TEST(MsiSearchRawValue, BinaryIsUppercaseHexAndEmptyIsUnset) {
    EXPECT_EQ(raw(RegValue{RegType::Binary, std::vector<u8>{0x01, 0xAB, 0xFF}}), "#x01ABFF");
    EXPECT_EQ(raw(RegValue{RegType::Binary, std::vector<u8>{}}), std::nullopt);
}

TEST(MsiSearchRawValue, OtherTypesAreTheBytesAsUtf16) {
    // R64_QWORD = \x7788\x5566\x3344\x1122, R64_NONE = \xab01,
    // R64_DWORDBE = \x5678\x1234.
    const auto q = raw(RegValue{RegType::Qword, u64{0x1122334455667788ull}});
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(*q, "\xE7\x9E\x88\xE5\x95\xA6\xE3\x8D\x84\xE1\x84\xA2");
    RegValue none{RegType::Raw, std::vector<u8>{0x01, 0xAB}};
    none.raw_type = 0;
    EXPECT_EQ(raw(none), "\xEA\xAC\x81");
    RegValue be{RegType::Raw, std::vector<u8>{0x78, 0x56, 0x34, 0x12}};
    be.raw_type = 5;
    EXPECT_EQ(raw(be), "\xE5\x99\xB8\xE1\x88\xB4");
}

// ---- Registry.Value encodings (msi_search_install_probe) ----------------------

TEST(MsiSearchEncode, PrefixesSelectTheType) {
    auto e = msi::encode_registry_value("#42");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->value.type, RegType::Dword);
    EXPECT_EQ(std::get<u32>(e->value.data), 42u);
    e = msi::encode_registry_value("#-5");
    EXPECT_EQ(std::get<u32>(e->value.data), 0xFFFFFFFBu);
    e = msi::encode_registry_value("#+7");
    EXPECT_EQ(std::get<u32>(e->value.data), 7u);
    e = msi::encode_registry_value("#4294967295");
    EXPECT_EQ(std::get<u32>(e->value.data), 0xFFFFFFFFu);
    e = msi::encode_registry_value("#4294967296");  // dwordover wraps to 0
    EXPECT_EQ(std::get<u32>(e->value.data), 0u);
    e = msi::encode_registry_value("#x01abFF");
    EXPECT_EQ(e->value.type, RegType::Binary);
    EXPECT_EQ(std::get<std::vector<u8>>(e->value.data), (std::vector<u8>{0x01, 0xAB, 0xFF}));
    e = msi::encode_registry_value("#x123");  // binodd = 01 23
    EXPECT_EQ(std::get<std::vector<u8>>(e->value.data), (std::vector<u8>{0x01, 0x23}));
    e = msi::encode_registry_value("#x");  // binempty
    EXPECT_EQ(std::get<std::vector<u8>>(e->value.data).size(), 0u);
    e = msi::encode_registry_value("#X0A");  // binX
    EXPECT_EQ(std::get<std::vector<u8>>(e->value.data), (std::vector<u8>{0x0A}));
    e = msi::encode_registry_value("#%%SystemRoot%\\x");
    EXPECT_EQ(e->value.type, RegType::ExpandSz);
    EXPECT_EQ(std::get<std::wstring>(e->value.data), L"%SystemRoot%\\x");
    e = msi::encode_registry_value("#%plain");
    EXPECT_EQ(std::get<std::wstring>(e->value.data), L"plain");
    e = msi::encode_registry_value("##lit");
    EXPECT_EQ(e->value.type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(e->value.data), L"#lit");
    e = msi::encode_registry_value("###lit");
    EXPECT_EQ(std::get<std::wstring>(e->value.data), L"##lit");
    e = msi::encode_registry_value("");  // nullv: an empty REG_SZ
    EXPECT_EQ(e->value.type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(e->value.data), L"");
}

TEST(MsiSearchEncode, UnstorableValuesAreRejected) {
    // hashonly, dwordbad, dwordhex, dwordspace, binbad: install exit 1603.
    for (const char* v : {"#", "#12ab", "#0x10", "# 3", "#xZZ"}) {
        EXPECT_FALSE(msi::encode_registry_value(v).has_value()) << v;
    }
}

TEST(MsiSearchEncode, TildeListsAppendPrependOrReplace) {
    auto e = msi::encode_registry_value("a\0b\0c"s);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->value.type, RegType::MultiSz);
    EXPECT_EQ(e->multi, EncodedRegistryValue::MultiMode::Replace);
    EXPECT_EQ(std::get<std::vector<std::wstring>>(e->value.data), (std::vector<std::wstring>{L"a", L"b", L"c"}));
    e = msi::encode_registry_value("\0b\0c"s);
    EXPECT_EQ(e->multi, EncodedRegistryValue::MultiMode::Append);
    e = msi::encode_registry_value("p\0q\0"s);
    EXPECT_EQ(e->multi, EncodedRegistryValue::MultiMode::Prepend);
    EXPECT_EQ(std::get<std::vector<std::wstring>>(e->value.data), (std::vector<std::wstring>{L"p", L"q"}));
    e = msi::encode_registry_value("\0r\0s\0"s);
    EXPECT_EQ(e->multi, EncodedRegistryValue::MultiMode::Replace);
    EXPECT_EQ(std::get<std::vector<std::wstring>>(e->value.data), (std::vector<std::wstring>{L"r", L"s"}));
    e = msi::encode_registry_value("\0"s);  // multiempty: an empty list
    EXPECT_EQ(std::get<std::vector<std::wstring>>(e->value.data).size(), 0u);
    e = msi::encode_registry_value("a\0\0b"s);  // multimiddleempty keeps ""
    EXPECT_EQ(std::get<std::vector<std::wstring>>(e->value.data), (std::vector<std::wstring>{L"a", L"", L"b"}));
}

TEST(MsiSearchEncode, CombineMovesExistingOccurrences) {
    using M = EncodedRegistryValue::MultiMode;
    EXPECT_EQ(msi::combine_multi_sz({L"x", L"b"}, {L"b", L"c"}, M::Append), (std::vector<std::wstring>{L"x", L"b", L"c"}));
    EXPECT_EQ(msi::combine_multi_sz({L"q", L"z"}, {L"p", L"q"}, M::Prepend), (std::vector<std::wstring>{L"p", L"q", L"z"}));
    EXPECT_EQ(msi::combine_multi_sz({L"old"}, {L"r", L"s"}, M::Replace), (std::vector<std::wstring>{L"r", L"s"}));
    EXPECT_EQ(msi::combine_multi_sz({L"keep", L"gone"}, {L"gone", L"new"}, M::Append),
              (std::vector<std::wstring>{L"keep", L"gone", L"new"}));
}

// ---- Signature filters (msi_search_probe S_* lines) ----------------------------

TEST(MsiSearchSignature, SizeAndDateBoundsAreInclusive) {
    msi::FileFacts f;
    f.size = 100;
    f.local_dos_time = 0x50CF6405;
    msi::SignatureFilter s;
    s.min_size = 100;
    EXPECT_TRUE(msi::signature_matches(s, f));
    s = {};
    s.max_size = 100;
    EXPECT_TRUE(msi::signature_matches(s, f));
    s = {};
    s.min_size = 101;
    EXPECT_FALSE(msi::signature_matches(s, f));
    s = {};
    s.max_size = 99;
    EXPECT_FALSE(msi::signature_matches(s, f));
    s = {};
    s.min_date = 0x50CF6405;
    s.max_date = 0x50CF6405;
    EXPECT_TRUE(msi::signature_matches(s, f));
    s.min_date = 0x50CF6406;
    EXPECT_FALSE(msi::signature_matches(s, f));
    s = {};
    s.max_date = 0x50CF6404;
    EXPECT_FALSE(msi::signature_matches(s, f));
}

TEST(MsiSearchSignature, UnversionedFilesCompareAsZero) {
    msi::FileFacts f;  // target.txt
    msi::SignatureFilter s;
    s.min_version = 1ull << 48;  // s_ver_unversioned "1.0"
    EXPECT_FALSE(msi::signature_matches(s, f));
    s = {};
    s.max_version = 99ull << 48;  // s_maxver_unversioned "99.0"
    EXPECT_TRUE(msi::signature_matches(s, f));
}

TEST(MsiSearchSignature, LanguagesMatterOnlyAtMinVersionEquality) {
    // kernel32 6.2.26100.9711, language 1033.
    const u64 v = (6ull << 48) | (2ull << 32) | (26100ull << 16) | 9711ull;
    msi::FileFacts f;
    f.version = v;
    f.languages = {1033};
    msi::SignatureFilter s;
    s.min_version = v;
    EXPECT_FALSE(msi::signature_matches(s, f));  // s_k_minexact_nolang
    s.languages = {1033};
    EXPECT_TRUE(msi::signature_matches(s, f));  // s_k_minexact_lang
    s.languages = {0};
    EXPECT_FALSE(msi::signature_matches(s, f));  // s_k_minexact_lang0
    s.languages = {1031};
    EXPECT_FALSE(msi::signature_matches(s, f));  // s_k_minexact_otherlang
    s.min_version = v - 1;
    EXPECT_TRUE(msi::signature_matches(s, f));  // s_k_minminus_otherlang
    s = {};
    s.max_version = v;
    EXPECT_TRUE(msi::signature_matches(s, f));  // s_k_maxexact
    s.max_version = v - 1;
    EXPECT_FALSE(msi::signature_matches(s, f));  // s_k_maxminus
    s = {};
    s.languages = {1031};
    EXPECT_TRUE(msi::signature_matches(s, f));  // s_k_lang_only
    s = {};
    s.min_version = 6ull << 48;
    EXPECT_TRUE(msi::signature_matches(s, f));  // s_k_min2part
}

// ---- .ini editing (msi_search_install_probe probe.ini) --------------------------

TEST(MsiSearchIni, GetTrimsAndStripsQuotes) {
    const auto doc = msi::IniDocument::parse(
        "[Sec]\r\nfields=one, two ,three\r\nquoted=\"q v\"\r\n  spaced  =  padded value  \r\nempty=\r\n");
    EXPECT_EQ(doc.get("Sec", "fields"), "one, two ,three");
    EXPECT_EQ(doc.get("sec", "FIELDS"), "one, two ,three");
    EXPECT_EQ(doc.get("Sec", "quoted"), "q v");
    EXPECT_EQ(doc.get("Sec", "spaced"), "padded value");
    EXPECT_EQ(doc.get("Sec", "empty"), "");
    EXPECT_EQ(doc.get("Sec", "nokey"), std::nullopt);
    EXPECT_EQ(doc.get("Nope", "fields"), std::nullopt);
}

TEST(MsiSearchIni, SetAppendsKeysAndSectionsAndRemoveDropsEmptySections) {
    auto doc = msi::IniDocument::parse("[Sec]\r\nk_add=old\r\nr_line=x\r\n[Other]\r\nonly=1\r\n");
    doc.set("Sec", "k_add", "v1");
    doc.set("Sec", "k_addnew", "n1");
    doc.set("NewSec", "k", "v");
    EXPECT_TRUE(doc.remove("Sec", "r_line"));
    EXPECT_TRUE(doc.remove("Other", "only"));
    EXPECT_EQ(doc.serialize(), "[Sec]\r\nk_add=v1\r\nk_addnew=n1\r\n[NewSec]\r\nk=v\r\n");
}

// ---- actions over a package -----------------------------------------------------

/// A package built in memory, a capsule registry, and C: on a temp folder.
class MsiSearchActionTest : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        root = std::filesystem::temp_directory_path() /
               ("vitreo-msi-search-" + std::to_string(::getpid()) + (x64() ? "-64" : "-32"));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "Windows", ec);
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        package = std::make_shared<msi::Package>(msi::Package::create());
        add_table("Property", {col("Property", 's', 72, false, true), col("Value", 's', 0, false, false)},
                  {{"ProductCode", "{6A0C1E2B-3D4F-4A5B-8C6D-7E8F9A0B1C2D}"}, {"PROBEKEYNAME", "VitreoMsiSearchProbe"}});
        services.vfs = &vfs;
        services.registry = &registry;
        services.manager = &mgr;
        services.guest_64bit = x64();
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    static msi::ColumnDef col(const char* name, char kind, u32 size, bool nullable, bool key) {
        return msi::make_column(name, kind, size, nullable, key, false, false);
    }
    void add_table(const std::string& name, std::vector<msi::ColumnDef> cols,
                   std::vector<std::vector<std::string>> rows) {
        ASSERT_EQ(package->create_table(name, std::move(cols), false), 0u) << name;
        msi::Table* t = package->find_table(name);
        ASSERT_NE(t, nullptr);
        for (auto& r : rows) ASSERT_EQ(package->insert_row(*t, r, false), 0u) << name;
    }
    void add_search_tables() {
        add_table("AppSearch", {col("Property", 's', 72, false, true), col("Signature_", 's', 72, false, true)}, {});
        add_table("Signature", {col("Signature", 's', 72, false, true), col("FileName", 's', 255, false, false),
                                col("MinVersion", 's', 20, true, false), col("MaxVersion", 's', 20, true, false),
                                col("MinSize", 'i', 4, true, false), col("MaxSize", 'i', 4, true, false),
                                col("MinDate", 'i', 4, true, false), col("MaxDate", 'i', 4, true, false),
                                col("Languages", 's', 255, true, false)},
                  {});
        add_table("RegLocator", {col("Signature_", 's', 72, false, true), col("Root", 'i', 2, false, false),
                                 col("Key", 's', 255, false, false), col("Name", 's', 255, true, false),
                                 col("Type", 'i', 2, true, false)},
                  {});
        add_table("IniLocator", {col("Signature_", 's', 72, false, true), col("FileName", 's', 255, false, false),
                                 col("Section", 's', 96, false, false), col("Key", 's', 128, false, false),
                                 col("Field", 'i', 2, true, false), col("Type", 'i', 2, true, false)},
                  {});
        add_table("DrLocator", {col("Signature_", 's', 72, false, true), col("Parent", 's', 72, true, true),
                                col("Path", 's', 255, true, true), col("Depth", 'i', 2, true, false)},
                  {});
        add_table("CompLocator", {col("Signature_", 's', 72, false, true), col("ComponentId", 's', 38, false, false),
                                  col("Type", 'i', 2, true, false)},
                  {});
        add_table("CCPSearch", {col("Signature_", 's', 72, false, true)}, {});
    }
    void row(const char* table, std::vector<std::string> cells) {
        msi::Table* t = package->find_table(table);
        ASSERT_NE(t, nullptr) << table;
        ASSERT_EQ(package->insert_row(*t, std::move(cells), false), 0u) << table;
    }
    void search(const std::string& property, const std::string& signature) {
        row("AppSearch", {property, signature});
    }
    void file_sig(const std::string& sig, const std::string& name) {
        row("Signature", {sig, name, "", "", "", "", "", "", ""});
    }

    void write_host(const std::string& relative, const std::string& body) {
        const auto p = root / relative;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
    }
    void seed_files() {
        write_host("VitreoMsiProbe/a/b/c/target.txt", std::string(100, 'x'));
        write_host("VitreoMsiProbe/a/target2.txt", std::string(10, 'x'));
        write_host("Windows/vitreomsiprobe.ini",
                   "[Sec]\r\nfields=one, two ,three\r\ndir=C:\\VitreoMsiProbe\\a\r\n"
                   "file=C:\\VitreoMsiProbe\\a\\b\\c\\target.txt\r\n"
                   "filefield=x,C:\\VitreoMsiProbe\\a\\b\\c\\target.txt\r\nempty=\r\n");
    }
    // The probes seed and read their keys in an explicit registry view:
    // msi_search_probe in the 64-bit view, msi_search_install_probe in the
    // view of its component (view()).
    capsule::RegView seed_view = capsule::RegView::Native64;
    void set_reg(capsule::RegHandle hive, const std::wstring& key, const std::wstring& name, RegValue v) {
        auto k = registry.open_key(hive, key, true, seed_view);
        ASSERT_TRUE(k.is_ok());
        ASSERT_TRUE(registry.set_value(k.value(), name, v).is_ok());
        registry.close_key(k.value());
    }
    std::optional<RegValue> get_reg(capsule::RegHandle hive, const std::wstring& key, const std::wstring& name) {
        auto k = registry.open_key(hive, key, false, seed_view);
        if (k.is_err()) return std::nullopt;
        auto v = registry.query_value(k.value(), name);
        registry.close_key(k.value());
        if (v.is_err()) return std::nullopt;
        return v.value();
    }
    bool reg_key_exists(capsule::RegHandle hive, const std::wstring& key) {
        auto k = registry.open_key(hive, key, false, seed_view);
        if (k.is_err()) return false;
        registry.close_key(k.value());
        return true;
    }

    msi::Session& start() {
        session = std::make_unique<msi::Session>();
        EXPECT_TRUE(session->initialize(package));
        session->set_property("WindowsFolder", "C:\\Windows\\");
        session->set_environment([](const std::string& n) -> std::optional<std::string> {
            if (n == "SystemDrive") return "C:"s;
            return std::nullopt;
        });
        session->item_states().component_action = [this](const std::string& c) -> std::optional<i32> {
            auto it = component_states.find(c);
            if (it == component_states.end()) return std::nullopt;
            return it->second;
        };
        ctx = std::make_unique<msi::ActionContext>(*session, services);
        return *session;
    }

    /// Run an action's script ops in order; a failing op rolls back what
    /// ran, newest first (the engine's InstallFinalize contract).
    u32 run_script(const msi::ActionFn& action) {
        std::vector<msi::ScriptOp> ops;
        const u32 rc = msi::collect_script_ops(*ctx, action, ops);
        if (rc != 0) return rc;
        std::vector<msi::ScriptOp*> ran;
        for (auto& op : ops) {
            const u32 r = op.run ? op.run() : 0;
            if (r != 0) {
                for (auto it = ran.rbegin(); it != ran.rend(); ++it)
                    if ((*it)->rollback) (*it)->rollback();
                return r;
            }
            ran.push_back(&op);
        }
        last_ops = std::move(ops);
        return 0;
    }
    void rollback_last() {
        for (auto it = last_ops.rbegin(); it != last_ops.rend(); ++it)
            if (it->rollback) it->rollback();
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
    capsule::Registry registry;
    msi::Services services;
    std::shared_ptr<msi::Package> package;
    std::unique_ptr<msi::Session> session;
    std::unique_ptr<msi::ActionContext> ctx;
    std::map<std::string, i32> component_states;
    std::vector<msi::ScriptOp> last_ops;
};

constexpr wchar_t kProbeKey[] = L"Software\\VitreoMsiSearchProbe";
constexpr i32 kLocal = 3, kAbsent = 2;

TEST_P(MsiSearchActionTest, RegLocatorRawValuesAndTheLaunchConditionInput) {
    add_search_tables();
    // The capsule's own HKLM\...\Windows NT\CurrentVersion, as LibreOffice's
    // WINMAJORVER search reads it (Root 2, Type 18).
    row("RegLocator", {"nt64", "2", "Software\\Microsoft\\Windows NT\\CurrentVersion", "CurrentMajorVersionNumber", "18"});
    row("RegLocator", {"nt32", "2", "Software\\Microsoft\\Windows NT\\CurrentVersion", "CurrentMajorVersionNumber", "2"});
    search("WINMAJORVER", "nt64");
    search("R_NT32", "nt32");
    set_reg(capsule::HKEY_LOCAL_MACHINE, kProbeKey, L"", RegValue{RegType::Sz, L"defval"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, kProbeKey, L"sz", RegValue{RegType::Sz, L"plain text"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, kProbeKey, L"multi",
            RegValue{RegType::MultiSz, std::vector<std::wstring>{L"a", L"b", L"c"}});
    set_reg(capsule::HKEY_CURRENT_USER, kProbeKey, L"hkcu", RegValue{RegType::Sz, L"user"s});
    row("RegLocator", {"r_default", "2", "Software\\VitreoMsiSearchProbe", "", "18"});
    row("RegLocator", {"r_fmtkey", "2", "Software\\[PROBEKEYNAME]", "sz", "18"});
    row("RegLocator", {"r_multi", "2", "Software\\VitreoMsiSearchProbe", "multi", "18"});
    row("RegLocator", {"r_hkcu", "1", "Software\\VitreoMsiSearchProbe", "hkcu", "2"});
    row("RegLocator", {"r_missing", "2", "Software\\VitreoMsiSearchProbe", "missingvalue", "18"});
    row("RegLocator", {"r_missingkey", "2", "Software\\VitreoMsiSearchProbeMissing", "x", "18"});
    search("R_DEFAULT", "r_default");
    search("R_FMTKEY", "r_fmtkey");
    search("R_MULTI", "r_multi");
    search("R_HKCU", "r_hkcu");
    search("R_MISSING", "r_missing");
    search("R_MISSINGKEY", "r_missingkey");
    msi::Session& s = start();
    ASSERT_EQ(msi::action_app_search(*ctx), 0u);
    EXPECT_EQ(s.property("WINMAJORVER"), "#10");
    EXPECT_EQ(s.property("R_NT32"), "#10");
    EXPECT_EQ(s.property("R_DEFAULT"), "defval");
    EXPECT_EQ(s.property("R_FMTKEY"), "plain text");
    EXPECT_EQ(s.property("R_MULTI"), std::string("\0a\0b\0c\0", 7));
    EXPECT_EQ(s.property("R_HKCU"), "user");
    EXPECT_FALSE(s.has_property("R_MISSING"));
    EXPECT_FALSE(s.has_property("R_MISSINGKEY"));
}

TEST_P(MsiSearchActionTest, RegLocatorPathsByType) {
    add_search_tables();
    seed_files();
    const std::wstring k = kProbeKey;
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"dirval", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\a"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"dirc", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\a\\b\\c"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"fileval",
            RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\a\\b\\c\\target.txt"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"filequoted",
            RegValue{RegType::Sz, L"\"C:\\VitreoMsiProbe\\a\\b\\c\\target.txt\" /arg"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"dirmissing", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\missing"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"parentmissing", RegValue{RegType::Sz, L"C:\\VitreoMsiNope\\x\\y"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"expanddir",
            RegValue{RegType::ExpandSz, L"%SystemDrive%\\VitreoMsiProbe\\a"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"dirdword", RegValue{RegType::Dword, u32{7}});
    const std::string key = "Software\\VitreoMsiSearchProbe";
    struct Case { const char* prop; const char* value; const char* type; bool sig; const char* expect; };
    const Case cases[] = {
        {"D_DIRVAL", "dirval", "16", false, "C:\\VitreoMsiProbe\\a\\"},
        {"FN_DIRVAL", "dirval", "17", false, "C:\\VitreoMsiProbe\\"},
        {"FS_DIRVAL", "dirval", "17", true, nullptr},
        {"DS_DIRVAL", "dirval", "16", true, nullptr},
        {"DS_DIRC", "dirc", "16", true, "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt"},
        {"FS_DIRC", "dirc", "17", true, nullptr},
        {"D_FILEVAL", "fileval", "16", false, nullptr},
        {"FN_FILEVAL", "fileval", "17", false, "C:\\VitreoMsiProbe\\a\\b\\c\\"},
        {"FS_FILEVAL", "fileval", "17", true, "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt"},
        {"FS_FILEQUOTED", "filequoted", "17", true, "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt"},
        {"FN_FILEQUOTED", "filequoted", "17", false, "C:\\VitreoMsiProbe\\a\\b\\c\\"},
        {"D_DIRMISSING", "dirmissing", "16", false, nullptr},
        {"FN_DIRMISSING", "dirmissing", "17", false, "C:\\VitreoMsiProbe\\"},
        {"FN_PARENTMISSING", "parentmissing", "17", false, nullptr},
        {"D_EXPANDDIR", "expanddir", "16", false, "C:\\VitreoMsiProbe\\a\\"},
        {"D_DIRDWORD", "dirdword", "16", false, nullptr},
        {"T_NULL", "fileval", "", true, nullptr},          // a null Type is a directory
        {"T_NULL_NOSIG", "fileval", "", false, nullptr},
        {"T_RAW_SIG", "fileval", "18", true, "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt"},
    };
    for (const Case& c : cases) {
        const std::string sig = std::string("s_") + c.prop;
        row("RegLocator", {sig, "2", key, c.value, c.type});
        if (c.sig) file_sig(sig, "target.txt");
        search(c.prop, sig);
    }
    msi::Session& s = start();
    ASSERT_EQ(msi::action_app_search(*ctx), 0u);
    for (const Case& c : cases) {
        if (c.expect) EXPECT_EQ(s.property(c.prop), c.expect) << c.prop;
        else EXPECT_FALSE(s.has_property(c.prop)) << c.prop << " = " << s.property(c.prop);
    }
}

TEST_P(MsiSearchActionTest, DrLocatorDepthParentsAndFixedDrives) {
    add_search_tables();
    seed_files();
    set_reg(capsule::HKEY_LOCAL_MACHINE, kProbeKey, L"dirval", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\a"s});
    row("RegLocator", {"d_dirval", "2", "Software\\VitreoMsiSearchProbe", "dirval", "16"});
    row("DrLocator", {"dr_dir", "", "C:\\VitreoMsiProbe\\a", "0"});
    row("DrLocator", {"dr_dirslash", "", "C:\\VitreoMsiProbe\\a\\", "0"});
    row("DrLocator", {"dr_dirmissing", "", "C:\\VitreoMsiProbe\\missing", "0"});
    row("DrLocator", {"dr_dir_isfile", "", "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt", "0"});
    row("DrLocator", {"dr_dirdepth", "", "C:\\VitreoMsiProbe", "5"});
    const char* depths[] = {"", "0", "1", "2", "3"};
    for (int i = 0; i < 5; ++i) {
        const std::string sig = "dr_depth" + std::to_string(i);
        file_sig(sig, "target.txt");
        row("DrLocator", {sig, "", "C:\\VitreoMsiProbe\\a", depths[i]});
        search("DR_DEPTH" + std::to_string(i), sig);
    }
    row("DrLocator", {"dr_child", "dr_dir", "b", "0"});
    row("DrLocator", {"dr_child2", "dr_dir", "b\\c", "0"});
    row("DrLocator", {"dr_childnull", "dr_dir", "", "0"});
    row("DrLocator", {"dr_childabs", "dr_dir", "C:\\VitreoMsiProbe\\a\\b", "0"});
    row("DrLocator", {"dr_regparent", "d_dirval", "b\\c", "0"});
    file_sig("dr_regparent_file", "target.txt");
    row("DrLocator", {"dr_regparent_file", "d_dirval", "", "2"});
    row("DrLocator", {"dr_parentmissing", "dr_dirmissing", "b", "0"});
    row("DrLocator", {"dr_relative", "", "VitreoMsiProbe\\a", "0"});
    row("DrLocator", {"dr_nopath", "", "", "0"});
    row("DrLocator", {"dr_envfmt", "", "[%SystemDrive]\\VitreoMsiProbe", "0"});
    row("DrLocator", {"dr_fwdslash", "", "C:/VitreoMsiProbe/a", "0"});
    file_sig("s_case", "TARGET.TXT");
    row("DrLocator", {"s_case", "", "C:\\VitreoMsiProbe\\a\\b\\c", "0"});
    row("Signature", {"s_size_gt", "target.txt", "", "", "101", "", "", "", ""});
    row("DrLocator", {"s_size_gt", "", "C:\\VitreoMsiProbe\\a\\b\\c", "0"});
    for (const char* p : {"dr_dir", "dr_dirslash", "dr_dirmissing", "dr_dir_isfile", "dr_dirdepth", "dr_child",
                          "dr_child2", "dr_childnull", "dr_childabs", "dr_regparent", "dr_regparent_file",
                          "dr_parentmissing", "dr_relative", "dr_nopath", "dr_envfmt", "dr_fwdslash", "s_case",
                          "s_size_gt"}) {
        std::string prop = p;
        for (char& ch : prop) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        search(prop, p);
    }
    msi::Session& s = start();
    ASSERT_EQ(msi::action_app_search(*ctx), 0u);
    const std::string a = "C:\\VitreoMsiProbe\\a\\";
    const std::string target = "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt";
    EXPECT_EQ(s.property("DR_DIR"), a);
    EXPECT_EQ(s.property("DR_DIRSLASH"), a);
    EXPECT_FALSE(s.has_property("DR_DIRMISSING"));
    EXPECT_FALSE(s.has_property("DR_DIR_ISFILE"));
    EXPECT_EQ(s.property("DR_DIRDEPTH"), "C:\\VitreoMsiProbe\\");
    EXPECT_FALSE(s.has_property("DR_DEPTH0"));
    EXPECT_FALSE(s.has_property("DR_DEPTH1"));
    EXPECT_FALSE(s.has_property("DR_DEPTH2"));
    EXPECT_EQ(s.property("DR_DEPTH3"), target);
    EXPECT_EQ(s.property("DR_DEPTH4"), target);
    EXPECT_EQ(s.property("DR_CHILD"), a + "b\\");
    EXPECT_EQ(s.property("DR_CHILD2"), a + "b\\c\\");
    EXPECT_EQ(s.property("DR_CHILDNULL"), a);
    EXPECT_FALSE(s.has_property("DR_CHILDABS"));
    EXPECT_EQ(s.property("DR_REGPARENT"), a + "b\\c\\");
    EXPECT_EQ(s.property("DR_REGPARENT_FILE"), target);
    EXPECT_FALSE(s.has_property("DR_PARENTMISSING"));
    EXPECT_EQ(s.property("DR_RELATIVE"), a);
    EXPECT_EQ(s.property("DR_NOPATH"), "C:\\");
    EXPECT_EQ(s.property("DR_ENVFMT"), "C:\\VitreoMsiProbe\\");
    EXPECT_FALSE(s.has_property("DR_FWDSLASH"));
    EXPECT_EQ(s.property("S_CASE"), "C:\\VitreoMsiProbe\\a\\b\\c\\TARGET.TXT");
    EXPECT_FALSE(s.has_property("S_SIZE_GT"));
}

TEST_P(MsiSearchActionTest, IniLocatorFieldsAndTypes) {
    add_search_tables();
    seed_files();
    struct Case { const char* sig; const char* key; const char* field; const char* type; bool filesig; const char* expect; };
    const std::string target = "C:\\VitreoMsiProbe\\a\\b\\c\\target.txt";
    const Case cases[] = {
        {"I_RAW_F0", "fields", "0", "2", false, "one, two ,three"},
        {"I_RAW_FNULL", "fields", "", "2", false, "one, two ,three"},
        {"I_RAW_F1", "fields", "1", "2", false, "one"},
        {"I_RAW_F2", "fields", "2", "2", false, " two "},
        {"I_RAW_F3", "fields", "3", "2", false, "three"},
        {"I_RAW_F4", "fields", "4", "2", false, nullptr},
        {"I_RAW_MISSING", "nokey", "", "2", false, nullptr},
        {"I_RAW_EMPTY", "empty", "", "2", false, nullptr},
        {"I_DIR", "dir", "", "0", false, "C:\\VitreoMsiProbe\\a\\"},
        {"I_DIR_ISFILE", "file", "", "0", false, nullptr},
        {"I_DIR_SIG", "dir", "", "0", true, nullptr},
        {"I_FILE_SIG", "file", "", "1", true, target.c_str()},
        {"I_FILE_NOSIG", "file", "", "1", false, "C:\\VitreoMsiProbe\\a\\b\\c\\"},
        {"I_FILE_FIELD2", "filefield", "2", "1", true, target.c_str()},
        {"I_TYPE_NULL", "file", "", "", true, target.c_str()},
        {"I_TYPE_NULL_NOSIG", "file", "", "", false, "C:\\VitreoMsiProbe\\a\\b\\c\\"},
        {"I_RAW_SIG", "file", "", "2", true, target.c_str()},
    };
    for (const Case& c : cases) {
        row("IniLocator", {c.sig, "vitreomsiprobe.ini", "Sec", c.key, c.field, c.type});
        if (c.filesig) file_sig(c.sig, "target.txt");
        search(c.sig, c.sig);
    }
    row("IniLocator", {"I_BADSECTION", "vitreomsiprobe.ini", "Nope", "fields", "", "2"});
    row("IniLocator", {"I_BADFILE", "vitreomissing.ini", "Sec", "fields", "", "2"});
    search("I_BADSECTION", "I_BADSECTION");
    search("I_BADFILE", "I_BADFILE");
    msi::Session& s = start();
    ASSERT_EQ(msi::action_app_search(*ctx), 0u);
    for (const Case& c : cases) {
        if (c.expect) EXPECT_EQ(s.property(c.sig), c.expect) << c.sig;
        else EXPECT_FALSE(s.has_property(c.sig)) << c.sig << " = " << s.property(c.sig);
    }
    EXPECT_FALSE(s.has_property("I_BADSECTION"));
    EXPECT_FALSE(s.has_property("I_BADFILE"));
}

TEST_P(MsiSearchActionTest, CompLocatorReadsTheInstalledKeyPath) {
    add_search_tables();
    seed_files();
    // Component {E5371B0F-C703-4264-9D9A-800DEA3AE7AD} registered for the
    // machine (S-1-5-18) with a file key path.
    set_reg(capsule::HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Components\\"
            L"F0B1735E307C4624D9A908D0AEA37EDA",
            L"D3C2B16AF5E48764A9CBED0F21436587", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\a\\target2.txt"s});
    const std::string comp = "{E5371B0F-C703-4264-9D9A-800DEA3AE7AD}";
    row("CompLocator", {"c_file", comp, "1"});
    row("CompLocator", {"c_dir", comp, "0"});
    row("CompLocator", {"c_null", comp, ""});
    row("CompLocator", {"c_file_sig", comp, "1"});
    file_sig("c_file_sig", "target2.txt");
    row("CompLocator", {"c_file_badsig", comp, "1"});
    file_sig("c_file_badsig", "vitreo_not_this.dll");
    row("CompLocator", {"c_unknown", "{0B1C2D3E-4F50-4612-8374-95A6B7C8D9E0}", "1"});
    for (const char* p : {"c_file", "c_dir", "c_null", "c_file_sig", "c_file_badsig", "c_unknown"}) search(p, p);
    msi::Session& s = start();
    ASSERT_EQ(msi::action_app_search(*ctx), 0u);
    EXPECT_EQ(s.property("c_file"), "C:\\VitreoMsiProbe\\a\\");
    EXPECT_FALSE(s.has_property("c_dir"));
    EXPECT_EQ(s.property("c_null"), "C:\\VitreoMsiProbe\\a\\");
    EXPECT_EQ(s.property("c_file_sig"), "C:\\VitreoMsiProbe\\a\\target2.txt");
    EXPECT_EQ(s.property("c_file_badsig"), "C:\\VitreoMsiProbe\\a\\target2.txt");
    EXPECT_FALSE(s.has_property("c_unknown"));
}

TEST_P(MsiSearchActionTest, AppSearchRowOrderPresetsAndLocatorOrder) {
    add_search_tables();
    seed_files();
    set_reg(capsule::HKEY_LOCAL_MACHINE, kProbeKey, L"dirmissing", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\missing"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, kProbeKey, L"dirc", RegValue{RegType::Sz, L"C:\\VitreoMsiProbe\\a\\b\\c"s});
    row("DrLocator", {"dr_dir", "", "C:\\VitreoMsiProbe\\a", "0"});
    row("DrLocator", {"dr_child", "dr_dir", "b", "0"});
    row("DrLocator", {"dr_dirmissing", "", "C:\\VitreoMsiProbe\\missing", "0"});
    // RegLocator is consulted before DrLocator; a miss falls through.
    row("RegLocator", {"multi_loc", "2", "Software\\VitreoMsiSearchProbe", "dirmissing", "16"});
    row("DrLocator", {"multi_loc", "", "C:\\VitreoMsiProbe\\a", "0"});
    row("RegLocator", {"multi_loc2", "2", "Software\\VitreoMsiSearchProbe", "dirc", "16"});
    row("DrLocator", {"multi_loc2", "", "C:\\VitreoMsiProbe\\a", "0"});
    row("Property", {"PRESET", "original"});
    row("Property", {"PRESETFOUND", "original"});
    search("PRESET", "dr_dirmissing");
    search("PRESETFOUND", "dr_dir");
    search("TWOSIG", "dr_dir");
    search("TWOSIG", "dr_child");
    search("TWOSIGMISS", "dr_dir");
    search("TWOSIGMISS", "dr_dirmissing");
    search("MULTI_LOC", "multi_loc");
    search("MULTI_LOC2", "multi_loc2");
    msi::Session& s = start();
    ASSERT_EQ(msi::action_app_search(*ctx), 0u);
    EXPECT_EQ(s.property("PRESET"), "original");
    EXPECT_EQ(s.property("PRESETFOUND"), "C:\\VitreoMsiProbe\\a\\");
    EXPECT_EQ(s.property("TWOSIG"), "C:\\VitreoMsiProbe\\a\\b\\");
    EXPECT_EQ(s.property("TWOSIGMISS"), "C:\\VitreoMsiProbe\\a\\");
    EXPECT_EQ(s.property("MULTI_LOC"), "C:\\VitreoMsiProbe\\a\\");
    EXPECT_EQ(s.property("MULTI_LOC2"), "C:\\VitreoMsiProbe\\a\\b\\c\\");
}

TEST_P(MsiSearchActionTest, CcpSearchAndRmccpSearch) {
    add_search_tables();
    seed_files();
    row("DrLocator", {"dr_dir", "", "C:\\VitreoMsiProbe\\a", "0"});
    row("DrLocator", {"dr_dirmissing", "", "C:\\VitreoMsiProbe\\missing", "0"});
    row("DrLocator", {"rm_rel", "", "a", "0"});
    row("CCPSearch", {"dr_dirmissing"});
    msi::Session& s = start();
    ASSERT_EQ(msi::action_ccp_search(*ctx), 0u);
    EXPECT_FALSE(s.has_property("CCP_Success"));
    // RMCCPSearch without CCP_DRIVE: 1603 ("RMCCPSearch no-drive").
    EXPECT_EQ(msi::action_rmccp_search(*ctx), 1603u);

    row("CCPSearch", {"dr_dir"});
    msi::Session& s2 = start();
    ASSERT_EQ(msi::action_ccp_search(*ctx), 0u);
    EXPECT_EQ(s2.property("CCP_Success"), "1");
    s2.set_property("CCP_Success", "");
    s2.set_property("CCP_DRIVE", "C:\\VitreoMsiProbe\\");
    EXPECT_EQ(msi::action_rmccp_search(*ctx), 0u);  // absolute DrLocator path
    EXPECT_EQ(s2.property("CCP_Success"), "1");

    // A relative path on a fixed CCP_DRIVE finds nothing ("rm-relative").
    msi::Table* ccp = package->find_table("CCPSearch");
    while (!ccp->rows.empty()) package->delete_row(*ccp, 0);
    row("CCPSearch", {"rm_rel"});
    msi::Session& s3 = start();
    s3.set_property("CCP_DRIVE", "C:\\VitreoMsiProbe\\");
    EXPECT_EQ(msi::action_rmccp_search(*ctx), 1603u);
    EXPECT_FALSE(s3.has_property("CCP_Success"));
}

// ---- Registry-table script ops -----------------------------------------------

class MsiSearchRegistryTest : public MsiSearchActionTest {
protected:
    void add_registry_tables() {
        // A 32-bit component's rows are in the 32-bit view.
        seed_view = x64() ? capsule::RegView::Native64 : capsule::RegView::Wow32;
        add_table("Component", {col("Component", 's', 72, false, true), col("ComponentId", 's', 38, true, false),
                                col("Directory_", 's', 72, false, false), col("Attributes", 'i', 2, false, false),
                                col("Condition", 's', 255, true, false), col("KeyPath", 's', 72, true, false)},
                  {{"C", "{9D3F4B5C-6E7A-4F8B-9CAD-3E4F5A6B7C8D}", "TARGETDIR", x64() ? "260" : "4", "", "reg_keypath"}});
        add_table("Registry", {col("Registry", 's', 72, false, true), col("Root", 'i', 2, false, false),
                               col("Key", 's', 255, false, false), col("Name", 's', 255, true, false),
                               col("Value", 's', 0, true, false), col("Component_", 's', 72, false, false)},
                  {});
        add_table("RemoveRegistry", {col("RemoveRegistry", 's', 72, false, true), col("Root", 'i', 2, false, false),
                                     col("Key", 's', 255, false, false), col("Name", 's', 255, true, false),
                                     col("Component_", 's', 72, false, false)},
                  {});
    }
    void reg_row(const std::string& id, const std::string& root, const std::string& key, const std::string& name,
                 const std::string& value) {
        row("Registry", {id, root, key, name, value, "C"});
    }
};

constexpr wchar_t kRegProbe[] = L"Software\\VitreoMsiRegProbe";

TEST_P(MsiSearchRegistryTest, WriteThenRemoveMatchesTheMeasuredInstallAndUninstall) {
    add_registry_tables();
    const std::wstring k = kRegProbe;
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"multiappend", RegValue{RegType::MultiSz, std::vector<std::wstring>{L"x", L"b"}});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"multiprepend", RegValue{RegType::MultiSz, std::vector<std::wstring>{L"q", L"z"}});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"multireplace", RegValue{RegType::MultiSz, std::vector<std::wstring>{L"old"}});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"multiappendsz", RegValue{RegType::Sz, L"single"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"overwrite", RegValue{RegType::Dword, u32{5}});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k, L"toremove", RegValue{RegType::Sz, L"bye"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k + L"\\rmkey\\sub", L"v", RegValue{RegType::Sz, L"deep"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k + L"\\minus\\sub", L"v", RegValue{RegType::Sz, L"deep"s});
    set_reg(capsule::HKEY_LOCAL_MACHINE, k + L"\\uninst", L"preexisting", RegValue{RegType::Sz, L"stay"s});
    const std::string K = "Software\\VitreoMsiRegProbe";
    reg_row("reg_keypath", "2", K, "keypath", "kp");
    reg_row("dword", "2", K, "dword", "#42");
    reg_row("hash2", "2", K, "hash2", "##lit");
    reg_row("multiappend", "2", K, "multiappend", "[~]b[~]c");
    reg_row("multiprepend", "2", K, "multiprepend", "p[~]q[~]");
    reg_row("multireplace", "2", K, "multireplace", "[~]r[~]s[~]");
    reg_row("multiappendsz", "2", K, "multiappendsz", "[~]t");
    reg_row("overwrite", "2", K, "overwrite", "text");
    reg_row("nullv", "2", K, "nullv", "");
    reg_row("dflt", "2", K, "", "dflt");
    reg_row("fmt", "2", K, "[PROBEKEYNAME]", "v");
    reg_row("plus", "2", K + "\\plus", "+", "");
    reg_row("star", "2", K + "\\star", "*", "");
    reg_row("minus", "2", K + "\\minus", "-", "");
    reg_row("plusval", "2", K + "\\plusval", "+", "x");
    reg_row("uninst", "2", K + "\\uninst", "mine", "v");
    reg_row("deep", "2", "Software\\VitreoMsiRegProbeDeep\\a\\b", "v", "x");
    reg_row("rootm1", "-1", "Software\\VitreoMsiRegProbeRootM1", "v", "m1");
    reg_row("hkcr", "0", "VitreoMsiRegProbe.Cls", "v", "cls");
    reg_row("hkcu", "1", K, "hkcu", "u");
    row("RemoveRegistry", {"rr_value", "2", K, "toremove", "C"});
    row("RemoveRegistry", {"rr_missing", "2", K, "nonexistent", "C"});
    row("RemoveRegistry", {"rr_key", "2", K + "\\rmkey", "-", "C"});
    row("Property", {"ALLUSERS", "1"});

    // Install.
    component_states["C"] = kLocal;
    start();
    ASSERT_EQ(run_script(msi::action_remove_registry_values), 0u);
    ASSERT_EQ(run_script(msi::action_write_registry_values), 0u);
    auto multi = [&](const wchar_t* name) {
        auto v = get_reg(capsule::HKEY_LOCAL_MACHINE, k, name);
        return v && v->type == RegType::MultiSz ? std::get<std::vector<std::wstring>>(v->data)
                                                 : std::vector<std::wstring>{L"<not multi>"};
    };
    EXPECT_EQ(multi(L"multiappend"), (std::vector<std::wstring>{L"x", L"b", L"c"}));
    EXPECT_EQ(multi(L"multiprepend"), (std::vector<std::wstring>{L"p", L"q", L"z"}));
    EXPECT_EQ(multi(L"multireplace"), (std::vector<std::wstring>{L"r", L"s"}));
    EXPECT_EQ(multi(L"multiappendsz"), (std::vector<std::wstring>{L"t"}));
    auto ov = get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"overwrite");
    ASSERT_TRUE(ov);
    EXPECT_EQ(ov->type, RegType::Sz);
    EXPECT_EQ(std::get<u32>(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"dword")->data), 42u);
    EXPECT_EQ(std::get<std::wstring>(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"hash2")->data), L"#lit");
    EXPECT_EQ(std::get<std::wstring>(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"nullv")->data), L"");
    EXPECT_EQ(std::get<std::wstring>(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"")->data), L"dflt");
    EXPECT_TRUE(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"VitreoMsiSearchProbe").has_value());
    EXPECT_FALSE(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"toremove").has_value());
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\rmkey"));
    EXPECT_TRUE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\plus"));
    EXPECT_TRUE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\star"));
    EXPECT_TRUE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\minus\\sub"));
    EXPECT_TRUE(get_reg(capsule::HKEY_LOCAL_MACHINE, k + L"\\plusval", L"+").has_value());
    EXPECT_TRUE(get_reg(capsule::HKEY_LOCAL_MACHINE, L"Software\\VitreoMsiRegProbeRootM1", L"v").has_value());
    EXPECT_TRUE(get_reg(capsule::HKEY_LOCAL_MACHINE, L"Software\\Classes\\VitreoMsiRegProbe.Cls", L"v").has_value());
    EXPECT_TRUE(get_reg(capsule::HKEY_CURRENT_USER, k, L"hkcu").has_value());

    // Uninstall.
    component_states["C"] = kAbsent;
    start();
    ASSERT_EQ(run_script(msi::action_remove_registry_values), 0u);
    ASSERT_EQ(run_script(msi::action_write_registry_values), 0u);
    EXPECT_EQ(multi(L"multiappend"), (std::vector<std::wstring>{L"x"}));
    EXPECT_EQ(multi(L"multiprepend"), (std::vector<std::wstring>{L"z"}));
    EXPECT_FALSE(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"multireplace").has_value());
    EXPECT_FALSE(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"multiappendsz").has_value());
    EXPECT_FALSE(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"overwrite").has_value());
    EXPECT_FALSE(get_reg(capsule::HKEY_LOCAL_MACHINE, k, L"keypath").has_value());
    EXPECT_TRUE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\plus"));
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\star"));
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\minus"));
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, k + L"\\plusval"));
    EXPECT_TRUE(get_reg(capsule::HKEY_LOCAL_MACHINE, k + L"\\uninst", L"preexisting").has_value());
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, L"Software\\VitreoMsiRegProbeDeep"));
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, L"Software\\VitreoMsiRegProbeRootM1"));
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, L"Software\\Classes\\VitreoMsiRegProbe.Cls"));
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_CURRENT_USER, k));
}

TEST_P(MsiSearchRegistryTest, AnUnstorableValueFailsAndRollsBack) {
    add_registry_tables();
    const std::string K = "Software\\VitreoMsiRegProbe";
    reg_row("a_first", "2", K, "first", "1");
    reg_row("hashonly", "2", K, "hashonly", "#");
    component_states["C"] = kLocal;
    start();
    EXPECT_EQ(run_script(msi::action_write_registry_values), 1603u);
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_LOCAL_MACHINE, kRegProbe));
}

TEST_P(MsiSearchRegistryTest, OutsideTheScriptTheActionFails) {
    add_registry_tables();
    component_states["C"] = kLocal;
    start();
    EXPECT_EQ(msi::action_write_registry_values(*ctx), 1603u);
}

TEST_P(MsiSearchRegistryTest, PerUserRootsFollowAllUsers) {
    add_registry_tables();
    reg_row("rootm1", "-1", "Software\\VitreoMsiRegProbeRootM1", "v", "m1");
    reg_row("hkcr", "0", "VitreoMsiRegProbe.Cls", "v", "cls");
    component_states["C"] = kLocal;
    start();
    ASSERT_EQ(run_script(msi::action_write_registry_values), 0u);
    EXPECT_TRUE(get_reg(capsule::HKEY_CURRENT_USER, L"Software\\VitreoMsiRegProbeRootM1", L"v").has_value());
    EXPECT_TRUE(get_reg(capsule::HKEY_CURRENT_USER, L"Software\\Classes\\VitreoMsiRegProbe.Cls", L"v").has_value());
    rollback_last();
    EXPECT_FALSE(reg_key_exists(capsule::HKEY_CURRENT_USER, L"Software\\VitreoMsiRegProbeRootM1"));
}

// ---- .ini and environment script ops -------------------------------------------

TEST_P(MsiSearchRegistryTest, IniValuesInstallAndUninstall) {
    add_registry_tables();
    write_host("VitreoMsiRegProbe/probe.ini",
               "[Sec]\r\nk_create=keep\r\nk_tag=a,b\r\nk_tag2=a,b\r\nk_add=old\r\nr_line=x\r\nr_tag=x,y,z\r\n"
               "r_tagonly=y\r\nr_tagmissing=x,z\r\n[Other]\r\nonly=1\r\n");
    add_table("IniFile", {col("IniFile", 's', 72, false, true), col("FileName", 's', 255, false, false),
                          col("DirProperty", 's', 72, true, false), col("Section", 's', 96, false, false),
                          col("Key", 's', 128, false, false), col("Value", 's', 255, false, false),
                          col("Action", 'i', 2, false, false), col("Component_", 's', 72, false, false)},
              {{"i_add", "probe.ini", "INIDIR", "Sec", "k_add", "v1", "0", "C"},
               {"i_addnew", "probe.ini", "INIDIR", "Sec", "k_addnew", "n1", "0", "C"},
               {"i_create", "probe.ini", "INIDIR", "Sec", "k_create", "new", "1", "C"},
               {"i_createnew", "probe.ini", "INIDIR", "Sec", "k_createnew", "fresh", "1", "C"},
               {"i_tag", "probe.ini", "INIDIR", "Sec", "k_tag", "c", "3", "C"},
               {"i_tag2", "probe.ini", "INIDIR", "Sec", "k_tag2", "b", "3", "C"},
               {"i_tagnew", "probe.ini", "INIDIR", "Sec", "k_tagnew", "t", "3", "C"},
               {"i_newsec", "probe.ini", "INIDIR", "NewSec", "k", "v", "0", "C"}});
    add_table("RemoveIniFile", {col("RemoveIniFile", 's', 72, false, true), col("FileName", 's', 255, false, false),
                                col("DirProperty", 's', 72, true, false), col("Section", 's', 96, false, false),
                                col("Key", 's', 128, false, false), col("Value", 's', 255, true, false),
                                col("Action", 'i', 2, false, false), col("Component_", 's', 72, false, false)},
              {{"ri_line", "probe.ini", "INIDIR", "Sec", "r_line", "", "2", "C"},
               {"ri_tag", "probe.ini", "INIDIR", "Sec", "r_tag", "y", "4", "C"},
               {"ri_tagonly", "probe.ini", "INIDIR", "Sec", "r_tagonly", "y", "4", "C"},
               {"ri_tagmissing", "probe.ini", "INIDIR", "Sec", "r_tagmissing", "y", "4", "C"},
               {"ri_lastinsec", "probe.ini", "INIDIR", "Other", "only", "", "2", "C"}});
    row("Property", {"INIDIR", "C:\\VitreoMsiRegProbe\\"});
    auto read_ini = [&] {
        std::ifstream in(root / "VitreoMsiRegProbe/probe.ini", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    component_states["C"] = kLocal;
    start();
    ASSERT_EQ(run_script(msi::action_remove_ini_values), 0u);
    ASSERT_EQ(run_script(msi::action_write_ini_values), 0u);
    EXPECT_EQ(read_ini(),
              "[Sec]\r\nk_create=keep\r\nk_tag=a,b,c\r\nk_tag2=a,b,b\r\nk_add=v1\r\nr_tag=x,z\r\nr_tagmissing=x,z\r\n"
              "k_addnew=n1\r\nk_createnew=fresh\r\nk_tagnew=t\r\n[NewSec]\r\nk=v\r\n");
    component_states["C"] = kAbsent;
    start();
    ASSERT_EQ(run_script(msi::action_remove_ini_values), 0u);
    EXPECT_EQ(read_ini(), "[Sec]\r\nk_tag=a,b\r\nk_tag2=a\r\nr_tag=x,z\r\nr_tagmissing=x,z\r\n");
}

TEST_P(MsiSearchRegistryTest, EnvironmentInstallAndUninstall) {
    add_registry_tables();
    add_table("Environment", {col("Environment", 's', 72, false, true), col("Name", 's', 255, false, false),
                              col("Value", 's', 255, true, false), col("Component_", 's', 72, false, false)},
              {{"e_set", "=-VITREOPROBE_SET", "v1", "C"},
               {"e_plus", "+VITREOPROBE_PLUS", "new", "C"},
               {"e_plusnew", "+VITREOPROBE_PLUSNEW", "fresh", "C"},
               {"e_app", "VITREOPROBE_APP", "[~];tail", "C"},
               {"e_pre", "VITREOPROBE_PRE", "head;[~]", "C"},
               {"e_sys", "*VITREOPROBE_SYS", "sysv", "C"},
               {"e_del", "!VITREOPROBE_DEL", "dv", "C"},
               {"e_delany", "!VITREOPROBE_DELANY", "", "C"},
               {"e_delmismatch", "!VITREOPROBE_DELMISMATCH", "dv", "C"},
               {"e_exp", "VITREOPROBE_EXP", "%SystemRoot%\\x", "C"},
               {"e_nullset", "=VITREOPROBE_NULLSET", "", "C"},
               {"e_sysapp", "*=-VITREOPROBE_SYSAPP", "[~];more", "C"}});
    const std::wstring user = L"Environment";
    const std::wstring sys = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    for (auto [n, v] : {std::pair{L"VITREOPROBE_PLUS", L"keep"}, std::pair{L"VITREOPROBE_APP", L"base"},
                        std::pair{L"VITREOPROBE_PRE", L"base"}, std::pair{L"VITREOPROBE_DEL", L"dv"},
                        std::pair{L"VITREOPROBE_DELANY", L"zz"}, std::pair{L"VITREOPROBE_DELMISMATCH", L"other"},
                        std::pair{L"VITREOPROBE_NULLSET", L"was"}})
        set_reg(capsule::HKEY_CURRENT_USER, user, n, RegValue{RegType::Sz, std::wstring(v)});
    set_reg(capsule::HKEY_LOCAL_MACHINE, sys, L"VITREOPROBE_SYSAPP", RegValue{RegType::Sz, L"sysbase"s});
    auto env = [&](capsule::RegHandle hive, const std::wstring& key, const wchar_t* name) -> std::string {
        auto v = get_reg(hive, key, name);
        if (!v) return "<unset>";
        return std::string(v->type == RegType::ExpandSz ? "X:" : "") +
               std::string(std::get<std::wstring>(v->data).begin(), std::get<std::wstring>(v->data).end());
    };
    component_states["C"] = kLocal;
    start();
    ASSERT_EQ(run_script(msi::action_remove_environment_strings), 0u);
    ASSERT_EQ(run_script(msi::action_write_environment_strings), 0u);
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_SET"), "v1");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_PLUS"), "keep");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_PLUSNEW"), "fresh");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_APP"), "base;tail");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_PRE"), "head;base");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_DEL"), "<unset>");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_DELANY"), "<unset>");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_DELMISMATCH"), "other");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_EXP"), "X:%SystemRoot%\\x");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_NULLSET"), "<unset>");
    EXPECT_EQ(env(capsule::HKEY_LOCAL_MACHINE, sys, L"VITREOPROBE_SYS"), "sysv");
    EXPECT_EQ(env(capsule::HKEY_LOCAL_MACHINE, sys, L"VITREOPROBE_SYSAPP"), "sysbase;more");
    component_states["C"] = kAbsent;
    start();
    ASSERT_EQ(run_script(msi::action_remove_environment_strings), 0u);
    ASSERT_EQ(run_script(msi::action_write_environment_strings), 0u);
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_SET"), "<unset>");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_PLUS"), "keep");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_PLUSNEW"), "fresh");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_APP"), "base");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_PRE"), "base");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_DELMISMATCH"), "other");
    EXPECT_EQ(env(capsule::HKEY_CURRENT_USER, user, L"VITREOPROBE_EXP"), "<unset>");
    EXPECT_EQ(env(capsule::HKEY_LOCAL_MACHINE, sys, L"VITREOPROBE_SYS"), "<unset>");
    EXPECT_EQ(env(capsule::HKEY_LOCAL_MACHINE, sys, L"VITREOPROBE_SYSAPP"), "sysbase");
}

TEST_P(MsiSearchRegistryTest, OdbcRowsFailLoudlyAndAbsentTablesSucceed) {
    add_registry_tables();
    component_states["C"] = kLocal;
    start();
    EXPECT_EQ(run_script(msi::action_install_odbc), 0u);
    EXPECT_EQ(run_script(msi::action_remove_odbc), 0u);
    EXPECT_EQ(msi::action_set_odbc_folders(*ctx), 0u);
    add_table("ODBCDriver", {col("Driver", 's', 72, false, true), col("Component_", 's', 72, false, false),
                             col("Description", 's', 255, false, false), col("File_", 's', 72, false, false),
                             col("File_Setup", 's', 72, true, false)},
              {{"drv", "C", "Probe Driver", "f", ""}});
    start();
    EXPECT_EQ(run_script(msi::action_install_odbc), 1603u);
}

TEST_P(MsiSearchRegistryTest, DeleteAndConfigureServicesThroughTheScm) {
    add_registry_tables();
    ScmManager scm(registry, vfs);
    ServiceConfig cfg;
    cfg.name = L"VitreoProbeSvc";
    cfg.image_path = L"\"C:\\probe\\svc.exe\"";
    cfg.start_type = service_constants::SERVICE_AUTO_START;
    u32 error = 0;
    ASSERT_TRUE(scm.create_service(cfg, error));
    add_table("ServiceControl", {col("ServiceControl", 's', 72, false, true), col("Name", 's', 255, false, false),
                                 col("Event", 'i', 2, false, false), col("Arguments", 's', 255, true, false),
                                 col("Wait", 'i', 2, true, false), col("Component_", 's', 72, false, false)},
              {{"sc_delete", "VitreoProbeSvc", "8", "", "", "C"}});
    add_table("MsiServiceConfig", {col("MsiServiceConfig", 's', 72, false, true), col("Name", 's', 255, false, false),
                                   col("Event", 'i', 2, false, false), col("ConfigType", 'i', 4, false, false),
                                   col("Argument", 's', 255, true, false), col("Component_", 's', 72, false, false)},
              {{"cfg_delayed", "VitreoProbeSvc", "1", "3", "1", "C"}});
    component_states["C"] = kLocal;
    start();
    ASSERT_EQ(run_script(msi::action_msi_configure_services), 0u);
    auto delayed = get_reg(capsule::HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\VitreoProbeSvc",
                           L"DelayedAutostart");
    ASSERT_TRUE(delayed);
    EXPECT_EQ(std::get<u32>(delayed->data), 1u);
    ASSERT_EQ(run_script(msi::action_delete_services), 0u);
    EXPECT_FALSE(scm.service_exists(L"VitreoProbeSvc"));
    rollback_last();  // the delete's rollback restores the service entry
    EXPECT_TRUE(scm.service_exists(L"VitreoProbeSvc"));
    // A service name that is not installed is nothing to delete.
    msi::Table* t = package->find_table("ServiceControl");
    package->update_cell(*t, 0, 1, "VitreoNoSuchSvc");
    start();
    EXPECT_EQ(run_script(msi::action_delete_services), 0u);
}

TEST(MsiSearchRegistration, RegistersEveryAction) {
    msi::ActionRegistry reg;
    msi::register_search_actions(reg);
    for (const char* name : {"AppSearch", "CCPSearch", "RMCCPSearch", "WriteRegistryValues", "RemoveRegistryValues",
                             "WriteIniValues", "RemoveIniValues", "WriteEnvironmentStrings",
                             "RemoveEnvironmentStrings", "InstallODBC", "RemoveODBC", "SetODBCFolders",
                             "InstallServices", "StartServices", "StopServices", "DeleteServices",
                             "MsiConfigureServices", "AllocateRegistrySpace"})
        EXPECT_NE(reg.find(name), nullptr) << name;
}

INSTANTIATE_MSI_BITNESS(MsiSearchActionTest);
INSTANTIATE_MSI_BITNESS(MsiSearchRegistryTest);

}  // namespace
}  // namespace vitreo::winapi::msi_test
