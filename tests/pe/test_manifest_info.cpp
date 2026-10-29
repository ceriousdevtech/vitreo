// SPDX-License-Identifier: LGPL-2.1-only

#include "core/pe/parser/manifest_info.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace vitreo::pe {
namespace {

std::span<const u8> bytes(const std::string& value) {
    return {
        reinterpret_cast<const u8*>(value.data()),
        value.size(),
    };
}

void write_u16(std::vector<u8>& data, usize offset, u16 value) {
    data[offset] = static_cast<u8>(value);
    data[offset + 1] = static_cast<u8>(value >> 8);
}

void write_u32(std::vector<u8>& data, usize offset, u32 value) {
    for (usize index = 0; index < 4; ++index) {
        data[offset + index] =
            static_cast<u8>(value >> (index * 8));
    }
}

TEST(ManifestInfoTest, ParsesRequireAdministrator) {
    const std::string manifest =
        R"(<?xml version="1.0"?><assembly><trustInfo><security>)"
        R"(<requestedPrivileges><requestedExecutionLevel )"
        R"(level="requireAdministrator" uiAccess="false"/>)"
        R"(</requestedPrivileges></security></trustInfo></assembly>)";

    EXPECT_EQ(
        parse_manifest_execution_level(bytes(manifest)),
        RequestedExecutionLevel::RequireAdministrator);
}

TEST(ManifestInfoTest, ParsesHighestAvailableCaseInsensitively) {
    const std::string manifest =
        R"(<asmv3:requestedExecutionLevel LEVEL='highestAvailable'/>)";

    EXPECT_EQ(
        parse_manifest_execution_level(bytes(manifest)),
        RequestedExecutionLevel::HighestAvailable);
}

TEST(ManifestInfoTest, RequestsElevationForHighestAvailableAndRequireAdministrator) {
    // Vitreo's user is a protected administrator (manifest_info.hpp). Learn
    // /MANIFESTUAC and "How User Account Control works": for an administrator
    // in Admin Approval Mode both highestAvailable and requireAdministrator
    // prompt for consent and run with the full token; asInvoker and an
    // absent request run with the filtered token.
    EXPECT_FALSE(requests_elevation(RequestedExecutionLevel::Unspecified));
    EXPECT_FALSE(requests_elevation(RequestedExecutionLevel::AsInvoker));
    EXPECT_TRUE(requests_elevation(RequestedExecutionLevel::HighestAvailable));
    EXPECT_TRUE(
        requests_elevation(RequestedExecutionLevel::RequireAdministrator));
}

TEST(ManifestInfoTest, IgnoresCommentedRequest) {
    const std::string manifest =
        R"(<!-- <requestedExecutionLevel level="requireAdministrator"/> -->)"
        R"(<requestedExecutionLevel level="asInvoker"/>)";

    EXPECT_EQ(
        parse_manifest_execution_level(bytes(manifest)),
        RequestedExecutionLevel::AsInvoker);
}

TEST(ManifestInfoTest, ParsesUtf16LittleEndianManifest) {
    const std::string ascii =
        R"(<requestedExecutionLevel level="requireAdministrator"/>)";
    std::vector<u8> utf16{0xFF, 0xFE};
    for (char ch : ascii) {
        utf16.push_back(static_cast<u8>(ch));
        utf16.push_back(0);
    }

    EXPECT_EQ(
        parse_manifest_execution_level(utf16),
        RequestedExecutionLevel::RequireAdministrator);
}

TEST(ManifestInfoTest, DetectsWindows10SupportedOsGuid) {
    const std::string manifest =
        R"(<assembly><compatibility><application>)"
        R"(<supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>)"
        R"(</application></compatibility></assembly>)";

    EXPECT_TRUE(parse_manifest_supports_windows_10(bytes(manifest)));
}

TEST(ManifestInfoTest, IgnoresCommentedWindows10SupportedOsGuid) {
    const std::string manifest =
        R"(<assembly><!-- <supportedOS Id=")"
        R"({8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/> -->)"
        R"(</assembly>)";

    EXPECT_FALSE(parse_manifest_supports_windows_10(bytes(manifest)));
}

TEST(ManifestInfoTest, ParsesPrivateAssemblyDependencies) {
    const std::string manifest =
        R"(<assembly><dependency><dependentAssembly>)"
        R"(<assemblyIdentity type="win32" name="Vendor.Runtime.4.2" )"
        R"(version="4.2.0.0"/>)"
        R"(</dependentAssembly></dependency>)"
        R"(<!-- <dependentAssembly><assemblyIdentity name="ignored"/> )"
        R"(</dependentAssembly> -->)"
        R"(<dependency><dependentAssembly>)"
        R"(<assemblyIdentity name='Second_Runtime'/>)"
        R"(</dependentAssembly></dependency></assembly>)";

    const auto assemblies = parse_manifest_private_assemblies(bytes(manifest));
    ASSERT_EQ(assemblies.size(), 2u);
    EXPECT_EQ(assemblies[0], "vendor.runtime.4.2");
    EXPECT_EQ(assemblies[1], "second_runtime");
}

TEST(ManifestInfoTest, DetectsCommonControlsVersion6Dependency) {
    // Learn "Enabling Visual Styles": the dependency that selects comctl32 v6.
    const std::string v6 =
        R"(<assembly><dependency><dependentAssembly>)"
        R"(<assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" )"
        R"(version="6.0.0.0" processorArchitecture="*" )"
        R"(publicKeyToken="6595b64144ccf1df" language="*"/>)"
        R"(</dependentAssembly></dependency></assembly>)";
    EXPECT_TRUE(parse_manifest_common_controls_v6(bytes(v6)));

    const std::string v5 =
        R"(<assembly><dependency><dependentAssembly>)"
        R"(<assemblyIdentity name="Microsoft.Windows.Common-Controls" version="5.82.0.0"/>)"
        R"(</dependentAssembly></dependency></assembly>)";
    EXPECT_FALSE(parse_manifest_common_controls_v6(bytes(v5)));

    const std::string commented =
        R"(<assembly><!-- <dependency><dependentAssembly>)"
        R"(<assemblyIdentity name="Microsoft.Windows.Common-Controls" version="6.0.0.0"/>)"
        R"(</dependentAssembly></dependency> --></assembly>)";
    EXPECT_FALSE(parse_manifest_common_controls_v6(bytes(commented)));
    EXPECT_FALSE(parse_manifest_common_controls_v6({}));
}

TEST(ManifestInfoTest, RejectsPrivateAssemblyPathTraversal) {
    const std::string manifest =
        R"(<assembly><dependency><dependentAssembly>)"
        R"(<assemblyIdentity name="../../outside"/>)"
        R"(</dependentAssembly></dependency></assembly>)";

    EXPECT_TRUE(parse_manifest_private_assemblies(bytes(manifest)).empty());
}

TEST(ManifestInfoTest, LocatesManifestInPeResourceTree) {
    const std::string manifest =
        R"(<requestedExecutionLevel level="requireAdministrator"/>)";
    std::vector<u8> data(0x100 + manifest.size(), 0);

    // Root directory -> RT_MANIFEST(24) type directory.
    write_u16(data, 14, 1);
    write_u32(data, 16, 24);
    write_u32(data, 20, 0x80000020u);
    // Type directory -> resource ID 1 directory.
    write_u16(data, 0x20 + 14, 1);
    write_u32(data, 0x20 + 16, 1);
    write_u32(data, 0x20 + 20, 0x80000040u);
    // Name directory -> language 0 data entry.
    write_u16(data, 0x40 + 14, 1);
    write_u32(data, 0x40 + 16, 0);
    write_u32(data, 0x40 + 20, 0x60u);
    // IMAGE_RESOURCE_DATA_ENTRY points to RVA 0x1100.
    write_u32(data, 0x60, 0x1100u);
    write_u32(data, 0x64, static_cast<u32>(manifest.size()));
    std::copy(manifest.begin(), manifest.end(), data.begin() + 0x100);

    ParsedPe pe{};
    pe.num_data_directories = DIR_COUNT;
    pe.data_directories[DIR_RESOURCE] = {
        0x1000u,
        static_cast<u32>(data.size()),
    };
    ParsedSection resources{};
    resources.virtual_address = 0x1000u;
    resources.virtual_size = static_cast<u32>(data.size());
    resources.raw_offset = 0;
    resources.raw_size = static_cast<u32>(data.size());
    pe.sections.push_back(resources);

    EXPECT_EQ(
        pe_requested_execution_level(data, pe),
        RequestedExecutionLevel::RequireAdministrator);
}

TEST(ManifestInfoTest, DetectsLegacy32BitGuiInstallerByBasename) {
    ParsedPe pe{};
    pe.is_64bit = false;
    pe.subsystem = Subsystem::WindowsGui;

    EXPECT_TRUE(is_legacy_installer_candidate(
        RequestedExecutionLevel::Unspecified, pe,
        R"(C:\Games\Blizzard Installer\Installer.exe)"));
    EXPECT_TRUE(is_legacy_installer_candidate(
        RequestedExecutionLevel::Unspecified, pe,
        "/tmp/StarCraft_1152_Update.EXE"));
    EXPECT_FALSE(is_legacy_installer_candidate(
        RequestedExecutionLevel::Unspecified, pe,
        R"(C:\Installer\StarCraft.exe)"));
}

TEST(ManifestInfoTest, ParsesActiveCodePageUtf8) {
    const std::string utf8_manifest =
        R"(<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">)"
        R"(<application xmlns="urn:schemas-microsoft-com:asm.v3">)"
        R"(<windowsSettings>)"
        R"(<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">)"
        R"(UTF-8</activeCodePage>)"
        R"(</windowsSettings></application></assembly>)";
    EXPECT_TRUE(parse_manifest_active_code_page_utf8(bytes(utf8_manifest)));

    const std::string legacy =
        R"(<activeCodePage>Legacy</activeCodePage>)";
    EXPECT_FALSE(parse_manifest_active_code_page_utf8(bytes(legacy)));

    const std::string commented =
        R"(<!-- <activeCodePage>UTF-8</activeCodePage> -->)"
        R"(<activeCodePage>Legacy</activeCodePage>)";
    EXPECT_FALSE(parse_manifest_active_code_page_utf8(bytes(commented)));
}

// Learn "Application manifests" / "Setting the default DPI awareness for a
// process": <dpiAwareness> (first recognised list entry) wins over
// <dpiAware>; the result is the DPI_AWARENESS_CONTEXT pseudo-handle value.
TEST(ManifestInfoTest, DpiAwarenessFromManifest) {
    const std::string v2 =
        R"(<assembly><application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>)"
        R"(<dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>)"
        R"(<dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">)"
        R"( PerMonitorV2, PerMonitor </dpiAwareness></windowsSettings></application></assembly>)";
    EXPECT_EQ(parse_manifest_dpi_awareness_context(bytes(v2)), -4);
    const std::string unknown_first = R"(<dpiAwareness>bogus,system</dpiAwareness>)";
    EXPECT_EQ(parse_manifest_dpi_awareness_context(bytes(unknown_first)), -2);
    const std::string aware_true = R"(<asmv3:dpiAware>True</asmv3:dpiAware>)";
    EXPECT_EQ(parse_manifest_dpi_awareness_context(bytes(aware_true)), -2);
    const std::string aware_pm = R"(<dpiAware>true/pm</dpiAware>)";
    EXPECT_EQ(parse_manifest_dpi_awareness_context(bytes(aware_pm)), -3);
    const std::string aware_false = R"(<dpiAware>false</dpiAware>)";
    EXPECT_EQ(parse_manifest_dpi_awareness_context(bytes(aware_false)), -1);
    const std::string none = R"(<!-- <dpiAware>true</dpiAware> --><assembly/>)";
    EXPECT_EQ(parse_manifest_dpi_awareness_context(bytes(none)), 0);
}

TEST(ManifestInfoTest, LegacyInstallerDetectionHonorsManifestAndImageType) {
    ParsedPe pe{};
    pe.is_64bit = false;
    pe.subsystem = Subsystem::WindowsGui;

    EXPECT_FALSE(is_legacy_installer_candidate(
        RequestedExecutionLevel::AsInvoker, pe, "Installer.exe"));

    pe.is_64bit = true;
    EXPECT_FALSE(is_legacy_installer_candidate(
        RequestedExecutionLevel::Unspecified, pe, "Installer.exe"));

    pe.is_64bit = false;
    pe.subsystem = Subsystem::WindowsCui;
    EXPECT_FALSE(is_legacy_installer_candidate(
        RequestedExecutionLevel::Unspecified, pe, "Installer.exe"));
}

} // namespace
} // namespace vitreo::pe
