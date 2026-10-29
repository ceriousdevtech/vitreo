/// @file test_powrprof.cpp
/// @brief POWRPROF.DLL contract tests.

#include <gtest/gtest.h>

#include <algorithm>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/system/sysinfo.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/system/powrprof/powrprof.hpp>
#include "platform/host_hardware_fixtures.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 PROCESSOR_INFORMATION = 11;
constexpr u32 SYSTEM_POWER_INFORMATION = 12;
constexpr u32 STATUS_SUCCESS = 0x00000000u;
constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000Du;
constexpr u32 STATUS_BUFFER_TOO_SMALL = 0xC0000023u;
constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BBu;

class PowrprofTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);

        exports = create_powrprof_thunks(sysinfo);
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [](const ExportEntry& entry) {
                return entry.name == "CallNtPowerInformation";
            });
        ASSERT_NE(it, exports.entries.end());
        call = &it->handler;
    }

    void set_args(u32 level, GuestAddr input, u32 input_length,
                  GuestAddr output, u32 output_length) {
        cpu.gpr.rcx = level;
        cpu.gpr.rdx = input;
        cpu.gpr.r8 = input_length;
        cpu.gpr.r9 = output;
        ASSERT_TRUE(memory.write_u64(
            cpu.gpr.rsp + 0x28, output_length).is_ok());
    }

    void invoke() {
        ASSERT_NE(call, nullptr);
        (*call)(marshaler);
    }

    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    SysInfoManager sysinfo;
    DllExports exports;
    ApiHandler* call{nullptr};
};

TEST_F(PowrprofTest, ExportsCallNtPowerInformationWithFiveArguments) {
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(exports.entries.size(), 1u);
    EXPECT_EQ(exports.entries.front().stdcall_bytes, 20u);
}

TEST_F(PowrprofTest, ProcessorInformationMatchesGuestProcessorCount) {
    // Each processor reports its core class's highest clock: on an M4 Pro the
    // Efficiency cores (0-3) 2592 MHz and the Performance cores 4512 MHz.
    test::ScopedHostHardware host(test::m4_pro_profile());
    constexpr GuestAddr output = DATA_BASE + 0x100;
    constexpr u32 entry_size = 24;
    const u32 count = 14;

    set_args(PROCESSOR_INFORMATION, 0, 0, output, count * entry_size);
    invoke();

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_SUCCESS);
    for (u32 index = 0; index < count; ++index) {
        const GuestAddr entry = output + index * entry_size;
        EXPECT_EQ(memory.read_u32(entry + 0).value(), index);
        const u32 mhz = index < 4 ? 2592u : 4512u;
        EXPECT_EQ(memory.read_u32(entry + 4).value(), mhz);   // MaxMhz
        EXPECT_EQ(memory.read_u32(entry + 8).value(), mhz);   // CurrentMhz
        EXPECT_EQ(memory.read_u32(entry + 12).value(), mhz);  // MhzLimit
        EXPECT_EQ(memory.read_u32(entry + 16).value(), 0u);
        EXPECT_EQ(memory.read_u32(entry + 20).value(), 0u);
    }
}

TEST_F(PowrprofTest, ProcessorInformationRejectsSmallOutputBuffer) {
    constexpr GuestAddr output = DATA_BASE + 0x100;
    ASSERT_TRUE(memory.write_u32(output, 0xA5A5A5A5u).is_ok());

    set_args(PROCESSOR_INFORMATION, 0, 0, output, 23);
    invoke();

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_BUFFER_TOO_SMALL);
    EXPECT_EQ(memory.read_u32(output).value(), 0xA5A5A5A5u);
}

TEST_F(PowrprofTest, ProcessorInformationRejectsInputBuffer) {
    set_args(PROCESSOR_INFORMATION, DATA_BASE, 1,
             DATA_BASE + 0x100, 96);
    invoke();

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_INVALID_PARAMETER);
}

TEST_F(PowrprofTest, SystemPowerInformationReportsActiveGuestState) {
    constexpr GuestAddr output = DATA_BASE + 0x100;

    set_args(SYSTEM_POWER_INFORMATION, 0, 0, output, 16);
    invoke();

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(output + 0).value(), 90u);
    EXPECT_EQ(memory.read_u32(output + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(output + 8).value(), 0u);
    EXPECT_EQ(memory.read_u8(output + 12).value(), 2u);
    EXPECT_EQ(memory.read_u8(output + 13).value(), 0u);
    EXPECT_EQ(memory.read_u8(output + 14).value(), 0u);
    EXPECT_EQ(memory.read_u8(output + 15).value(), 0u);
}

TEST_F(PowrprofTest, SystemPowerInformationRejectsSmallOutputBuffer) {
    constexpr GuestAddr output = DATA_BASE + 0x100;
    ASSERT_TRUE(memory.write_u32(output, 0x5A5A5A5Au).is_ok());

    set_args(SYSTEM_POWER_INFORMATION, 0, 0, output, 15);
    invoke();

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_BUFFER_TOO_SMALL);
    EXPECT_EQ(memory.read_u32(output).value(), 0x5A5A5A5Au);
}

// tools/oracle_probe/s7_misc_probe.expected: levels outside the queryable
// set (undefined ones included) are STATUS_INVALID_PARAMETER; a queryable
// level Vitreo does not model fails loudly with STATUS_NOT_SUPPORTED.
TEST_F(PowrprofTest, UnsupportedInformationLevelFailsHonestly) {
    set_args(0xFFFFu, 0, 0, DATA_BASE + 0x100, 96);
    invoke();
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xC000000Du);  // STATUS_INVALID_PARAMETER

    set_args(0x14u /* LastWakeTime */, 0, 0, DATA_BASE + 0x100, 8);
    invoke();
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_NOT_SUPPORTED);
}

// SystemBatteryState on a machine with AC power and no battery (the state
// GetSystemPowerStatus reports): measured AcOnLine 1, all else 0; 32 bytes.
TEST_F(PowrprofTest, SystemBatteryStateReportsAcPowerWithoutBattery) {
    constexpr GuestAddr output = DATA_BASE + 0x100;
    for (u32 i = 0; i < 40; ++i) ASSERT_TRUE(memory.write_u8(output + i, 0xCC).is_ok());
    set_args(5u, 0, 0, output, 31);
    invoke();
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_BUFFER_TOO_SMALL);
    set_args(5u, 0, 0, output, 32);
    invoke();
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u8(output).value(), 1u);       // AcOnLine
    EXPECT_EQ(memory.read_u8(output + 1).value(), 0u);   // BatteryPresent
    EXPECT_EQ(memory.read_u32(output + 20).value(), 0u); // EstimatedTime
    EXPECT_EQ(memory.read_u8(output + 32).value(), 0xCCu);
}

} // namespace
