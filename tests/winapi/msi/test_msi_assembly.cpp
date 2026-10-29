// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_assembly.cpp
/// @brief Contract tests for Windows Installer assemblies (MsiAssembly /
/// MsiAssemblyName, the global assembly cache, MsiPublishAssemblies /
/// MsiUnpublishAssemblies, MsiProvideAssembly), both guest bitnesses.
///
/// Expected values come from Windows 11 (tools/oracle_probe/msi_assembly_*):
/// the packages tests/winapi/msi/fixtures/assembly_{x86,x64,p2} (built by
/// msi_assembly_build.ps1) are installed with the engine into a temporary C:
/// drive and compared with what Windows did for the same packages: the files
/// in the assembly caches and the application directory, the Assemblies
/// registry and component key paths (…_state.expected), and every
/// MsiProvideAssembly / MsiGetComponentPath / MsiLocateComponent answer
/// (…_api32/_api64.expected, replayed line by line).

#include "msi_fixture.hpp"

#include <capsule/registry/registry.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/locale/string_conv.hpp>
#include <winapi/system/msi/msi_assembly.hpp>
#include <winapi/system/msi/msi_engine.hpp>
#include <winapi/system/msi/msi_register.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace vitreo::winapi::msi_test {
namespace {

namespace fs = std::filesystem;

const std::string kDir = VITREO_ORACLE_PROBE_DIR;
const std::string kUserSid = "S-1-5-21-1828410828-4144948699-2202159460-1001";  // the oracle's user
const std::string kP1 = "{6A1F0C52-3B7E-4D29-8E41-90C2B5D7A3E1}";
const std::string kP2 = "{C4E7A9B1-5D3F-4A62-9B08-7E1D2C3F4A5B}";

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

std::wstring wide(const std::string& s) {
    const std::vector<u16> u = utf8_to_utf16(s);
    return std::wstring(u.begin(), u.end());
}
std::string narrow(const std::wstring& w) {
    std::vector<u16> u(w.begin(), w.end());
    return utf16_to_utf8(u.data(), static_cast<int>(u.size()));
}

/// The oracle's %WINDIR% is "C:\WINDOWS"; the capsule's WindowsFolder is
/// "C:\Windows" (the case of the directory, not a contract difference).
std::string capsule_spelling(std::string s) {
    for (usize p = 0; (p = s.find("C:\\WINDOWS\\", p)) != std::string::npos; p += 11) s.replace(p, 11, "C:\\Windows\\");
    return s;
}

/// The inverse for a probe line: a path written by the capsule (even one
/// cut short by a small buffer) spelled with the oracle's "C:\WINDOWS".
std::string oracle_spelling(std::string s) {
    static const std::string kCapsule = "C:\\Windows", kOracle = "C:\\WINDOWS";
    for (usize p = 0; (p = s.find("[C:\\W", p)) != std::string::npos; ++p) {
        usize k = 0;
        while (k < kCapsule.size() && p + 1 + k < s.size() && s[p + 1 + k] == kCapsule[k]) ++k;
        if (p + 1 + k == s.size() || k == kCapsule.size() || s[p + 1 + k] == ']')
            for (usize i = 0; i < k; ++i) s[p + 1 + i] = kOracle[i];
    }
    return s;
}

/// Lines of a dump section that describe what the capsule can hold: no
/// global Win32 (WinSxS) assembly (a recorded gap), no other product's
/// assembly (stdole), no Fusion caches.
bool comparable(const std::string& l) {
    return l.find("Vitreo.Probe.W32,") == std::string::npos && l.find("stdole") == std::string::npos &&
           l.find("WinSxS") == std::string::npos && l.find("9C00-00000000000B}") == std::string::npos;
}

class MsiAssemblyTest : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        root = fs::temp_directory_path() /
               ("vitreo_msi_asm_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(root);
        fs::create_directories(root / "src");
        const fs::path fixtures = fs::path(__FILE__).parent_path() / "fixtures";
        for (const char* p : {"assembly_x86", "assembly_x64", "assembly_p2"})
            fs::copy(fixtures / p, root / "src" / p, fs::copy_options::recursive);
        fs::create_directories(root / "Windows");
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
        mgr.set_registry(&reg);
        mgr.set_user_sid(wide(kUserSid));
        engine_handlers = msi_handlers(mgr);
        msi::install_registration_state_provider();
        services.vfs = &vfs;
        services.registry = &reg;
        services.manager = &mgr;
        services.guest_64bit = true;
    }
    void TearDown() override { fs::remove_all(root); }

    u32 install(const std::string& package, std::map<std::string, std::string> props) {
        std::ifstream in(root / "src" / package / "asm.msi", std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
        auto pkg = msi::Package::open(bytes);
        if (!pkg) return 1620;
        msi::Engine::Options o;
        o.package_path = "C:\\src\\" + package + "\\asm.msi";
        o.ui_level = 2;
        o.server = true;
        o.allow_system_changes = true;
        o.command_line = std::move(props);
        u32 error = 0;
        auto engine = msi::Engine::open(mgr, std::make_shared<msi::Package>(std::move(*pkg)), o, services, error);
        if (!engine) return error;
        return engine->run_top_level("INSTALL");
    }

    [[nodiscard]] fs::path host(const std::string& guest) const {
        std::string rel = guest.substr(3);
        for (char& c : rel) if (c == '\\') c = '/';
        return root / rel;
    }

    /// The [files] section of a dump (Vitreo-owned paths, comparable ones).
    std::set<std::string> expected_files(const std::string& state) {
        std::set<std::string> out;
        bool on = false;
        for (const std::string& l : read_lines(kDir + "/" + state)) {
            if (l == "[files]") { on = true; continue; }
            if (on && l.rfind("  ", 0) == 0 && comparable(l)) out.insert(capsule_spelling(l.substr(2)));
        }
        return out;
    }
    /// The same listing of the capsule drive.
    std::set<std::string> actual_files() {
        std::set<std::string> out;
        const std::pair<const char*, const char*> roots[] = {
            {"Windows/Microsoft.NET/assembly", "C:\\Windows\\Microsoft.NET\\assembly"},
            {"Windows/assembly", "C:\\Windows\\assembly"},
            {"Program Files (x86)/VitreoAsm1", "C:\\Program Files (x86)\\VitreoAsm1"},
            {"Program Files/VitreoAsm1", "C:\\Program Files\\VitreoAsm1"},
            {"Program Files (x86)/VitreoAsm2", "C:\\Program Files (x86)\\VitreoAsm2"}};
        for (const auto& [rel, guest] : roots) {
            const fs::path base = root / rel;
            if (!fs::exists(base)) continue;
            for (const auto& e : fs::recursive_directory_iterator(base)) {
                std::string r = fs::relative(e.path(), base).string();
                for (char& c : r) if (c == '/') c = '\\';
                const std::string full = std::string(guest) + "\\" + r;
                if (full.find("Vitreo") == std::string::npos && full.find("vitreo") == std::string::npos) continue;
                if (full.find("\\assembly\\tmp\\") != std::string::npos) continue;  // the dump skips staging
                out.insert(full + " " + (e.is_directory() ? std::string("<dir>") : std::to_string(e.file_size())));
            }
        }
        return out;
    }

    /// "Kind:value" of @p name under @p key as the dump prints it.
    std::string value(u64 hive, const std::string& key, const std::string& name) {
        auto k = reg.open_key(hive, wide(key), false);
        if (k.is_err()) return "<no key>";
        auto v = reg.query_value(k.value(), wide(name));
        reg.close_key(k.value());
        if (v.is_err()) return "<no value>";
        const auto& d = v.value().data;
        if (v.value().type == capsule::RegType::Sz) return "String:" + narrow(std::get<std::wstring>(d));
        if (v.value().type == capsule::RegType::MultiSz) {
            std::string s = "MultiString:";
            for (const auto& e : std::get<std::vector<std::wstring>>(d)) s += "<" + narrow(e) + ">";
            return s;
        }
        return "<other>";
    }
    bool key_exists(u64 hive, const std::string& key) {
        auto k = reg.open_key(hive, wide(key), false);
        if (k.is_err()) return false;
        reg.close_key(k.value());
        return true;
    }

    /// Compare every Assemblies / Win32Assemblies value and assembly-component
    /// key path of @p state (64-bit view) with the capsule registry.
    int compare_registry(const std::string& state) {
        u64 hive = 0;
        std::string key;
        bool on = false;
        int compared = 0;
        for (const std::string& l : read_lines(kDir + "/" + state)) {
            if (l.rfind("[", 0) == 0) {
                const usize close = l.find("] ");
                if (close == std::string::npos) { on = false; continue; }
                const std::string tag = l.substr(1, close - 1);
                key = l.substr(close + 2);
                hive = tag.rfind("LocalMachine", 0) == 0 ? capsule::HKEY_LOCAL_MACHINE : capsule::HKEY_CURRENT_USER;
                on = tag.find("/64") != std::string::npos &&
                     (key.find("Assemblies") != std::string::npos || key.find("\\Components\\") != std::string::npos);
                continue;
            }
            if (!on || l.rfind("  ", 0) != 0 || !comparable(l)) continue;
            const usize eq = l.find(" = ");
            const std::string name = l.substr(2, eq - 2), want = l.substr(eq + 3);
            // Only assembly key paths ('<' / '>') and the private assemblies'
            // file key paths; the ordinary components belong to the
            // registration-area tests.
            if (key.find("\\Components\\") != std::string::npos && want.rfind("String:<", 0) != 0 &&
                want.find("Priv.dll") == std::string::npos && want.find("W32P.dll") == std::string::npos)
                continue;
            if (key.find("\\Components\\01E0C5D7E55AA1B4C9000000000000B0") != std::string::npos) continue;  // W32 global
            ++compared;
            EXPECT_EQ(value(hive, key, name), want) << key << " : " << name;
        }
        return compared;
    }

    // ---- guest calls ----
    GuestAddr next = DATA + 0x2000;
    GuestAddr wstr(const std::string& s) {
        const GuestAddr at = next;
        const std::vector<u16> u = utf8_to_utf16(s);
        for (usize i = 0; i < u.size(); ++i) EXPECT_TRUE(memory.write_u16(at + i * 2, u[i]).is_ok());
        EXPECT_TRUE(memory.write_u16(at + u.size() * 2, 0).is_ok());
        next += (u.size() + 1) * 2 + 16;
        return at;
    }
    GuestAddr astr(const std::string& s) {
        const GuestAddr at = next;
        for (usize i = 0; i < s.size(); ++i) EXPECT_TRUE(memory.write_u8(at + i, static_cast<u8>(s[i])).is_ok());
        EXPECT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
        next += s.size() + 17;
        return at;
    }
    std::string read_w(GuestAddr at, usize max) {
        std::string out;
        for (usize i = 0; i < max; ++i) {
            const u16 c = memory.read_u16(at + i * 2).value();
            if (c == 0 || c == '#') break;
            out += static_cast<char>(c);
        }
        return out;
    }
    std::string read_a(GuestAddr at, usize max) {
        std::string out;
        for (usize i = 0; i < max; ++i) {
            const u8 c = memory.read_u8(at + i).value();
            if (c == 0 || c == '#') break;
            out += static_cast<char>(c);
        }
        return out;
    }

    /// One request line of msi_assembly_requests*.txt, run as
    /// msi_assembly_provide_probe.c runs it; returns the probe's output line.
    std::string run_request(const std::string& full, const std::string& dir) {
        next = DATA + 0x2000;
        auto expand = [&](std::string s) {
            for (usize p = 0; (p = s.find("%D", p)) != std::string::npos;) { s.replace(p, 2, dir); p += dir.size(); }
            return s;
        };
        if (full.rfind("comp ", 0) == 0) {
            const std::string product = full.substr(5, 38), comp = full.substr(44, 38);
            std::string out;
            for (const bool locate : {false, true}) {
                next = DATA + 0x2000;
                const GuestAddr path = next;
                next += 4096;
                const GuestAddr pcch = next;
                next += 16;
                write_int(pcch, 1024);
                write_wide(path, u"");
                const i32 st = locate ? static_cast<i32>(call("MsiLocateComponentW", {I(wstr(comp)), I(path), I(pcch)}))
                                      : static_cast<i32>(call("MsiGetComponentPathW", {I(wstr(product)), I(wstr(comp)), I(path), I(pcch)}));
                out += full + " => " + (locate ? "locate" : "getpath") + " state=" + std::to_string(st) +
                       " cch=" + std::to_string(read_int(pcch)) + " [" + (st >= 0 ? read_w(path, 1024) : "") + "]\n";
            }
            out.pop_back();
            return out;
        }
        std::string line = full;
        u32 cch = 1024;
        bool nullbuf = false, nullboth = false, ansi = false;
        for (;;) {
            if (line.rfind("cch=", 0) == 0) {
                const usize sp = line.find(' ');
                cch = static_cast<u32>(std::stoul(line.substr(4, sp - 4)));
                line = line.substr(sp + 1);
            } else if (line.rfind("nullbuf ", 0) == 0) { nullbuf = true; cch = 0; line = line.substr(8); }
            else if (line.rfind("nullboth ", 0) == 0) { nullboth = true; line = line.substr(9); }
            else if (line.rfind("ansi ", 0) == 0) { ansi = true; line = line.substr(5); }
            else break;
        }
        const usize s1 = line.find(' '), s2 = line.find(' ', s1 + 1), bar = line.find('|', s2 + 1);
        const i32 mode = std::stoi(line.substr(0, s1));
        const u32 info = static_cast<u32>(std::stoul(line.substr(s1 + 1, s2 - s1 - 1)));
        const std::string ctx = expand(line.substr(s2 + 1, bar - s2 - 1)), name = expand(line.substr(bar + 1));
        const GuestAddr path = next;
        next += 4096;
        std::vector<u8> fill(4096, '#');
        if (!ansi) for (usize i = 0; i + 1 < fill.size(); i += 2) fill[i + 1] = 0;
        EXPECT_TRUE(memory.write_bytes(path, fill.data(), fill.size()).is_ok());
        const GuestAddr pcch = next;
        next += 16;
        write_int(pcch, static_cast<i32>(cch));
        const GuestAddr n = name == "NULL" ? 0 : (ansi ? astr(name) : wstr(name));
        const GuestAddr c = ctx == "-" ? 0 : (ansi ? astr(ctx) : wstr(ctx));
        const u64 r = call(ansi ? "MsiProvideAssemblyA" : "MsiProvideAssemblyW",
                           {I(n), I(c), I(static_cast<u32>(mode)), I(info), I(nullbuf || nullboth ? 0 : path),
                            I(nullboth ? 0 : pcch)});
        const std::string shown = nullbuf || nullboth ? std::string{} : (ansi ? read_a(path, 1023) : read_w(path, 1023));
        return full + " => ret=" + std::to_string(r) + " cch=" + std::to_string(nullboth ? 0 : read_int(pcch)) + " [" +
               shown + "]";
    }

    /// Replay every request of @p expected against the installed product.
    int replay(const std::string& expected, const std::string& dir) {
        int n = 0;
        for (const std::string& want : read_lines(kDir + "/" + expected)) {
            if (want.empty() || !comparable(want)) continue;
            // Lines naming the absent x64 assembly in an x86 package and the
            // like are compared too; only the WinSxS / stdole ones are skipped.
            const std::string request = want.substr(0, want.find(" => "));
            std::string got = run_request(request, dir);
            if (request.rfind("comp ", 0) == 0) {
                // Two probe lines per request: match the one of this kind.
                const bool locate = want.find("=> locate") != std::string::npos;
                const usize nl = got.find('\n');
                got = locate ? got.substr(nl + 1) : got.substr(0, nl);
            }
            ++n;
            EXPECT_EQ(oracle_spelling(got), want);
        }
        return n;
    }

    fs::path root;
    capsule::Vfs vfs;
    capsule::Registry reg;
    msi::HandlerMap engine_handlers;
    msi::Services services;
};

constexpr const char* kMachineAssemblies = "SOFTWARE\\Classes\\Installer\\Assemblies\\Global";

TEST_P(MsiAssemblyTest, MachineX86InstallPlacesAndPublishesWhatWindowsDoes) {
    ASSERT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"CLR2", "1"}}), 0u);
    EXPECT_EQ(actual_files(), expected_files("msi_assembly_machine_x86_state.expected"));
    EXPECT_GT(compare_registry("msi_assembly_machine_x86_state.expected"), 15);
    // Nothing is left in the staging area once the script committed.
    const fs::path tmp = root / "Windows" / "assembly" / "tmp";
    if (fs::exists(tmp)) EXPECT_TRUE(fs::is_empty(tmp));
    EXPECT_GT(replay(GetParam() ? "msi_assembly_machine_x86_api64.expected" : "msi_assembly_machine_x86_api32.expected",
                     "C:\\Program Files (x86)\\VitreoAsm1"),
              80);
}

TEST_P(MsiAssemblyTest, MachineX64InstallPlacesAndPublishesWhatWindowsDoes) {
    ASSERT_EQ(install("assembly_x64", {{"ALLUSERS", "1"}, {"CLR2", "1"}}), 0u);
    EXPECT_EQ(actual_files(), expected_files("msi_assembly_machine_x64_state.expected"));
    EXPECT_GT(compare_registry("msi_assembly_machine_x64_state.expected"), 15);
    EXPECT_GT(replay(GetParam() ? "msi_assembly_machine_x64_api64.expected" : "msi_assembly_machine_x64_api32.expected",
                     "C:\\Program Files\\VitreoAsm1"),
              80);
}

TEST_P(MsiAssemblyTest, PerUserInstallPublishesUnderHkcu) {
    ASSERT_EQ(install("assembly_x86", {{"CLR2", "1"}}), 0u);
    EXPECT_EQ(actual_files(), expected_files("msi_assembly_user_x86_state.expected"));
    EXPECT_GT(compare_registry("msi_assembly_user_x86_state.expected"), 15);
    EXPECT_FALSE(key_exists(capsule::HKEY_LOCAL_MACHINE, kMachineAssemblies));
}

TEST_P(MsiAssemblyTest, MissingKeyFilesFollowTheInstallModes) {
    ASSERT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"CLR2", "1"}}), 0u);
    fs::remove(host("C:\\Windows\\Microsoft.NET\\assembly\\GAC_MSIL\\Vitreo.Probe.Msil\\v4.0_1.2.3.4__809ea388321253ab\\Vitreo.Probe.Msil.dll"));
    fs::remove(host("C:\\Program Files (x86)\\VitreoAsm1\\Vitreo.Probe.Priv.dll"));
    EXPECT_EQ(replay(GetParam() ? "msi_assembly_machine_x86_missing_api64.expected"
                                : "msi_assembly_machine_x86_missing_api32.expected",
                     "C:\\Program Files (x86)\\VitreoAsm1"),
              11);
}

TEST_P(MsiAssemblyTest, UninstallRemovesTheCacheCopyAndThePublication) {
    ASSERT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"CLR2", "1"}}), 0u);
    ASSERT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"REMOVE", "ALL"}}), 0u);
    // Windows left only the other product's stdole publication
    // (msi_assembly_machine_x86_removed_state.expected; its leftover empty
    // directory is the missing-file scenario's).
    EXPECT_EQ(actual_files(), std::set<std::string>{});
    EXPECT_FALSE(key_exists(capsule::HKEY_LOCAL_MACHINE, kMachineAssemblies));
    EXPECT_FALSE(key_exists(capsule::HKEY_LOCAL_MACHINE, "SOFTWARE\\Classes\\Installer\\Win32Assemblies"));
    EXPECT_FALSE(fs::exists(root / "Windows" / "Microsoft.NET" / "assembly" / "GAC_MSIL" / "Vitreo.Probe.Msil"));
}

TEST_P(MsiAssemblyTest, TheCacheCopyStaysUntilTheLastProductIsRemoved) {
    const std::string msil = "Vitreo.Probe.Msil,Version=\"1.2.3.4\",Culture=\"neutral\",PublicKeyToken=\"809ea388321253ab\","
                             "ProcessorArchitecture=\"MSIL\"";
    const fs::path file = host("C:\\Windows\\Microsoft.NET\\assembly\\GAC_MSIL\\Vitreo.Probe.Msil\\"
                               "v4.0_1.2.3.4__809ea388321253ab\\Vitreo.Probe.Msil.dll");
    auto expected_value = [&](const std::string& state) {
        for (const std::string& l : read_lines(kDir + "/" + state))
            if (l.rfind("  " + msil + " = ", 0) == 0) return l.substr(l.find(" = ") + 3);
        return std::string("<no value>");
    };
    ASSERT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}}), 0u);
    ASSERT_EQ(install("assembly_p2", {{"ALLUSERS", "1"}}), 0u);
    EXPECT_EQ(value(capsule::HKEY_LOCAL_MACHINE, kMachineAssemblies, msil), expected_value("msi_assembly_refcount_both_state.expected"));
    ASSERT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"REMOVE", "ALL"}}), 0u);
    EXPECT_EQ(value(capsule::HKEY_LOCAL_MACHINE, kMachineAssemblies, msil),
              expected_value("msi_assembly_refcount_p2only_state.expected"));
    EXPECT_TRUE(fs::exists(file));
    EXPECT_EQ(actual_files(), expected_files("msi_assembly_refcount_p2only_state.expected"));
    ASSERT_EQ(install("assembly_p2", {{"ALLUSERS", "1"}, {"REMOVE", "ALL"}}), 0u);
    EXPECT_FALSE(fs::exists(file));
    EXPECT_EQ(actual_files(), std::set<std::string>{});
}

TEST_P(MsiAssemblyTest, AFailedInstallCommitsNoAssembly) {
    // msi_assembly_rollback_*.expected: a deferred action fails after
    // InstallFiles / MsiPublishAssemblies; nothing remains.
    EXPECT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"FAILAT", "1"}}), 1603u);
    EXPECT_EQ(actual_files(), std::set<std::string>{});
    EXPECT_FALSE(key_exists(capsule::HKEY_LOCAL_MACHINE, kMachineAssemblies));
    const fs::path tmp = root / "Windows" / "assembly" / "tmp";
    if (fs::exists(tmp)) EXPECT_TRUE(fs::is_empty(tmp));
}

TEST_P(MsiAssemblyTest, AGlobalWin32AssemblyFailsTheInstall) {
    // Recorded gap: WinSxS (registration.md "Assemblies").
    EXPECT_EQ(install("assembly_x86", {{"ALLUSERS", "1"}, {"W32", "1"}}), 1603u);
    EXPECT_EQ(actual_files(), std::set<std::string>{});
}

INSTANTIATE_MSI_BITNESS(MsiAssemblyTest);

// ---- pure contract pieces --------------------------------------------------------------

TEST(MsiAssemblyImage, ArchitectureAndRuntimeOfTheFixtureAssemblies) {
    const fs::path dir = fs::path(__FILE__).parent_path() / "fixtures" / "assembly_x64" / "PFiles" / "VitreoAsm1";
    auto image = [&](const char* name) {
        std::ifstream in(dir / name, std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
        return msi::read_clr_image(bytes);
    };
    // csc /platform:anycpu|x86|x64 and ilasm /mdv=v2.0.50727
    // (msi_assembly_assets.ps1); the GAC_* the oracle put each in.
    EXPECT_EQ(image("Vitreo.Probe.Msil.dll")->architecture, "MSIL");
    EXPECT_EQ(image("Vitreo.Probe.Msil.dll")->runtime, "v4.0.30319");
    EXPECT_EQ(image("Vitreo.Probe.X86.dll")->architecture, "x86");
    EXPECT_EQ(image("Vitreo.Probe.X64.dll")->architecture, "AMD64");
    EXPECT_EQ(image("Vitreo.Probe.Clr2.dll")->runtime, "v2.0.50727");
    EXPECT_FALSE(image("VitreoW32.dll").has_value());  // a native image
    EXPECT_FALSE(image("Vitreo.Probe.Msil.config").has_value());
}

}  // namespace
}  // namespace vitreo::winapi::msi_test
