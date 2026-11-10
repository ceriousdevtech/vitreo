// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_actctx.cpp
/// @brief kernel32 activation contexts against the native observations in
/// tests/oracle_data/actctx_probe.x86.expected (Windows 11 26100): the
/// probe's own private-assembly scenario replayed through the exports.

#include "kernel32_cpuz_fixture.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/memory/memory.hpp>

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr u32 kInvalid = 0xFFFFFFFFu;

class ActCtxTest : public Kernel32CpuzTest {
protected:
    void SetUp() override {
        Kernel32CpuzTest::SetUp();
        root = fs::temp_directory_path() / ("vitreo_actctx_" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "probe" / "Probe.Asm");
        write(root / "probe" / "app.manifest",
              "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
              "<assemblyIdentity type=\"win32\" name=\"Probe.App\" version=\"1.0.0.0\" processorArchitecture=\"x86\"/>"
              "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Probe.Asm\" version=\"1.2.3.4\" "
              "processorArchitecture=\"x86\"/></dependentAssembly></dependency></assembly>");
        write(root / "probe" / "Probe.Asm" / "Probe.Asm.manifest",
              "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
              "<assemblyIdentity type=\"win32\" name=\"Probe.Asm\" version=\"1.2.3.4\" processorArchitecture=\"x86\"/>"
              "<file name=\"probea.dll\"/><file name=\"ProbeB.dll\"/></assembly>");
        write(root / "probe" / "missing.manifest",
              "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
              "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"No.Such\" version=\"1.0.0.0\" "
              "processorArchitecture=\"x86\"/></dependentAssembly></dependency></assembly>");
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_virtual_memory_manager(&vmm);
    }
    void TearDown() override { fs::remove_all(root); }

    static void write(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

    void args(std::initializer_list<u32> values) {
        u32 offset = 4;
        for (const u32 v : values) {
            ASSERT_TRUE(memory.write_u32(static_cast<GuestAddr>(cpu.gpr.rsp) + offset, v).is_ok());
            offset += 4;
        }
    }
    void put_string(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(at + static_cast<GuestAddr>(i), static_cast<u8>(s[i]));
        (void)memory.write_u8(at + static_cast<GuestAddr>(s.size()), 0);
    }
    u32 last_error() { return get_last_error(); }

    // ACTCTXA (x86): cbSize, dwFlags, lpSource, wProcessorArchitecture,
    // wLangId, lpAssemblyDirectory, lpResourceName, lpApplicationName, hModule.
    u32 create(const std::string& source, u32 flags, u32 cb = 32, const std::string& dir = {}) {
        (void)memory.write_u32(kInfo, cb);
        (void)memory.write_u32(kInfo + 4, flags);
        put_string(kSource, source);
        (void)memory.write_u32(kInfo + 8, kSource);
        (void)memory.write_u32(kInfo + 12, 0);
        put_string(kDir, dir);
        (void)memory.write_u32(kInfo + 16, dir.empty() ? 0 : kDir);
        for (u32 o = 20; o < 32; o += 4) (void)memory.write_u32(kInfo + o, 0);
        args({kInfo});
        mgr.impl_CreateActCtxA(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }
    // ACTCTX_SECTION_KEYED_DATA is 64 bytes on x86.
    bool find(const std::string& name, u32 flags = 0, u32 section = 2, u32 cb = 64) {
        for (u32 o = 0; o < 64; o += 4) (void)memory.write_u32(kKeyed + o, 0xCCCCCCCCu);
        (void)memory.write_u32(kKeyed, cb);
        put_string(kName, name);
        args({flags, 0, section, kName, kKeyed});
        mgr.impl_FindActCtxSectionStringA(marshaler);
        return cpu.gpr.rax != 0;
    }
    u32 keyed(u32 offset) { return memory.read_u32(kKeyed + offset).value_or(0); }

    static constexpr GuestAddr kInfo = DATA_BASE + 0x100;
    static constexpr GuestAddr kSource = DATA_BASE + 0x200;
    static constexpr GuestAddr kDir = DATA_BASE + 0x300;
    static constexpr GuestAddr kKeyed = DATA_BASE + 0x400;
    static constexpr GuestAddr kName = DATA_BASE + 0x500;
    static constexpr GuestAddr kOut = DATA_BASE + 0x600;

    VirtualMemoryManager vmm{memory};
    fs::path root;
};

TEST_F(ActCtxTest, CreationErrorsMatchWindows) {
    set_last_error(0xDEADBEEFu);
    const u32 h = create("C:\\probe\\app.manifest", 0x4, 32, "C:\\probe");
    ASSERT_NE(h, kInvalid);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(last_error(), 0u);  // last_error_on_success_probe: cleared on success
    args({h});
    mgr.impl_ReleaseActCtx(marshaler);

    EXPECT_EQ(create("C:\\probe\\nope.manifest", 0), kInvalid);
    EXPECT_EQ(last_error(), 2u);
    EXPECT_EQ(create("C:\\probe\\missing.manifest", 0), kInvalid);
    EXPECT_EQ(last_error(), 14001u);
    EXPECT_EQ(create("C:\\probe\\app.manifest", 0, 0), kInvalid);
    EXPECT_EQ(last_error(), 87u);
    EXPECT_EQ(create("C:\\probe\\app.manifest", 0x100), kInvalid);
    EXPECT_EQ(last_error(), 87u);
}

TEST_F(ActCtxTest, ActiveContextRedirectsItsDllsWithTheNativeKeyedData) {
    // Nothing activated: GetCurrentActCtx succeeds with NULL, error untouched.
    set_last_error(0xDEADBEEFu);
    args({kOut});
    mgr.impl_GetCurrentActCtx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(kOut).value(), 0u);
    EXPECT_EQ(last_error(), 0xDEADBEEFu);
    EXPECT_FALSE(find("probea.dll"));
    EXPECT_EQ(last_error(), 14007u);

    const u32 h = create("C:\\probe\\app.manifest", 0x4, 32, "C:\\probe");
    ASSERT_NE(h, kInvalid);
    args({h, kOut});
    mgr.impl_ActivateActCtx(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 cookie = memory.read_u32(kOut).value();
    args({kOut});
    mgr.impl_GetCurrentActCtx(marshaler);
    EXPECT_EQ(memory.read_u32(kOut).value(), h);
    args({h});
    mgr.impl_ReleaseActCtx(marshaler);  // GetCurrentActCtx's reference

    for (const char* name : {"probea.dll", "PROBEA.DLL"}) {
        SCOPED_TRACE(name);
        ASSERT_TRUE(find(name));
        EXPECT_EQ(keyed(4), 1u);                 // ulDataFormatVersion
        EXPECT_EQ(keyed(12), 20u);               // ulLength
        EXPECT_EQ(keyed(16), 0u);                // lpSectionGlobalData
        EXPECT_EQ(keyed(20), 0u);                // ulSectionGlobalDataLength
        EXPECT_EQ(keyed(8) - keyed(24), 116u);   // lpData - lpSectionBase
        EXPECT_EQ(keyed(28), 180u);              // ulSectionTotalLength
        EXPECT_EQ(keyed(32), 0u);                // hActCtx without RETURN_HACTCTX
        EXPECT_EQ(keyed(36), 2u);                // ulAssemblyRosterIndex
    }
    ASSERT_TRUE(find("ProbeB.dll"));
    EXPECT_EQ(keyed(8) - keyed(24), 160u);
    // The section bytes themselves are compared with the native dump in test_sxs.cpp.
    EXPECT_EQ(memory.read_u32(keyed(24)).value(), 0x64487353u);  // 'SsHd'

    ASSERT_TRUE(find("probea.dll", 0x1 | 0x2));
    EXPECT_EQ(keyed(32), h);
    EXPECT_EQ(keyed(40), 0u);                    // ulFlags
    args({h});
    mgr.impl_ReleaseActCtx(marshaler);           // the reference RETURN_HACTCTX added

    EXPECT_FALSE(find("missing.dll"));
    EXPECT_EQ(last_error(), 14007u);
    EXPECT_FALSE(find("probea"));                // no extension is added
    EXPECT_EQ(last_error(), 14007u);
    EXPECT_FALSE(find("x", 0, 3));
    EXPECT_EQ(last_error(), 14007u);             // window class section
    EXPECT_FALSE(find("x", 0, 4));
    EXPECT_EQ(last_error(), 14002u);             // COM server section
    EXPECT_FALSE(find("x", 0, 99));
    EXPECT_EQ(last_error(), 14000u);
    EXPECT_FALSE(find("probea.dll", 0x8));
    EXPECT_EQ(last_error(), 87u);
    EXPECT_FALSE(find("probea.dll", 0, 2, 32));  // cbSize = offset of hActCtx
    EXPECT_EQ(last_error(), 87u);
    EXPECT_TRUE(find("probea.dll", 0, 2, 40));   // cbSize = offset of ulFlags
    EXPECT_FALSE(find("probea.dll", 0x1 | 0x2, 2, 40));
    EXPECT_EQ(last_error(), 87u);
    EXPECT_TRUE(find("probea.dll", 0x1 | 0x2, 2, 44));
    args({h});
    mgr.impl_ReleaseActCtx(marshaler);
    args({0, 0, 2, 0, kKeyed});
    mgr.impl_FindActCtxSectionStringA(marshaler);  // NULL name
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(last_error(), 87u);

    // QueryActCtxW basic information: 8 bytes on x86, {hActCtx, dwFlags}.
    args({0, h, 0, 1, kOut, 0, kOut + 0x40});
    mgr.impl_QueryActCtxW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(last_error(), 122u);
    EXPECT_EQ(memory.read_u32(kOut + 0x40).value(), 8u);
    args({0, h, 0, 1, kOut, 8, kOut + 0x40});
    mgr.impl_QueryActCtxW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(kOut).value(), h);
    EXPECT_EQ(memory.read_u32(kOut + 4).value(), 0u);
    args({h});
    mgr.impl_ReleaseActCtx(marshaler);

    // The redirection the loader asks for follows the active context.
    EXPECT_EQ(mgr.sxs_redirect_dll("PROBEB.DLL"), std::optional<std::string>("C:\\probe\\Probe.Asm\\ProbeB.dll"));

    args({0, cookie});
    mgr.impl_DeactivateActCtx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_FALSE(find("probea.dll"));
    EXPECT_EQ(last_error(), 14007u);
    EXPECT_FALSE(mgr.sxs_redirect_dll("probea.dll"));
    args({h});
    mgr.impl_ReleaseActCtx(marshaler);
}

TEST_F(ActCtxTest, ProcessDefaultIsUsedWithoutActivationAndSetOnce) {
    set_last_error(0xDEADBEEFu);
    EXPECT_EQ(create("C:\\probe\\app.manifest", 0x10 | 0x4, 32, "C:\\probe"), 0u);  // NULL
    EXPECT_EQ(last_error(), 0xDEADBEEFu);
    ASSERT_TRUE(find("probea.dll", 0x1));
    EXPECT_EQ(keyed(32), 0u);    // the process default has no handle
    EXPECT_EQ(keyed(36), 2u);
    EXPECT_FALSE(find("missing.dll"));
    EXPECT_EQ(last_error(), 14007u);
    args({kOut});
    mgr.impl_GetCurrentActCtx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(kOut).value(), 0u);
    EXPECT_EQ(create("C:\\probe\\app.manifest", 0x10 | 0x4, 32, "C:\\probe"), kInvalid);
    EXPECT_EQ(last_error(), 14011u);
    EXPECT_EQ(mgr.sxs_redirect_dll("probea.dll"), std::optional<std::string>("C:\\probe\\Probe.Asm\\probea.dll"));
}

}  // namespace

namespace {

// tests/oracle_data/short_path_probe.x86.expected (Windows 11). The capsule
// volume has no 8.3 names, so the short form is the given path; the existence
// rules, errors and sizing are Windows'.
TEST_F(ActCtxTest, GetShortPathNameFollowsTheMeasuredContract) {
    fs::create_directories(root / "short" / "Long Directory Name");
    write(root / "short" / "Long Directory Name" / "long file name.txt", "x");
    mgr.set_current_directory("C:\\short");
    const auto call = [&](const char* path, u32 cch, bool null_out) -> std::pair<u32, std::string> {
        put_string(kName, path);
        for (u32 i = 0; i < 0x100; ++i) (void)memory.write_u8(kOut + i, '#');
        set_last_error(0xDEADBEEFu);
        args({kName, null_out ? 0u : static_cast<u32>(kOut), cch});
        mgr.impl_GetShortPathNameA(marshaler);
        const u32 r = static_cast<u32>(cpu.gpr.rax);
        std::string out;
        if (!null_out && r > 0 && r < cch) for (u32 i = 0; i < r; ++i) out.push_back(static_cast<char>(memory.read_u8(kOut + i).value()));
        return {r, out};
    };
    EXPECT_EQ(call("C:\\short\\Long Directory Name\\long file name.txt", 300, false),
              (std::pair<u32, std::string>{47, "C:\\short\\Long Directory Name\\long file name.txt"}));
    EXPECT_EQ(last_error(), 0xDEADBEEFu);
    EXPECT_EQ(call("C:\\short\\Long Directory Name\\", 300, false).second, "C:\\short\\Long Directory Name\\");
    EXPECT_EQ(call("Long Directory Name\\long file name.txt", 300, false).second,
              "Long Directory Name\\long file name.txt");  // relative stays relative
    EXPECT_EQ(call("C:\\short\\Long Directory Name\\nope.txt", 300, false).first, 0u);
    EXPECT_EQ(last_error(), 2u);
    EXPECT_EQ(call("C:\\short\\NoSuchDir\\file.txt", 300, false).first, 0u);
    EXPECT_EQ(last_error(), 3u);
    EXPECT_EQ(call("Long Directory Name\\nope.txt", 300, false).first, 0u);
    EXPECT_EQ(last_error(), 2u);
    EXPECT_EQ(call("", 300, false).first, 0u);
    EXPECT_EQ(last_error(), 3u);
    EXPECT_EQ(call("C:\\short\\Long Directory Name", 0, true).first, 29u);   // length + NUL
    EXPECT_EQ(call("C:\\short\\Long Directory Name", 5, false).first, 29u);
    EXPECT_EQ(call("C:\\short\\Long Directory Name", 28, false).first, 29u); // no room for NUL
    EXPECT_EQ(call("C:\\short\\Long Directory Name", 29, false).first, 28u);
    set_last_error(0xDEADBEEFu);
    args({0, 0, 0});
    mgr.impl_GetShortPathNameA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(last_error(), 87u);
}

}  // namespace
