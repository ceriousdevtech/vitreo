#include <gtest/gtest.h>
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/filesystem/drive_type_contract.inc"
#include <filesystem>
#include <cstring>
#include <cstdlib>
using namespace vitreo;
using namespace vitreo::winapi;
namespace oracle {
#include "../../oracle_data/drive_type_cases.generated.inc"
}

TEST(DriveTypeContract, WindowsOracleCasesAcrossBothEncodingsAndGuestAbis) {
    std::string pattern = (std::filesystem::temp_directory_path() / "vitreo-drive-type-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back(0);
    ASSERT_NE(::mkdtemp(buffer.data()), nullptr);
    const std::filesystem::path root(buffer.data());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    ASSERT_TRUE(std::filesystem::create_directory(root / "Windows"));
    class OpticalFixture : public capsule::HostFileSystem {
    public:
        using capsule::HostFileSystem::HostFileSystem;
        bool is_optical() const noexcept override { return true; }
    };
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    ASSERT_TRUE(vfs.mount_drive('D', std::make_unique<OpticalFixture>(root.string())).is_ok());
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    Kernel32ExtManager manager(memory, vfs);
    for (const bool is64 : {false, true}) for (const bool wide : {false, true}) {
        const auto call = [&](const char* path) {
            cpu::CpuState state;
            if (is64) { state.init_long_mode(); state.gpr.rsp = 0x4000; }
            else { state.init_protected_mode(); state.set_sp(0x4000); }
            Marshaler marshaler(memory); marshaler.attach_cpu(state);
            if (path) {
                if (wide) EXPECT_TRUE(marshaler.write_wide_string(0x1000, path).is_ok());
                else EXPECT_TRUE(memory.write_bytes(0x1000, reinterpret_cast<const Byte*>(path),
                                                    std::strlen(path) + 1).is_ok());
            }
            const u32 address = path ? 0x1000 : 0;
            if (is64) state.gpr.rcx = address;
            else EXPECT_TRUE(memory.write_u32(0x4004, address).is_ok());
            if (wide) manager.impl_GetDriveTypeW(marshaler);
            else manager.impl_GetDriveTypeA(marshaler);
            return static_cast<u32>(state.gpr.rax);
        };
        for (const auto& expected : oracle::drive_type_cases) {
            SCOPED_TRACE(expected.label);
            manager.set_current_directory(expected.cwd);
            set_last_error(oracle::drive_type_error_sentinel);
            EXPECT_EQ(call(expected.path), expected.type);
            EXPECT_EQ(get_last_error(), expected.error);
        }
        manager.set_current_directory("D:\\Windows");
        EXPECT_EQ(call(nullptr), drive_type_sdk::DRIVE_CDROM);
        EXPECT_EQ(call("D:\\"), drive_type_sdk::DRIVE_CDROM);
        EXPECT_EQ(call("D:"), drive_type_sdk::DRIVE_CDROM);
    }
}
