/// @file test_disk_free_space.cpp
/// @brief GetDiskFreeSpaceA/W contract, measured on Windows by
///        tools/oracle_probe/disk_free_probe.c (disk_free_probe.expected;
///        x86 and x64 identical).
///
/// Regression: a host volume's VolumeInfo carried a filesystem name but no
/// geometry, so GetDiskFreeSpaceA reported 0 sectors, 0 bytes and 0 clusters
/// for every capsule drive, and StarCraft refused to start with "Available
/// free space on the C: drive: 0".

#include <gtest/gtest.h>

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/core/marshaler.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/statvfs.h>

#include "platform/host_volume_space.hpp"
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::capsule;
using namespace vitreo::winapi;

namespace {

constexpr u32 kSentinel = 0xA5A5A5A5u;

struct Outputs {
    u64 ret = 0;
    u32 error = 0;
    u32 spc = 0;
    u32 bps = 0;
    u32 free = 0;
    u32 total = 0;
};

class DiskFreeSpaceTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kData = 0x1000;
    static constexpr GuestAddr kOut = 0x1800;
    static constexpr GuestAddr kStack = 0x80000;

    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vitreo_disk_free_" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "Folder");
        std::ofstream(root / "Folder" / "data.bin", std::ios::binary)
            .write("x", 1);
        ASSERT_TRUE(vfs.mount_drive(
            'C', std::make_unique<HostFileSystem>(root.string())).is_ok());
        ASSERT_TRUE(memory.map_region(kData, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = kStack + 0x800;
        marshaler.attach_cpu(cpu);
        manager.set_current_directory("C:\\Folder");
    }

    void TearDown() override { std::filesystem::remove_all(root); }

    /// Call with lpRootPathName = @p path_addr (0 = NULL) and the four
    /// outputs at kOut (or NULL where @p out_mask's bit is clear).
    Outputs call(bool wide, GuestAddr path_addr, u32 out_mask = 0xF) {
        for (u32 i = 0; i < 4; ++i) {
            EXPECT_TRUE(memory.write_u32(kOut + 4 * i, kSentinel).is_ok());
        }
        EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4,
                                     static_cast<u32>(path_addr)).is_ok());
        for (u32 i = 0; i < 4; ++i) {
            const u32 ptr = (out_mask >> i) & 1u ? static_cast<u32>(kOut + 4 * i) : 0u;
            EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 8 + 4 * i, ptr).is_ok());
        }
        set_last_error(0xDEAD);
        if (wide) {
            manager.impl_GetDiskFreeSpaceW(marshaler);
        } else {
            manager.impl_GetDiskFreeSpaceA(marshaler);
        }
        Outputs o;
        o.ret = cpu.gpr.rax;
        o.error = get_last_error();
        o.spc = memory.read_u32(kOut).value();
        o.bps = memory.read_u32(kOut + 4).value();
        o.free = memory.read_u32(kOut + 8).value();
        o.total = memory.read_u32(kOut + 12).value();
        return o;
    }

    Outputs call_a(const char* path) {
        if (path == nullptr) return call(false, 0);
        EXPECT_TRUE(marshaler.write_ansi_string(kData, path).is_ok());
        return call(false, kData);
    }

    Outputs call_w(const char* path) {
        if (path == nullptr) return call(true, 0);
        EXPECT_TRUE(marshaler.write_wide_string(kData, path).is_ok());
        return call(true, kData);
    }

    /// The host volume holding the test root, in GetDiskFreeSpace's units:
    /// NTFS reports 512-byte sectors and a cluster of the allocation unit.
    struct statvfs host() const {
        struct statvfs st {};
        EXPECT_EQ(::statvfs(root.c_str(), &st), 0);
        return st;
    }

    /// disk_free_probe: success leaves the last error and reports the
    /// volume's real geometry; free <= total, no 2 GB cap.
    void expect_success(const Outputs& o) {
        const struct statvfs st = host();
        EXPECT_EQ(o.ret, 1u);
        EXPECT_EQ(o.error, 0xDEADu);
        EXPECT_EQ(o.bps, 512u);
        EXPECT_EQ(o.spc, static_cast<u32>(st.f_frsize / 512));
        EXPECT_NE(o.total, 0u);
        EXPECT_LE(o.free, o.total);
        EXPECT_EQ(o.total, static_cast<u32>(std::min<u64>(st.f_blocks, 0xFFFFFFFFull)));
    }

    /// disk_free_probe: these failures write 0 to *lpBytesPerSector only.
    void expect_failure(const Outputs& o, u32 error) {
        EXPECT_EQ(o.ret, 0u);
        EXPECT_EQ(o.error, error);
        EXPECT_EQ(o.bps, 0u);
        EXPECT_EQ(o.spc, kSentinel);
        EXPECT_EQ(o.free, kSentinel);
        EXPECT_EQ(o.total, kSentinel);
    }

    std::filesystem::path root;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    Vfs vfs;
    Kernel32ExtManager manager{memory, vfs};
};

TEST(HostVolumeInfoTest, ReportsTheHostVolumesGeometryAndSpace) {
    const std::string dir = std::filesystem::temp_directory_path().string();
    HostFileSystem fs(dir);
    VolumeInfo info;
    ASSERT_TRUE(fs.volume_info(info));
    struct statvfs st {};
    ASSERT_EQ(::statvfs(dir.c_str(), &st), 0);
    EXPECT_EQ(info.filesystem_name, "NTFS");
    EXPECT_EQ(static_cast<u64>(info.bytes_per_sector) * info.sectors_per_cluster,
              static_cast<u64>(st.f_frsize));
    EXPECT_EQ(info.total_bytes, static_cast<u64>(st.f_blocks) * st.f_frsize);
    EXPECT_EQ(info.total_clusters,
              static_cast<u32>(std::min<u64>(st.f_blocks, 0xFFFFFFFFull)));
    EXPECT_LE(info.available_bytes, info.free_bytes);
    EXPECT_LE(info.free_bytes, info.total_bytes);
}

TEST_F(DiskFreeSpaceTest, RootReportsTheVolumesRealGeometry) {
    const Outputs o = call_a("C:\\");
    expect_success(o);
    // The value StarCraft computes: spc/1024 * bps/1024 * free, in MB.
    // Free is what the caller can write: unallocated blocks plus the
    // purgeable space macOS reclaims on demand (the figure Finder shows).
    platform::HostVolumeSpace space;
    ASSERT_TRUE(platform::host_volume_space(root.c_str(), space));
    const double free_mb = (o.spc / 1024.0) * (o.bps / 1024.0) * o.free;
    const double host_mb = static_cast<double>(space.available_bytes) / (1024.0 * 1024.0);
    EXPECT_NEAR(free_mb, host_mb, host_mb * 0.01 + 1.0);
    // A_root total_bytes_gt_2g: the counts are not capped at 2 GB.
    EXPECT_GT(static_cast<u64>(o.total) * o.spc * o.bps, 0x80000000ull);
}

TEST_F(DiskFreeSpaceTest, NullIsTheCurrentDrivesRoot) {
    expect_success(call_a(nullptr));
    expect_success(call_w(nullptr));
}

TEST_F(DiskFreeSpaceTest, AcceptsDirectoriesAndRootsWithoutTheTrailingBackslash) {
    // A_drive_noslash, A_dir_slash, A_dir_noslash, A_forward_slash, A_lowercase.
    for (const char* path : {"C:", "C:\\Folder\\", "C:\\Folder", "C:/", "c:\\"}) {
        SCOPED_TRACE(path);
        expect_success(call_a(path));
    }
}

TEST_F(DiskFreeSpaceTest, MissingDirectoryFailsFileNotFound) {
    expect_failure(call_a("C:\\Folder\\nodir\\"), error_codes::ERROR_FILE_NOT_FOUND);
    expect_failure(call_w("C:\\Folder\\nodir\\"), error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(DiskFreeSpaceTest, FileFailsErrorDirectory) {
    expect_failure(call_a("C:\\Folder\\data.bin"), error_codes::ERROR_DIRECTORY);
    expect_failure(call_w("C:\\Folder\\data.bin"), error_codes::ERROR_DIRECTORY);
}

TEST_F(DiskFreeSpaceTest, UnmountedDriveFailsPathNotFound) {
    expect_failure(call_a("Q:\\"), error_codes::ERROR_PATH_NOT_FOUND);
    expect_failure(call_w("Q:\\"), error_codes::ERROR_PATH_NOT_FOUND);
}

TEST_F(DiskFreeSpaceTest, EmptyPathFailsWithoutTouchingAnyOutput) {
    const Outputs o = call_a("");
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.error, error_codes::ERROR_PATH_NOT_FOUND);
    EXPECT_EQ(o.spc, kSentinel);
    EXPECT_EQ(o.bps, kSentinel);
    EXPECT_EQ(o.free, kSentinel);
    EXPECT_EQ(o.total, kSentinel);
}

TEST_F(DiskFreeSpaceTest, OutputsMayBeNull) {
    ASSERT_TRUE(marshaler.write_ansi_string(kData, "C:\\").is_ok());
    // A_all_null_out.
    const Outputs none = call(false, kData, 0x0);
    EXPECT_EQ(none.ret, 1u);
    EXPECT_EQ(none.error, 0xDEADu);
    // A_only_free_out: only lpNumberOfFreeClusters is written.
    const Outputs free_only = call(false, kData, 0x4);
    EXPECT_EQ(free_only.ret, 1u);
    EXPECT_NE(free_only.free, kSentinel);
    EXPECT_EQ(free_only.spc, kSentinel);
    EXPECT_EQ(free_only.bps, kSentinel);
    EXPECT_EQ(free_only.total, kSentinel);
}

TEST_F(DiskFreeSpaceTest, WideAndAnsiDescribeTheSameVolume) {
    const Outputs a = call_a("C:\\");
    const Outputs w = call_w("C:\\");
    expect_success(w);
    EXPECT_EQ(w.spc, a.spc);
    EXPECT_EQ(w.bps, a.bps);
    EXPECT_EQ(w.total, a.total);
}

TEST_F(DiskFreeSpaceTest, AgreesWithGetDiskFreeSpaceExW) {
    // A_vs_ExW: total bytes equal; free clusters x cluster within 1% of the
    // caller-available bytes.
    const Outputs a = call_a("C:\\");
    ASSERT_EQ(a.ret, 1u);
    ASSERT_TRUE(marshaler.write_wide_string(kData, "C:\\").is_ok());
    const GuestAddr avail = kOut + 0x40, total = kOut + 0x48, tfree = kOut + 0x50;
    EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, static_cast<u32>(kData)).is_ok());
    EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, static_cast<u32>(avail)).is_ok());
    EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, static_cast<u32>(total)).is_ok());
    EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, static_cast<u32>(tfree)).is_ok());
    manager.impl_GetDiskFreeSpaceExW(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    auto read64 = [&](GuestAddr p) {
        return static_cast<u64>(memory.read_u32(p).value()) |
               (static_cast<u64>(memory.read_u32(p + 4).value()) << 32);
    };
    const u64 cluster = static_cast<u64>(a.spc) * a.bps;
    EXPECT_EQ(static_cast<u64>(a.total) * cluster, read64(total));
    const u64 a_free = static_cast<u64>(a.free) * cluster;
    const u64 ex_avail = read64(avail);
    const u64 diff = a_free > ex_avail ? a_free - ex_avail : ex_avail - a_free;
    EXPECT_LE(diff * 100, ex_avail);
    EXPECT_LE(ex_avail, read64(tfree));
}

}  // namespace
