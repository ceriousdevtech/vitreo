/// @file test_uxtheme.cpp
/// @brief Contract tests for uxtheme.dll window theme attributes, theme font
///        and transition failures, and buffered-paint initialisation.
///        The Aero metric and API replays are in test_uxtheme_oracle.cpp.

#include <gtest/gtest.h>

#include <algorithm>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/ui/uxtheme/uxtheme.hpp>
#include <winapi/kernel32/error.hpp>

using namespace vitreo;
using namespace vitreo::windowing;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kDataBase = 0x10000;
constexpr GuestAddr kStackBase = 0x20000;
constexpr GuestAddr kOptions = kDataBase + 0x100;
constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEHandle = 0x80070006u;
constexpr u32 kEInvalidArg = 0x80070057u;

class UxThemeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            kDataBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            kStackBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = kStackBase + 0x800;
        marshaler.attach_cpu(cpu);

        WindowData data{};
        data.parent = HWND_DESKTOP;
        hwnd = hwnd_mgr.create(data);

        exports = create_uxtheme_thunks(&hwnd_mgr);
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [](const ExportEntry& entry) {
                return entry.name == "SetWindowThemeAttribute";
            });
        ASSERT_NE(it, exports.entries.end());
        set_attribute = &*it;
    }

    void set_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * sizeof(u32), value).is_ok());
    }

    u32 call(u32 target_hwnd, u32 attribute, GuestAddr options, u32 size) {
        set_arg(0, target_hwnd);
        set_arg(1, attribute);
        set_arg(2, static_cast<u32>(options));
        set_arg(3, size);
        set_attribute->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HwndManager hwnd_mgr;
    HWND hwnd = NULL_HWND;
    DllExports exports;
    ExportEntry* set_attribute = nullptr;
};

TEST_F(UxThemeTest, ExportsSetWindowThemeAttributeWithCorrectStdcallSize) {
    ASSERT_NE(set_attribute, nullptr);
    EXPECT_EQ(set_attribute->stdcall_bytes, 16u);
}

TEST_F(UxThemeTest, AppliesAndClearsOnlyMaskedNonClientFlags) {
    // Add NODRAWCAPTION (1) and NODRAWICON (2).
    ASSERT_TRUE(memory.write_u32(kOptions, 0x3).is_ok());
    ASSERT_TRUE(memory.write_u32(kOptions + 4, 0x3).is_ok());
    EXPECT_EQ(call(hwnd, 1, kOptions, 8), kSOk);
    EXPECT_EQ(hwnd_mgr.theme_nonclient_attributes(hwnd), 0x3u);

    // Clear only NODRAWCAPTION; NODRAWICON remains set.
    ASSERT_TRUE(memory.write_u32(kOptions, 0x0).is_ok());
    ASSERT_TRUE(memory.write_u32(kOptions + 4, 0x1).is_ok());
    EXPECT_EQ(call(hwnd, 1, kOptions, 8), kSOk);
    EXPECT_EQ(hwnd_mgr.theme_nonclient_attributes(hwnd), 0x2u);
}

TEST_F(UxThemeTest, RejectsInvalidWindowWithoutChangingState) {
    ASSERT_TRUE(memory.write_u32(kOptions, 0x1).is_ok());
    ASSERT_TRUE(memory.write_u32(kOptions + 4, 0x1).is_ok());
    // uxtheme_aero_probe "setwindowthemeattribute nullhwnd": E_FAIL, last
    // error ERROR_INVALID_WINDOW_HANDLE.
    set_last_error(0xDEAD);
    EXPECT_EQ(call(0xDEADBEEFu, 1, kOptions, 8), 0x80004005u);
    EXPECT_EQ(get_last_error(), 1400u);
    EXPECT_EQ(hwnd_mgr.theme_nonclient_attributes(hwnd), 0u);
}

TEST_F(UxThemeTest, RejectsUnknownAttributeAndBadOptions) {
    ASSERT_TRUE(memory.write_u32(kOptions, 0x1).is_ok());
    ASSERT_TRUE(memory.write_u32(kOptions + 4, 0x1).is_ok());
    EXPECT_EQ(call(hwnd, 2, kOptions, 8), kEInvalidArg);
    EXPECT_EQ(call(hwnd, 1, 0, 8), kEInvalidArg);
    EXPECT_EQ(call(hwnd, 1, kOptions, 4), kEInvalidArg);
    EXPECT_EQ(call(hwnd, 1, kDataBase + 0x2000, 8), kEInvalidArg);
    EXPECT_EQ(hwnd_mgr.theme_nonclient_attributes(hwnd), 0u);
}

} // namespace

// Expected values: tools/oracle_probe/propsheet_theme_session_probe.expected
// (x64) and .x86.expected — identical in both bitnesses.
TEST_F(UxThemeTest, ThemeFontAndTransitionDurationMeasuredFailuresBothBitnesses) {
    auto find = [this](const char* name) -> const ExportEntry* {
        for (const auto& e : exports.entries) {
            if (e.name == name) return &e;
        }
        return nullptr;
    };
    const ExportEntry* font = find("GetThemeFont");
    const ExportEntry* duration = find("GetThemeTransitionDuration");
    ASSERT_NE(font, nullptr);
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(font->stdcall_bytes, 24u);
    EXPECT_EQ(duration->stdcall_bytes, 24u);
    constexpr GuestAddr kOut = kDataBase + 0x400;
    constexpr u32 kEPointer = 0x80004003u;

    for (bool is64 : {false, true}) {
        cpu = cpu::CpuState{};
        if (!is64) cpu.init_protected_mode();
        cpu.gpr.rsp = kStackBase + 0x800;
        marshaler.attach_cpu(cpu);
        auto args = [&](u32 theme, GuestAddr out) {
            if (is64) {
                cpu.gpr.rcx = theme;
                cpu.gpr.rdx = 0;
                cpu.gpr.r8 = 1;
                cpu.gpr.r9 = 1;
                ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 210).is_ok());
                ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, out).is_ok());
            } else {
                set_arg(0, theme);
                set_arg(1, 0);
                set_arg(2, 1);
                set_arg(3, 1);
                set_arg(4, 210);
                set_arg(5, static_cast<u32>(out));
            }
        };
        for (u32 theme : {0u, 0x1234u}) {
            ASSERT_TRUE(memory.write_u32(kOut, 0xCCCCCCCCu).is_ok());
            args(theme, kOut);
            set_last_error(0xDEAD);
            font->handler(marshaler);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEHandle);
            EXPECT_EQ(memory.read_u32(kOut).value(), 0xCCCCCCCCu);
            EXPECT_EQ(get_last_error(), 0xDEADu);

            ASSERT_TRUE(memory.write_u32(kOut, 0xDEADBEEFu).is_ok());
            duration->handler(marshaler);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEHandle);
            EXPECT_EQ(memory.read_u32(kOut).value(), 0xDEADBEEFu);
            EXPECT_EQ(get_last_error(), 0xDEADu);
        }
        args(0, 0);
        font->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEPointer);
        duration->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEHandle);
    }
}

TEST_F(UxThemeTest, BufferedPaintInitBalancesPerThread) {
    const auto init = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& entry) { return entry.name == "BufferedPaintInit"; });
    const auto uninit = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& entry) { return entry.name == "BufferedPaintUnInit"; });
    const auto render = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& entry) { return entry.name == "BufferedPaintRenderAnimation"; });
    const auto stop = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& entry) { return entry.name == "BufferedPaintStopAllAnimations"; });
    ASSERT_NE(init, exports.entries.end());
    ASSERT_NE(uninit, exports.entries.end());
    ASSERT_NE(render, exports.entries.end());
    ASSERT_NE(stop, exports.entries.end());
    EXPECT_EQ(init->stdcall_bytes, 0u);
    EXPECT_EQ(uninit->stdcall_bytes, 0u);
    EXPECT_EQ(render->stdcall_bytes, 8u);
    EXPECT_EQ(stop->stdcall_bytes, 4u);

    uninit->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80004005u);
    init->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    init->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    uninit->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    uninit->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    uninit->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80004005u);

    render->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    set_arg(0, hwnd);
    stop->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    // uxtheme_aero_probe "banim ... stop_null=00000000".
    set_arg(0, 0);
    stop->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
}
