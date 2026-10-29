/// @file test_comdlg32.cpp
/// @brief Unit tests for COMDLG32.dll common dialog stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/ui/comdlg32/comdlg32.hpp>

#include <algorithm>

using namespace vitreo::winapi;

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST(Comdlg32Test, ThunksContainExpectedEntries) {
    DllExports exports = create_comdlg32_thunks();
    EXPECT_EQ(exports.entries.size(), 6u);
}

TEST(Comdlg32Test, ThunkHasGetFileTitleA) {
    DllExports exports = create_comdlg32_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetFileTitleA"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 12u);
}

TEST(Comdlg32Test, ThunkHasGetSaveFileNameA) {
    DllExports exports = create_comdlg32_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetSaveFileNameA"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);
}

TEST(Comdlg32Test, ThunkHasGetOpenFileNameW) {
    DllExports exports = create_comdlg32_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetOpenFileNameW"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);
}

TEST(Comdlg32Test, ThunkHasGetSaveFileNameW) {
    DllExports exports = create_comdlg32_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetSaveFileNameW"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);
}

TEST(Comdlg32Test, ThunkHasCommDlgExtendedError) {
    DllExports exports = create_comdlg32_thunks();
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "CommDlgExtendedError"; });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 0u);
}

TEST(Comdlg32Test, AllHandlersAreCallable) {
    DllExports exports = create_comdlg32_thunks();
    for (const auto& entry : exports.entries) {
        EXPECT_TRUE(entry.handler != nullptr) << "Handler for " << entry.name << " is null";
    }
}

#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <filesystem>

TEST(Comdlg32Test, NativeSelectionUsesGuestPathFilterPatternAndBitnessOffsets) {
    using namespace vitreo;
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x5000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x1000);
        cpu.gpr.rcx = 0x2000;
        ASSERT_TRUE(memory.write_u32(0x1004, 0x2000).is_ok());
        Marshaler m(memory);
        m.attach_cpu(cpu);
        capsule::Vfs vfs;
        const auto root = std::filesystem::temp_directory_path().string();
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        const auto ptr = [&](u32 off32, u32 off64, u64 value) {
            return x64 ? memory.write_u64(0x2000 + off64, value)
                       : memory.write_u32(0x2000 + off32, static_cast<u32>(value));
        };
        ASSERT_TRUE(ptr(28, 48, 0x3000).is_ok());
        ASSERT_TRUE(ptr(12, 24, 0x4000).is_ok());
        ASSERT_TRUE(memory.write_u32(0x2000 + (x64 ? 56 : 32), 256).is_ok());
        ASSERT_TRUE(memory.write_u32(0x2000 + (x64 ? 44 : 24), 1).is_ok());
        ASSERT_TRUE(memory.write_u32(0x2000 + (x64 ? 96 : 52), 0x80000).is_ok());
        ASSERT_TRUE(m.write_wide_string(0x4000, "All files (*.*)").is_ok());
        ASSERT_TRUE(m.write_wide_string(0x4000 + 32, "*.*").is_ok());
        bool shown = false;
        auto exports = create_comdlg32_thunks(nullptr, &vfs,
            [&](const FileDialogRequest& request) -> std::optional<std::string> {
                shown = true;
                EXPECT_TRUE(request.save);
                EXPECT_EQ(request.filter, "*.*");
                return root + "/dialog-validation.txt";
            });
        auto save = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const auto& e) { return e.name == "GetSaveFileNameW"; });
        ASSERT_NE(save, exports.entries.end());
        save->handler(m);
        ASSERT_TRUE(shown);
        ASSERT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(m.read_wide_string(0x3000).value(), "C:\\dialog-validation.txt");
        EXPECT_EQ(memory.read_u16(0x2000 + (x64 ? 100 : 56)).value(), 3u);
        EXPECT_EQ(memory.read_u16(0x2000 + (x64 ? 102 : 58)).value(), 21u);
        EXPECT_EQ(memory.read_u32(0x2000 + (x64 ? 96 : 52)).value(), 0x80000u);
        // A small caller buffer must fail without overwriting its boundary.
        ASSERT_TRUE(memory.write_u32(0x2000 + (x64 ? 56 : 32), 2).is_ok());
        ASSERT_TRUE(memory.write_u32(0x3004, 0xDEADBEEF).is_ok());
        save->handler(m);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(memory.read_u32(0x3004).value(), 0xDEADBEEFu);
        auto error = std::find_if(exports.entries.begin(), exports.entries.end(),
            [](const auto& e) { return e.name == "CommDlgExtendedError"; });
        error->handler(m);
        EXPECT_EQ(cpu.gpr.rax, 0x3003u);
    }
}
