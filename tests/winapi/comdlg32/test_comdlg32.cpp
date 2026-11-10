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
    EXPECT_EQ(exports.entries.size(), 9u);
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

namespace {

/// One OPENFILENAME / CHOOSECOLOR call at 0x2000 with the argument on the
/// x86 stack or in RCX.
struct DialogGuest {
    explicit DialogGuest(bool x64_abi) : x64(x64_abi) {
        EXPECT_TRUE(memory.map_region(0x1000, 0x5000, vitreo::MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x1000);
        cpu.gpr.rcx = 0x2000;
        EXPECT_TRUE(memory.write_u32(0x1004, 0x2000).is_ok());
        m.attach_cpu(cpu);
    }
    vitreo::u64 call(DllExports& exports, const char* name) {
        auto e = std::find_if(exports.entries.begin(), exports.entries.end(),
                              [name](const auto& x) { return x.name == name; });
        EXPECT_NE(e, exports.entries.end()) << name;
        cpu.gpr.rax = 0xdead;
        e->handler(m);
        return cpu.gpr.rax;
    }
    vitreo::u64 extended_error(DllExports& exports) { return call(exports, "CommDlgExtendedError"); }
    bool x64;
    vitreo::GuestMemory memory;
    vitreo::cpu::CpuState cpu;
    Marshaler m{memory};
};

}  // namespace

TEST(Comdlg32Test, GetOpenFileNameAWritesAnsiPathOffsetsAndFileTitle) {
    using namespace vitreo;
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64);
        DialogGuest g(x64);
        capsule::Vfs vfs;
        const auto root = std::filesystem::temp_directory_path().string();
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        const auto ptr = [&](u32 off32, u32 off64, u64 value) {
            return x64 ? g.memory.write_u64(0x2000 + off64, value)
                       : g.memory.write_u32(0x2000 + off32, static_cast<u32>(value));
        };
        // commdlg.h OPENFILENAMEA: lStructSize 88/152, lpstrFilter 12/24,
        // nFilterIndex 24/44, lpstrFile 28/48, nMaxFile 32/56,
        // lpstrFileTitle 36/64, nMaxFileTitle 40/72.
        ASSERT_TRUE(g.memory.write_u32(0x2000, x64 ? 152 : 88).is_ok());
        ASSERT_TRUE(ptr(28, 48, 0x3000).is_ok());
        ASSERT_TRUE(g.memory.write_u32(0x2000 + (x64 ? 56 : 32), 260).is_ok());
        ASSERT_TRUE(ptr(12, 24, 0x4000).is_ok());
        ASSERT_TRUE(g.memory.write_u32(0x2000 + (x64 ? 44 : 24), 2).is_ok());
        ASSERT_TRUE(g.m.write_ansi_string(0x4000, "Text").is_ok());
        ASSERT_TRUE(g.m.write_ansi_string(0x4005, "*.txt").is_ok());
        ASSERT_TRUE(g.m.write_ansi_string(0x400b, "Maps").is_ok());
        ASSERT_TRUE(g.m.write_ansi_string(0x4010, "*.bzn").is_ok());
        ASSERT_TRUE(g.memory.write_u8(0x4016, 0).is_ok());
        ASSERT_TRUE(ptr(36, 64, 0x3800).is_ok());
        ASSERT_TRUE(g.memory.write_u32(0x2000 + (x64 ? 72 : 40), 64).is_ok());
        auto exports = create_comdlg32_thunks(nullptr, &vfs,
            [&](const FileDialogRequest& request) -> std::optional<std::string> {
                EXPECT_FALSE(request.save);
                EXPECT_EQ(request.filter, "*.bzn");
                return root + "/level01.bzn";
            });
        ASSERT_EQ(g.call(exports, "GetOpenFileNameA"), 1u);
        EXPECT_EQ(g.m.read_ansi_string(0x3000).value(), "C:\\level01.bzn");
        EXPECT_EQ(g.memory.read_u16(0x2000 + (x64 ? 100 : 56)).value(), 3u);
        EXPECT_EQ(g.memory.read_u16(0x2000 + (x64 ? 102 : 58)).value(), 11u);
        EXPECT_EQ(g.m.read_ansi_string(0x3800).value(), "level01.bzn");
        // Cancel: FALSE and CommDlgExtendedError 0.
        auto cancel = create_comdlg32_thunks(nullptr, &vfs,
            [](const FileDialogRequest&) -> std::optional<std::string> { return std::nullopt; });
        EXPECT_EQ(g.call(cancel, "GetOpenFileNameA"), 0u);
        EXPECT_EQ(g.extended_error(cancel), 0u);
    }
}

TEST(Comdlg32Test, ChooseColorFollowsTheCHOOSECOLORContractBothAbis) {
    using namespace vitreo;
    // commdlg.h CHOOSECOLOR: lStructSize 36/72, hInstance 8/16, rgbResult
    // 12/24, lpCustColors 16/32, Flags 20/40, lpfnHook 28/56,
    // lpTemplateName 32/64.
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64);
        for (const char* api : {"ChooseColorA", "ChooseColorW"}) {
            SCOPED_TRACE(api);
            DialogGuest g(x64);
            const u32 size = x64 ? 72 : 36, rgb = x64 ? 24 : 12, flags = x64 ? 40 : 20;
            const u32 hook = x64 ? 56 : 28;
            std::optional<u32> seen;
            std::optional<u32> answer = 0x00336699u;
            auto exports = create_comdlg32_thunks(nullptr, nullptr, {},
                [&](u32 initial) { seen = initial; return answer; });
            for (u32 i = 0; i < size; i += 4) ASSERT_TRUE(g.memory.write_u32(0x2000 + i, 0).is_ok());
            ASSERT_TRUE(g.memory.write_u32(0x2000, size).is_ok());
            ASSERT_TRUE(g.memory.write_u32(0x2000 + rgb, 0x00112233u).is_ok());
            // Without CC_RGBINIT the panel starts at black.
            ASSERT_EQ(g.call(exports, api), 1u);
            EXPECT_EQ(seen, 0u);
            EXPECT_EQ(g.memory.read_u32(0x2000 + rgb).value(), 0x00336699u);
            // CC_RGBINIT seeds it with rgbResult.
            ASSERT_TRUE(g.memory.write_u32(0x2000 + flags, 0x1).is_ok());
            ASSERT_EQ(g.call(exports, api), 1u);
            EXPECT_EQ(seen, 0x00336699u);
            // Cancel: FALSE, rgbResult kept, extended error 0.
            answer.reset();
            ASSERT_TRUE(g.memory.write_u32(0x2000 + rgb, 0x00445566u).is_ok());
            EXPECT_EQ(g.call(exports, api), 0u);
            EXPECT_EQ(g.memory.read_u32(0x2000 + rgb).value(), 0x00445566u);
            EXPECT_EQ(g.extended_error(exports), 0u);
            // Wrong lStructSize: CDERR_STRUCTSIZE.
            ASSERT_TRUE(g.memory.write_u32(0x2000, size + 4).is_ok());
            EXPECT_EQ(g.call(exports, api), 0u);
            EXPECT_EQ(g.extended_error(exports), 0x0001u);
            ASSERT_TRUE(g.memory.write_u32(0x2000, size).is_ok());
            // CC_ENABLEHOOK without a hook: CDERR_NOHOOK; with one: the
            // guest-drawn dialog is refused, not silently replaced.
            ASSERT_TRUE(g.memory.write_u32(0x2000 + flags, 0x10).is_ok());
            EXPECT_EQ(g.call(exports, api), 0u);
            EXPECT_EQ(g.extended_error(exports), 0x000Bu);
            ASSERT_TRUE((x64 ? g.memory.write_u64(0x2000 + hook, 0x401000)
                             : g.memory.write_u32(0x2000 + hook, 0x401000)).is_ok());
            EXPECT_EQ(g.call(exports, api), 0u);
            EXPECT_EQ(g.extended_error(exports), 0xFFFFu);
            // CC_ENABLETEMPLATE without hInstance/template: CDERR_NOTEMPLATE.
            ASSERT_TRUE(g.memory.write_u32(0x2000 + flags, 0x20).is_ok());
            EXPECT_EQ(g.call(exports, api), 0u);
            EXPECT_EQ(g.extended_error(exports), 0x0003u);
        }
    }
}
