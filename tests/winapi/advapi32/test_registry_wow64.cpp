/// @file test_registry_wow64.cpp
/// @brief The WOW64 registry redirector through the advapi32 thunks.
///
/// Every check replays one line of tools/oracle_probe/wow64_registry_probe.c
/// and compares Vitreo's result with that line of the Windows 11 output,
/// tests/oracle_data/wow64_registry_probe.expected (x64 process) and
/// wow64_registry_probe.x86.expected (x86 process, i.e. WOW64). The NT key
/// name a line reports is compared as Vitreo's key path ("\REGISTRY\MACHINE"
/// is hkey_local_machine, the user's hive and its _Classes hive are
/// hkey_current_user and hkey_current_user\software\classes).
///
/// The oracle machine has keys every Windows installation has (Microsoft\Ole,
/// Policies, Classes\Interface ...); SetUp creates the ones the probe touches
/// in the 64-bit Windows layout before the replay.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;

namespace {

constexpr u32 KEY_READ_ = 0x20019;
constexpr u32 KEY_ALL_ACCESS_ = 0xF003F;
constexpr u32 W64 = 0x0100;  // KEY_WOW64_64KEY
constexpr u32 W32 = 0x0200;  // KEY_WOW64_32KEY

const std::wstring P = L"VitreoProbe-W64R";
const std::wstring G1 = L"{7E1C3B5A-0D42-4F7B-9A61-5C2E8B3D4A01}";
const std::wstring G2 = L"{7E1C3B5A-0D42-4F7B-9A61-5C2E8B3D4A02}";
const std::wstring G3 = L"{7E1C3B5A-0D42-4F7B-9A61-5C2E8B3D4A03}";

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// tag -> expected line, with the NT key name rewritten to Vitreo's key path.
std::map<std::string, std::string> load_expected(bool x86) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "../../oracle_data" /
                      (x86 ? "wow64_registry_probe.x86.expected" : "wow64_registry_probe.expected");
    std::ifstream in(path);
    std::map<std::string, std::string> out;
    const std::regex user_classes(R"(\\REGISTRY\\USER\\S-[0-9-]+_Classes)");
    const std::regex user(R"(\\REGISTRY\\USER\\S-[0-9-]+)");
    const std::regex machine(R"(\\REGISTRY\\MACHINE)");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        line = line.substr(first);
        // Tags can hold spaces ("class_Media Type_hkcr_32key").
        auto space = line.find(" ret=");
        if (space == std::string::npos) space = line.find(" status=");
        if (space == std::string::npos) space = line.find(' ');
        if (space == std::string::npos) continue;
        const std::string tag = line.substr(0, space);
        if (tag.rfind("disp=", 0) == 0) continue;
        line = std::regex_replace(line, user_classes, "hkey_current_user\\software\\classes");
        line = std::regex_replace(line, user, "hkey_current_user");
        line = std::regex_replace(line, machine, "hkey_local_machine");
        const auto name = line.find(" name=");
        if (name != std::string::npos) {
            auto end = line.find(" view=", name);
            if (end == std::string::npos) end = line.size();
            line = line.substr(0, name + 6) + lower(line.substr(name + 6, end - name - 6)) +
                   line.substr(end);
        }
        out.emplace(tag, line);
    }
    return out;
}

class RegistryWow64Test : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        x86 = GetParam();
        registry = std::make_unique<Registry>();
        handles = std::make_unique<HandleTable>();
        thunks = std::make_unique<RegistryThunks>(*registry, *handles);
        // Keys every Windows installation has, in the 64-bit layout.
        for (const wchar_t* k : {
                 L"SOFTWARE\\Clients", L"SOFTWARE\\Policies", L"SOFTWARE\\RegisteredApplications",
                 L"SOFTWARE\\Microsoft\\Ole", L"SOFTWARE\\Microsoft\\COM3", L"SOFTWARE\\Microsoft\\Rpc",
                 L"SOFTWARE\\WOW6432Node\\Microsoft\\Cryptography",
                 L"SOFTWARE\\Classes\\Interface", L"SOFTWARE\\Classes\\TypeLib", L"SOFTWARE\\Classes\\AppID",
                 L"SOFTWARE\\Classes\\DirectShow", L"SOFTWARE\\Classes\\Media Type",
                 L"SOFTWARE\\Classes\\MediaFoundation", L"SOFTWARE\\Classes\\.txt",
                 L"SOFTWARE\\Classes\\Installer", L"SOFTWARE\\Classes\\Record",
                 L"SOFTWARE\\Classes\\Component Categories", L"SOFTWARE\\Classes\\PROTOCOLS",
                 L"SOFTWARE\\Classes\\MIME", L"SOFTWARE\\Classes\\Drive", L"SOFTWARE\\Classes\\Folder",
                 L"SOFTWARE\\Classes\\WOW6432Node\\Interface", L"SOFTWARE\\Classes\\WOW6432Node\\TypeLib",
                 L"SOFTWARE\\Classes\\WOW6432Node\\AppID", L"SOFTWARE\\Classes\\WOW6432Node\\DirectShow",
                 L"SOFTWARE\\Classes\\WOW6432Node\\Media Type",
                 L"SOFTWARE\\Classes\\WOW6432Node\\MediaFoundation"}) {
            make(HKEY_LOCAL_MACHINE, k);
        }
        for (const wchar_t* k : {
                 L"Software\\Wow6432Node", L"Software\\Classes\\CLSID", L"Software\\Classes\\Interface",
                 L"Software\\Classes\\TypeLib", L"Software\\Classes\\AppID", L"Software\\Classes\\.txt",
                 L"Software\\Classes\\WOW6432Node\\CLSID", L"Software\\Classes\\WOW6432Node\\Interface"}) {
            make(HKEY_CURRENT_USER, k);
        }
        registry->set_wow64_process(x86);
        expected = load_expected(x86);
        ASSERT_FALSE(expected.empty());
    }

    void make(RegHandle root, const wchar_t* path) {
        auto h = registry->open_key(root, path, true, RegView::Native64);
        ASSERT_TRUE(h.is_ok());
        registry->close_key(h.value());
    }

    std::string describe(const std::string& tag, u32 err, u64 h) {
        std::string line = tag + " ret=" + std::to_string(err);
        if (err != ERROR_SUCCESS) return line;
        const auto path = thunks->key_path(h);
        line += " name=" + (path ? narrow(*path) : std::string("?"));
        u8 buf[128] = {};
        u32 type = 0, size = sizeof buf;
        std::string view = "-";
        if (thunks->reg_query_value_ex_w(h, L"view", &type, buf, &size) == ERROR_SUCCESS) {
            view.clear();
            for (u32 i = 0; i + 1 < size && (buf[i] || buf[i + 1]); i += 2) view.push_back(static_cast<char>(buf[i]));
        }
        return line + " view=" + view;
    }

    /// Compare one replayed line with the oracle's. A merged HKCR handle has
    /// no single NT name in Vitreo, so for it only ret and view are compared.
    void check(const std::string& actual) {
        const std::string tag = actual.substr(0, actual.find(" ret="));
        const auto it = expected.find(tag);
        ASSERT_NE(it, expected.end()) << "no oracle line for " << tag;
        if (actual.find(" name=hkey_classes_root") != std::string::npos) {
            const auto strip = [](const std::string& s) {
                const auto n = s.find(" name=");
                if (n == std::string::npos) return s;
                return s.substr(0, n) + s.substr(s.find(" view=", n));
            };
            EXPECT_EQ(strip(actual), strip(it->second)) << (x86 ? "x86" : "x64");
            return;
        }
        EXPECT_EQ(actual, it->second) << (x86 ? "x86" : "x64");
    }

    u64 create(const std::string& tag, RegHandle root, const std::wstring& sub, u32 sam,
               const wchar_t* view) {
        u64 h = 0;
        u32 disp = 0;
        const u32 err = thunks->reg_create_key_ex_w(root, sub, &h, &disp, KEY_ALL_ACCESS_ | sam);
        if (err == ERROR_SUCCESS && view != nullptr) {
            const std::wstring v(view);
            EXPECT_EQ(thunks->reg_set_value_ex_w(h, L"view", 1,
                                                 reinterpret_cast<const u8*>(std::u16string(v.begin(), v.end()).c_str()),
                                                 static_cast<u32>((v.size() + 1) * 2)),
                      ERROR_SUCCESS);
        }
        if (!tag.empty()) check(describe(tag, err, h));
        return err == ERROR_SUCCESS ? h : 0;
    }

    void open(const std::string& tag, u64 root, const std::wstring& sub, u32 sam) {
        u64 h = 0;
        const u32 err = thunks->reg_open_key_ex_w(root, sub, 0, sam, &h);
        check(describe(tag, err, h));
        if (err == ERROR_SUCCESS) thunks->reg_close_key(h);
    }

    bool has_subkey(u64 h, const std::wstring& name) {
        for (u32 i = 0;; ++i) {
            std::wstring n;
            if (thunks->reg_enum_key_ex_w(h, i, n) != ERROR_SUCCESS) return false;
            std::wstring a = n, b = name;
            std::transform(a.begin(), a.end(), a.begin(), ::towlower);
            std::transform(b.begin(), b.end(), b.begin(), ::towlower);
            if (a == b) return true;
        }
    }

    void enumerate(const std::string& tag, RegHandle root, const std::wstring& sub, u32 sam,
                   const std::vector<std::wstring>& names) {
        u64 h = 0;
        const u32 err = thunks->reg_open_key_ex_w(root, sub, 0, KEY_READ_ | sam, &h);
        std::string line = tag + " ret=" + std::to_string(err);
        if (err == ERROR_SUCCESS) {
            for (const auto& n : names) line += " " + narrow(n) + "=" + (has_subkey(h, n) ? "1" : "0");
            thunks->reg_close_key(h);
        }
        check(line);
    }

    bool exists(const std::wstring& sub, u32 view) {
        u64 h = 0;
        if (thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ_ | view, &h) != ERROR_SUCCESS)
            return false;
        thunks->reg_close_key(h);
        return true;
    }

    bool x86 = false;
    std::unique_ptr<Registry> registry;
    std::unique_ptr<HandleTable> handles;
    std::unique_ptr<RegistryThunks> thunks;
    std::map<std::string, std::string> expected;
};

TEST_P(RegistryWow64Test, CreateAndOpenInEachView) {
    const u64 c64 = create("create_hklm_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, W64, L"64");
    const u64 c32 = create("create_hklm_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, W32, L"32");
    (void)create("create_hklm_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Def", 0, L"def");
    (void)create("create_both_flags", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Both", W32 | W64, nullptr);
    open("open_both_flags", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, KEY_READ_ | W32 | W64);
    open("open_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, KEY_READ_);
    open("open_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, KEY_READ_ | W32);
    open("open_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, KEY_READ_ | W64);
    open("open_lower_case", HKEY_LOCAL_MACHINE, L"software\\vitreoprobe-w64r", KEY_READ_);

    // Explicit WOW6432Node paths and the links WOW6432Node carries.
    open("explicit_wow_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\" + P, KEY_READ_);
    open("explicit_wow_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\" + P, KEY_READ_ | W32);
    open("explicit_wow_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\" + P, KEY_READ_ | W64);
    open("explicit_wow_lower_32key", HKEY_LOCAL_MACHINE, L"software\\wow6432node\\" + P, KEY_READ_ | W32);
    open("explicit_wow_root_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node", KEY_READ_);
    open("explicit_wow_root_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node", KEY_READ_ | W32);
    open("explicit_wow_root_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node", KEY_READ_ | W64);
    open("explicit_wow_wow_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Wow6432Node", KEY_READ_ | W32);
    open("explicit_wow_wow_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Wow6432Node", KEY_READ_ | W64);
    open("explicit_wow_classes_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Classes", KEY_READ_ | W32);
    open("explicit_wow_classes_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Classes", KEY_READ_ | W64);
    open("explicit_wow_classes_clsid_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Classes\\CLSID",
         KEY_READ_ | W64);
    open("explicit_wow_policies_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Policies", KEY_READ_ | W64);
    open("explicit_wow_clients_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Clients", KEY_READ_ | W64);
    open("explicit_wow_regapps_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\RegisteredApplications",
         KEY_READ_ | W64);
    open("explicit_wow_ole_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft\\Ole", KEY_READ_ | W64);
    open("explicit_wow_ole_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft\\Ole", KEY_READ_ | W32);
    open("explicit_wow_ole_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft\\Ole", KEY_READ_);
    open("explicit_wow_classes_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Classes", KEY_READ_);
    open("hkcr_wow_clsid_default", HKEY_CLASSES_ROOT, L"Wow6432Node\\CLSID", KEY_READ_);
    open("hkcr_wow_clsid_32key", HKEY_CLASSES_ROOT, L"Wow6432Node\\CLSID", KEY_READ_ | W32);
    open("explicit_classes_wow_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Wow6432Node", KEY_READ_ | W64);
    open("explicit_classes_wow_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Wow6432Node", KEY_READ_ | W32);
    (void)create("create_explicit_wow_child_default", HKEY_LOCAL_MACHINE,
                 L"SOFTWARE\\Wow6432Node\\" + P + L"\\ExplicitChild", 0, nullptr);
    open("seen_from_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\ExplicitChild", KEY_READ_ | W32);
    open("seen_from_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\ExplicitChild", KEY_READ_ | W64);

    // The view a handle was opened in carries to opens relative to it.
    u64 s32 = 0, s64 = 0, sdef = 0;
    ASSERT_EQ(thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ_ | W32, &s32), 0u);
    ASSERT_EQ(thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ_ | W64, &s64), 0u);
    ASSERT_EQ(thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ_, &sdef), 0u);
    check(describe("software_32key", 0, s32));
    check(describe("software_64key", 0, s64));
    check(describe("software_default", 0, sdef));
    open("from_s32_child_default", s32, P, KEY_READ_);
    open("from_s32_child_64key", s32, P, KEY_READ_ | W64);
    open("from_s32_child_32key", s32, P, KEY_READ_ | W32);
    open("from_s64_child_default", s64, P, KEY_READ_);
    open("from_s64_child_32key", s64, P, KEY_READ_ | W32);
    open("from_s64_child_64key", s64, P, KEY_READ_ | W64);
    open("from_sdef_child_default", sdef, P, KEY_READ_);
    open("from_sdef_child_32key", sdef, P, KEY_READ_ | W32);
    open("from_sdef_child_64key", sdef, P, KEY_READ_ | W64);
    open("from_s64_empty_32key", s64, L"", KEY_READ_ | W32);
    open("from_s32_empty_64key", s32, L"", KEY_READ_ | W64);
    open("from_s32_microsoft_ole_default", s32, L"Microsoft\\Ole", KEY_READ_);
    open("from_s32_classes_default", s32, L"Classes", KEY_READ_);
    open("from_s32_classes_clsid_default", s32, L"Classes\\CLSID", KEY_READ_);
    open("from_s32_classes_64key", s32, L"Classes", KEY_READ_ | W64);
    open("from_s32_microsoft_ole_64key", s32, L"Microsoft\\Ole", KEY_READ_ | W64);
    open("from_s64_microsoft_ole_32key", s64, L"Microsoft\\Ole", KEY_READ_ | W32);
    u64 r32 = 0, r64 = 0;
    check("hklm_null_32key ret=" +
          std::to_string(thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"", 0, KEY_READ_ | W32, &r32)));
    open("from_hklm32_software_child_default", r32, L"SOFTWARE\\" + P, KEY_READ_);
    check("hklm_null_64key ret=" +
          std::to_string(thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"", 0, KEY_READ_ | W64, &r64)));
    open("from_hklm64_software_child_default", r64, L"SOFTWARE\\" + P, KEY_READ_);
    for (u64 h : {s32, s64, sdef, r32, r64}) thunks->reg_close_key(h);

    // Enumeration in each view.
    const std::vector<std::wstring> names{L"Wow6432Node", L"Classes", L"Clients", L"Policies",
                                          L"RegisteredApplications", P, L"Microsoft"};
    enumerate("enum_software_default", HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, names);
    enumerate("enum_software_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE", W32, names);
    enumerate("enum_software_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE", W64, names);
    enumerate("enum_wow_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node", W64, names);
    const std::vector<std::wstring> cnames{L"Wow6432Node", L"CLSID", L"Interface", L"TypeLib", L"AppID", L".txt"};
    enumerate("enum_classes_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0, cnames);
    enumerate("enum_classes_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", W32, cnames);
    enumerate("enum_classes_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", W64, cnames);
    enumerate("enum_classes_wow_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Wow6432Node", W64, cnames);
    enumerate("enum_hkcr_default", HKEY_CLASSES_ROOT, L"", 0, cnames);
    enumerate("enum_hkcr_32key", HKEY_CLASSES_ROOT, L"", W32, cnames);
    enumerate("enum_hkcr_64key", HKEY_CLASSES_ROOT, L"", W64, cnames);
    const std::vector<std::wstring> mnames{L"Ole", L"COM3", L"Cryptography", L"Windows", L"Windows NT",
                                           L"DirectX", L"Rpc"};
    enumerate("enum_microsoft_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft", W32, mnames);
    enumerate("enum_wow_microsoft_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft", W64, mnames);
    const std::vector<std::wstring> pnames{L"Def", L"ExplicitChild"};
    enumerate("enum_probe_default", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, 0, pnames);
    enumerate("enum_probe_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, W32, pnames);
    enumerate("enum_probe_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, W64, pnames);

    // RegQueryInfoKey counts the view's own subkeys.
    for (const auto& [tag, h] : {std::pair<std::string, u64>{"c32_info", c32}, {"c64_info", c64}}) {
        u32 subkeys = 0, maxsub = 0, values = 0;
        const u32 err = thunks->reg_query_info_key_a(h, &subkeys, &maxsub, &values, nullptr, nullptr);
        check(tag + " ret=" + std::to_string(err) + " subkeys=" + std::to_string(subkeys) +
              " maxsub=" + std::to_string(maxsub) + " values=" + std::to_string(values));
    }

    // RegGetValue RRF_SUBKEY_WOW6464KEY / RRF_SUBKEY_WOW6432KEY.
    const std::pair<const char*, u32> getvalue[] = {
        {"getvalue_default", 0}, {"getvalue_6464", 0x10000}, {"getvalue_6432", 0x20000},
        {"getvalue_both", 0x30000}};
    for (const auto& [tag, flags] : getvalue) {
        u8 buf[64] = {};
        u32 size = sizeof buf;
        u32 type = 0;
        const u32 err = thunks->reg_get_value_w(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, L"view", 0x2 | flags,
                                                &type, buf, &size);
        std::string view = "-";
        if (err == ERROR_SUCCESS) {
            view.clear();
            for (u32 i = 0; i + 1 < size && buf[i]; i += 2) view.push_back(static_cast<char>(buf[i]));
        }
        check(std::string(tag) + " ret=" + std::to_string(err) + " view=" + view);
    }
    thunks->reg_close_key(c32);
    thunks->reg_close_key(c64);
}

TEST_P(RegistryWow64Test, DeleteInEachView) {
    (void)create("", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, W64, L"64");
    (void)create("", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, W32, L"32");
    const char* cases[] = {"delex_32key", "delex_64key", "delex_zero", "delkey_default", "deltree_default",
                           "delex_read_32key", "delex_both", "delex_read_only"};
    for (int c = 0; c < 8; ++c) {
        for (u32 view : {W64, W32}) {
            thunks->reg_close_key(create("", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Del", view, nullptr));
            thunks->reg_close_key(
                create("", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Tree\\Leaf", view, nullptr));
        }
        const std::wstring del = L"SOFTWARE\\" + P + L"\\Del";
        u32 err = 0;
        switch (c) {
            case 0: err = thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, del, W32, 0); break;
            case 1: err = thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, del, W64, 0); break;
            case 2: err = thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, del, 0, 0); break;
            case 3: err = thunks->reg_delete_key(HKEY_LOCAL_MACHINE, del); break;
            case 4: err = thunks->reg_delete_tree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Tree"); break;
            case 5: err = thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, del, KEY_READ_ | W32, 0); break;
            case 6: err = thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, del, W32 | W64, 0); break;
            case 7: err = thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, del, KEY_READ_, 0); break;
        }
        const std::wstring sub = c == 4 ? L"SOFTWARE\\" + P + L"\\Tree" : del;
        check(std::string(cases[c]) + " ret=" + std::to_string(err) + " left32=" +
              (exists(sub, W32) ? "1" : "0") + " left64=" + (exists(sub, W64) ? "1" : "0"));
        for (u32 view : {W64, W32}) {
            u64 h = 0;
            if (thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P, 0, KEY_ALL_ACCESS_ | view, &h) ==
                ERROR_SUCCESS) {
                (void)thunks->reg_delete_tree(h, L"Del");
                (void)thunks->reg_delete_tree(h, L"Tree");
                thunks->reg_close_key(h);
            }
        }
    }
    for (u32 view : {W32, W64}) {
        thunks->reg_close_key(create("", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Tree\\Leaf", view, nullptr));
    }
    u64 t32 = 0;
    ASSERT_EQ(thunks->reg_open_key_ex_w(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Tree", 0,
                                        KEY_ALL_ACCESS_ | W32, &t32), 0u);
    const u32 err = thunks->reg_delete_tree(t32, L"");
    const std::wstring leaf = L"SOFTWARE\\" + P + L"\\Tree\\Leaf";
    check("deltree_handle32_empty ret=" + std::to_string(err) + " leaf_left32=" + (exists(leaf, W32) ? "1" : "0") +
          " leaf_left64=" + (exists(leaf, W64) ? "1" : "0"));
    thunks->reg_close_key(t32);
    thunks->reg_close_key(create("", HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + P + L"\\Del", W32, nullptr));
    check("delex_explicit_wow_64key ret=" +
          std::to_string(thunks->reg_delete_key_ex(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\" + P + L"\\Del",
                                                   W64, 0)));
}

TEST_P(RegistryWow64Test, CurrentUserAndClasses) {
    thunks->reg_close_key(create("create_hkcu_default", HKEY_CURRENT_USER, L"Software\\" + P, 0, L"def"));
    open("open_hkcu_32key", HKEY_CURRENT_USER, L"Software\\" + P, KEY_READ_ | W32);
    open("open_hkcu_64key", HKEY_CURRENT_USER, L"Software\\" + P, KEY_READ_ | W64);
    open("open_hkcu_explicit_wow", HKEY_CURRENT_USER, L"Software\\Wow6432Node", KEY_READ_);
    open("open_hkcu_classes_wow_64key", HKEY_CURRENT_USER, L"Software\\Classes\\Wow6432Node", KEY_READ_ | W64);

    thunks->reg_close_key(create("create_hkcr_clsid_default", HKEY_CLASSES_ROOT, L"CLSID\\" + G1, 0, L"def"));
    open("g1_hkcr_32key", HKEY_CLASSES_ROOT, L"CLSID\\" + G1, KEY_READ_ | W32);
    open("g1_hkcr_64key", HKEY_CLASSES_ROOT, L"CLSID\\" + G1, KEY_READ_ | W64);
    open("g1_hkcr_explicit_wow_64key", HKEY_CLASSES_ROOT, L"Wow6432Node\\CLSID\\" + G1, KEY_READ_ | W64);
    open("g1_hklm_classes_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + G1, KEY_READ_ | W32);
    open("g1_hkcu_classes_32key", HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" + G1, KEY_READ_ | W32);
    thunks->reg_close_key(create("", HKEY_CLASSES_ROOT, L"CLSID\\" + G2 + L"\\InprocServer32", W32, L"32"));
    thunks->reg_close_key(create("", HKEY_CLASSES_ROOT, L"CLSID\\" + G2 + L"\\InprocServer32", W64, L"64"));
    open("g2_hkcr_default", HKEY_CLASSES_ROOT, L"CLSID\\" + G2 + L"\\InprocServer32", KEY_READ_);
    // Where the HKCR creates landed, by their physical machine keys.
    open("create_hkcr_clsid_32key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Wow6432Node\\CLSID\\" + G2 +
         L"\\InprocServer32", KEY_READ_ | W64);
    open("create_hkcr_clsid_64key", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + G2 + L"\\InprocServer32",
         KEY_READ_ | W64);
    thunks->reg_close_key(
        create("create_hkcu_classes_clsid_default", HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" + G3, 0, L"def"));
    open("g3_hkcr_default", HKEY_CLASSES_ROOT, L"CLSID\\" + G3, KEY_READ_);
    open("g3_hkcr_32key", HKEY_CLASSES_ROOT, L"CLSID\\" + G3, KEY_READ_ | W32);
    open("g3_hkcr_64key", HKEY_CLASSES_ROOT, L"CLSID\\" + G3, KEY_READ_ | W64);
    u64 cl32 = 0, cl64 = 0;
    ASSERT_EQ(thunks->reg_open_key_ex_w(HKEY_CLASSES_ROOT, L"CLSID", 0, KEY_READ_ | W32, &cl32), 0u);
    ASSERT_EQ(thunks->reg_open_key_ex_w(HKEY_CLASSES_ROOT, L"CLSID", 0, KEY_READ_ | W64, &cl64), 0u);
    open("from_cl32_g2_default", cl32, G2 + L"\\InprocServer32", KEY_READ_);
    open("from_cl64_g2_default", cl64, G2 + L"\\InprocServer32", KEY_READ_);
    open("from_cl64_g2_32key", cl64, G2 + L"\\InprocServer32", KEY_READ_ | W32);
    thunks->reg_close_key(cl32);
    thunks->reg_close_key(cl64);

    for (const wchar_t* sub : {L"CLSID", L"Interface", L"TypeLib", L"AppID", L"DirectShow", L"Media Type",
                               L"MediaFoundation", L".txt", L"txtfile", L"Wow6432Node", L"Installer", L"Record",
                               L"Component Categories", L"FileType", L"Protocols", L"MIME", L"Drive", L"Folder"}) {
        const std::string s = narrow(sub);
        open("class_" + s + "_hkcr_32key", HKEY_CLASSES_ROOT, sub, KEY_READ_ | W32);
        open("class_" + s + "_hklm_32key", HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\Classes\\") + sub,
             KEY_READ_ | W32);
        open("class_" + s + "_hklm_64key", HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\Classes\\") + sub,
             KEY_READ_ | W64);
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, RegistryWow64Test, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x86" : "x64"; });

// RegDisableReflectionKey / RegEnableReflectionKey / RegQueryReflectionKey:
// reflection is gone since Windows 7; every handle, valid or not, succeeds
// and the query reports TRUE (refl_* lines, identical on x86 and x64).
TEST(RegistryReflectionTest, AllSucceedAndReportDisabled) {
    for (const bool x64 : {false, true}) {
        Registry registry;
        HandleTable handles;
        RegistryThunks thunks(registry, handles);
        const DllExports exports = create_advapi32_registry_thunks(thunks);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        constexpr GuestAddr kStack = 0x18000, kOut = 0x10000;
        const auto call = [&](const char* name, std::vector<u64> args) {
            cpu::CpuState cpu;
            if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.set_sp(kStack);
            (void)memory.write_u64(kStack, 0xDEADBEEF);
            if (x64) {
                if (!args.empty()) cpu.gpr.rcx = args[0];
                if (args.size() > 1) cpu.gpr.rdx = args[1];
            } else {
                for (usize i = 0; i < args.size(); ++i)
                    (void)memory.write_u32(kStack + 4 + static_cast<GuestAddr>(i) * 4, static_cast<u32>(args[i]));
            }
            Marshaler m(memory);
            m.attach_cpu(cpu);
            const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                         [&](const ExportEntry& e) { return e.name == name; });
            EXPECT_NE(it, exports.entries.end()) << name;
            it->handler(m);
            return static_cast<u32>(cpu.gpr.rax);
        };
        for (const u64 key : {u64{HKEY_LOCAL_MACHINE}, u64{0x1234}, u64{0}}) {
            (void)memory.write_u32(kOut, 77);
            EXPECT_EQ(call("RegQueryReflectionKey", {key, kOut}), ERROR_SUCCESS);
            EXPECT_EQ(memory.read_u32(kOut).value_or(0), 1u);
            EXPECT_EQ(call("RegDisableReflectionKey", {key}), ERROR_SUCCESS);
            EXPECT_EQ(call("RegEnableReflectionKey", {key}), ERROR_SUCCESS);
        }
    }
}

}  // namespace
