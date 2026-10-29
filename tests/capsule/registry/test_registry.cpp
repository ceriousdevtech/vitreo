/// @file test_registry.cpp
/// @brief Tests for REG-001 capsule registry core

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <capsule/registry/registry.hpp>
#include <capsule/registry/hive.hpp>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::capsule;

TEST(RegistryOrderingTest, MatchesUppercaseLexicalOrdering) {
    const auto upper = [](std::wstring text) {
        for (auto& c : text) c = static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(c)));
        return text;
    };
    const std::vector<std::wstring> names = {
        L"", L"a", L"A", L"aB", L"Ab", L"abc", L"ABCD", L"b", L"[", L"_",
        L"Software", L"software", L"SOFTWAREX", L"\u00e9", L"\u00c9", L"\u03a9",
        L"\u03c9", L"\U0001f600", std::wstring(L"a\0b", 3), std::wstring(L"A\0C", 3),
        std::wstring(1024, L'a'), std::wstring(1024, L'A') + L"z"
    };
    RegCaseInsensitive compare;
    for (const auto& a : names) {
        for (const auto& b : names) {
            EXPECT_EQ(compare(a, b), upper(a) < upper(b));
        }
    }
}

class RegistryTest : public ::testing::Test {
protected:
    Registry reg;
};

// ---------------------------------------------------------------------------
// 1. Open root key returns valid handle
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, OpenRootKeyReturnsValidHandle) {
    auto r = reg.open_key(HKEY_LOCAL_MACHINE, L"", false);
    ASSERT_TRUE(r.is_ok());
    // Handle is valid – close should not crash.
    reg.close_key(r.value());
}

TEST_F(RegistryTest, AllFourRootsAreValid) {
    // Each root handle is implicitly open.
    auto keys = reg.enum_keys(HKEY_CLASSES_ROOT);
    ASSERT_TRUE(keys.is_ok());

    keys = reg.enum_keys(HKEY_CURRENT_USER);
    ASSERT_TRUE(keys.is_ok());

    keys = reg.enum_keys(HKEY_LOCAL_MACHINE);
    ASSERT_TRUE(keys.is_ok());

    keys = reg.enum_keys(HKEY_USERS);
    ASSERT_TRUE(keys.is_ok());
}

TEST_F(RegistryTest, HkcrMergesUserAndMachineClassesWithUserPriority) {
    constexpr const wchar_t* shared =
        L"Software\\Classes\\CLSID\\{11111111-2222-3333-4444-555555555555}";
    constexpr const wchar_t* machine_only =
        L"Software\\Classes\\CLSID\\{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";
    const RegValue machine_value{
        RegType::Sz, {std::wstring(L"machine-server.exe")}};
    const RegValue user_value{
        RegType::Sz, {std::wstring(L"user-server.exe")}};

    auto machine = reg.open_key(HKEY_LOCAL_MACHINE, shared, true);
    ASSERT_TRUE(machine.is_ok());
    ASSERT_TRUE(reg.set_value(machine.value(), L"", machine_value).is_ok());
    auto user = reg.open_key(HKEY_CURRENT_USER, shared, true);
    ASSERT_TRUE(user.is_ok());
    ASSERT_TRUE(reg.set_value(user.value(), L"", user_value).is_ok());
    auto fallback = reg.open_key(HKEY_LOCAL_MACHINE, machine_only, true);
    ASSERT_TRUE(fallback.is_ok());
    ASSERT_TRUE(reg.set_value(fallback.value(), L"", machine_value).is_ok());

    auto classes = reg.open_key(HKEY_CLASSES_ROOT, L"CLSID", false);
    ASSERT_TRUE(classes.is_ok());
    auto merged_shared = reg.open_key(
        classes.value(), L"{11111111-2222-3333-4444-555555555555}", false);
    ASSERT_TRUE(merged_shared.is_ok());
    auto shared_value = reg.query_value(merged_shared.value(), L"");
    ASSERT_TRUE(shared_value.is_ok());
    EXPECT_EQ(std::get<std::wstring>(shared_value.value().data),
              L"user-server.exe");

    auto merged_machine = reg.open_key(
        classes.value(), L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}", false);
    ASSERT_TRUE(merged_machine.is_ok());
    auto fallback_value = reg.query_value(merged_machine.value(), L"");
    ASSERT_TRUE(fallback_value.is_ok());
    EXPECT_EQ(std::get<std::wstring>(fallback_value.value().data),
              L"machine-server.exe");

    auto names = reg.enum_keys(classes.value());
    ASSERT_TRUE(names.is_ok());
    // Both registrations, each once, beside the classes the OS defaults seed.
    for (const wchar_t* clsid : {L"{11111111-2222-3333-4444-555555555555}",
                                 L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"}) {
        EXPECT_EQ(std::count(names.value().begin(), names.value().end(), std::wstring(clsid)), 1)
            << std::string(clsid, clsid + 38);
    }
    EXPECT_EQ(reg.canonical_key_path(merged_shared.value()),
              L"hkey_classes_root\\clsid\\{11111111-2222-3333-4444-555555555555}");
}

TEST_F(RegistryTest, HardwareKeysAreVolatile) {
    // Windows never saves HKLM\HARDWARE; it is rebuilt at every boot (Vitreo:
    // every process start). Everything else in HKLM is saved.
    auto cpu = reg.open_key(HKEY_LOCAL_MACHINE,
                            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
    ASSERT_TRUE(cpu.is_ok());
    ASSERT_TRUE(reg.set_value(cpu.value(), L"~MHz", RegValue{RegType::Dword, {u32{4512}}}).is_ok());
    reg.close_key(cpu.value());
    auto software = reg.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VitreoVolatileTest", true);
    ASSERT_TRUE(software.is_ok());
    ASSERT_TRUE(reg.set_value(software.value(), L"Kept", RegValue{RegType::Dword, {u32{1}}}).is_ok());
    reg.close_key(software.value());

    const auto dir = std::filesystem::temp_directory_path() /
                     ("vitreo-registry-volatile-" + std::to_string(::getpid()));
    reg.save(dir);
    std::ifstream in(dir / "HKLM.reg");
    const std::string hive((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(hive.find("HARDWARE"), std::string::npos) << hive.substr(0, 300);
    EXPECT_NE(hive.find("VitreoVolatileTest"), std::string::npos);

    Registry reloaded;
    reloaded.load(dir);
    EXPECT_TRUE(reloaded.open_key(HKEY_LOCAL_MACHINE,
                                  L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", false).is_err());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 2. Create subkey and re-open returns same data
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, CreateSubkeyAndReopen) {
    // Create the key
    auto h1 = reg.open_key(HKEY_CURRENT_USER, L"Software\\TestApp", true);
    ASSERT_TRUE(h1.is_ok());

    // Write a value into it
    RegValue val{RegType::Sz, {std::wstring(L"hello")}};
    auto sr = reg.set_value(h1.value(), L"Greeting", val);
    ASSERT_TRUE(sr.is_ok());
    reg.close_key(h1.value());

    // Re-open (no create) and verify
    auto h2 = reg.open_key(HKEY_CURRENT_USER, L"Software\\TestApp", false);
    ASSERT_TRUE(h2.is_ok());

    auto qr = reg.query_value(h2.value(), L"Greeting");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(qr.value().type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(qr.value().data), L"hello");

    reg.close_key(h2.value());
}

// ---------------------------------------------------------------------------
// 3. Set/query REG_SZ value round-trips
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, SetQueryRegSzRoundTrip) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"StringTest", true);
    ASSERT_TRUE(h.is_ok());

    RegValue val{RegType::Sz, {std::wstring(L"test_string")}};
    ASSERT_TRUE(reg.set_value(h.value(), L"Name", val).is_ok());

    auto qr = reg.query_value(h.value(), L"Name");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(qr.value().type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(qr.value().data), L"test_string");

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 4. Set/query REG_DWORD value round-trips
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, SetQueryRegDwordRoundTrip) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"DwordTest", true);
    ASSERT_TRUE(h.is_ok());

    RegValue val{RegType::Dword, {u32{42}}};
    ASSERT_TRUE(reg.set_value(h.value(), L"Count", val).is_ok());

    auto qr = reg.query_value(h.value(), L"Count");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(qr.value().type, RegType::Dword);
    EXPECT_EQ(std::get<u32>(qr.value().data), u32{42});

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 5. Delete value removes it
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, DeleteValueRemovesIt) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"DeleteTest", true);
    ASSERT_TRUE(h.is_ok());

    RegValue val{RegType::Sz, {std::wstring(L"temp")}};
    ASSERT_TRUE(reg.set_value(h.value(), L"Temp", val).is_ok());

    // Confirm it's there
    ASSERT_TRUE(reg.query_value(h.value(), L"Temp").is_ok());

    // Delete it
    ASSERT_TRUE(reg.delete_value(h.value(), L"Temp").is_ok());

    // Should be gone
    auto qr = reg.query_value(h.value(), L"Temp");
    EXPECT_TRUE(qr.is_err());
    EXPECT_EQ(qr.error(), Error::NotFound);

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 6. Enum subkeys returns created keys
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, EnumSubkeysReturnsCreatedKeys) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"EnumParent", true);
    ASSERT_TRUE(h.is_ok());

    // Create three child keys
    ASSERT_TRUE(reg.open_key(h.value(), L"Alpha", true).is_ok());
    ASSERT_TRUE(reg.open_key(h.value(), L"Bravo", true).is_ok());
    ASSERT_TRUE(reg.open_key(h.value(), L"Charlie", true).is_ok());

    auto keys = reg.enum_keys(h.value());
    ASSERT_TRUE(keys.is_ok());
    ASSERT_EQ(keys.value().size(), usize{3});

    // Sort to check deterministically
    auto v = keys.value();
    std::sort(v.begin(), v.end());
    EXPECT_EQ(v[0], L"Alpha");
    EXPECT_EQ(v[1], L"Bravo");
    EXPECT_EQ(v[2], L"Charlie");

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 7. Case-insensitive key lookup
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, CaseInsensitiveKeyLookup) {
    // Create with mixed case
    auto h1 = reg.open_key(HKEY_CURRENT_USER, L"Software\\MyCoolApp", true);
    ASSERT_TRUE(h1.is_ok());

    RegValue val{RegType::Dword, {u32{7}}};
    ASSERT_TRUE(reg.set_value(h1.value(), L"Setting", val).is_ok());
    reg.close_key(h1.value());

    // Re-open with different case
    auto h2 = reg.open_key(HKEY_CURRENT_USER, L"SOFTWARE\\MYCOOLAPP", false);
    ASSERT_TRUE(h2.is_ok());

    auto qr = reg.query_value(h2.value(), L"setting");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(std::get<u32>(qr.value().data), u32{7});

    reg.close_key(h2.value());
}

// ---------------------------------------------------------------------------
// 8. Open non-existent key without create flag fails
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, OpenNonExistentWithoutCreateFails) {
    auto r = reg.open_key(HKEY_LOCAL_MACHINE, L"DoesNot\\Exist\\At\\All", false);
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), Error::NotFound);
}

// ---------------------------------------------------------------------------
// 9. Close handle invalidates it
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, CloseHandleInvalidatesIt) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"ToClose", true);
    ASSERT_TRUE(h.is_ok());
    auto handle = h.value();

    // Should work while open
    ASSERT_TRUE(reg.enum_keys(handle).is_ok());

    // Close it
    reg.close_key(handle);

    // Now operations on the closed handle fail
    auto qr = reg.query_value(handle, L"");
    EXPECT_TRUE(qr.is_err());
    EXPECT_EQ(qr.error(), Error::InvalidHandle);
}

// ---------------------------------------------------------------------------
// 10. Pre-populated HKLM CurrentVersion defaults
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, HklmCurrentVersionPrePopulated) {
    auto h = reg.open_key(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                           false);
    ASSERT_TRUE(h.is_ok());

    auto pn = reg.query_value(h.value(), L"ProductName");
    ASSERT_TRUE(pn.is_ok());
    EXPECT_EQ(pn.value().type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(pn.value().data), L"Windows 10 Enterprise");

    auto cb = reg.query_value(h.value(), L"CurrentBuild");
    ASSERT_TRUE(cb.is_ok());
    EXPECT_EQ(std::get<std::wstring>(cb.value().data), L"19041");  // pe::kOsBuildNumber

    auto maj = reg.query_value(h.value(), L"CurrentMajorVersionNumber");
    ASSERT_TRUE(maj.is_ok());
    EXPECT_EQ(maj.value().type, RegType::Dword);
    EXPECT_EQ(std::get<u32>(maj.value().data), u32{10});

    reg.close_key(h.value());
}

TEST_F(RegistryTest, HklmTcpipParametersPrePopulated) {
    // CEF DnsConfigServiceWin requires this key for KEY_NOTIFY watches.
    auto h = reg.open_key(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", false);
    ASSERT_TRUE(h.is_ok());
    auto ns = reg.query_value(h.value(), L"NameServer");
    ASSERT_TRUE(ns.is_ok());
    EXPECT_EQ(ns.value().type, RegType::Sz);
    EXPECT_FALSE(std::get<std::wstring>(ns.value().data).empty());
    reg.close_key(h.value());

    auto h6 = reg.open_key(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters", false);
    EXPECT_TRUE(h6.is_ok());
    if (h6.is_ok()) reg.close_key(h6.value());
}

TEST_F(RegistryTest, HklmFontSubstitutesPrePopulated) {
    // Learn, "Using MS Shell Dlg and MS Shell Dlg 2": the logical-font
    // mappings are stored in this key; "MS Shell Dlg" maps to Microsoft Sans
    // Serif (non-Japanese UI language) and "MS Shell Dlg 2" to Tahoma. The
    // key holds exactly Vitreo's documented alias set, nothing copied from a
    // Windows registry export.
    auto h = reg.open_key(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
        false);
    ASSERT_TRUE(h.is_ok());

    const auto shell = reg.query_value(h.value(), L"MS Shell Dlg");
    ASSERT_TRUE(shell.is_ok());
    EXPECT_EQ(shell.value().type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(shell.value().data),
              L"Microsoft Sans Serif");

    const auto shell2 = reg.query_value(h.value(), L"MS Shell Dlg 2");
    ASSERT_TRUE(shell2.is_ok());
    EXPECT_EQ(shell2.value().type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(shell2.value().data), L"Tahoma");

    for (const wchar_t* undocumented : {L"Helvetica", L"Times", L"Helv", L"Tms Rmn"}) {
        EXPECT_FALSE(reg.query_value(h.value(), undocumented).is_ok());
    }
    reg.close_key(h.value());
}

TEST_F(RegistryTest, RunAndRunOnceKeysPrePopulatedForUserAndMachine) {
    constexpr const wchar_t* paths[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
    };
    constexpr RegHandle roots[] = {
        HKEY_CURRENT_USER,
        HKEY_LOCAL_MACHINE,
    };

    for (const RegHandle root : roots) {
        for (const wchar_t* path : paths) {
            auto h = reg.open_key(root, path, false);
            ASSERT_TRUE(h.is_ok());
            reg.close_key(h.value());
        }
    }
}

// ---------------------------------------------------------------------------
// 11. Set/query REG_QWORD value
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, SetQueryRegQwordRoundTrip) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"QwordTest", true);
    ASSERT_TRUE(h.is_ok());

    RegValue val{RegType::Qword, {u64{0x123456789ABCDEF0ULL}}};
    ASSERT_TRUE(reg.set_value(h.value(), L"BigNumber", val).is_ok());

    auto qr = reg.query_value(h.value(), L"BigNumber");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(qr.value().type, RegType::Qword);
    EXPECT_EQ(std::get<u64>(qr.value().data), u64{0x123456789ABCDEF0ULL});

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 12. Set/query REG_BINARY value
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, SetQueryRegBinaryRoundTrip) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"BinaryTest", true);
    ASSERT_TRUE(h.is_ok());

    std::vector<u8> blob = {0xDE, 0xAD, 0xBE, 0xEF};
    RegValue val{RegType::Binary, {blob}};
    ASSERT_TRUE(reg.set_value(h.value(), L"Blob", val).is_ok());

    auto qr = reg.query_value(h.value(), L"Blob");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(qr.value().type, RegType::Binary);
    EXPECT_EQ(std::get<std::vector<u8>>(qr.value().data), blob);

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 13. Enum values lists value names
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, EnumValuesListsNames) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"ValEnumTest", true);
    ASSERT_TRUE(h.is_ok());

    reg.set_value(h.value(), L"Alpha", RegValue{RegType::Dword, {u32{1}}});
    reg.set_value(h.value(), L"Bravo", RegValue{RegType::Dword, {u32{2}}});

    auto vr = reg.enum_values(h.value());
    ASSERT_TRUE(vr.is_ok());
    ASSERT_EQ(vr.value().size(), usize{2});

    auto names = vr.value();
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], L"Alpha");
    EXPECT_EQ(names[1], L"Bravo");

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 14. Delete non-existent value returns NotFound
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, DeleteNonExistentValueFails) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"DelNonExist", true);
    ASSERT_TRUE(h.is_ok());

    auto dr = reg.delete_value(h.value(), L"ghost");
    EXPECT_TRUE(dr.is_err());
    EXPECT_EQ(dr.error(), Error::NotFound);

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// 15. Multi-level nested path creation
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, MultiLevelNestedPathCreation) {
    auto h = reg.open_key(HKEY_LOCAL_MACHINE,
                           L"A\\B\\C\\D\\E\\F", true);
    ASSERT_TRUE(h.is_ok());

    RegValue val{RegType::Sz, {std::wstring(L"deep")}};
    ASSERT_TRUE(reg.set_value(h.value(), L"Depth", val).is_ok());

    // Navigate back up and check children
    auto top = reg.open_key(HKEY_LOCAL_MACHINE, L"A", false);
    ASSERT_TRUE(top.is_ok());
    auto k = reg.enum_keys(top.value());
    ASSERT_TRUE(k.is_ok());
    ASSERT_EQ(k.value().size(), usize{1});
    EXPECT_EQ(k.value()[0], L"B");

    reg.close_key(h.value());
    reg.close_key(top.value());
}

// ---------------------------------------------------------------------------
// Hive path splitter tests
// ---------------------------------------------------------------------------
TEST(RegPathSplitTest, EmptyPath) {
    auto parts = split_reg_path(L"");
    EXPECT_TRUE(parts.empty());
}

TEST(RegPathSplitTest, SingleComponent) {
    auto parts = split_reg_path(L"SOFTWARE");
    ASSERT_EQ(parts.size(), usize{1});
    EXPECT_EQ(parts[0], L"SOFTWARE");
}

TEST(RegPathSplitTest, MultipleComponents) {
    auto parts = split_reg_path(L"SOFTWARE\\Microsoft\\Windows");
    ASSERT_EQ(parts.size(), usize{3});
    EXPECT_EQ(parts[0], L"SOFTWARE");
    EXPECT_EQ(parts[1], L"Microsoft");
    EXPECT_EQ(parts[2], L"Windows");
}

TEST(RegPathSplitTest, LeadingAndTrailingBackslash) {
    auto parts = split_reg_path(L"\\SOFTWARE\\Microsoft\\");
    ASSERT_EQ(parts.size(), usize{2});
    EXPECT_EQ(parts[0], L"SOFTWARE");
    EXPECT_EQ(parts[1], L"Microsoft");
}

// ---------------------------------------------------------------------------
// REG_MULTI_SZ round-trip
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, SetQueryRegMultiSzRoundTrip) {
    auto h = reg.open_key(HKEY_CURRENT_USER, L"MultiSzTest", true);
    ASSERT_TRUE(h.is_ok());

    std::vector<std::wstring> strs = {L"one", L"two", L"three"};
    RegValue val{RegType::MultiSz, {strs}};
    ASSERT_TRUE(reg.set_value(h.value(), L"List", val).is_ok());

    auto qr = reg.query_value(h.value(), L"List");
    ASSERT_TRUE(qr.is_ok());
    EXPECT_EQ(qr.value().type, RegType::MultiSz);
    auto& got = std::get<std::vector<std::wstring>>(qr.value().data);
    ASSERT_EQ(got.size(), usize{3});
    EXPECT_EQ(got[0], L"one");
    EXPECT_EQ(got[1], L"two");
    EXPECT_EQ(got[2], L"three");

    reg.close_key(h.value());
}

// ---------------------------------------------------------------------------
// Invalid handle returns proper error
// ---------------------------------------------------------------------------
TEST_F(RegistryTest, InvalidHandleErrors) {
    RegHandle bad = 0xDEADBEEF;

    auto qr = reg.query_value(bad, L"");
    EXPECT_TRUE(qr.is_err());
    EXPECT_EQ(qr.error(), Error::InvalidHandle);

    auto sr = reg.set_value(bad, L"x", RegValue{});
    EXPECT_TRUE(sr.is_err());
    EXPECT_EQ(sr.error(), Error::InvalidHandle);

    auto dr = reg.delete_value(bad, L"x");
    EXPECT_TRUE(dr.is_err());
    EXPECT_EQ(dr.error(), Error::InvalidHandle);

    auto ek = reg.enum_keys(bad);
    EXPECT_TRUE(ek.is_err());
    EXPECT_EQ(ek.error(), Error::InvalidHandle);

    auto ev = reg.enum_values(bad);
    EXPECT_TRUE(ev.is_err());
    EXPECT_EQ(ev.error(), Error::InvalidHandle);
}

// Storm SMemAlloc page-pad flags load from
// HKCU\Software\Blizzard Entertainment\Internal\"Debug Memory" as REG_DWORD.
// Hive round-trip must preserve the spaced path and DWORD so guest SRegLoad
// can enable the large-alloc guard page offset.
TEST_F(RegistryTest, HiveRoundTripPreservesSpacedPathDword) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-storm-flags-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    {
        std::ofstream out(directory / "HKCU.reg");
        out << "# Vitreo Registry Hive: HKCU.reg\n\n";
        out << "[Software\\\\Blizzard Entertainment\\\\Internal]\n";
        out << "\"Debug Memory\"=dword:1\n";
    }

    Registry loaded;
    loaded.load(directory);
    auto key = loaded.open_key(
        HKEY_CURRENT_USER,
        L"Software\\Blizzard Entertainment\\Internal",
        false);
    ASSERT_TRUE(key.is_ok());
    auto value = loaded.query_value(key.value(), L"Debug Memory");
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value().type, RegType::Dword);
    EXPECT_EQ(std::get<u32>(value.value().data), 1u);
    loaded.close_key(key.value());

    std::filesystem::remove_all(directory, error);
}

TEST(RegistryLiveJournalTest, PublishesKeyAndValueMutationsAcrossInstances) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-live-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    Registry writer;
    Registry reader;
    writer.load(directory);
    reader.load(directory);

    auto key = writer.open_key(
        HKEY_LOCAL_MACHINE,
        L"Software\\Classes\\CLSID\\"
        L"{12345678-ABCD-4321-8000-112233445566}",
        true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(writer.set_value(
        key.value(), L"AppID", RegValue{
            RegType::Sz,
            std::wstring(L"{87654321-DCBA-1234-8000-665544332211}")})
                    .is_ok());

    // The second process-shaped Registry object loaded before the mutation.
    // Its next lookup must replay the capsule journal and see the new key.
    auto peer_key = reader.open_key(
        HKEY_CLASSES_ROOT,
        L"CLSID\\{12345678-ABCD-4321-8000-112233445566}", false);
    ASSERT_TRUE(peer_key.is_ok());
    auto value = reader.query_value(peer_key.value(), L"AppID");
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(std::get<std::wstring>(value.value().data),
              L"{87654321-DCBA-1234-8000-665544332211}");

    ASSERT_TRUE(writer.set_value(
        key.value(), L"AppID", RegValue{
            RegType::Sz,
            std::wstring(L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}")})
                    .is_ok());
    value = reader.query_value(peer_key.value(), L"AppID");
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(std::get<std::wstring>(value.value().data),
              L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}");

    ASSERT_TRUE(writer.delete_value(key.value(), L"AppID").is_ok());
    EXPECT_TRUE(reader.query_value(peer_key.value(), L"AppID").is_err());
    ASSERT_TRUE(writer.set_value(
        key.value(), L"AppID", RegValue{
            RegType::Sz,
            std::wstring(L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}")})
                    .is_ok());

    auto child = writer.open_key(key.value(), L"Transient", true);
    ASSERT_TRUE(child.is_ok());
    auto peer_child = reader.open_key(peer_key.value(), L"Transient", false);
    ASSERT_TRUE(peer_child.is_ok());
    reader.close_key(peer_child.value());
    ASSERT_TRUE(writer.delete_key(key.value(), L"Transient").is_ok());
    EXPECT_TRUE(reader.open_key(
        peer_key.value(), L"Transient", false).is_err());

    writer.save(directory);
    Registry reloaded;
    reloaded.load(directory);
    auto reloaded_key = reloaded.open_key(
        HKEY_CLASSES_ROOT,
        L"CLSID\\{12345678-ABCD-4321-8000-112233445566}", false);
    ASSERT_TRUE(reloaded_key.is_ok());
    value = reloaded.query_value(reloaded_key.value(), L"AppID");
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(std::get<std::wstring>(value.value().data),
              L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}");

    std::filesystem::remove_all(directory, error);
}

TEST(RegistryLiveJournalTest, ReopeningExistingKeyDoesNotAppendMutation) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-live-reopen-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    Registry registry;
    registry.load(directory);
    auto first = registry.open_key(
        HKEY_CURRENT_USER, L"Software\\JournalReopen", true);
    ASSERT_TRUE(first.is_ok());
    registry.close_key(first.value());

    const auto journal = directory / "live.reglog";
    const auto after_create = std::filesystem::file_size(journal);
    ASSERT_GT(after_create, 0u);

    for (int iteration = 0; iteration < 100; ++iteration) {
        auto reopened = registry.open_key(
            HKEY_CURRENT_USER, L"Software\\JournalReopen", true);
        ASSERT_TRUE(reopened.is_ok());
        registry.close_key(reopened.value());
    }
    EXPECT_EQ(std::filesystem::file_size(journal), after_create);

    std::filesystem::remove_all(directory, error);
}

// A version subkey whose name is numeric with a dot ("1.0") is an ordinary key
// name, but it is also the shape SimCity 4 reads its install language from
// (HKLM\Software\Maxis\SimCity 4\1.0\Language). Loading it back has to survive
// both the dotted component and the mixed case of the query.
TEST_F(RegistryTest, HiveRoundTripPreservesDottedVersionSubkey) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-dotted-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    {
        std::ofstream out(directory / "HKLM.reg");
        out << "# Vitreo Registry Hive: HKLM.reg\n\n";
        out << "[SOFTWARE\\\\maxis\\\\simcity 4]\n";
        out << "\"IsDeluxe\"=dword:1\n\n";
        out << "[SOFTWARE\\\\maxis\\\\simcity 4\\\\1.0]\n";
        out << "\"Language\"=sz:UKEnglsh\n";
    }

    Registry loaded;
    loaded.load(directory);
    auto key = loaded.open_key(
        HKEY_LOCAL_MACHINE, L"Software\\Maxis\\SimCity 4\\1.0", false);
    ASSERT_TRUE(key.is_ok());
    auto value = loaded.query_value(key.value(), L"Language");
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value().type, RegType::Sz);
    loaded.close_key(key.value());

    std::filesystem::remove_all(directory, error);
}

// Load-then-save must not lose subkeys. The installer writes SimCity 4's
// language under a version subkey and Vitreo persists the hive on exit; a save
// that drops untouched keys silently un-installs whatever the guest wrote in
// an earlier session.
TEST_F(RegistryTest, SaveAfterLoadPreservesUntouchedSubkeys) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-resave-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    {
        std::ofstream out(directory / "HKLM.reg");
        out << "# Vitreo Registry Hive: HKLM.reg\n\n";
        out << "[SOFTWARE\\\\maxis\\\\simcity 4]\n";
        out << "\"IsDeluxe\"=dword:1\n\n";
        out << "[SOFTWARE\\\\maxis\\\\simcity 4\\\\1.0]\n";
        out << "\"Language\"=sz:UKEnglsh\n";
    }

    Registry first;
    first.load(directory);
    first.save(directory);

    Registry second;
    second.load(directory);
    auto key = second.open_key(
        HKEY_LOCAL_MACHINE, L"Software\\Maxis\\SimCity 4\\1.0", false);
    ASSERT_TRUE(key.is_ok()) << "version subkey lost across load/save";
    auto value = second.query_value(key.value(), L"Language");
    EXPECT_TRUE(value.is_ok());
    second.close_key(key.value());

    std::filesystem::remove_all(directory, error);
}

// HKLM\SOFTWARE\Microsoft\DirectX as every Windows since XP SP2 has it
// (winoracle `reg query`, Windows 11 26200): Version "4.09.00.0904" and
// InstalledVersion 00 00 00 09 00 00 00 00.
TEST(RegistryDefaultsTest, DirectXVersionMatchesWindows) {
    Registry reg;
    auto key = reg.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\DirectX", false);
    ASSERT_TRUE(key.is_ok());
    auto version = reg.query_value(key.value(), L"Version");
    ASSERT_TRUE(version.is_ok());
    EXPECT_EQ(version.value().type, RegType::Sz);
    EXPECT_EQ(std::get<std::wstring>(version.value().data), L"4.09.00.0904");
    auto installed = reg.query_value(key.value(), L"InstalledVersion");
    ASSERT_TRUE(installed.is_ok());
    EXPECT_EQ(installed.value().type, RegType::Binary);
    EXPECT_EQ(std::get<std::vector<u8>>(installed.value().data), (std::vector<u8>{0, 0, 0, 9, 0, 0, 0, 0}));
    reg.close_key(key.value());
}

// =============================================================================
// WOW64 registry views (Learn, "Registry Redirector"; oracle files from
// tools/oracle_probe/wow64_registry_probe.c, Windows 11 26200)
// =============================================================================

namespace {

std::wstring lower_path(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    return s;
}

std::wstring join_path(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& p : parts) {
        if (!out.empty()) out.push_back(L'\\');
        out += p;
    }
    return lower_path(out);
}

std::wstring widen_ascii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::filesystem::path oracle_file(const char* name) {
    return std::filesystem::path(__FILE__).parent_path() / "../../oracle_data" / name;
}

}  // namespace

// Every key the oracle's 32-bit walk of HKLM\SOFTWARE and HKCU\Software
// classified (wow64_registry_walk.x86.expected: a line per key whose
// classification differs from its parent's) maps to the same physical key.
TEST(RegistryWow64RedirectTest, MatchesOracleWalk) {
    std::ifstream in(oracle_file("wow64_registry_walk.x86.expected"));
    ASSERT_TRUE(in.is_open());
    std::string line;
    int checked = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        const auto arrow = line.find(" -> ");
        if (space == std::string::npos || arrow == std::string::npos) continue;
        const std::string kind = line.substr(0, space);
        std::string key = line.substr(space + 1, arrow - space - 1);
        std::string nt = line.substr(arrow + 4);
        std::vector<std::wstring> logical;
        if (key.rfind("HKLM\\", 0) == 0) {
            logical = split_reg_path(L"HKEY_LOCAL_MACHINE\\" + widen_ascii(key.substr(5)));
        } else if (key.rfind("HKCU\\", 0) == 0) {
            logical = split_reg_path(L"HKEY_CURRENT_USER\\" + widen_ascii(key.substr(5)));
        } else {
            continue;
        }
        const std::wstring got = join_path(Registry::redirect_path(logical, /*wow32=*/true));
        if (kind == "shared") {
            EXPECT_EQ(got, join_path(logical)) << line;
            ++checked;
        } else if (kind == "redirected" || kind == "absent32") {
            if (kind == "redirected") {
                // \REGISTRY\MACHINE -> HKEY_LOCAL_MACHINE; the user hive and
                // its _Classes hive -> HKEY_CURRENT_USER[\Software\Classes].
                std::wstring expected = widen_ascii(nt);
                const auto replace = [&](const std::wstring& from, const std::wstring& to) {
                    if (expected.rfind(from, 0) == 0) expected = to + expected.substr(from.size());
                };
                replace(L"\\REGISTRY\\MACHINE", L"HKEY_LOCAL_MACHINE");
                if (expected.rfind(L"\\REGISTRY\\USER\\", 0) == 0) {
                    const auto end = expected.find(L'\\', 15);
                    const std::wstring hive = expected.substr(15, end - 15);
                    const bool classes = hive.size() > 8 && hive.substr(hive.size() - 8) == L"_Classes";
                    expected = std::wstring(classes ? L"HKEY_CURRENT_USER\\Software\\Classes"
                                                    : L"HKEY_CURRENT_USER") +
                               (end == std::wstring::npos ? L"" : expected.substr(end));
                }
                EXPECT_EQ(got, lower_path(expected)) << line;
            } else {
                EXPECT_NE(got.find(L"wow6432node"), std::wstring::npos) << line;
            }
            ++checked;
        }
    }
    EXPECT_GT(checked, 80);
}

// The OS keys Vitreo seeds exist in the view Windows has them in
// (wow64_registry_seeds.x86.expected).
TEST(RegistryWow64DefaultsTest, SeedsBothViewsAsWindows) {
    Registry reg;
    const auto text = [&](RegView view, const wchar_t* path, const wchar_t* name) -> std::wstring {
        auto key = reg.open_key(HKEY_LOCAL_MACHINE, path, false, view);
        if (key.is_err()) return L"<missing>";
        auto value = reg.query_value(key.value(), name);
        reg.close_key(key.value());
        if (value.is_err()) return L"<no value>";
        return std::get<std::wstring>(value.value().data);
    };
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion";
    EXPECT_EQ(text(RegView::Native64, cv, L"ProgramFilesDir"), L"C:\\Program Files");
    EXPECT_EQ(text(RegView::Wow32, cv, L"ProgramFilesDir"), L"C:\\Program Files (x86)");
    EXPECT_EQ(text(RegView::Native64, cv, L"CommonFilesDir"), L"C:\\Program Files\\Common Files");
    EXPECT_EQ(text(RegView::Wow32, cv, L"CommonFilesDir"), L"C:\\Program Files (x86)\\Common Files");
    for (RegView view : {RegView::Native64, RegView::Wow32}) {
        EXPECT_EQ(text(view, cv, L"ProgramFilesDir (x86)"), L"C:\\Program Files (x86)");
        EXPECT_EQ(text(view, cv, L"ProgramW6432Dir"), L"C:\\Program Files");
        EXPECT_EQ(text(view, cv, L"CommonW6432Dir"), L"C:\\Program Files\\Common Files");
        EXPECT_EQ(text(view, L"SOFTWARE\\Microsoft\\DirectX", L"Version"), L"4.09.00.0904");
        EXPECT_EQ(text(view, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentVersion"), L"6.3");
        EXPECT_EQ(text(view, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
                       L"Common AppData"),
                  L"C:\\ProgramData");
        EXPECT_NE(text(view, L"SOFTWARE\\Classes\\CLSID\\{F6D90F11-9C73-11D3-B32E-00C04F990BB4}\\InprocServer32",
                       L""),
                  L"<missing>");
        EXPECT_NE(text(view, L"SOFTWARE\\Classes\\CLSID\\{CACAF262-9370-4615-A13B-9F5539DA4C0A}\\InprocServer32",
                       L""),
                  L"<missing>");
        auto run = reg.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", false,
                                view);
        EXPECT_TRUE(run.is_ok());
    }
    // Shared keys have one copy: the 32-bit view opens the native key.
    auto fonts = reg.open_key(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", false,
                              RegView::Wow32);
    ASSERT_TRUE(fonts.is_ok());
    EXPECT_EQ(reg.canonical_key_path(fonts.value()),
              L"hkey_local_machine\\software\\microsoft\\windows nt\\currentversion\\fontsubstitutes");
    // The OS .NET shim's InstallRoot, per view (seeds lines 238 / 241).
    EXPECT_EQ(text(RegView::Native64, L"SOFTWARE\\Microsoft\\.NETFramework", L"InstallRoot"),
              L"C:\\Windows\\Microsoft.NET\\Framework64\\");
    EXPECT_EQ(text(RegView::Wow32, L"SOFTWARE\\Microsoft\\.NETFramework", L"InstallRoot"),
              L"C:\\Windows\\Microsoft.NET\\Framework\\");
    // Nothing else of the Framework: the installer owns it.
    EXPECT_TRUE(reg.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\.NETFramework\\Policy", false,
                             RegView::Native64).is_err());
}

// Both views persist in the 64-bit Windows layout (32-bit data under
// WOW6432Node) and a capsule written before views existed still loads, its
// keys in the 64-bit view.
TEST(RegistryWow64PersistenceTest, ViewsRoundTripAndOldCapsulesLoad) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-wow64-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    {
        // An old-format hive: no WOW6432Node anywhere.
        std::ofstream old(directory / "HKLM.reg");
        old << "# Vitreo Registry Hive: HKLM.reg\n\n[SOFTWARE\\OldVendor]\n\"Path\"=sz:C:\\\\Old\n\n";
    }
    {
        Registry wow;
        wow.set_wow64_process(true);
        wow.load(directory);
        auto key = wow.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NewVendor", true);
        ASSERT_TRUE(key.is_ok());
        ASSERT_TRUE(wow.set_value(key.value(), L"Bits", RegValue{RegType::Dword, {u32{32}}}).is_ok());
        EXPECT_EQ(wow.canonical_key_path(key.value()), L"hkey_local_machine\\software\\wow6432node\\newvendor");
        // The old data sits in the 64-bit view, which a 32-bit process does
        // not see by default.
        EXPECT_TRUE(wow.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OldVendor", false).is_err());
        EXPECT_TRUE(wow.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OldVendor", false, RegView::Native64).is_ok());
        wow.save(directory);
    }
    std::ifstream in(directory / "HKLM.reg");
    const std::string hive((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(hive.find("[SOFTWARE\\\\WOW6432Node\\\\NewVendor]"), std::string::npos);
    EXPECT_NE(hive.find("[SOFTWARE\\\\OldVendor]"), std::string::npos);

    Registry native;
    native.load(directory);
    EXPECT_TRUE(native.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NewVendor", false).is_err());
    auto wow_key = native.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\NewVendor", false);
    ASSERT_TRUE(wow_key.is_ok());
    auto bits = native.query_value(wow_key.value(), L"Bits");
    ASSERT_TRUE(bits.is_ok());
    EXPECT_EQ(std::get<u32>(bits.value().data), 32u);
    auto old_key = native.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OldVendor", false);
    ASSERT_TRUE(old_key.is_ok());
    EXPECT_EQ(std::get<std::wstring>(native.query_value(old_key.value(), L"Path").value().data), L"C:\\Old");
    std::filesystem::remove_all(directory, error);
}

// A 32- and a 64-bit process of one capsule share the store through the live
// journal, each seeing the other's writes in the right view.
TEST(RegistryWow64LiveJournalTest, ViewsShareOneStoreAcrossBitness) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("vitreo-registry-wow64-live-" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    Registry wow;
    Registry native;
    wow.set_wow64_process(true);
    wow.load(directory);
    native.load(directory);

    auto key = wow.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vendor\\App", true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(wow.set_value(key.value(), L"Bits", RegValue{RegType::Dword, {u32{32}}}).is_ok());
    auto clsid = wow.open_key(HKEY_CLASSES_ROOT, L"CLSID\\{0F0E0D0C-0B0A-0908-0706-050403020100}\\InprocServer32",
                              true);
    ASSERT_TRUE(clsid.is_ok());
    ASSERT_TRUE(wow.set_value(clsid.value(), L"", RegValue{RegType::Sz, {std::wstring(L"x86.dll")}}).is_ok());

    EXPECT_TRUE(native.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vendor\\App", false).is_err());
    auto peer = native.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Vendor\\App", false);
    ASSERT_TRUE(peer.is_ok());
    EXPECT_EQ(std::get<u32>(native.query_value(peer.value(), L"Bits").value().data), 32u);
    // The peer creates the keys with their names' case.
    auto vendor = native.open_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Vendor", false);
    ASSERT_TRUE(vendor.is_ok());
    EXPECT_EQ(native.enum_keys(vendor.value()).value(), std::vector<std::wstring>{L"App"});
    EXPECT_TRUE(native.open_key(HKEY_CLASSES_ROOT, L"CLSID\\{0F0E0D0C-0B0A-0908-0706-050403020100}", false)
                    .is_err());
    auto peer_clsid = native.open_key(
        HKEY_CLASSES_ROOT, L"CLSID\\{0F0E0D0C-0B0A-0908-0706-050403020100}\\InprocServer32", false,
        RegView::Wow32);
    ASSERT_TRUE(peer_clsid.is_ok());
    EXPECT_EQ(std::get<std::wstring>(native.query_value(peer_clsid.value(), L"").value().data), L"x86.dll");
    std::filesystem::remove_all(directory, error);
}
