// Disk-space source/namespace contract: Learn GetDiskFreeSpace{Ex} A/W;
// Windows status/output behavior: disk_free_probe.expected, files_ns_probe.expected.
#include <gtest/gtest.h>
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <tuple>
#include <unistd.h>
using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;
namespace {
class CapacityFileSystem : public HostFileSystem {
public:
    using HostFileSystem::HostFileSystem;
    bool unavailable{}, missing_geometry{}, optical{}, real{}, fail_stat{};
    bool is_optical() const noexcept override { return optical; }
    Result<FileInfo, VfsError> stat(std::string_view path) override {
        if (fail_stat) return Result<FileInfo, VfsError>::err(VfsError::AccessDenied);
        return HostFileSystem::stat(path);
    }
    bool volume_info(VolumeInfo& info) const noexcept override {
        if (unavailable) return false;
        if (real) return HostFileSystem::volume_info(info);
        // Independent controlled provider inputs, not alleged Windows constants.
        info = VolumeInfo{};
        info.total_bytes = 17ull << 30; info.free_bytes = 9ull << 30; info.available_bytes = 2ull << 30;
        if (!missing_geometry) {
            info.bytes_per_sector = 512; info.sectors_per_cluster = 8;
            info.total_clusters = static_cast<u32>(info.total_bytes / 4096);
            info.free_clusters = static_cast<u32>(info.available_bytes / 4096);
        }
        return true;
    }
};
class DiskSpaceContract : public ::testing::TestWithParam<std::tuple<bool, bool>> {
protected:
    GuestMemory memory;
    Vfs vfs;
    Kernel32ExtManager manager{memory, vfs};
    std::filesystem::path root;
    CapacityFileSystem* backend{};
    bool is64() const { return std::get<0>(GetParam()); }
    bool wide() const { return std::get<1>(GetParam()); }
    static constexpr u64 sentinel = 0xa5a5a5a5a5a5a5a5ull;
    void SetUp() override {
        char pattern[] = "/tmp/vitreo-disk-capacity-XXXXXX";
        const auto temp = ::mkdtemp(pattern); ASSERT_NE(temp, nullptr); root = temp;
        std::filesystem::create_directories(root / "Folder");
        std::ofstream(root / "Folder" / "data.bin").put('x');
        auto mount = std::make_unique<CapacityFileSystem>(root.string()); backend = mount.get();
        ASSERT_TRUE(vfs.mount_drive('C', std::move(mount)).is_ok());
        ASSERT_TRUE(memory.map_region(0x10000, 0x6000, MemoryProtection::ReadWrite).is_ok());
        manager.set_current_directory("C:\\Folder");
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    bool call(bool extended, const char* path, unsigned output_mask = 15) {
        Marshaler marshaler(memory);
        if (path) {
            if (wide()) EXPECT_TRUE(marshaler.write_wide_string(0x11000, path).is_ok());
            else EXPECT_TRUE(marshaler.write_ansi_string(0x11000, path).is_ok());
        }
        std::array<u64, 5> args{path ? 0x11000u : 0u};
        for (usize i = 1; i < args.size(); ++i) {
            (void)memory.write_u64(0x12000 + (i - 1) * 16, sentinel);
            args[i] = output_mask & (1u << (i - 1)) ? 0x12000 + (i - 1) * 16 : 0;
        }
        cpu::CpuState state;
        const usize count = extended ? 4 : 5;
        if (is64()) {
            state.init_long_mode(); state.gpr.rsp = 0x14000;
            const std::array<u64*, 4> registers{&state.gpr.rcx, &state.gpr.rdx, &state.gpr.r8, &state.gpr.r9};
            for (usize i = 0; i < count; ++i) {
                if (i < 4) *registers[i] = args[i];
                else (void)memory.write_u64(0x14000 + (i + 1) * 8, args[i]);
            }
        } else {
            state.init_protected_mode(); state.set_sp(0x14000);
            for (usize i = 0; i < count; ++i) (void)memory.write_u32(0x14000 + (i + 1) * 4, static_cast<u32>(args[i]));
        }
        marshaler.attach_cpu(state); set_last_error(0xdead);
        if (extended) {
            if (wide()) manager.impl_GetDiskFreeSpaceExW(marshaler);
            else manager.impl_GetDiskFreeSpaceExA(marshaler);
        } else {
            if (wide()) manager.impl_GetDiskFreeSpaceW(marshaler);
            else manager.impl_GetDiskFreeSpaceA(marshaler);
        }
        return state.gpr.rax != 0;
    }
    u64 out(usize i) { return memory.read_u64(0x12000 + i * 16).value(); }
};
TEST_P(DiskSpaceContract, CallerAvailableAndAllFreeBytesRemainDistinctAnd64Bit) {
    ASSERT_TRUE(call(true, "C:\\Folder")); EXPECT_EQ(get_last_error(), 0xdeadu);
    EXPECT_EQ(out(0), 2ull << 30); EXPECT_EQ(out(1), 17ull << 30); EXPECT_EQ(out(2), 9ull << 30);
    ASSERT_TRUE(call(true, nullptr)); EXPECT_EQ(out(1), 17ull << 30);
    ASSERT_TRUE(call(true, "C:\\", 0));
    for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
    ASSERT_TRUE(call(false, "C:\\Folder")); EXPECT_EQ(get_last_error(), 0xdeadu);
    EXPECT_EQ(static_cast<u32>(out(0)), 8u); EXPECT_EQ(static_cast<u32>(out(1)), 512u);
    EXPECT_EQ(static_cast<u32>(out(2)), (2ull << 30) / 4096);
    EXPECT_EQ(static_cast<u32>(out(3)), (17ull << 30) / 4096);
    for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i) >> 32, sentinel >> 32);
}
TEST_P(DiskSpaceContract, InvalidDirectoryNeverReturnsUnrelatedHostCapacity) {
    for (const auto& [path, error] : {std::pair{"Q:\\", error_codes::ERROR_PATH_NOT_FOUND},
        std::pair{"C:\\Folder\\missing", error_codes::ERROR_PATH_NOT_FOUND},
        std::pair{"C:\\Folder\\data.bin", error_codes::ERROR_DIRECTORY}}) {
        SCOPED_TRACE(path); EXPECT_FALSE(call(true, path)); EXPECT_EQ(get_last_error(), error);
        for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
    }
    EXPECT_FALSE(call(false, "C:\\Folder\\missing")); EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(static_cast<u32>(out(1)), 0u);
    EXPECT_EQ(out(0), sentinel); EXPECT_EQ(out(2), sentinel); EXPECT_EQ(out(3), sentinel);
    EXPECT_FALSE(call(false, "")); EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
    for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
}
TEST_P(DiskSpaceContract, UnsupportedMetadataFailsLoudlyWithoutFabricatedStatistics) {
    backend->unavailable = true;
    EXPECT_FALSE(call(true, "C:\\")); EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
    EXPECT_FALSE(call(false, "C:\\")); EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_EQ(static_cast<u32>(out(1)), 0u);
    backend->unavailable = false; backend->missing_geometry = true;
    EXPECT_FALSE(call(false, "C:\\")); EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    // Exact byte counts can still satisfy Ex; unavailable geometry is not fabricated.
    EXPECT_TRUE(call(true, "C:\\")); EXPECT_EQ(out(1), 17ull << 30);
}
TEST_P(DiskSpaceContract, StatFailurePreservesOutputsAndReportsUnderlyingFailure) {
    backend->fail_stat = true;
    EXPECT_FALSE(call(true, "C:\\")); EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
}
TEST_P(DiskSpaceContract, OpticalMediaAlwaysHasZeroCallerAvailableAndFreeBytes) {
    backend->optical = true;
    ASSERT_TRUE(call(true, "C:\\")); EXPECT_EQ(out(0), 0u); EXPECT_EQ(out(2), 0u); EXPECT_EQ(out(1), 17ull << 30);
    ASSERT_TRUE(call(false, "C:\\")); EXPECT_EQ(static_cast<u32>(out(2)), 0u);
}
TEST_P(DiskSpaceContract, RealHostFilesystemReportsItsMountedVolume) {
    backend->real = true; VolumeInfo measured; ASSERT_TRUE(backend->volume_info(measured));
    ASSERT_NE(measured.total_bytes, 0u);
    ASSERT_TRUE(call(true, "C:\\Folder")); EXPECT_EQ(out(1), measured.total_bytes);
    // Host free space can change concurrently; only total capacity is invariant.
    EXPECT_LE(out(0), out(1)); EXPECT_LE(out(2), out(1));
    ASSERT_TRUE(call(false, "C:\\")); EXPECT_EQ(static_cast<u32>(out(0)), measured.sectors_per_cluster);
    EXPECT_EQ(static_cast<u32>(out(1)), measured.bytes_per_sector);
}

// Expected status/output phases are the Windows x86/x64 namespace probe rows,
// not results obtained from this resolver. The controlled provider above only
// supplies independent capacity data.
TEST_P(DiskSpaceContract, DosNamespacesPreserveLiteralNamesAndDriveRelativeCwd) {
    std::filesystem::create_directories(root / "Folder" / "Child");
    ASSERT_TRUE(vfs.create_dir(R"(\\?\C:\Trailing.)").is_ok());
    ASSERT_TRUE(vfs.create_dir(R"(\\?\C:\Spacing )").is_ok());
    EXPECT_TRUE(std::filesystem::is_directory(root / "Trailing."));
    EXPECT_TRUE(std::filesystem::is_directory(root / "Spacing "));
    for (const char* path : {R"(C:Child)", R"(\\?\C:\Folder)", R"(\\.\C:\Folder)",
        R"(\??\C:\Folder)", R"(\\?\C:\Trailing.)", R"(\\?\C:\Spacing )",
        R"(\\?\C:\\Folder)"}) {
        SCOPED_TRACE(path);
        for (bool ex : {false, true}) { ASSERT_TRUE(call(ex, path)); EXPECT_EQ(get_last_error(), 0xdeadu); }
    }
}
TEST_P(DiskSpaceContract, VerbatimComponentsAreWalkedInOrderWithoutHostTraversal) {
    struct Row { const char* path; u32 error; };
    const Row rows[] = {
        {R"(\\?\C:\Folder\.)", error_codes::ERROR_INVALID_NAME},
        {R"(\\?\C:\Folder\.\)", error_codes::ERROR_INVALID_NAME},
        {R"(\\?\C:\Folder\.\Child)", error_codes::ERROR_PATH_NOT_FOUND},
        {R"(\\?\C:\Folder\..)", error_codes::ERROR_INVALID_NAME},
        {R"(\\?\C:\Missing\..)", error_codes::ERROR_PATH_NOT_FOUND},
        {R"(\\?\C:\Missing\.)", error_codes::ERROR_PATH_NOT_FOUND},
        {R"(\\?\C:\..\Folder)", error_codes::ERROR_INVALID_NAME},
        {R"(\\?\C:\Folder\\Child)", error_codes::ERROR_INVALID_NAME},
        {R"(\\?\C:\\\Folder)", error_codes::ERROR_INVALID_NAME},
        {R"(\\?\C:\Folder/Child)", error_codes::ERROR_INVALID_NAME},
    };
    for (const auto& row : rows) for (bool ex : {false, true}) {
        SCOPED_TRACE(row.path); EXPECT_FALSE(call(ex, row.path)); EXPECT_EQ(get_last_error(), row.error);
        EXPECT_EQ(out(0), sentinel); EXPECT_EQ(out(2), sentinel); EXPECT_EQ(out(3), sentinel);
        EXPECT_EQ(out(1), ex ? sentinel : sentinel & 0xffffffff00000000ull);
    }
}
TEST_P(DiskSpaceContract, OrdinaryIntermediatePeriodUsesDosLookupNotLiteralLookup) {
    std::ofstream(root / "Trailing").put('x');
    std::filesystem::create_directories(root / "Trailing." / "Child");
    for (bool ex : {false, true}) {
        EXPECT_FALSE(call(ex, R"(C:\Trailing.\Child)"));
        EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
        EXPECT_TRUE(call(ex, R"(\\?\C:\Trailing.\Child)"));
    }
}
TEST_P(DiskSpaceContract, EarlyDeviceAndLegacyLengthFailuresPreserveEveryOutput) {
    for (const char* path : {"", R"(\\?\C:)", R"(\\.\C:)", R"(NUL)", R"(\\.\NUL)"})
        for (bool ex : {false, true}) {
            SCOPED_TRACE(path); EXPECT_FALSE(call(ex, path));
            EXPECT_EQ(get_last_error(), *path ? error_codes::ERROR_INVALID_FUNCTION : error_codes::ERROR_PATH_NOT_FOUND);
            for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
        }
    for (usize length : {259u, 260u, 261u}) {
        std::string path = "C:\\Folder\\"; path += std::string(length-path.size(), 'x');
        std::filesystem::create_directories(root / "Folder" / std::string(length-10, 'x'));
        for (bool ex : {false, true}) {
            SCOPED_TRACE(length); EXPECT_EQ(call(ex, path.c_str()), length == 259u);
            if (length != 259u) {
                EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
                for (usize i = 0; i < 4; ++i) EXPECT_EQ(out(i), sentinel);
            }
            EXPECT_TRUE(call(ex, ("\\\\?\\" + path).c_str()));
        }
    }
}
TEST_P(DiskSpaceContract, VerbatimIntermediateProviderFailuresAreNotRewrittenAsMissing) {
    backend->fail_stat = true;
    for (bool ex : {false, true}) {
        EXPECT_FALSE(call(ex, R"(\\?\C:\Folder\Child)"));
        EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
        EXPECT_EQ(out(0), sentinel); EXPECT_EQ(out(2), sentinel); EXPECT_EQ(out(3), sentinel);
        EXPECT_EQ(out(1), ex ? sentinel : sentinel & 0xffffffff00000000ull);
    }
}
INSTANTIATE_TEST_SUITE_P(BothAbisAndEncodings, DiskSpaceContract,
    ::testing::Combine(::testing::Bool(), ::testing::Bool()));
} // namespace
