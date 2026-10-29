/// @file test_file_information.cpp
/// @brief GetFileInformationByHandle contract regressions.

#include <gtest/gtest.h>

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/core/marshaler.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::capsule;
using namespace vitreo::winapi;

namespace {

class FileInformationTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kData = 0x1000;
    static constexpr GuestAddr kStack = 0x80000;

    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vitreo_file_info_" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "Folder");
        std::ofstream(root / "Folder" / "data.bin", std::ios::binary)
            .write("metadata", 8);

        ASSERT_TRUE(vfs.mount_drive(
            'C', std::make_unique<HostFileSystem>(root.string())).is_ok());
        ASSERT_TRUE(memory.map_region(kData, 0x1000,
                                     MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x1000,
                                     MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = kStack + 0x800;
        marshaler.attach_cpu(cpu);
        files = std::make_unique<FileManager>(vfs, handles, memory);
        manager.set_file_manager(files.get());
    }

    void TearDown() override {
        files.reset();
        std::filesystem::remove_all(root);
    }

    bool query(Handle handle, GuestAddr output = kData) {
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, handle).is_ok());
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 8,
                                     static_cast<u32>(output)).is_ok());
        manager.impl_GetFileInformationByHandle(marshaler);
        return cpu.gpr.rax != 0;
    }

    bool get_file_time(Handle handle, GuestAddr creation, GuestAddr access,
                       GuestAddr write) {
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, handle).is_ok());
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 8,
                                     static_cast<u32>(creation)).is_ok());
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 12,
                                     static_cast<u32>(access)).is_ok());
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 16,
                                     static_cast<u32>(write)).is_ok());
        manager.impl_GetFileTime(marshaler);
        return cpu.gpr.rax != 0;
    }

    /// GetFileInformationByHandle's last-write FILETIME, for cross-checking.
    u64 file_information_write_time(Handle handle) {
        EXPECT_TRUE(query(handle, kData + 0x200));
        return read_u64(kData + 0x200 + 20);
    }

    u64 read_u64(GuestAddr address) {
        return static_cast<u64>(memory.read_u32(address).value()) |
            (static_cast<u64>(memory.read_u32(address + 4).value()) << 32);
    }

    u64 read_high_low(GuestAddr address) {
        return (static_cast<u64>(memory.read_u32(address).value()) << 32) |
            static_cast<u64>(memory.read_u32(address + 4).value());
    }

    std::filesystem::path root;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    Vfs vfs;
    HandleTable handles;
    std::unique_ptr<FileManager> files;
    Kernel32ExtManager manager{memory, vfs};
};

TEST_F(FileInformationTest, ReturnsRealDirectoryMetadata) {
    // A directory handle needs FILE_FLAG_BACKUP_SEMANTICS on Windows, and
    // create_file enforces that contract.
    const Handle handle = files->create_file(
        "C:\\Folder", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS);
    ASSERT_NE(handle, INVALID_HANDLE);
    ASSERT_TRUE(query(handle));

    EXPECT_NE(memory.read_u32(kData).value() & 0x10u, 0u);
    EXPECT_NE(read_u64(kData + 4), 0u);
    EXPECT_NE(read_u64(kData + 44), 0u);
}

TEST_F(FileInformationTest, IdentityIsStableAcrossOpenHandles) {
    const Handle first = files->create_file(
        "C:\\Folder\\data.bin", GENERIC_READ, FILE_SHARE_READ,
        OPEN_EXISTING, 0);
    const Handle second = files->create_file(
        "C:\\Folder\\data.bin", GENERIC_READ, FILE_SHARE_READ,
        OPEN_EXISTING, 0);
    ASSERT_NE(first, INVALID_HANDLE);
    ASSERT_NE(second, INVALID_HANDLE);

    ASSERT_TRUE(query(first, kData));
    const u32 first_volume = memory.read_u32(kData + 28).value();
    const u64 first_id = read_high_low(kData + 44);
    const u64 first_size = read_high_low(kData + 32);

    ASSERT_TRUE(query(second, kData + 0x100));
    EXPECT_EQ(memory.read_u32(kData + 0x100 + 28).value(), first_volume);
    EXPECT_EQ(read_high_low(kData + 0x100 + 44), first_id);
    EXPECT_EQ(read_high_low(kData + 0x100 + 32), first_size);
    EXPECT_EQ(first_size, 8u);
    EXPECT_NE(first_id, 0u);
}

TEST_F(FileInformationTest, InvalidHandleFailsHonestly) {
    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(query(0xdeadbeefu));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FileInformationTest, GetFileTimeStoresRealTimestamps) {
    // Learn (GetFileTime): each non-NULL pointer receives that time as a
    // FILETIME (100 ns intervals since 1601-01-01 UTC). Returning TRUE while
    // storing nothing leaves the caller reading a zero FILETIME, which no real
    // file has — MSVC's time conversion then fails with ERROR_INVALID_DATA.
    const Handle handle = files->create_file(
        "C:\\Folder\\data.bin", GENERIC_READ, FILE_SHARE_READ,
        OPEN_EXISTING, 0);
    ASSERT_NE(handle, INVALID_HANDLE);
    ASSERT_TRUE(get_file_time(handle, kData, kData + 8, kData + 16));

    EXPECT_NE(read_u64(kData), 0u);
    EXPECT_NE(read_u64(kData + 8), 0u);
    EXPECT_NE(read_u64(kData + 16), 0u);
    EXPECT_EQ(read_u64(kData + 16), file_information_write_time(handle));
}

TEST_F(FileInformationTest, GetFileTimeHonorsNullPointers) {
    // Learn: "This parameter can be NULL if the application does not require
    // this information." A NULL slot must be left untouched, not written.
    const Handle handle = files->create_file(
        "C:\\Folder\\data.bin", GENERIC_READ, FILE_SHARE_READ,
        OPEN_EXISTING, 0);
    ASSERT_NE(handle, INVALID_HANDLE);
    ASSERT_TRUE(memory.write_u32(kData, 0xa5a5a5a5u).is_ok());
    ASSERT_TRUE(memory.write_u32(kData + 4, 0xa5a5a5a5u).is_ok());

    ASSERT_TRUE(get_file_time(handle, 0, 0, kData + 16));

    EXPECT_EQ(read_u64(kData), 0xa5a5a5a5a5a5a5a5ull);
    EXPECT_NE(read_u64(kData + 16), 0u);
}

TEST_F(FileInformationTest, GetFileTimeRejectsUnknownHandle) {
    set_last_error(error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(get_file_time(0xdeadbeefu, kData, kData + 8, kData + 16));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FileInformationTest, NullOutputFailsHonestly) {
    const Handle handle = files->create_file(
        "C:\\Folder\\data.bin", GENERIC_READ, FILE_SHARE_READ,
        OPEN_EXISTING, 0);
    ASSERT_NE(handle, INVALID_HANDLE);
    EXPECT_FALSE(query(handle, 0));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

} // namespace
