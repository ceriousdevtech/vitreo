/// @file test_advpack.cpp
/// @brief advpack.dll!IsNTAdmin: name, ordinal 43 and stdcall size from the
///        real advpack.dll export table and advpub.h (two DWORD-sized
///        parameters, 8 bytes); the answer is the process token's elevation.
///        oracle/isnt_probe.c on winoracle: the last error and *lpdwReserved
///        are left untouched, and the result equals IsUserAnAdmin.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/advpack/advpack.hpp"

#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

TEST(AdvpackTest, IsNTAdminReportsTheTokenElevationBothAbis) {
    for (bool elevated : {false, true}) {
        GuestElevationState state(elevated);
        const DllExports exports = create_advpack_thunks(&state);
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [](const ExportEntry& e) { return e.name == "IsNTAdmin"; });
        ASSERT_NE(it, exports.entries.end());
        EXPECT_EQ(it->ordinal, 43u);
        EXPECT_EQ(it->stdcall_bytes, 8u);
        for (bool x64 : {false, true}) {
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
            cpu::CpuState cpu;
            if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.set_sp(0x10800);
            Marshaler m(memory);
            m.attach_cpu(cpu);
            constexpr GuestAddr kReserved = 0x10100;
            ASSERT_TRUE(memory.write_u32(kReserved, 0x77).is_ok());
            if (x64) { cpu.gpr.rcx = 0x1234; cpu.gpr.rdx = kReserved; }
            else {
                ASSERT_TRUE(memory.write_u32(0x10804, 0x1234).is_ok());
                ASSERT_TRUE(memory.write_u32(0x10808, kReserved).is_ok());
            }
            set_last_error(0xdead);
            cpu.gpr.rax = 0x5a5a5a5a;
            it->handler(m);
            EXPECT_EQ(memory.read_u32(kReserved).value(), 0x77u);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), elevated ? 1u : 0u);
            EXPECT_EQ(get_last_error(), 0xdeadu);  // never sets the last error
        }
    }
}

TEST(AdvpackTest, NoElevationStateIsNotAnAdministrator) {
    const DllExports exports = create_advpack_thunks(nullptr);
    ASSERT_EQ(exports.entries.size(), 1u);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x10800);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    exports.entries.front().handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}
