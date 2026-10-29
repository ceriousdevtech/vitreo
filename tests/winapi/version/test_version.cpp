/// @file test_version.cpp
/// @brief Tests for VersionManager (WIN32-087)

#include "winapi/common/synthetic_version_resource.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <vitreo/common.hpp>
#include <winapi/system/version/version.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <vector>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

using Bytes = std::vector<u8>;

void append_u16(Bytes& bytes, u16 value) {
    bytes.push_back(static_cast<u8>(value));
    bytes.push_back(static_cast<u8>(value >> 8));
}

void write_u16(Bytes& bytes, usize offset, u16 value) {
    bytes[offset] = static_cast<u8>(value);
    bytes[offset + 1] = static_cast<u8>(value >> 8);
}

void write_u32(Bytes& bytes, usize offset, u32 value) {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] =
            static_cast<u8>(value >> (index * 8));
    }
}

u32 read_u32(const Bytes& bytes, usize offset) {
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}

void align4(Bytes& bytes) {
    while ((bytes.size() & 3u) != 0) bytes.push_back(0);
}

Bytes utf16_value(std::u16string_view value) {
    Bytes bytes;
    for (char16_t unit : value) {
        append_u16(bytes, static_cast<u16>(unit));
    }
    append_u16(bytes, 0);
    return bytes;
}

Bytes make_block(
    std::u16string_view key,
    u16 value_length,
    u16 type,
    const Bytes& value,
    std::initializer_list<Bytes> children = {}) {
    Bytes block(6, 0);
    write_u16(block, 2, value_length);
    write_u16(block, 4, type);
    for (char16_t unit : key) {
        append_u16(block, static_cast<u16>(unit));
    }
    append_u16(block, 0);
    align4(block);
    block.insert(block.end(), value.begin(), value.end());
    align4(block);
    for (const Bytes& child : children) {
        block.insert(block.end(), child.begin(), child.end());
        align4(block);
    }
    write_u16(block, 0, static_cast<u16>(block.size()));
    return block;
}

Bytes make_version_block(bool large = false) {
    Bytes fixed_info(52, 0);
    write_u32(fixed_info, 0, 0xFEEF04BDu);
    write_u32(fixed_info, 4, 0x00010000u);
    write_u32(fixed_info, 8, 0x00010002u);

    const Bytes translation{
        0x09, 0x04, 0xB0, 0x04}; // language 0409, codepage 04B0
    const Bytes var_file_info = make_block(
        u"VarFileInfo", 0, 1, {},
        {make_block(u"Translation", 4, 0, translation)});

    const std::u16string product =
        large ? std::u16string(700, u'V')
              : std::u16string(u"Steam Client Service");
    const Bytes product_name = make_block(
        u"ProductName",
        static_cast<u16>(product.size() + 1),
        1,
        utf16_value(product));
    const Bytes string_table = make_block(
        u"040904B0", 0, 1, {}, {product_name});
    const Bytes string_file_info = make_block(
        u"StringFileInfo", 0, 1, {}, {string_table});

    return make_block(
        u"VS_VERSION_INFO", 52, 0, fixed_info,
        {string_file_info, var_file_info});
}

Bytes make_pe_with_version_resource(const Bytes& resource) {
    constexpr usize pe_offset = 0x80;
    constexpr usize coff_offset = pe_offset + 4;
    constexpr usize optional_offset = coff_offset + 20;
    constexpr usize optional_size = 0xE0;
    constexpr usize section_offset = optional_offset + optional_size;
    constexpr usize raw_offset = 0x200;
    constexpr usize resource_offset = 0x100;
    const usize raw_size =
        (resource_offset + resource.size() + 0x1FFu) & ~usize{0x1FFu};
    Bytes pe(raw_offset + raw_size, 0);

    write_u16(pe, 0, 0x5A4D);
    write_u32(pe, 0x3C, static_cast<u32>(pe_offset));
    write_u32(pe, pe_offset, 0x00004550);
    write_u16(pe, coff_offset, 0x014C);
    write_u16(pe, coff_offset + 2, 1);
    write_u16(pe, coff_offset + 16, optional_size);
    write_u16(pe, optional_offset, 0x010B);
    write_u32(pe, optional_offset + 92, 16);
    write_u32(pe, optional_offset + 0x70, 0x1000);
    write_u32(
        pe, optional_offset + 0x74,
        static_cast<u32>(resource_offset + resource.size()));

    std::memcpy(
        pe.data() + section_offset, ".rsrc", sizeof(".rsrc") - 1);
    write_u32(
        pe, section_offset + 8,
        static_cast<u32>(resource_offset + resource.size()));
    write_u32(pe, section_offset + 12, 0x1000);
    write_u32(pe, section_offset + 16, static_cast<u32>(raw_size));
    write_u32(pe, section_offset + 20, raw_offset);

    const usize root = raw_offset;
    write_u16(pe, root + 14, 1);
    write_u32(pe, root + 16, 16);
    write_u32(pe, root + 20, 0x80000020u);
    write_u16(pe, root + 0x20 + 14, 1);
    write_u32(pe, root + 0x20 + 16, 1);
    write_u32(pe, root + 0x20 + 20, 0x80000040u);
    write_u16(pe, root + 0x40 + 14, 1);
    write_u32(pe, root + 0x40 + 16, 1033);
    write_u32(pe, root + 0x40 + 20, 0x60);
    write_u32(pe, root + 0x60, 0x1000 + resource_offset);
    write_u32(
        pe, root + 0x64, static_cast<u32>(resource.size()));
    std::copy(
        resource.begin(), resource.end(),
        pe.begin() + static_cast<std::ptrdiff_t>(
                         raw_offset + resource_offset));
    return pe;
}

Bytes make_szdd_literals(std::span<const u8> plain) {
    Bytes compressed(14, 0);
    const std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    std::copy(signature.begin(), signature.end(), compressed.begin());
    compressed[8] = 'A';
    compressed[9] = '_';
    write_u32(compressed, 10, static_cast<u32>(plain.size()));
    for (usize offset = 0; offset < plain.size(); offset += 8) {
        const usize count = std::min<usize>(8, plain.size() - offset);
        compressed.push_back(
            static_cast<u8>((1u << count) - 1u));
        compressed.insert(
            compressed.end(), plain.begin() + static_cast<std::ptrdiff_t>(offset),
            plain.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    return compressed;
}

ExportEntry* find_export(DllExports& exports, std::string_view name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

} // namespace

// =============================================================================
// Test Fixture
// =============================================================================

class VersionTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vitreo-version-test-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        ASSERT_TRUE(std::filesystem::create_directories(root, ec));
        ASSERT_TRUE(vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(
                     root.string())).is_ok());
        mgr.set_vfs(&vfs);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void write_file(std::string_view name, const Bytes& bytes) {
        std::ofstream file(
            root / name, std::ios::binary | std::ios::trunc);
        file.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    VersionManager mgr;
    capsule::Vfs vfs;
    std::filesystem::path root;
};

// =============================================================================
// GetFileVersionInfoSize Tests
// =============================================================================

TEST_F(VersionTest, GetFileVersionInfoSizeW_ReturnsZero) {
    u32 handle = 0xDEAD;
    u32 size = mgr.get_file_version_info_size("kernel32.dll", handle);
    EXPECT_EQ(size, 0u);
}

// A file that does not exist reports the file-open error, not
// ERROR_RESOURCE_TYPE_NOT_FOUND (tools/oracle_probe/version_probe.expected
// size_missing_bare / size_missing_path: gle=2).
TEST_F(VersionTest, GetFileVersionInfoSizeW_MissingFileIsFileNotFound) {
    u32 handle = 0;
    const u32 size =
        mgr.get_file_version_info_size("kernel32.dll", handle);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(mgr.last_error(), vitreo::winapi::error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(VersionTest, GetFileVersionInfoSizeA_ReturnsZero) {
    u32 handle = 0xDEAD;
    u32 size = mgr.get_file_version_info_size("user32.dll", handle);
    EXPECT_EQ(size, 0u);
}

TEST_F(VersionTest, GetFileVersionInfoSizeW_HandleSetToZero) {
    u32 handle = 0xDEAD;
    const u32 size =
        mgr.get_file_version_info_size("kernel32.dll", handle);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(handle, 0u);
}

TEST_F(VersionTest, SyntheticSystemDllExposesConsistentFixedVersionInfo) {
    ThunkRegistry registry(0x70000000);
    ModuleManager modules(registry);
    modules.register_module(
        "kernel32.dll", ModuleManager::KERNEL32_BASE,
        "C:\\Windows\\System32\\kernel32.dll");
    mgr.set_module_manager(&modules);

    u32 handle = 0xDEADu;
    const u32 size = mgr.get_file_version_info_size(
        "C:\\Windows\\System32\\kernel32.dll", handle);
    ASSERT_GT(size, 52u);
    EXPECT_EQ(handle, 0u);

    Bytes resource(size);
    ASSERT_TRUE(mgr.get_file_version_info(
        "C:\\Windows\\System32\\kernel32.dll", 0, size,
        resource.data()));
    const auto fixed = mgr.ver_query_value(resource, "\\");
    ASSERT_TRUE(fixed.has_value());
    ASSERT_GE(resource.size(), fixed->value_offset + 52u);
    EXPECT_EQ(read_u32(resource, fixed->value_offset), 0xFEEF04BDu);
    EXPECT_EQ(
        read_u32(resource, fixed->value_offset + 8),
        (10u << 16));
    EXPECT_EQ(
        read_u32(resource, fixed->value_offset + 12),
        (19041u << 16) | 1u);
}

// A built-in nothing has loaded reports the version resource its System32
// file carries (the synthetic image embeds it), whatever case the query
// spells the name in: InternalName and OriginalFilename are the file's.
TEST_F(VersionTest, UnloadedBuiltInDllReportsItsFilesResourceInAnyCase) {
    ThunkRegistry registry(0x70000000);
    ModuleManager modules(registry);
    mgr.set_module_manager(&modules);
    mgr.set_builtin_dll_query([](std::string_view name) {
        return name == "dsound.dll" || name == "DSOUND.DLL";
    });
    const auto expected =
        vitreo::winapi::make_synthetic_system_version_resource("dsound.dll");
    for (const char* query : {"C:\\Windows\\System32\\dsound.dll",
                              "C:\\Windows\\System32\\DSOUND.DLL"}) {
        SCOPED_TRACE(query);
        u32 handle = 0xDEADu;
        const u32 size = mgr.get_file_version_info_size(query, handle);
        ASSERT_GE(size, expected.size());
        Bytes resource(size);
        ASSERT_TRUE(mgr.get_file_version_info(query, 0, size, resource.data()));
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                               resource.begin()));
    }
}

TEST_F(VersionTest, ExplicitSameNamedAppDllDoesNotUseSyntheticMetadata) {
    write_file("kernel32.dll", Bytes(64, 0));

    ThunkRegistry registry(0x70000000);
    ModuleManager modules(registry);
    modules.register_module(
        "kernel32.dll", ModuleManager::KERNEL32_BASE,
        "C:\\Windows\\System32\\kernel32.dll");
    mgr.set_module_manager(&modules);

    u32 handle = 0;
    EXPECT_EQ(
        mgr.get_file_version_info_size(
            "C:\\kernel32.dll", handle),
        0u);
    // version_probe.expected size_not_pe: gle=714 (ERROR_RESOURCE_DATA_NOT_FOUND)
    // for an on-disk file that is not a PE with a version resource — not
    // ERROR_FILE_NOT_FOUND, and not the synthetic System32 kernel32 metadata.
    EXPECT_EQ(mgr.last_error(),
              vitreo::winapi::error_codes::ERROR_RESOURCE_DATA_NOT_FOUND);
}

// =============================================================================
// GetFileVersionInfo Tests
// =============================================================================

TEST_F(VersionTest, GetFileVersionInfoW_ReturnsFalse) {
    u8 buf[64] = {};
    bool ok = mgr.get_file_version_info("kernel32.dll", 0, sizeof(buf), buf);
    EXPECT_FALSE(ok);
}

TEST_F(VersionTest, GetFileVersionInfoA_ReturnsFalse) {
    u8 buf[64] = {};
    bool ok = mgr.get_file_version_info("user32.dll", 0, sizeof(buf), buf);
    EXPECT_FALSE(ok);
}

// =============================================================================
// GetFileVersionInfoEx Tests
// =============================================================================

TEST_F(VersionTest, GetFileVersionInfoExW_ReturnsFalse) {
    u8 buf[64] = {};
    bool ok = mgr.get_file_version_info_ex(0, "kernel32.dll", 0,
                                             sizeof(buf), buf);
    EXPECT_FALSE(ok);
}

// =============================================================================
// VerQueryValue Tests
// =============================================================================

TEST_F(VersionTest, VerQueryValueW_ReturnsFalse) {
    EXPECT_FALSE(mgr.ver_query_value({}, "\\").has_value());
}

TEST_F(VersionTest, VerQueryValueA_ReturnsFalse) {
    EXPECT_FALSE(mgr.ver_query_value(
        {}, "\\StringFileInfo\\040904b0\\FileVersion").has_value());
}

TEST_F(VersionTest, VerQueryValueResolvesRootTranslationAndString) {
    const Bytes block = make_version_block();

    const auto root_query = mgr.ver_query_value(block, "\\");
    ASSERT_TRUE(root_query.has_value());
    EXPECT_EQ(root_query->length, 52u);
    ASSERT_LE(root_query->value_offset + 4, block.size());
    EXPECT_EQ(
        block[root_query->value_offset], 0xBDu);

    const auto translation = mgr.ver_query_value(
        block, "\\varfileinfo\\translation");
    ASSERT_TRUE(translation.has_value());
    EXPECT_EQ(translation->length, 4u);
    EXPECT_EQ(
        Bytes(
            block.begin() + translation->value_offset,
            block.begin() + translation->value_offset + 4),
        (Bytes{0x09, 0x04, 0xB0, 0x04}));

    const auto product = mgr.ver_query_value(
        block, "\\StringFileInfo\\040904B0\\ProductName");
    ASSERT_TRUE(product.has_value());
    EXPECT_EQ(
        product->length,
        std::strlen("Steam Client Service") + 1);
}

TEST_F(VersionTest, VerQueryValueRejectsMalformedFixedInfo) {
    Bytes block = make_version_block();
    const auto root_query = mgr.ver_query_value(block, "\\");
    ASSERT_TRUE(root_query.has_value());
    block[root_query->value_offset] ^= 0xFFu;
    EXPECT_FALSE(mgr.ver_query_value(
        block, "\\VarFileInfo\\Translation").has_value());
}

TEST_F(VersionTest, VerQueryValueWritesPointerAtX64GuestWidth) {
    const Bytes block = make_version_block();
    constexpr GuestAddr base = 0x1'0001'0000ull;
    constexpr GuestAddr block_address = base;
    constexpr GuestAddr query_address = base + 0x2000;
    constexpr GuestAddr value_out = base + 0x2100;
    constexpr GuestAddr length_out = base + 0x2110;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        block_address, block.data(), block.size()).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_wide_string(
        query_address, "\\VarFileInfo\\Translation").is_ok());
    ASSERT_TRUE(memory.write_u64(
        value_out, 0xCCCC'CCCC'CCCC'CCCCull).is_ok());

    cpu.gpr.rcx = block_address;
    cpu.gpr.rdx = query_address;
    cpu.gpr.r8 = value_out;
    cpu.gpr.r9 = length_out;
    auto exports = create_version_thunks(mgr);
    auto* query_value = find_export(exports, "VerQueryValueW");
    ASSERT_NE(query_value, nullptr);
    query_value->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u64 result_address =
        memory.read_u64(value_out).value_or(0);
    EXPECT_GT(result_address, 0xFFFF'FFFFull);
    EXPECT_GE(result_address, block_address);
    EXPECT_LT(
        result_address,
        block_address + static_cast<GuestAddr>(block.size()));
    EXPECT_EQ(memory.read_u32(length_out).value_or(0), 4u);
}

TEST_F(VersionTest, LargeResourceCopiesFullyAndThunkQueriesTranslation) {
    const Bytes resource = make_version_block(true);
    ASSERT_GT(resource.size(), 1024u);
    write_file(
        "SteamService.exe",
        make_pe_with_version_resource(resource));

    u32 ignored = 0;
    // s7_version_probe: a Unicode resource reports 2 * size + 4.
    ASSERT_EQ(
        mgr.get_file_version_info_size(
            "C:\\SteamService.exe", ignored),
        resource.size() * 2 + 4);

    GuestMemory memory;
    constexpr GuestAddr data_base = 0x10000;
    constexpr GuestAddr stack_base = 0x30000;
    ASSERT_TRUE(memory.map_region(
        data_base, 0x10000,
        MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack_base, 0x1000,
        MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack_base + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr filename = data_base;
    constexpr GuestAddr version_data = data_base + 0x1000;
    constexpr GuestAddr query = data_base + 0x8000;
    constexpr GuestAddr value_out = query + 0x100;
    constexpr GuestAddr length_out = query + 0x108;
    ASSERT_TRUE(marshaler.write_wide_string(
        filename, "C:\\SteamService.exe").is_ok());

    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(stack + 4, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 12, static_cast<u32>(resource.size())).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, version_data).is_ok());

    auto exports = create_version_thunks(mgr);
    auto* get_info = find_export(exports, "GetFileVersionInfoW");
    ASSERT_NE(get_info, nullptr);
    get_info->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    Bytes copied(resource.size());
    ASSERT_TRUE(memory.read_bytes(
        version_data, copied.data(), copied.size()).is_ok());
    EXPECT_EQ(copied, resource);

    ASSERT_TRUE(marshaler.write_wide_string(
        query, "\\VarFileInfo\\Translation").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, version_data).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, query).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, value_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, length_out).is_ok());
    auto* query_value = find_export(exports, "VerQueryValueW");
    ASSERT_NE(query_value, nullptr);
    query_value->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    const u32 result_address =
        memory.read_u32(value_out).value_or(0);
    EXPECT_GE(result_address, version_data);
    EXPECT_LT(
        result_address,
        version_data + static_cast<GuestAddr>(resource.size()));
    EXPECT_EQ(memory.read_u32(length_out).value_or(0), 4u);
    std::array<u8, 4> translation{};
    ASSERT_TRUE(memory.read_bytes(
        result_address, translation.data(),
        translation.size()).is_ok());
    EXPECT_EQ(
        translation,
        (std::array<u8, 4>{0x09, 0x04, 0xB0, 0x04}));
}

TEST_F(VersionTest, GetFileVersionInfoThunkRejectsUndersizedGuestBuffer) {
    const Bytes resource = make_version_block(true);
    write_file(
        "SteamService.exe",
        make_pe_with_version_resource(resource));

    GuestMemory memory;
    constexpr GuestAddr data_base = 0x10000;
    constexpr GuestAddr stack_base = 0x30000;
    ASSERT_TRUE(memory.map_region(
        data_base, 0x10000,
        MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack_base, 0x1000,
        MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack_base + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr filename = data_base;
    constexpr GuestAddr version_data = data_base + 0x1000;
    ASSERT_TRUE(marshaler.write_wide_string(
        filename, "C:\\SteamService.exe").is_ok());
    ASSERT_TRUE(memory.write_u8(version_data, 0xA5).is_ok());
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(stack + 4, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 12,
        static_cast<u32>(resource.size() - 1)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, version_data).is_ok());

    auto exports = create_version_thunks(mgr);
    auto* get_info = find_export(exports, "GetFileVersionInfoW");
    ASSERT_NE(get_info, nullptr);
    set_last_error(error_codes::ERROR_SUCCESS);
    get_info->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(
        get_last_error(),
        error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u8(version_data).value_or(0), 0xA5u);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(VersionTest, EmptyFilename_SetsInvalidParameter_Size) {
    u32 handle = 0;
    u32 size = mgr.get_file_version_info_size("", handle);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(mgr.last_error(), vitreo::winapi::error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(VersionTest, EmptyFilename_SetsInvalidParameter_Info) {
    bool ok = mgr.get_file_version_info("", 0, 0, nullptr);
    EXPECT_FALSE(ok);
    EXPECT_EQ(mgr.last_error(), vitreo::winapi::error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(VersionTest, EmptyFilename_SetsInvalidParameter_InfoEx) {
    bool ok = mgr.get_file_version_info_ex(0, "", 0, 0, nullptr);
    EXPECT_FALSE(ok);
    EXPECT_EQ(mgr.last_error(), vitreo::winapi::error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// VerInstallFile Tests
// =============================================================================

TEST_F(VersionTest, VerInstallFileExpandsSzddIntoDestination) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(root / "src", ec));
    ASSERT_TRUE(std::filesystem::create_directories(root / "dst", ec));
    const Bytes setup_dll{
        'M', 'Z', 0x90, 0x00, 'S', 'e', 't', 'u', 'p', '.', 'd', 'l', 'l'};
    write_file("src/setup.dl_", make_szdd_literals(setup_dll));

    std::string temporary_file;
    const u32 result = mgr.install_file(
        0, "setup.dl_", "Setup.dll", "C:\\src", "C:\\dst", "",
        temporary_file);

    EXPECT_EQ(result, 0u);
    EXPECT_TRUE(temporary_file.empty());
    std::ifstream installed(root / "dst" / "Setup.dll", std::ios::binary);
    const Bytes actual{
        std::istreambuf_iterator<char>(installed),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(actual, setup_dll);
}

TEST_F(VersionTest, VerInstallFileRejectsTruncatedSzdd) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(root / "src", ec));
    ASSERT_TRUE(std::filesystem::create_directories(root / "dst", ec));
    Bytes truncated{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33,
        'A', '_', 0x10, 0x00, 0x00, 0x00, 0xFF, 'M'};
    write_file("src/setup.dl_", truncated);

    std::string temporary_file;
    const u32 result = mgr.install_file(
        0, "setup.dl_", "Setup.dll", "C:\\src", "C:\\dst", "",
        temporary_file);

    EXPECT_EQ(
        result,
        version_install_flags::VIF_CANNOTLOADLZ32);
    EXPECT_FALSE(std::filesystem::exists(root / "dst" / "Setup.dll"));
}

TEST_F(VersionTest, VerInstallFilePreservesExistingDestinationAsMismatch) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(root / "src", ec));
    ASSERT_TRUE(std::filesystem::create_directories(root / "dst", ec));
    const Bytes old_bytes{'o', 'l', 'd'};
    const Bytes new_bytes{'n', 'e', 'w'};
    write_file("src/new.dll", new_bytes);
    write_file("dst/Setup.dll", old_bytes);

    std::string temporary_file;
    const u32 result = mgr.install_file(
        0, "new.dll", "Setup.dll", "C:\\src", "C:\\dst", "C:\\old",
        temporary_file);

    EXPECT_EQ(
        result,
        version_install_flags::VIF_TEMPFILE |
            version_install_flags::VIF_MISMATCH);
    EXPECT_FALSE(temporary_file.empty());
    std::ifstream original(root / "dst" / "Setup.dll", std::ios::binary);
    const Bytes actual_original{
        std::istreambuf_iterator<char>(original),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(actual_original, old_bytes);
    std::ifstream candidate(root / "dst" / temporary_file, std::ios::binary);
    const Bytes actual_candidate{
        std::istreambuf_iterator<char>(candidate),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(actual_candidate, new_bytes);
}

TEST_F(VersionTest, VerInstallFileForceReplacesExistingDestination) {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(root / "src", ec));
    ASSERT_TRUE(std::filesystem::create_directories(root / "dst", ec));
    const Bytes new_bytes{'n', 'e', 'w'};
    write_file("src/new.dll", new_bytes);
    write_file("dst/Setup.dll", Bytes{'o', 'l', 'd'});

    std::string temporary_file;
    const u32 result = mgr.install_file(
        version_install_flags::VIFF_FORCEINSTALL,
        "new.dll", "Setup.dll", "C:\\src", "C:\\dst", "C:\\old",
        temporary_file);

    EXPECT_EQ(result, 0u);
    std::ifstream installed(root / "dst" / "Setup.dll", std::ios::binary);
    const Bytes actual{
        std::istreambuf_iterator<char>(installed),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(actual, new_bytes);
}

// =============================================================================
// Thunk Registration Tests
// =============================================================================

// Ordinals from the Windows 11 version.dll export table (System32 and
// SysWOW64 agree); stdcall sizes from winver.h prototypes. The table used to
// carry invented ordinals 1-11, so an import by ordinal bound the wrong
// function (e.g. #1 was GetFileVersionInfoSizeW, Windows' GetFileVersionInfoA).
TEST_F(VersionTest, ExportsUseWindowsOrdinalsAndStdcallSizes) {
    struct Row { const char* name; u16 ordinal; u16 bytes; };
    const Row rows[] = {
        {"GetFileVersionInfoA", 1, 16},       {"GetFileVersionInfoExW", 4, 20},
        {"GetFileVersionInfoSizeA", 5, 8},    {"GetFileVersionInfoSizeExW", 7, 12},
        {"GetFileVersionInfoSizeW", 8, 8},    {"GetFileVersionInfoW", 9, 16},
        {"VerInstallFileA", 12, 32},          {"VerLanguageNameA", 14, 12},
        {"VerLanguageNameW", 15, 12},         {"VerQueryValueA", 16, 16},
        {"VerQueryValueW", 17, 16},
    };
    DllExports exports = create_version_thunks(mgr);
    for (const Row& row : rows) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == row.name; });
        ASSERT_NE(it, exports.entries.end()) << row.name;
        EXPECT_EQ(it->ordinal, row.ordinal) << row.name;
        EXPECT_EQ(it->stdcall_bytes, row.bytes) << row.name;
    }
}

TEST_F(VersionTest, ThunksContainGetFileVersionInfoSizeW) {
    DllExports exports = create_version_thunks(mgr);
    bool found = false;
    for (const auto& e : exports.entries) {
        if (e.name == "GetFileVersionInfoSizeW") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(VersionTest, GetFileVersionInfoExWUsesFiveArgumentStackSize) {
    DllExports exports = create_version_thunks(mgr);
    const auto* entry =
        find_export(exports, "GetFileVersionInfoExW");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 20u);
}

TEST_F(VersionTest, VerInstallFileAUsesEightArgumentStackSize) {
    DllExports exports = create_version_thunks(mgr);
    const auto* entry = find_export(exports, "VerInstallFileA");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 32u);
}

// S7 audit: steamclient / Steam2.dll / SDL3 read version strings through the
// ANSI API. Measured (tools/oracle_probe/s7_version_probe.expected, x86 = x64):
// GetFileVersionInfoSize reports 2 * wLength + 4, GetFileVersionInfo copies
// the resource and appends "FE2X", and VerQueryValueA converts a text value
// into the area behind the resource at the same offset (+ wLength + 4) with
// *puLen = ANSI bytes including the NUL. A miss is FALSE +
// ERROR_RESOURCE_TYPE_NOT_FOUND with *puLen = 0.
TEST_F(VersionTest, S7AnsiQueryConvertsTextBehindTheResource) {
    const Bytes resource = make_version_block();
    write_file("Probe.exe", make_pe_with_version_resource(resource));
    u32 ignored = 0;
    const u32 reported = mgr.get_file_version_info_size("C:\\Probe.exe", ignored);
    ASSERT_EQ(reported, resource.size() * 2 + 4);

    GuestMemory memory;
    constexpr GuestAddr data_base = 0x10000;
    constexpr GuestAddr stack_base = 0x30000;
    ASSERT_TRUE(memory.map_region(data_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(stack_base, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack_base + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr filename = data_base;
    constexpr GuestAddr info = data_base + 0x1000;
    constexpr GuestAddr query = data_base + 0x8000;
    constexpr GuestAddr value_out = query + 0x100;
    constexpr GuestAddr length_out = query + 0x108;
    const std::string name = "C:\\Probe.exe";
    for (usize i = 0; i <= name.size(); ++i) {
        ASSERT_TRUE(memory.write_u8(filename + i, i < name.size() ? name[i] : 0).is_ok());
    }
    for (u32 i = 0; i < reported; ++i) ASSERT_TRUE(memory.write_u8(info + i, 0xCC).is_ok());

    auto exports = create_version_thunks(mgr);
    const GuestAddr stack = cpu.sp();
    ASSERT_TRUE(memory.write_u32(stack + 4, filename).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, reported).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, info).is_ok());
    find_export(exports, "GetFileVersionInfoA")->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 w_length = static_cast<u32>(resource.size());
    EXPECT_EQ(memory.read_u32(info + w_length).value_or(0), 0x58324546u);  // "FE2X"
    EXPECT_EQ(memory.read_u8(info + w_length + 4).value_or(0), 0xCCu);

    const auto query_a = [&](const char* path) {
        for (usize i = 0; i <= std::strlen(path); ++i) {
            EXPECT_TRUE(memory.write_u8(query + i, static_cast<u8>(path[i])).is_ok());
        }
        EXPECT_TRUE(memory.write_u32(length_out, 0x77).is_ok());
        EXPECT_TRUE(memory.write_u32(stack + 4, info).is_ok());
        EXPECT_TRUE(memory.write_u32(stack + 8, query).is_ok());
        EXPECT_TRUE(memory.write_u32(stack + 12, value_out).is_ok());
        EXPECT_TRUE(memory.write_u32(stack + 16, length_out).is_ok());
        find_export(exports, "VerQueryValueA")->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto w_value = mgr.ver_query_value(
        resource, "\\StringFileInfo\\040904B0\\ProductName");
    ASSERT_TRUE(w_value.has_value());
    ASSERT_EQ(query_a("\\StringFileInfo\\040904B0\\ProductName"), 1u);
    const u32 ansi = memory.read_u32(value_out).value_or(0);
    EXPECT_EQ(ansi, info + w_length + 4 + w_value->value_offset);
    EXPECT_EQ(memory.read_u32(length_out).value_or(0), 21u);  // 20 chars + NUL
    EXPECT_EQ(marshaler.read_ansi_string(ansi).value_or(""), "Steam Client Service");

    // Binary values are returned in place.
    ASSERT_EQ(query_a("\\VarFileInfo\\Translation"), 1u);
    EXPECT_LT(memory.read_u32(value_out).value_or(0), info + w_length);
    EXPECT_EQ(memory.read_u32(length_out).value_or(0), 4u);

    set_last_error(0xDEAD);
    EXPECT_EQ(query_a("\\StringFileInfo\\040904B0\\NoSuchValue"), 0u);
    EXPECT_EQ(memory.read_u32(length_out).value_or(1), 0u);
    EXPECT_EQ(get_last_error(), 1813u);
}
