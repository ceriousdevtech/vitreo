/// @file test_advapi32_cpuz.cpp
/// @brief Unit tests for advapi32 security/service stubs exercised by CPU-Z (32-bit stdcall)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/advapi32/security/security.hpp>
#include <winapi/advapi32/registry/registry.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/registry/registry.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Helper: find an ExportEntry by name
// =============================================================================

static ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& e : exports.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// =============================================================================
// Test Fixture — 32-bit stdcall calling convention
// =============================================================================

class Advapi32CpuzTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());
        // Map SID scratch region for AllocateAndInitializeSid
        auto sid_res = memory.map_region(0x5D000000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(sid_res.is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
    }

    /// Push a 32-bit stdcall argument at the given index
    void push_arg(usize index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4), value);
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HandleTable handles;
    capsule::Registry registry;
    RegistryThunks reg_thunks{registry, handles};
    SecurityStubs sec_stubs{handles};
};

TEST_F(Advapi32CpuzTest, CreateWellKnownSidBuildsBuiltinAdministratorsSid) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "CreateWellKnownSid");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x100;
    constexpr GuestAddr size = DATA_BASE + 0x180;
    ASSERT_TRUE(memory.write_u32(size, 16).is_ok());
    push_arg(0, 26);
    push_arg(1, 0);
    push_arg(2, sid);
    push_arg(3, size);

    set_last_error(0xDEADu);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_advapi32_probe.expected)
    EXPECT_EQ(memory.read_u32(size).value(), 16u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 2u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 32u);
    EXPECT_EQ(memory.read_u32(sid + 12).value(), 544u);
}

TEST_F(Advapi32CpuzTest, CreateWellKnownSidBuildsLocalServiceSid) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "CreateWellKnownSid");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x200;
    constexpr GuestAddr size = DATA_BASE + 0x280;
    ASSERT_TRUE(memory.write_u32(size, 12).is_ok());
    push_arg(0, 23);
    push_arg(1, 0);
    push_arg(2, sid);
    push_arg(3, size);

    set_last_error(0xDEADu);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_advapi32_probe.expected)
    EXPECT_EQ(memory.read_u32(size).value(), 12u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 19u);
}

TEST_F(Advapi32CpuzTest, CreateWellKnownSidBuildsNetworkServiceSid) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "CreateWellKnownSid");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x300;
    constexpr GuestAddr size = DATA_BASE + 0x380;
    ASSERT_TRUE(memory.write_u32(size, 12).is_ok());
    push_arg(0, 24);
    push_arg(1, 0);
    push_arg(2, sid);
    push_arg(3, size);

    set_last_error(0xDEADu);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_advapi32_probe.expected)
    EXPECT_EQ(memory.read_u32(size).value(), 12u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 20u);
}

TEST_F(Advapi32CpuzTest, CreateWellKnownSidBuildsSelfSid) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "CreateWellKnownSid");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x400;
    constexpr GuestAddr size = DATA_BASE + 0x480;
    ASSERT_TRUE(memory.write_u32(size, 12).is_ok());
    push_arg(0, 16);
    push_arg(1, 0);
    push_arg(2, sid);
    push_arg(3, size);

    set_last_error(0xDEADu);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_advapi32_probe.expected)
    EXPECT_EQ(memory.read_u32(size).value(), 12u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 10u);
}

TEST_F(Advapi32CpuzTest, CreateWellKnownSidBuildsLocalSystemSid) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "CreateWellKnownSid");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x500;
    constexpr GuestAddr size = DATA_BASE + 0x580;
    ASSERT_TRUE(memory.write_u32(size, 12).is_ok());
    push_arg(0, 22);
    push_arg(1, 0);
    push_arg(2, sid);
    push_arg(3, size);

    set_last_error(0xDEADu);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_advapi32_probe.expected)
    EXPECT_EQ(memory.read_u32(size).value(), 12u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 18u);
}

TEST_F(Advapi32CpuzTest, CreateWellKnownSidReportsRequiredBufferSize) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "CreateWellKnownSid");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x200;
    constexpr GuestAddr size = DATA_BASE + 0x280;
    ASSERT_TRUE(memory.write_u32(size, 8).is_ok());
    push_arg(0, 26);
    push_arg(1, 0);
    push_arg(2, sid);
    push_arg(3, size);

    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(size).value(), 16u);
}

TEST_F(Advapi32CpuzTest, ConvertSidToStringSidAFormatsAndAllocatesResult) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertSidToStringSidA");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x100;
    constexpr GuestAddr result_out = DATA_BASE + 0x180;
    const u8 sid_bytes[] = {
        1, 2, 0, 0, 0, 0, 0, 5,
        32, 0, 0, 0,
        0x20, 0x02, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes, sizeof(sid_bytes)).is_ok());
    push_arg(0, sid);
    push_arg(1, result_out);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr result = memory.read_u32(result_out).value();
    ASSERT_NE(result, 0u);
    const std::string expected = "S-1-5-32-544";
    for (u32 i = 0; i <= expected.size(); ++i) {
        EXPECT_EQ(memory.read_u8(result + i).value(),
                  i == expected.size() ? 0 : static_cast<u8>(expected[i]));
    }
}

TEST_F(Advapi32CpuzTest, ConvertSidToStringSidWFormatsAndAllocatesResult) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertSidToStringSidW");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x200;
    constexpr GuestAddr result_out = DATA_BASE + 0x280;
    const u8 sid_bytes[] = {1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes, sizeof(sid_bytes)).is_ok());
    push_arg(0, sid);
    push_arg(1, result_out);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr result = memory.read_u32(result_out).value();
    ASSERT_NE(result, 0u);
    const std::string expected = "S-1-5-18";
    for (u32 i = 0; i <= expected.size(); ++i) {
        EXPECT_EQ(memory.read_u16(result + i * 2).value(),
                  i == expected.size() ? 0 : static_cast<u16>(expected[i]));
    }
}

TEST_F(Advapi32CpuzTest, ConvertSidToStringSidRejectsInvalidSidAndClearsOutput) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertSidToStringSidA");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sid = DATA_BASE + 0x300;
    constexpr GuestAddr result_out = DATA_BASE + 0x380;
    ASSERT_TRUE(memory.write_u8(sid, 2).is_ok()); // Unsupported SID revision.
    ASSERT_TRUE(memory.write_u8(sid + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(result_out, 0xDEADBEEF).is_ok());
    push_arg(0, sid);
    push_arg(1, result_out);

    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(result_out).value(), 0u);
    EXPECT_EQ(get_last_error(), 1337u);
}

TEST_F(Advapi32CpuzTest, ConvertStringSidToSidWParsesWellKnownAlias) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertStringSidToSidW");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr text = DATA_BASE + 0x400;
    constexpr GuestAddr result_out = DATA_BASE + 0x480;
    ASSERT_TRUE(memory.write_u16(text, 'B').is_ok());
    ASSERT_TRUE(memory.write_u16(text + 2, 'A').is_ok());
    ASSERT_TRUE(memory.write_u16(text + 4, 0).is_ok());
    push_arg(0, text);
    push_arg(1, result_out);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr sid = memory.read_u32(result_out).value();
    ASSERT_NE(sid, 0u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 2u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 32u);
    EXPECT_EQ(memory.read_u32(sid + 12).value(), 544u);
}

TEST_F(Advapi32CpuzTest, ConvertStringSidToSidWParsesWorldAlias) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertStringSidToSidW");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr text = DATA_BASE + 0x400;
    constexpr GuestAddr result_out = DATA_BASE + 0x480;
    ASSERT_TRUE(memory.write_u16(text, 'W').is_ok());
    ASSERT_TRUE(memory.write_u16(text + 2, 'D').is_ok());
    ASSERT_TRUE(memory.write_u16(text + 4, 0).is_ok());
    push_arg(0, text);
    push_arg(1, result_out);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr sid = memory.read_u32(result_out).value();
    ASSERT_NE(sid, 0u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 1u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 0u);
}

TEST_F(Advapi32CpuzTest, ConvertStringSidToSidAParsesNumericSid) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertStringSidToSidA");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr text = DATA_BASE + 0x500;
    constexpr GuestAddr result_out = DATA_BASE + 0x580;
    const std::string sid_text = "S-1-5-18";
    ASSERT_TRUE(memory.write_bytes(
        text, reinterpret_cast<const u8*>(sid_text.c_str()),
        sid_text.size() + 1).is_ok());
    push_arg(0, text);
    push_arg(1, result_out);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr sid = memory.read_u32(result_out).value();
    ASSERT_NE(sid, 0u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 18u);
}

TEST_F(Advapi32CpuzTest, ConvertStringSidToSidAParsesInteractiveAlias) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertStringSidToSidA");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr text = DATA_BASE + 0x500;
    constexpr GuestAddr result_out = DATA_BASE + 0x580;
    const std::string sid_text = "IU";
    ASSERT_TRUE(memory.write_bytes(
        text, reinterpret_cast<const u8*>(sid_text.c_str()),
        sid_text.size() + 1).is_ok());
    push_arg(0, text);
    push_arg(1, result_out);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const GuestAddr sid = memory.read_u32(result_out).value();
    ASSERT_NE(sid, 0u);
    EXPECT_EQ(memory.read_u8(sid).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 1).value(), 1u);
    EXPECT_EQ(memory.read_u8(sid + 7).value(), 5u);
    EXPECT_EQ(memory.read_u32(sid + 8).value(), 4u);
}

TEST_F(Advapi32CpuzTest, ConvertStringSidToSidRejectsMalformedInput) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ConvertStringSidToSidA");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr text = DATA_BASE + 0x600;
    constexpr GuestAddr result_out = DATA_BASE + 0x680;
    // Windows parses "S-2-5-18" (revision 2) successfully; a revision that
    // does not fit the byte is ERROR_INVALID_SID (advapi_sddl_probe
    // sid "S-256-5-18" ok=0 gle=539).
    const std::string sid_text = "S-256-5-18";
    ASSERT_TRUE(memory.write_bytes(
        text, reinterpret_cast<const u8*>(sid_text.c_str()),
        sid_text.size() + 1).is_ok());
    ASSERT_TRUE(memory.write_u32(result_out, 0xDEADBEEF).is_ok());
    push_arg(0, text);
    push_arg(1, result_out);

    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    // Windows leaves *Sid alone on failure (advapi_sddl_probe sid lines).
    EXPECT_EQ(memory.read_u32(result_out).value(), 0xDEADBEEFu);
    EXPECT_EQ(get_last_error(), 1337u);
}

TEST_F(Advapi32CpuzTest, SetNamedSecurityInfoPersistsDescriptorForFileQuery) {
    char root_template[] = "/tmp/vitreo_security_test_XXXXXX";
    char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    ASSERT_TRUE(std::filesystem::create_directory(root_path / "target"));

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    DllExports exports = create_advapi32_security_thunks(sec_stubs, &vfs);
    auto* set_entry = find_export(exports, "SetNamedSecurityInfoW");
    auto* get_entry = find_export(exports, "GetFileSecurityW");
    ASSERT_NE(set_entry, nullptr);
    ASSERT_NE(get_entry, nullptr);

    constexpr GuestAddr path = DATA_BASE + 0x700;
    constexpr GuestAddr needed = DATA_BASE + 0x780;
    constexpr GuestAddr descriptor = DATA_BASE + 0x800;
    const std::string path_text = "C:\\target";
    for (usize i = 0; i < path_text.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(
            path + i * 2, static_cast<u8>(path_text[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(path + path_text.size() * 2, 0).is_ok());

    push_arg(0, path);
    push_arg(1, 1);   // SE_FILE_OBJECT
    push_arg(2, 0x4); // DACL_SECURITY_INFORMATION with a NULL DACL
    push_arg(3, 0);
    push_arg(4, 0);
    push_arg(5, 0);
    push_arg(6, 0);
    set_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);

    push_arg(0, path);
    push_arg(1, 0x4);
    push_arg(2, descriptor);
    push_arg(3, 256);
    push_arg(4, needed);
    get_entry->handler(marshaler);

    // Windows stores a NULL DACL set through SetNamedSecurityInfoW as
    // protected and a DACL-only query returns only the DACL: control 0x9404
    // (SELF_RELATIVE | DACL_PROTECTED | DACL_AUTO_INHERITED | DACL_PRESENT),
    // no owner, no group, no DACL body (advapi_named_null_dacl_probe
    // after_null_gfs_dacl, x86 = x64).
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(needed).value(), 20u);
    EXPECT_EQ(memory.read_u16(descriptor + 2).value(), 0x9404u);
    EXPECT_EQ(memory.read_u32(descriptor + 4).value(), 0u);  // Owner
    EXPECT_EQ(memory.read_u32(descriptor + 8).value(), 0u);  // Group
    EXPECT_EQ(memory.read_u32(descriptor + 16).value(), 0u); // NULL DACL
    std::filesystem::remove_all(root_path);
}

// Windows keeps an explicitly empty DACL empty (advapi_sdapi_probe
// sfs_dacl_empty: daclPresent=1 aces=0); Vitreo used to "repair" it into a
// synthetic three-ACE DACL, which no Windows build does.
TEST_F(Advapi32CpuzTest, SetFileSecurityAAndWPersistEmptyDaclAsWindowsDoes) {
    char root_template[] = "/tmp/vitreo_set_file_sec_XXXXXX";
    char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    ASSERT_TRUE(std::filesystem::create_directory(root_path / "target"));

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    DllExports exports = create_advapi32_security_thunks(sec_stubs, &vfs);
    auto* set_a_entry = find_export(exports, "SetFileSecurityA");
    auto* set_w_entry = find_export(exports, "SetFileSecurityW");
    auto* get_a_entry = find_export(exports, "GetFileSecurityA");
    auto* get_w_entry = find_export(exports, "GetFileSecurityW");
    ASSERT_NE(set_a_entry, nullptr);
    ASSERT_NE(set_w_entry, nullptr);
    ASSERT_NE(get_a_entry, nullptr);
    ASSERT_NE(get_w_entry, nullptr);

    constexpr GuestAddr path = DATA_BASE + 0x700;
    constexpr GuestAddr ansi_path = DATA_BASE + 0xA00;
    constexpr GuestAddr needed = DATA_BASE + 0x780;
    constexpr GuestAddr descriptor = DATA_BASE + 0x800;
    constexpr GuestAddr empty_acl = DATA_BASE + 0x900;
    // Absolute SECURITY_DESCRIPTOR (x86): rev, sbz1, control, owner*, group*,
    // sacl*, dacl*
    constexpr GuestAddr abs_sd = DATA_BASE + 0x980;
    const std::string path_text = "C:\\target";
    for (usize i = 0; i < path_text.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(
            path + i * 2, static_cast<u8>(path_text[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(path + path_text.size() * 2, 0).is_ok());

    ASSERT_TRUE(memory.write_u8(empty_acl + 0, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(empty_acl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(empty_acl + 2, 8).is_ok());
    ASSERT_TRUE(memory.write_u16(empty_acl + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(empty_acl + 6, 0).is_ok());

    ASSERT_TRUE(memory.write_u8(abs_sd + 0, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(abs_sd + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(abs_sd + 2, 0x0004).is_ok()); // SE_DACL_PRESENT
    ASSERT_TRUE(memory.write_u32(abs_sd + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(abs_sd + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(abs_sd + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(abs_sd + 16, static_cast<u32>(empty_acl)).is_ok());

    push_arg(0, path);
    push_arg(1, 0x4); // DACL_SECURITY_INFORMATION
    push_arg(2, abs_sd);
    set_w_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    ASSERT_TRUE(memory.write_bytes(
        ansi_path, reinterpret_cast<const u8*>(path_text.c_str()),
        path_text.size() + 1).is_ok());
    push_arg(0, ansi_path);
    push_arg(1, 0x4);
    push_arg(2, abs_sd);
    set_a_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    push_arg(0, path);
    push_arg(1, 0x4);
    push_arg(2, descriptor);
    push_arg(3, 256);
    push_arg(4, needed);
    get_w_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const u32 sd_size = memory.read_u32(needed).value();
    EXPECT_GE(sd_size, 28u);
    const u32 dacl_off = memory.read_u32(descriptor + 16).value();
    ASSERT_NE(dacl_off, 0u);
    EXPECT_EQ(memory.read_u16(descriptor + dacl_off + 4).value(), 0u);

    push_arg(0, ansi_path);
    push_arg(1, 0x4);
    push_arg(2, descriptor);
    push_arg(3, 256);
    push_arg(4, needed);
    get_a_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(needed).value(), sd_size);
    EXPECT_EQ(memory.read_u16(descriptor + dacl_off + 4).value(), 0u);
    std::filesystem::remove_all(root_path);
}

// A directory that only inherited at creation: owner, a DACL of inherited
// ACEs and control 0x8004 without SE_DACL_AUTO_INHERITED
// (advapi_named_null_dacl_probe fresh_dir_owner_dacl: count=3, each ACE
// flags 0x13 = OBJECT_INHERIT | CONTAINER_INHERIT | INHERITED).
TEST_F(Advapi32CpuzTest, GetNamedSecurityInfoWReturnsOwnerAndInheritedDacl) {
    char root_template[] = "/tmp/vitreo_named_sec_XXXXXX";
    char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    ASSERT_TRUE(std::filesystem::create_directory(root_path / "target"));

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    DllExports exports = create_advapi32_security_thunks(sec_stubs, &vfs);
    auto* entry = find_export(exports, "GetNamedSecurityInfoW");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr path = DATA_BASE + 0x700;
    constexpr GuestAddr owner_out = DATA_BASE + 0x780;
    constexpr GuestAddr dacl_out = DATA_BASE + 0x784;
    constexpr GuestAddr descriptor_out = DATA_BASE + 0x788;
    const std::string path_text = "C:\\target";
    for (usize i = 0; i < path_text.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(
            path + i * 2, static_cast<u8>(path_text[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(path + path_text.size() * 2, 0).is_ok());

    push_arg(0, path);
    push_arg(1, 1);    // SE_FILE_OBJECT
    push_arg(2, 0x5);  // OWNER | DACL
    push_arg(3, owner_out);
    push_arg(4, 0);
    push_arg(5, dacl_out);
    push_arg(6, 0);
    push_arg(7, descriptor_out);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    const GuestAddr descriptor = memory.read_u32(descriptor_out).value();
    ASSERT_NE(descriptor, 0u);
    EXPECT_EQ(memory.read_u16(descriptor + 2).value(), 0x8004u);
    EXPECT_EQ(memory.read_u32(owner_out).value(), descriptor + 20);
    EXPECT_EQ(memory.read_u8(descriptor + 20).value(), 1u);
    const u32 dacl_off = memory.read_u32(descriptor + 16).value();
    ASSERT_NE(dacl_off, 0u);
    const GuestAddr dacl = memory.read_u32(dacl_out).value();
    EXPECT_EQ(dacl, descriptor + dacl_off);
    const u16 ace_count = memory.read_u16(dacl + 4).value();
    ASSERT_EQ(ace_count, 3u);
    GuestAddr ace = dacl + 8;
    for (u16 i = 0; i < ace_count; ++i) {
        EXPECT_EQ(memory.read_u8(ace + 1).value(), 0x13u) << "ACE " << i;
        ace += memory.read_u16(ace + 2).value();
    }
    std::filesystem::remove_all(root_path);
}

TEST_F(Advapi32CpuzTest, AccessCheckGrantsMappedDesiredAccessForNullDacl) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "AccessCheck");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr sd = DATA_BASE + 0x100;
    constexpr GuestAddr mapping = DATA_BASE + 0x200;
    constexpr GuestAddr granted = DATA_BASE + 0x220;
    constexpr GuestAddr status = DATA_BASE + 0x224;
    constexpr GuestAddr priv_len = DATA_BASE + 0x228;
    const auto& sid = SecurityStubs::synthetic_sid();
    std::vector<u8> bytes(20 + sid.size() * 2, 0);
    bytes[0] = 1;
    bytes[2] = 0x04;
    bytes[3] = 0x80; // SE_DACL_PRESENT | SE_SELF_RELATIVE
    bytes[4] = 20;
    bytes[8] = static_cast<u8>(20 + sid.size());
    std::memcpy(bytes.data() + 20, sid.data(), sid.size());
    std::memcpy(bytes.data() + 20 + sid.size(), sid.data(), sid.size());
    ASSERT_TRUE(memory.write_bytes(sd, bytes.data(), bytes.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(mapping + 0, 0x00020001).is_ok()); // GenericRead
    ASSERT_TRUE(memory.write_u32(mapping + 4, 0x00020002).is_ok());
    ASSERT_TRUE(memory.write_u32(mapping + 8, 0x00020004).is_ok());
    ASSERT_TRUE(memory.write_u32(mapping + 12, 0x001F01FF).is_ok()); // GenericAll
    // A real impersonation token with TOKEN_QUERY and a 20-byte privilege
    // set: a bogus token is ERROR_INVALID_HANDLE and a short buffer is
    // ERROR_INSUFFICIENT_BUFFER (advapi_token2_probe ac_badtoken / ac_pslen*).
    constexpr GuestAddr priv_set = DATA_BASE + 0x240;
    ASSERT_TRUE(memory.write_u32(priv_len, 20).is_ok());
    Handle primary = 0, token = 0;
    u32 open_error = 0;
    ASSERT_TRUE(sec_stubs.open_process_token(0xFFFFFFFFu, 0x000F01FFu, primary, open_error));
    ASSERT_TRUE(sec_stubs.duplicate_token(primary, 2, token, 2, 0x0000000Cu));

    // DesiredAccess = READ_CONTROL (already specific)
    push_arg(0, sd);
    push_arg(1, token);
    push_arg(2, 0x00020000); // READ_CONTROL
    push_arg(3, mapping);
    push_arg(4, priv_set);
    push_arg(5, priv_len);
    push_arg(6, granted);
    push_arg(7, status);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(status).value(), 1u);
    EXPECT_EQ(memory.read_u32(granted).value(), 0x00020000u);

    // MAXIMUM_ALLOWED → GenericAll from mapping (not the 0x02000000 flag)
    push_arg(0, sd);
    push_arg(1, token);
    push_arg(2, 0x02000000);
    push_arg(3, mapping);
    push_arg(4, priv_set);
    push_arg(5, priv_len);
    push_arg(6, granted);
    push_arg(7, status);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(granted).value(), 0x001F01FFu);
}

TEST_F(Advapi32CpuzTest, MapGenericMaskReplacesGenericBits) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "MapGenericMask");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr mask = DATA_BASE + 0x100;
    constexpr GuestAddr mapping = DATA_BASE + 0x110;
    ASSERT_TRUE(memory.write_u32(mask, 0x80000000u).is_ok()); // GENERIC_READ
    ASSERT_TRUE(memory.write_u32(mapping + 0, 0x00020019).is_ok());
    ASSERT_TRUE(memory.write_u32(mapping + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(mapping + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(mapping + 12, 0).is_ok());
    push_arg(0, mask);
    push_arg(1, mapping);
    entry->handler(marshaler);
    EXPECT_EQ(memory.read_u32(mask).value(), 0x00020019u);
}

TEST_F(Advapi32CpuzTest, GetSecurityInfoReturnsSelfRelativeOwnerAndValidDacl) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "GetSecurityInfo");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr owner_out = DATA_BASE + 0x100;
    constexpr GuestAddr dacl_out = DATA_BASE + 0x104;
    constexpr GuestAddr descriptor_out = DATA_BASE + 0x108;
    // 0xABCD0001 is Vitreo's process window station: a user32 object, so
    // SE_WINDOW_OBJECT (7). As SE_KERNEL_OBJECT it names no kernel object
    // (ERROR_INVALID_HANDLE, advapi_sdapi_probe gsi_badhandle).
    push_arg(0, 0xABCD0001);
    push_arg(1, 7); // SE_WINDOW_OBJECT
    push_arg(2, 0x5); // OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
    push_arg(3, owner_out);
    push_arg(4, 0);
    push_arg(5, dacl_out);
    push_arg(6, 0);
    push_arg(7, descriptor_out);

    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const GuestAddr descriptor = memory.read_u32(descriptor_out).value();
    ASSERT_NE(descriptor, 0u);
    EXPECT_EQ(memory.read_u16(descriptor + 2).value(), 0x8004u);
    EXPECT_EQ(memory.read_u32(owner_out).value(), descriptor + 20);
    EXPECT_EQ(memory.read_u8(descriptor + 20).value(), 1u);
    const GuestAddr dacl = memory.read_u32(dacl_out).value();
    ASSERT_NE(dacl, 0u);
    EXPECT_EQ(memory.read_u8(dacl).value(), 2u); // ACL_REVISION
    EXPECT_EQ(memory.read_u16(dacl + 2).value(), 76u);
    EXPECT_EQ(memory.read_u16(dacl + 4).value(), 3u);
}

TEST_F(Advapi32CpuzTest, SetEntriesInAclBuildsDeniedSidAce) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* build_trustee = find_export(exports, "BuildTrusteeWithSidW");
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    ASSERT_NE(build_trustee, nullptr);
    ASSERT_NE(set_entries, nullptr);

    constexpr GuestAddr explicit_access = DATA_BASE + 0x100;
    constexpr GuestAddr trustee = explicit_access + 12;
    constexpr GuestAddr sid = DATA_BASE + 0x200;
    constexpr GuestAddr acl_out = DATA_BASE + 0x300;
    constexpr GuestAddr old_acl = DATA_BASE + 0x400;
    const auto& sid_bytes = SecurityStubs::synthetic_sid();
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes.data(), sid_bytes.size()).is_ok());

    push_arg(0, trustee);
    push_arg(1, sid);
    build_trustee->handler(marshaler);
    EXPECT_EQ(memory.read_u32(trustee + 16).value(), sid);

    ASSERT_TRUE(memory.write_u32(explicit_access, 0x000D013E).is_ok());
    ASSERT_TRUE(memory.write_u32(explicit_access + 4, 3).is_ok()); // DENY_ACCESS
    ASSERT_TRUE(memory.write_u32(explicit_access + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(old_acl, 0x0000000000080002).is_ok());
    push_arg(0, 1);
    push_arg(1, explicit_access);
    push_arg(2, old_acl);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);
    EXPECT_EQ(memory.read_u16(acl + 4).value(), 1u);
    EXPECT_EQ(memory.read_u8(acl + 8).value(), 1u); // ACCESS_DENIED_ACE_TYPE
    EXPECT_EQ(memory.read_u32(acl + 12).value(), 0x000D013Eu);
    EXPECT_EQ(memory.read_u8(acl + 16).value(), 1u); // SID revision
}

TEST_F(Advapi32CpuzTest, SetEntriesInAclABuildsGrantedSidAce) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* set_entries = find_export(exports, "SetEntriesInAclA");
    ASSERT_NE(set_entries, nullptr);

    constexpr GuestAddr explicit_access = DATA_BASE + 0x500;
    constexpr GuestAddr sid = DATA_BASE + 0x600;
    constexpr GuestAddr acl_out = DATA_BASE + 0x700;
    const auto& sid_bytes = SecurityStubs::synthetic_sid();
    ASSERT_TRUE(memory.write_bytes(
        sid, sid_bytes.data(), sid_bytes.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(explicit_access, 0x001F0001).is_ok());
    ASSERT_TRUE(memory.write_u32(explicit_access + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(explicit_access + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(explicit_access + 28, sid).is_ok());
    push_arg(0, 1);
    push_arg(1, explicit_access);
    push_arg(2, 0);
    push_arg(3, acl_out);

    set_entries->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);
    EXPECT_EQ(memory.read_u16(acl + 4).value(), 1u);
    EXPECT_EQ(memory.read_u8(acl + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(acl + 12).value(), 0x001F0001u);
    EXPECT_EQ(memory.read_u8(acl + 16).value(), 1u);
}

TEST_F(Advapi32CpuzTest, SetEntriesInAclWResolvesNamedTrustee) {
    // Learn: "You can use a name or a SID to identify a trustee."
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* build = find_export(exports, "BuildExplicitAccessWithNameW");
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    ASSERT_NE(build, nullptr);
    ASSERT_NE(set_entries, nullptr);

    constexpr GuestAddr name = DATA_BASE + 0x800;
    constexpr GuestAddr explicit_access = DATA_BASE + 0x880;
    constexpr GuestAddr acl_out = DATA_BASE + 0x900;
    const char16_t current_user[] = u"CURRENT_USER";
    ASSERT_TRUE(memory.write_bytes(
        name, reinterpret_cast<const u8*>(current_user),
        sizeof(current_user)).is_ok());

    push_arg(0, explicit_access);
    push_arg(1, name);
    push_arg(2, 0x001F0001);
    push_arg(3, 1); // GRANT_ACCESS
    push_arg(4, 0);
    build->handler(marshaler);

    push_arg(0, 1);
    push_arg(1, explicit_access);
    push_arg(2, 0);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);
    EXPECT_EQ(memory.read_u16(acl + 4).value(), 1u);
    EXPECT_EQ(memory.read_u8(acl + 8).value(), 0u); // ACCESS_ALLOWED
    const auto& sid = SecurityStubs::synthetic_sid();
    std::vector<u8> got(sid.size());
    ASSERT_TRUE(memory.read_bytes(acl + 16, got.data(), got.size()).is_ok());
    EXPECT_EQ(got, sid);
}

TEST_F(Advapi32CpuzTest, SetEntriesInAclWBuildsAuditAceForNamedTrustee) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* build = find_export(exports, "BuildExplicitAccessWithNameW");
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    ASSERT_NE(build, nullptr);
    ASSERT_NE(set_entries, nullptr);

    constexpr GuestAddr name = DATA_BASE + 0xA00;
    constexpr GuestAddr explicit_access = DATA_BASE + 0xA80;
    constexpr GuestAddr acl_out = DATA_BASE + 0xB00;
    const char16_t administrators[] = u"Administrators";
    ASSERT_TRUE(memory.write_bytes(
        name, reinterpret_cast<const u8*>(administrators),
        sizeof(administrators)).is_ok());

    push_arg(0, explicit_access);
    push_arg(1, name);
    push_arg(2, 0x000F003F); // KEY_ALL_ACCESS-ish
    push_arg(3, 5); // SET_AUDIT_SUCCESS
    push_arg(4, 0);
    build->handler(marshaler);

    push_arg(0, 1);
    push_arg(1, explicit_access);
    push_arg(2, 0);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);
    EXPECT_EQ(memory.read_u8(acl + 8).value(), 2u); // SYSTEM_AUDIT_ACE_TYPE
    EXPECT_EQ(memory.read_u8(acl + 9).value(), 0x40u); // SUCCESSFUL_ACCESS
    EXPECT_EQ(memory.read_u8(acl + 16).value(), 1u); // SID revision
    EXPECT_EQ(memory.read_u8(acl + 23).value(), 5u); // NT authority
    EXPECT_EQ(memory.read_u32(acl + 24).value(), 32u); // BUILTIN
    EXPECT_EQ(memory.read_u32(acl + 28).value(), 544u); // Administrators
}

TEST_F(Advapi32CpuzTest, SetEntriesInAclBuildsMultipleCanonicalAces) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    ASSERT_NE(set_entries, nullptr);

    constexpr GuestAddr entries = DATA_BASE + 0x100;
    constexpr GuestAddr sid = DATA_BASE + 0x300;
    constexpr GuestAddr acl_out = DATA_BASE + 0x380;
    const auto& sid_bytes = SecurityStubs::synthetic_sid();
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes.data(), sid_bytes.size()).is_ok());

    for (u32 index = 0; index < 3; ++index) {
        const GuestAddr entry = entries + index * 32;
        ASSERT_TRUE(memory.write_u32(entry, 0x100u << index).is_ok());
        ASSERT_TRUE(memory.write_u32(entry + 4, index == 1 ? 3u : 1u).is_ok());
        ASSERT_TRUE(memory.write_u32(entry + 8, index).is_ok());
        ASSERT_TRUE(memory.write_u32(entry + 28, sid).is_ok());
    }
    push_arg(0, 3);
    push_arg(1, entries);
    push_arg(2, 0);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);
    EXPECT_EQ(memory.read_u16(acl + 4).value(), 3u);
    EXPECT_EQ(memory.read_u8(acl + 8).value(), 1u); // deny ACE first
    const u16 first_size = memory.read_u16(acl + 10).value();
    EXPECT_EQ(memory.read_u8(acl + 8 + first_size).value(), 0u);
}

TEST_F(Advapi32CpuzTest, GetEffectiveRightsFromAclAppliesAllowAndDeny) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    auto* get_rights = find_export(exports, "GetEffectiveRightsFromAclW");
    auto* build_trustee = find_export(exports, "BuildTrusteeWithSidW");
    ASSERT_NE(set_entries, nullptr);
    ASSERT_NE(get_rights, nullptr);
    ASSERT_NE(build_trustee, nullptr);

    constexpr GuestAddr entries = DATA_BASE + 0x100;
    constexpr GuestAddr sid = DATA_BASE + 0x200;
    constexpr GuestAddr acl_out = DATA_BASE + 0x280;
    constexpr GuestAddr trustee = DATA_BASE + 0x2C0;
    constexpr GuestAddr mask_out = DATA_BASE + 0x300;
    const auto& sid_bytes = SecurityStubs::synthetic_sid();
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes.data(), sid_bytes.size()).is_ok());

    // GRANT 0x000F, then DENY 0x0001 → effective 0x000E
    ASSERT_TRUE(memory.write_u32(entries, 0x000F).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 4, 1).is_ok()); // GRANT_ACCESS
    ASSERT_TRUE(memory.write_u32(entries + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 28, sid).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 32, 0x0001).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 36, 3).is_ok()); // DENY_ACCESS
    ASSERT_TRUE(memory.write_u32(entries + 40, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 60, sid).is_ok());

    push_arg(0, 2);
    push_arg(1, entries);
    push_arg(2, 0);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);

    push_arg(0, trustee);
    push_arg(1, sid);
    build_trustee->handler(marshaler);

    push_arg(0, acl);
    push_arg(1, trustee);
    push_arg(2, mask_out);
    get_rights->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(mask_out).value(), 0x000Eu);
}

TEST_F(Advapi32CpuzTest, GetExplicitEntriesFromAclRoundTripsDeniedAce) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    auto* get_entries = find_export(exports, "GetExplicitEntriesFromAclW");
    ASSERT_NE(set_entries, nullptr);
    ASSERT_NE(get_entries, nullptr);

    constexpr GuestAddr explicit_access = DATA_BASE + 0x100;
    constexpr GuestAddr sid = DATA_BASE + 0x200;
    constexpr GuestAddr acl_out = DATA_BASE + 0x300;
    constexpr GuestAddr count_out = DATA_BASE + 0x340;
    constexpr GuestAddr list_out = DATA_BASE + 0x360;
    const auto& sid_bytes = SecurityStubs::synthetic_sid();
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes.data(), sid_bytes.size()).is_ok());

    ASSERT_TRUE(memory.write_u32(explicit_access, 0x000D013E).is_ok());
    ASSERT_TRUE(memory.write_u32(explicit_access + 4, 3).is_ok()); // DENY_ACCESS
    ASSERT_TRUE(memory.write_u32(explicit_access + 8, 0x3).is_ok()); // inheritance
    ASSERT_TRUE(memory.write_u32(explicit_access + 28, sid).is_ok());

    push_arg(0, 1);
    push_arg(1, explicit_access);
    push_arg(2, 0);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);

    push_arg(0, acl);
    push_arg(1, count_out);
    push_arg(2, list_out);
    get_entries->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(count_out).value(), 1u);
    const GuestAddr list = memory.read_u32(list_out).value();
    ASSERT_NE(list, 0u);
    EXPECT_EQ(memory.read_u32(list + 0).value(), 0x000D013Eu);
    EXPECT_EQ(memory.read_u32(list + 4).value(), 3u); // DENY_ACCESS
    EXPECT_EQ(memory.read_u32(list + 8).value(), 0x3u);
    EXPECT_EQ(memory.read_u32(list + 20).value(), 0u); // TRUSTEE_IS_SID
    const GuestAddr out_sid = memory.read_u32(list + 28).value();
    ASSERT_NE(out_sid, 0u);
    for (usize i = 0; i < sid_bytes.size(); ++i) {
        EXPECT_EQ(memory.read_u8(out_sid + i).value(), sid_bytes[i]);
    }
}

TEST_F(Advapi32CpuzTest, GetExplicitEntriesFromAclEmptyAclReturnsZero) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* get_entries = find_export(exports, "GetExplicitEntriesFromAclW");
    ASSERT_NE(get_entries, nullptr);

    constexpr GuestAddr acl = DATA_BASE + 0x100;
    constexpr GuestAddr count_out = DATA_BASE + 0x140;
    constexpr GuestAddr list_out = DATA_BASE + 0x160;
    ASSERT_TRUE(memory.write_u8(acl, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(acl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 2, 8).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(acl + 6, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(list_out, 0xdeadbeef).is_ok());

    push_arg(0, acl);
    push_arg(1, count_out);
    push_arg(2, list_out);
    get_entries->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(count_out).value(), 0u);
    EXPECT_EQ(memory.read_u32(list_out).value(), 0u);
}

TEST_F(Advapi32CpuzTest, SetEntriesInAclRevokesMatchingExplicitAces) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* set_entries = find_export(exports, "SetEntriesInAclW");
    ASSERT_NE(set_entries, nullptr);

    constexpr GuestAddr entries = DATA_BASE + 0x100;
    constexpr GuestAddr sid = DATA_BASE + 0x300;
    constexpr GuestAddr old_acl = DATA_BASE + 0x380;
    constexpr GuestAddr acl_out = DATA_BASE + 0x480;
    const auto& sid_bytes = SecurityStubs::synthetic_sid();
    ASSERT_TRUE(memory.write_bytes(sid, sid_bytes.data(), sid_bytes.size()).is_ok());

    // Chromium's lockdown DACL update combines a revoke entry with grants.
    ASSERT_TRUE(memory.write_u32(entries, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 4, 4).is_ok()); // REVOKE_ACCESS
    ASSERT_TRUE(memory.write_u32(entries + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 28, sid).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 32, 0x10000000).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 36, 1).is_ok()); // GRANT_ACCESS
    ASSERT_TRUE(memory.write_u32(entries + 40, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(entries + 60, sid).is_ok());

    const u16 ace_size = static_cast<u16>(8 + sid_bytes.size());
    const u16 acl_size = static_cast<u16>(8 + ace_size);
    ASSERT_TRUE(memory.write_u8(old_acl, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(old_acl + 1, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(old_acl + 2, acl_size).is_ok());
    ASSERT_TRUE(memory.write_u16(old_acl + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u16(old_acl + 6, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(old_acl + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(old_acl + 9, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(old_acl + 10, ace_size).is_ok());
    ASSERT_TRUE(memory.write_u32(old_acl + 12, 0x00020000).is_ok());
    ASSERT_TRUE(memory.write_bytes(old_acl + 16, sid_bytes.data(),
                                   sid_bytes.size()).is_ok());

    push_arg(0, 2);
    push_arg(1, entries);
    push_arg(2, old_acl);
    push_arg(3, acl_out);
    set_entries->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const GuestAddr acl = memory.read_u32(acl_out).value();
    ASSERT_NE(acl, 0u);
    EXPECT_EQ(memory.read_u16(acl + 4).value(), 1u);
    EXPECT_EQ(memory.read_u8(acl + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(acl + 12).value(), 0x10000000u);
}

TEST_F(Advapi32CpuzTest, RegisterTraceGuidsTracksProviderHandle) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* register_entry = find_export(exports, "RegisterTraceGuidsW");
    auto* unregister_entry = find_export(exports, "UnregisterTraceGuids");
    ASSERT_NE(register_entry, nullptr);
    ASSERT_NE(unregister_entry, nullptr);

    constexpr GuestAddr control_guid = DATA_BASE + 0x500;
    constexpr GuestAddr handle_out = DATA_BASE + 0x520;
    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, control_guid);
    push_arg(3, 0);
    push_arg(4, 0);
    push_arg(5, 0);
    push_arg(6, 0);
    push_arg(7, handle_out);
    register_entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const u64 trace_handle = memory.read_u64(handle_out).value();
    ASSERT_NE(trace_handle, 0u);

    push_arg(0, static_cast<u32>(trace_handle));
    push_arg(1, static_cast<u32>(trace_handle >> 32));
    unregister_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    unregister_entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 6u); // ERROR_INVALID_HANDLE
}


// =============================================================================
// ImpersonateSelf — 1 arg, returns TRUE
// =============================================================================

TEST_F(Advapi32CpuzTest, ImpersonateSelf_ReturnsTrue) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "ImpersonateSelf");
    ASSERT_NE(entry, nullptr);

    push_arg(0, 2); // SecurityImpersonation
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

TEST_F(Advapi32CpuzTest, ImpersonateLoggedOnUserAcceptsProcessToken) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* open = find_export(exports, "OpenProcessToken");
    auto* impersonate = find_export(exports, "ImpersonateLoggedOnUser");
    auto* revert = find_export(exports, "RevertToSelf");
    ASSERT_NE(open, nullptr);
    ASSERT_NE(impersonate, nullptr);
    ASSERT_NE(revert, nullptr);
    EXPECT_EQ(impersonate->stdcall_bytes, 4u);

    constexpr GuestAddr token_out = DATA_BASE + 0x280;
    push_arg(0, 0xFFFFFFFFu); // GetCurrentProcess pseudo-handle
    push_arg(1, 0x000F01FFu); // TOKEN_ALL_ACCESS
    push_arg(2, static_cast<u32>(token_out));
    open->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const u32 token = memory.read_u32(token_out).value_or(0);
    ASSERT_NE(token, 0u);

    push_arg(0, token);
    impersonate->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    push_arg(0, 0);
    impersonate->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    revert->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// =============================================================================
// RevertToSelf — 0 args, returns TRUE
// =============================================================================

TEST_F(Advapi32CpuzTest, RevertToSelf_ReturnsTrue) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "RevertToSelf");
    ASSERT_NE(entry, nullptr);

    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// =============================================================================
// IsValidSecurityDescriptor — 1 arg, returns TRUE
// =============================================================================

TEST_F(Advapi32CpuzTest, IsValidSecurityDescriptor_ReturnsTrue) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "IsValidSecurityDescriptor");
    ASSERT_NE(entry, nullptr);

    // A freshly initialised absolute descriptor (revision 1) is valid; a
    // revision-0 one is not (advapi_sdapi_probe ivsd_fresh / ivsd_rev2:
    // FALSE, ERROR_INVALID_SECURITY_DESCR).
    for (u32 i = 0; i < 20; ++i) ASSERT_TRUE(memory.write_u8(DATA_BASE + 0x200 + i, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(DATA_BASE + 0x200, 1).is_ok());
    push_arg(0, DATA_BASE + 0x200); // pSecurityDescriptor
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    ASSERT_TRUE(memory.write_u8(DATA_BASE + 0x200, 0).is_ok());
    push_arg(0, DATA_BASE + 0x200);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1338u);
}

// =============================================================================
// FreeSid — 1 arg, returns 0 (NULL on success)
// =============================================================================

TEST_F(Advapi32CpuzTest, FreeSid_ReturnsNull) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "FreeSid");
    ASSERT_NE(entry, nullptr);

    push_arg(0, 0x5D000000); // pSid
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// LookupPrivilegeValueA — 3 args, write "SeDebugPrivilege", returns TRUE
// =============================================================================

TEST_F(Advapi32CpuzTest, LookupPrivilegeValueA_ReturnsTrue) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "LookupPrivilegeValueA");
    ASSERT_NE(entry, nullptr);

    // Write ANSI string "SeDebugPrivilege\0" at DATA_BASE
    constexpr GuestAddr name_addr = DATA_BASE;
    const char priv[] = "SeDebugPrivilege";
    for (usize i = 0; i <= 16; ++i) {
        (void)memory.write_u8(name_addr + i, static_cast<u8>(priv[i]));
    }

    // LUID output buffer
    constexpr GuestAddr luid_addr = DATA_BASE + 0x100;
    (void)memory.write_u32(luid_addr, 0);
    (void)memory.write_u32(luid_addr + 4, 0);

    push_arg(0, 0);          // lpSystemName = NULL
    push_arg(1, name_addr);  // lpName
    push_arg(2, luid_addr);  // lpLuid
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    // LUID should be non-zero
    u32 low = memory.read_u32(luid_addr).value();
    u32 high = memory.read_u32(luid_addr + 4).value();
    EXPECT_NE(low | high, 0u);
}

// =============================================================================
// InitializeAcl — 3 args, returns TRUE and writes ACL header
// =============================================================================

TEST_F(Advapi32CpuzTest, InitializeAcl_ReturnsTrueAndWritesHeader) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "InitializeAcl");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr acl_addr = DATA_BASE + 0x300;
    constexpr u32 acl_len = 64;
    constexpr u32 acl_revision = 2; // ACL_REVISION

    push_arg(0, acl_addr);
    push_arg(1, acl_len);
    push_arg(2, acl_revision);
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    // Verify ACL header: AclRevision at offset 0
    EXPECT_EQ(memory.read_u8(acl_addr).value(), static_cast<u8>(acl_revision));
    // AclSize at offset 2 (u16)
    EXPECT_EQ(memory.read_u16(acl_addr + 2).value(), static_cast<u16>(acl_len));
    // AceCount at offset 4 should be 0
    EXPECT_EQ(memory.read_u16(acl_addr + 4).value(), 0u);
}

TEST_F(Advapi32CpuzTest, AddAceInsertsContiguousAceAndUpdatesCount) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* init = find_export(exports, "InitializeAcl");
    auto* add = find_export(exports, "AddAce");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->stdcall_bytes, 20u);

    constexpr GuestAddr acl_addr = DATA_BASE + 0x300;
    constexpr GuestAddr ace_addr = DATA_BASE + 0x400;
    push_arg(0, acl_addr);
    push_arg(1, 64);
    push_arg(2, 2);
    init->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    // ACCESS_ALLOWED_ACE with a minimal S-1-0 SID.
    const u8 ace[] = {
        0, 0, 16, 0,
        1, 0, 0, 0,
        1, 0, 0, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(ace_addr, ace, sizeof(ace)).is_ok());

    push_arg(0, acl_addr);
    push_arg(1, 2);
    push_arg(2, 0xFFFFFFFFu);
    push_arg(3, ace_addr);
    push_arg(4, sizeof(ace));
    add->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(acl_addr + 4).value(), 1u);
    for (usize i = 0; i < sizeof(ace); ++i) {
        EXPECT_EQ(memory.read_u8(acl_addr + 8 + i).value(), ace[i]);
    }
}

TEST_F(Advapi32CpuzTest, AddAceRejectsAceThatDoesNotFit) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* init = find_export(exports, "InitializeAcl");
    auto* add = find_export(exports, "AddAce");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(add, nullptr);

    constexpr GuestAddr acl_addr = DATA_BASE + 0x300;
    constexpr GuestAddr ace_addr = DATA_BASE + 0x400;
    push_arg(0, acl_addr);
    push_arg(1, 8);
    push_arg(2, 2);
    init->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    const u8 ace[] = {0, 0, 4, 0};
    ASSERT_TRUE(memory.write_bytes(ace_addr, ace, sizeof(ace)).is_ok());
    push_arg(0, acl_addr);
    push_arg(1, 2);
    push_arg(2, 0xFFFFFFFFu);
    push_arg(3, ace_addr);
    push_arg(4, sizeof(ace));
    add->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u16(acl_addr + 4).value(), 0u);
}

TEST_F(Advapi32CpuzTest, GetAceReturnsPointerToIndexedAclEntry) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* get_ace = find_export(exports, "GetAce");
    ASSERT_NE(get_ace, nullptr);
    EXPECT_EQ(get_ace->stdcall_bytes, 12u);

    constexpr GuestAddr acl = DATA_BASE + 0x300;
    constexpr GuestAddr out = DATA_BASE + 0x500;
    const u8 bytes[] = {
        2, 0, 40, 0, 2, 0, 0, 0, // ACL header
        0, 0, 16, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        1, 3, 16, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(acl, bytes, sizeof(bytes)).is_ok());

    push_arg(0, acl);
    push_arg(1, 1);
    push_arg(2, out);
    get_ace->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(out).value(), acl + 24);
    EXPECT_EQ(memory.read_u8(acl + 24).value(), 1u);
    EXPECT_EQ(memory.read_u8(acl + 25).value(), 3u);

    push_arg(0, acl);
    push_arg(1, 2);
    push_arg(2, out);
    get_ace->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
}

TEST_F(Advapi32CpuzTest, DecryptFileASucceedsForUnencryptedPath) {
    // Microsoft Learn: DecryptFile returns nonzero when the file is not
    // encrypted. Vitreo has no EFS, so any existing path succeeds.
    char root_template[] = "/tmp/vitreo_decrypt_XXXXXX";
    char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    ASSERT_TRUE(std::filesystem::create_directory(root_path / "payload"));
    ASSERT_TRUE(
        (std::ofstream(root_path / "payload" / "cab.bin") << "x").good());

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    DllExports exports = create_advapi32_security_thunks(sec_stubs, &vfs);
    auto* entry = find_export(exports, "DecryptFileA");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    constexpr GuestAddr path = DATA_BASE + 0x500;
    const char* path_text = "C:\\payload\\cab.bin";
    for (usize i = 0; path_text[i] != '\0'; ++i) {
        ASSERT_TRUE(memory.write_u8(path + i, static_cast<u8>(path_text[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u8(path + std::strlen(path_text), 0).is_ok());

    push_arg(0, static_cast<u32>(path));
    push_arg(1, 0);
    set_last_error(0xDEADu);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_advapi32_probe.expected)

    push_arg(0, static_cast<u32>(path));
    push_arg(1, 1); // dwReserved must be 0
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    std::filesystem::remove_all(root_path);
}

TEST_F(Advapi32CpuzTest, StartTraceAValidatesNullAndDeniesSession) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "StartTraceA");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12u);

    constexpr GuestAddr handle_out = DATA_BASE + 0x100;
    constexpr GuestAddr properties = DATA_BASE + 0x180;
    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(properties));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_INVALID_PARAMETER);

    push_arg(0, static_cast<u32>(handle_out));
    push_arg(2, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_INVALID_PARAMETER);

    push_arg(2, static_cast<u32>(properties));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_ACCESS_DENIED);
}

TEST_F(Advapi32CpuzTest, StopTraceARequiresProperties) {
    DllExports exports = create_advapi32_security_thunks(sec_stubs);
    auto* entry = find_export(exports, "StopTraceA");
    ASSERT_NE(entry, nullptr);

    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_INVALID_PARAMETER);

    // A well-formed EVENT_TRACE_PROPERTIES (x86: BufferSize, Wnode.Flags =
    // WNODE_FLAG_TRACED_GUID, LoggerNameOffset at 112) with a handle that
    // names no session: ERROR_INVALID_HANDLE (advapi_etw_probe stopw_handle).
    // A zero BufferSize would be ERROR_BAD_LENGTH first (ctw_size_zero).
    constexpr GuestAddr properties = DATA_BASE + 0x180;
    for (u32 i = 0; i < 0x80; i += 4) ASSERT_TRUE(memory.write_u32(properties + i, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(properties + 0, 0x78 + 0x40).is_ok());
    ASSERT_TRUE(memory.write_u32(properties + 44, 0x20000).is_ok());
    ASSERT_TRUE(memory.write_u32(properties + 112, 0x78).is_ok());
    push_arg(0, 1);
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, static_cast<u32>(properties));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_INVALID_HANDLE);
}
