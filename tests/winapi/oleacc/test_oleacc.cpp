/// @file test_oleacc.cpp
/// @brief Contract tests for Vitreo's partial oleacc.dll implementation.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/ui/oleacc/oleacc.hpp>

using namespace vitreo;
using namespace vitreo::windowing;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr DATA_BASE = 0x10000;
constexpr GuestAddr STACK_BASE = 0x20000;
constexpr GuestAddr IID_ADDRESS = DATA_BASE + 0x100;
constexpr GuestAddr OUTPUT_ADDRESS = DATA_BASE + 0x200;

constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_INVALIDARG = 0x80070057u;

constexpr std::array<u8, 16> IID_IACCESSIBLE{
    0xE0, 0x36, 0x87, 0x61,
    0x3D, 0x3C,
    0xCF, 0x11,
    0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

class OleaccTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);

        WindowData window{};
        window.parent = HWND_DESKTOP;
        hwnd = hwnd_manager.create(window);
        exports = create_oleacc_thunks(hwnd_manager);
        ASSERT_TRUE(memory.write_bytes(
            IID_ADDRESS, IID_IACCESSIBLE.data(),
            IID_IACCESSIBLE.size()).is_ok());
    }

    ExportEntry& export_named(std::string_view name) {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        EXPECT_NE(entry, exports.entries.end());
        return *entry;
    }

    void set_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * sizeof(u32), value).is_ok());
    }

    u32 invoke(std::string_view name, std::initializer_list<u32> args) {
        usize index = 0;
        for (const u32 argument : args) {
            set_arg(index++, argument);
        }
        export_named(name).handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HwndManager hwnd_manager;
    HWND hwnd = NULL_HWND;
    DllExports exports;
};

TEST_F(OleaccTest, ExposesRequiredSurfaceWithExactX86StackSizes) {
    ASSERT_EQ(exports.entries.size(), 5u);
    EXPECT_EQ(export_named("AccessibleChildren").stdcall_bytes, 20u);
    EXPECT_EQ(
        export_named("AccessibleObjectFromWindow").stdcall_bytes, 16u);
    EXPECT_EQ(
        export_named("CreateStdAccessibleObject").stdcall_bytes, 16u);
    EXPECT_EQ(export_named("LresultFromObject").stdcall_bytes, 12u);
    EXPECT_EQ(
        export_named("WindowFromAccessibleObject").stdcall_bytes, 8u);
}

TEST_F(OleaccTest, AccessibleObjectFromWindowFailsWithoutFabricatingObject) {
    ASSERT_TRUE(memory.write_u32(OUTPUT_ADDRESS, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(
        invoke(
            "AccessibleObjectFromWindow",
            {hwnd, 0xFFFFFFFCu, IID_ADDRESS, OUTPUT_ADDRESS}),
        E_NOTIMPL);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value_or(1), 0u);
}

TEST_F(OleaccTest, AccessibleObjectFromWindowRejectsInvalidWindowAndIid) {
    // Measured on Windows 11: an unknown window is E_FAIL with *ppv NULL
    // (tools/oracle_probe/s1win_dwm_probe.expected aofw_invalid_hwnd).
    EXPECT_EQ(
        invoke(
            "AccessibleObjectFromWindow",
            {0xDEADBEEFu, 0, IID_ADDRESS, OUTPUT_ADDRESS}),
        0x80004005u);

    ASSERT_TRUE(memory.write_u32(IID_ADDRESS, 0x12345678u).is_ok());
    EXPECT_EQ(
        invoke(
            "AccessibleObjectFromWindow",
            {hwnd, 0, IID_ADDRESS, OUTPUT_ADDRESS}),
        E_NOINTERFACE);
}

TEST_F(OleaccTest, CreateStdAccessibleObjectFailsWithoutFabricatingObject) {
    ASSERT_TRUE(memory.write_u32(OUTPUT_ADDRESS, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(
        invoke(
            "CreateStdAccessibleObject",
            {hwnd, 0xFFFFFFFCu, IID_ADDRESS, OUTPUT_ADDRESS}),
        E_NOTIMPL);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value_or(1), 0u);
}

TEST_F(OleaccTest, AccessibleChildrenClearsCountAndReportsUnsupportedObject) {
    ASSERT_TRUE(memory.write_u32(OUTPUT_ADDRESS, 7).is_ok());
    EXPECT_EQ(
        invoke(
            "AccessibleChildren",
            {DATA_BASE + 0x300, 0, 1, DATA_BASE + 0x400, OUTPUT_ADDRESS}),
        E_NOTIMPL);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value_or(1), 0u);

    EXPECT_EQ(
        invoke(
            "AccessibleChildren",
            {0, 0, 1, DATA_BASE + 0x400, OUTPUT_ADDRESS}),
        E_INVALIDARG);
}

TEST_F(OleaccTest, LresultFromObjectRejectsInvalidAndUnsupportedInterfaces) {
    EXPECT_EQ(
        invoke("LresultFromObject", {IID_ADDRESS, 0, 0}),
        E_INVALIDARG);

    ASSERT_TRUE(memory.write_u32(IID_ADDRESS, 0x12345678u).is_ok());
    EXPECT_EQ(
        invoke(
            "LresultFromObject",
            {IID_ADDRESS, 0, DATA_BASE + 0x300}),
        E_NOINTERFACE);
}

TEST_F(OleaccTest, WindowFromAccessibleObjectClearsOutputOnFailure) {
    ASSERT_TRUE(memory.write_u32(OUTPUT_ADDRESS, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(
        invoke(
            "WindowFromAccessibleObject",
            {DATA_BASE + 0x300, OUTPUT_ADDRESS}),
        E_NOTIMPL);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value_or(1), 0u);

    EXPECT_EQ(
        invoke("WindowFromAccessibleObject", {0, OUTPUT_ADDRESS}),
        E_INVALIDARG);
}

} // namespace
