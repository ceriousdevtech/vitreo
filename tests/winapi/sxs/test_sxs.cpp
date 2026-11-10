// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_sxs.cpp
/// @brief Side-by-side manifests, the assembly store, publisher policy and
/// activation-context generation (Learn: "Manifest Files Reference",
/// "Assembly Searching Sequence", "Publisher Configuration Files",
/// "Application Configuration Files").

#include <gtest/gtest.h>

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/common/sxs/sxs_context.hpp"
#include "winapi/common/sxs/sxs_manifest.hpp"
#include "winapi/common/sxs/sxs_section.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace vitreo::winapi::sxs {
namespace {

std::span<const u8> bytes_of(const std::string& s) {
    return {reinterpret_cast<const u8*>(s.data()), s.size()};
}

constexpr const char* kToken = "0123456789abcdef";

std::string assembly_manifest(const std::string& name, const std::string& version, const std::string& arch,
                              const std::string& token, std::initializer_list<std::string> files,
                              const std::string& extra = {}) {
    std::string m = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
                    "  <assemblyIdentity type=\"win32\" name=\"" + name + "\" version=\"" + version +
                    "\" processorArchitecture=\"" + arch + "\"" +
                    (token.empty() ? std::string{} : " publicKeyToken=\"" + token + "\"") + "/>\n";
    for (const std::string& f : files) m += "  <file name=\"" + f + "\"/>\n";
    m += extra + "</assembly>\n";
    return m;
}

std::string app_manifest(const std::string& dependency_xml) {
    return "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
           "<dependency>" + dependency_xml + "</dependency></assembly>";
}

std::string dependent(const std::string& name, const std::string& version, const std::string& token,
                      const std::string& arch = "x86") {
    return "<dependentAssembly><assemblyIdentity type=\"win32\" name=\"" + name + "\" version=\"" + version +
           "\" processorArchitecture=\"" + arch + "\"" +
           (token.empty() ? std::string{} : " publicKeyToken=\"" + token + "\"") + "/></dependentAssembly>";
}

TEST(SxsManifest, VersionIsFourPartsOfSixteenBits) {
    ASSERT_TRUE(Version::parse("8.0.50727.762"));
    EXPECT_EQ(Version::parse("8.0.50727.762")->str(), "8.0.50727.762");
    EXPECT_FALSE(Version::parse("8.0.50727"));
    EXPECT_FALSE(Version::parse("8.0.65536.0"));
    EXPECT_FALSE(Version::parse("8.0.1.2.3"));
    EXPECT_FALSE(Version::parse("8.0.x.1"));
    EXPECT_LT(*Version::parse("8.0.50727.42"), *Version::parse("8.0.50727.762"));
}

TEST(SxsManifest, ReadsIdentityFilesAndDependencies) {
    const std::string text = assembly_manifest(
        "Vendor.Lib", "1.2.3.4", "x86", "0123456789ABCDEF", {"a.dll", "B.dll"},
        "<noInheritable/><dependency optional=\"yes\">" + dependent("Other", "2.0.0.0", kToken) + "</dependency>");
    const auto m = parse_manifest(bytes_of(text));
    ASSERT_TRUE(m);
    ASSERT_TRUE(m->identity);
    EXPECT_EQ(m->identity->name, "Vendor.Lib");
    EXPECT_EQ(m->identity->public_key_token, "0123456789abcdef");  // compared case-insensitively
    EXPECT_EQ(m->files, (std::vector<std::string>{"a.dll", "B.dll"}));
    EXPECT_TRUE(m->no_inheritable);
    ASSERT_EQ(m->dependencies.size(), 1u);
    EXPECT_TRUE(m->dependencies[0].optional);
    EXPECT_EQ(m->dependencies[0].identity.name, "Other");
}

TEST(SxsManifest, MatchesElementsByNamespaceNotPrefix) {
    // A prefixed asm.v1 document is the same document; an element in another
    // namespace is not part of the side-by-side schema.
    const std::string text =
        "<asmv1:assembly xmlns:asmv1=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        "<asmv1:assemblyIdentity type=\"win32\" name=\"P\" version=\"1.0.0.0\" processorArchitecture=\"x86\"/>"
        "<other:file xmlns:other=\"urn:example\" name=\"ignored.dll\"/>"
        "<asmv1:file name=\"kept.dll\"/></asmv1:assembly>";
    const auto m = parse_manifest(bytes_of(text));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->files, (std::vector<std::string>{"kept.dll"}));
    EXPECT_FALSE(parse_manifest(bytes_of("<assembly xmlns=\"urn:example\"/>")));
}

TEST(SxsManifest, RecordsDeclarationsTheContextDoesNotCarry) {
    const std::string text =
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        "<file name=\"c.dll\"><comClass clsid=\"{00000000-0000-0000-0000-000000000001}\"/>"
        "<windowClass>Edit</windowClass></file>"
        "<comInterfaceExternalProxyStub name=\"I\" iid=\"{00000000-0000-0000-0000-000000000002}\"/></assembly>";
    const auto m = parse_manifest(bytes_of(text));
    ASSERT_TRUE(m);
    EXPECT_EQ(m->unbuilt_sections,
              (std::vector<std::string>{"comClass", "windowClass", "comInterfaceExternalProxyStub"}));
    EXPECT_TRUE(parse_manifest(bytes_of(assembly_manifest("P", "1.0.0.0", "x86", "", {"p.dll"})))->unbuilt_sections.empty());
}

TEST(SxsManifest, AcceptsUtf16WithBomAndRejectsMalformed) {
    const std::string text = assembly_manifest("U", "1.0.0.0", "x86", "", {"u.dll"});
    std::vector<u8> utf16{0xFF, 0xFE};
    for (const char c : text) { utf16.push_back(static_cast<u8>(c)); utf16.push_back(0); }
    const auto m = parse_manifest(utf16);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->files, (std::vector<std::string>{"u.dll"}));
    EXPECT_FALSE(parse_manifest(bytes_of("<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\"><file>")));
    EXPECT_FALSE(parse_manifest(bytes_of(assembly_manifest("V", "1.0", "x86", "", {}))));  // 2-part version
}

TEST(SxsManifest, ReadsApplicationConfigurationRedirects) {
    const std::string text =
        "<configuration><windows><assemblyBinding xmlns=\"urn:schemas-microsoft-com:asm.v1\">"
        "<publisherPolicy apply=\"no\"/>"
        "<dependentAssembly><assemblyIdentity type=\"win32\" name=\"Lib\" processorArchitecture=\"x86\" "
        "publicKeyToken=\"0123456789abcdef\"/>"
        "<bindingRedirect oldVersion=\"1.0.0.0-1.0.9.9\" newVersion=\"1.0.5.0\"/></dependentAssembly>"
        "</assemblyBinding></windows></configuration>";
    const auto c = parse_app_config(bytes_of(text));
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->publisher_policy_disabled);
    ASSERT_EQ(c->dependencies.size(), 1u);
    ASSERT_EQ(c->dependencies[0].redirects.size(), 1u);
    EXPECT_EQ(c->dependencies[0].redirects[0].new_version.str(), "1.0.5.0");
}

class SxsStore : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / ("vitreo_sxs_" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "app");
        std::filesystem::create_directories(root / "stage");
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    void host_write(const std::string& relative, const std::string& content) {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }
    void install(const std::string& manifest, std::initializer_list<std::string> files) {
        std::map<std::string, std::string> sources;
        for (const std::string& f : files) {
            host_write("stage/" + f, "payload:" + f);
            sources[f] = "C:\\stage\\" + f;
        }
        ASSERT_EQ(store_install(vfs, "C:\\Windows", bytes_of(manifest), sources), 0u);
    }
    ContextResult context(const std::string& manifest, std::optional<AppConfig> config = std::nullopt) {
        app_text = manifest;
        ContextRequest r;
        r.manifest = bytes_of(app_text);
        r.manifest_path = "C:\\app\\app.exe";
        r.root_directory = "C:\\app";
        r.application_dir = "C:\\app";
        r.architecture = "x86";
        r.config = std::move(config);
        return build_context(vfs, r);
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
    std::string app_text;
};

TEST_F(SxsStore, KeyKeepsTheWindowsDirectoryShape) {
    Identity id;
    id.type = "win32";
    id.name = "Microsoft.VC80.CRT";
    id.version = Version::parse("8.0.50727.762");
    id.architecture = "x86";
    id.public_key_token = "1fc8b3b9a1e18e3b";
    const std::string key = store_key(id);
    EXPECT_EQ(key.rfind("x86_microsoft.vc80.crt_1fc8b3b9a1e18e3b_8.0.50727.762_none_", 0), 0u) << key;
    EXPECT_EQ(key.size(), std::string("x86_microsoft.vc80.crt_1fc8b3b9a1e18e3b_8.0.50727.762_none_").size() + 16);
}

TEST_F(SxsStore, SharedAssemblyResolvesFromStoreAndRedirectsItsDlls) {
    install(assembly_manifest("Vendor.CRT", "8.0.50727.762", "x86", kToken, {"vcrt.dll", "vcpp.dll"}), {"vcrt.dll", "vcpp.dll"});
    const auto r = context(app_manifest(dependent("Vendor.CRT", "8.0.50727.762", kToken, "*")));
    ASSERT_TRUE(r.data) << r.detail;
    ASSERT_EQ(r.data->assemblies.size(), 2u);
    EXPECT_TRUE(r.data->assemblies[1].shared);
    const auto path = r.data->redirect_dll("VCRT.DLL");
    ASSERT_TRUE(path);
    // Measured MSI layout: WinSxS\Fusion\<arch>_<name>_<token>_none_<hash>\<maj.min>\<version>\.
    EXPECT_EQ(path->rfind("C:\\Windows\\WinSxS\\Fusion\\x86_vendor.crt_0123456789abcdef_none_", 0), 0u) << *path;
    EXPECT_NE(path->find("\\8.0\\8.0.50727.762\\vcrt.dll"), std::string::npos) << *path;
    EXPECT_TRUE(vfs.stat(*path).is_ok());
    EXPECT_FALSE(r.data->redirect_dll("other.dll"));
}

TEST_F(SxsStore, PublisherPolicyRedirectsAnOlderReference) {
    install(assembly_manifest("Vendor.CRT", "8.0.50727.762", "x86", kToken, {"vcrt.dll"}), {"vcrt.dll"});
    const std::string policy =
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        "<assemblyIdentity type=\"win32-policy\" name=\"policy.8.0.Vendor.CRT\" version=\"8.0.50727.762\" "
        "processorArchitecture=\"x86\" publicKeyToken=\"0123456789abcdef\"/>"
        "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Vendor.CRT\" "
        "processorArchitecture=\"x86\" publicKeyToken=\"0123456789abcdef\"/>"
        "<bindingRedirect oldVersion=\"8.0.41204.256-8.0.50608.0\" newVersion=\"8.0.50727.762\"/>"
        "<bindingRedirect oldVersion=\"8.0.50727.42-8.0.50727.762\" newVersion=\"8.0.50727.762\"/>"
        "</dependentAssembly></dependency></assembly>";
    install(policy, {});

    const auto r = context(app_manifest(dependent("Vendor.CRT", "8.0.50727.42", kToken)));
    ASSERT_TRUE(r.data) << r.detail;
    EXPECT_EQ(r.data->assemblies[1].identity->version->str(), "8.0.50727.762");

    // Outside every range: nothing redirects 8.0.0.1, and it is not installed.
    const auto miss = context(app_manifest(dependent("Vendor.CRT", "8.0.0.1", kToken)));
    EXPECT_FALSE(miss.data);
    EXPECT_EQ(miss.error, kErrorSxsCantGenActCtx);

    // publisherPolicy apply="no" in the application configuration file.
    AppConfig no_policy;
    no_policy.publisher_policy_disabled = true;
    const auto off = context(app_manifest(dependent("Vendor.CRT", "8.0.50727.42", kToken)), no_policy);
    EXPECT_FALSE(off.data);
    EXPECT_EQ(off.error, kErrorSxsCantGenActCtx);
}

TEST_F(SxsStore, ApplicationConfigurationRedirectComesFirst) {
    install(assembly_manifest("Vendor.Lib", "1.0.5.0", "x86", kToken, {"lib.dll"}), {"lib.dll"});
    AppConfig config;
    Dependency d;
    d.identity.name = "Vendor.Lib";
    d.redirects.push_back({*Version::parse("1.0.0.0"), *Version::parse("1.0.9.9"), *Version::parse("1.0.5.0")});
    config.dependencies.push_back(d);
    const auto r = context(app_manifest(dependent("Vendor.Lib", "1.0.0.7", kToken)), config);
    ASSERT_TRUE(r.data) << r.detail;
    EXPECT_EQ(r.data->assemblies[1].identity->version->str(), "1.0.5.0");
}

TEST_F(SxsStore, PrivateAssemblyIsProbedInTheApplicationDirectory) {
    host_write("app/Probe.Asm/Probe.Asm.manifest", assembly_manifest("Probe.Asm", "1.2.3.4", "x86", "", {"probea.dll"}));
    host_write("app/Probe.Asm/probea.dll", "x");
    const auto r = context(app_manifest(dependent("Probe.Asm", "1.2.3.4", "")));
    ASSERT_TRUE(r.data) << r.detail;
    EXPECT_FALSE(r.data->assemblies[1].shared);
    EXPECT_EQ(r.data->redirect_dll("probea.dll"), std::optional<std::string>("C:\\app\\Probe.Asm\\probea.dll"));

    // A different version is not this assembly.
    const auto wrong = context(app_manifest(dependent("Probe.Asm", "1.2.3.5", "")));
    EXPECT_FALSE(wrong.data);
}

TEST_F(SxsStore, CultureDirectoryIsProbedBeforeTheNeutralOne) {
    const std::string localized =
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        "<assemblyIdentity type=\"win32\" name=\"Loc\" version=\"1.0.0.0\" processorArchitecture=\"x86\" "
        "language=\"de-de\"/><file name=\"loc.dll\"/></assembly>";
    host_write("app/de-de/Loc.manifest", localized);
    const std::string dep = "<dependentAssembly><assemblyIdentity type=\"win32\" name=\"Loc\" version=\"1.0.0.0\" "
                            "processorArchitecture=\"x86\" language=\"de-de\"/></dependentAssembly>";
    const auto r = context(app_manifest(dep));
    ASSERT_TRUE(r.data) << r.detail;
    EXPECT_EQ(r.data->redirect_dll("loc.dll"), std::optional<std::string>("C:\\app\\de-de\\loc.dll"));
}

TEST_F(SxsStore, OptionalDependencyMayBeMissingRequiredMayNot) {
    const auto optional = context(
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><dependency optional=\"yes\">" +
        dependent("Nowhere", "1.0.0.0", kToken) + "</dependency></assembly>");
    ASSERT_TRUE(optional.data) << optional.detail;
    EXPECT_EQ(optional.data->assemblies.size(), 1u);
    const auto required = context(app_manifest(dependent("Nowhere", "1.0.0.0", kToken)));
    EXPECT_FALSE(required.data);
    EXPECT_EQ(required.error, kErrorSxsCantGenActCtx);
    EXPECT_NE(required.detail.find("Nowhere"), std::string::npos);
}

TEST_F(SxsStore, DependenciesOfAnAssemblyResolveTooAndDuplicatesFail) {
    install(assembly_manifest("Base", "1.0.0.0", "x86", kToken, {"base.dll"}), {"base.dll"});
    install(assembly_manifest("Upper", "1.0.0.0", "x86", kToken, {"upper.dll"},
                              "<dependency>" + dependent("Base", "1.0.0.0", kToken) + "</dependency>"),
            {"upper.dll"});
    const auto r = context(app_manifest(dependent("Upper", "1.0.0.0", kToken)));
    ASSERT_TRUE(r.data) << r.detail;
    EXPECT_EQ(r.data->assemblies.size(), 3u);
    EXPECT_TRUE(r.data->redirect_dll("base.dll"));

    install(assembly_manifest("Clash", "1.0.0.0", "x86", kToken, {"base.dll"}), {"base.dll"});
    const auto clash = context(app_manifest(dependent("Base", "1.0.0.0", kToken) + dependent("Clash", "1.0.0.0", kToken)));
    EXPECT_FALSE(clash.data);
    EXPECT_EQ(clash.error, kErrorSxsDuplicateDllName);
}

TEST_F(SxsStore, InstallRequiresEveryListedFileAndRemoveUndoesIt) {
    const std::string m = assembly_manifest("Gone", "1.0.0.0", "x86", kToken, {"g.dll"});
    EXPECT_EQ(store_install(vfs, "C:\\Windows", bytes_of(m), {}), 2u);  // ERROR_FILE_NOT_FOUND
    EXPECT_EQ(store_install(vfs, "C:\\Windows", bytes_of(assembly_manifest("NoToken", "1.0.0.0", "x86", "", {})), {}),
              kErrorSxsManifestFormatError);
    install(m, {"g.dll"});
    const auto id = parse_manifest(bytes_of(m))->identity;
    ASSERT_TRUE(store_find(vfs, "C:\\Windows", *id));
    EXPECT_TRUE(store_remove(vfs, "C:\\Windows", *id));
    EXPECT_FALSE(store_find(vfs, "C:\\Windows", *id));
    EXPECT_FALSE(store_remove(vfs, "C:\\Windows", *id));
}

std::filesystem::path oracle_dir() {
    return std::filesystem::absolute(__FILE__).parent_path().parent_path().parent_path() / "oracle_data";
}

// The DLL redirection section native Windows built for the probe's context
// (root app.manifest + private Probe.Asm listing probea.dll, ProbeB.dll):
// identical bytes for x86 and x64 (actctx_probe.*.expected raw_section).
TEST(SxsSection, MatchesNativeDllRedirectionSectionBytes) {
    for (const char* file : {"actctx_probe.x86.expected", "actctx_probe.x64.expected"}) {
        SCOPED_TRACE(file);
        std::ifstream in(oracle_dir() / file);
        ASSERT_TRUE(in.is_open());
        std::vector<u8> native;
        std::string line;
        const std::string prefix = "active.find.probea.dll.raw_section.";
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind(prefix, 0) != 0) continue;
            const std::string hex = line.substr(line.find('=') + 1);
            for (usize i = 0; i + 1 < hex.size(); i += 3) native.push_back(static_cast<u8>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        ASSERT_EQ(native.size(), 180u);

        ContextData context;
        context.assemblies.resize(2);
        context.assemblies[1].files = {"probea.dll", "ProbeB.dll"};
        const auto section = build_dll_redirection_section(context);
        EXPECT_EQ(section.bytes, native);
        ASSERT_EQ(section.entries.count("probea.dll"), 1u);
        EXPECT_EQ(section.entries.at("probea.dll").data_offset, 116u);  // lpData - lpSectionBase
        EXPECT_EQ(section.entries.at("probeb.dll").data_offset, 160u);
        EXPECT_EQ(section.entries.at("probeb.dll").roster_index, 2u);   // ulAssemblyRosterIndex
    }
    EXPECT_EQ(pseudo_key("probea.dll"), 0x49def007u);
    EXPECT_EQ(pseudo_key("PROBEA.DLL"), 0x49def007u);
}

TEST_F(SxsStore, OperatingSystemAssembliesBindToTheBuiltInModules) {
    // actctx_os_assemblies.*.expected (Windows 11): the OS's own token, the
    // same major.minor, a version not above the OS's binds to the OS's
    // version. Vitreo's OS version is 10.0.19041 (core/pe/os_version.hpp).
    const std::string os = "6595b64144ccf1df";
    const auto bound = [&](const std::string& name, const std::string& version, const std::string& token,
                           const std::string& arch = "*") -> std::string {
        const auto r = context(app_manifest(dependent(name, version, token, arch)));
        if (!r.data) return "error " + std::to_string(r.error);
        return r.data->assemblies[1].identity->version->str();
    };
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.0.0", os), "6.0.19041.0");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.0.0", os, "x86"), "6.0.19041.0");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.19041.0", os), "6.0.19041.0");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.19041.1", os), "error 14001");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.30000.0", os), "error 14001");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.1.0.0", os), "error 14001");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "5.82.0.0", os), "5.82.19041.0");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "7.0.0.0", os), "error 14001");
    EXPECT_EQ(bound("Microsoft.Windows.GdiPlus", "1.0.0.0", os), "1.0.19041.0");
    EXPECT_EQ(bound("Microsoft.Windows.GdiPlus", "1.1.0.0", os), "1.1.19041.0");
    EXPECT_EQ(bound("Microsoft.Windows.GdiPlus", "1.2.0.0", os), "error 14001");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.0.0", ""), "error 14001");
    EXPECT_EQ(bound("Microsoft.Windows.Common-Controls", "6.0.0.0", "0000000000000000"), "error 14001");

    // Common-Controls brings its Resources assembly; the files are Vitreo's
    // built-in modules, so the loader is not redirected to a path.
    const auto r = context(app_manifest(dependent("Microsoft.Windows.Common-Controls", "6.0.0.0", os, "*") +
                                        dependent("Microsoft.Windows.Common-Controls", "6.0.0.0", os, "x86")));
    ASSERT_TRUE(r.data) << r.detail;
    ASSERT_EQ(r.data->assemblies.size(), 3u);
    EXPECT_TRUE(r.data->assemblies[1].builtin);
    EXPECT_EQ(r.data->assemblies[2].identity->name, "Microsoft.Windows.Common-Controls.Resources");
    EXPECT_EQ(r.data->assemblies[2].identity->language, "en-us");
    EXPECT_FALSE(r.data->redirect_dll("comctl32.dll"));
    EXPECT_FALSE(vfs.stat("C:\\Windows\\WinSxS").is_ok());  // nothing written for them
}

// The section native Windows built for a Common-Controls 6.0.0.0 dependency:
// keys from two assemblies, element list sorted by pseudo-key, keys and data
// in roster order (actctx_os_assemblies.*.expected, identical x86 / x64).
TEST(SxsSection, MatchesNativeSectionWithKeysFromTwoAssemblies) {
    const std::vector<u8> native = {
        0x53,0x73,0x48,0x64,0x2c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
        0x03,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xa2,0x4e,0xf3,0xbb,
        0x8c,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0xb0,0x00,0x00,0x00,0x14,0x00,0x00,0x00,
        0x03,0x00,0x00,0x00,0x4f,0xbc,0xc4,0xf1,0x5c,0x00,0x00,0x00,0x18,0x00,0x00,0x00,
        0x78,0x00,0x00,0x00,0x14,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x63,0x00,0x6f,0x00,
        0x6d,0x00,0x63,0x00,0x74,0x00,0x6c,0x00,0x33,0x00,0x32,0x00,0x2e,0x00,0x64,0x00,
        0x6c,0x00,0x6c,0x00,0x00,0x00,0x00,0x00,0x14,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x63,0x00,0x6f,0x00,
        0x6d,0x00,0x63,0x00,0x74,0x00,0x6c,0x00,0x33,0x00,0x32,0x00,0x2e,0x00,0x64,0x00,
        0x6c,0x00,0x6c,0x00,0x2e,0x00,0x6d,0x00,0x75,0x00,0x69,0x00,0x00,0x00,0x00,0x00,
        0x14,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00};
    ContextData context;
    context.assemblies.resize(3);
    context.assemblies[1].files = {"comctl32.dll"};
    context.assemblies[2].files = {"comctl32.dll.mui"};
    const auto section = build_dll_redirection_section(context);
    EXPECT_EQ(section.bytes, native);
    EXPECT_EQ(section.entries.at("comctl32.dll").data_offset, 120u);   // measured lpData - base
    EXPECT_EQ(section.entries.at("comctl32.dll").roster_index, 2u);
}

}  // namespace
}  // namespace vitreo::winapi::sxs
