#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/pe/loader/teb_setup.hpp>
#include <winapi/kernel32/system/kuser_shared_data.hpp>
#include <winapi/kernel32/system/time.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

TEST(KuserSharedDataTest, PrimaryTebIsNotSharedUserData) {
    EXPECT_NE(pe::PRIMARY_TEB_ADDRESS, pe::SHARED_USER_DATA_ADDRESS);
    EXPECT_EQ(pe::SHARED_USER_DATA_ADDRESS, 0x7FFE0000ull);
}

TEST(KuserSharedDataTest, InstallsClockAndQpcFrequency) {
    GuestMemory memory;
    ASSERT_TRUE(install_kuser_shared_data(memory));

    const u64 qpf =
        memory.read_u64(pe::SHARED_USER_DATA_ADDRESS + 0x300).value();
    EXPECT_EQ(qpf, kQpcHz);

    const u64 ticks =
        memory.read_u64(pe::SHARED_USER_DATA_ADDRESS + 0x320).value();
    EXPECT_GT(ticks, 0u);

    const u32 sys_low =
        memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + 0x14).value();
    const u32 sys_high =
        memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + 0x18).value();
    const u64 filetime =
        (static_cast<u64>(sys_high) << 32) | sys_low;
    // 2020-01-01 UTC as FILETIME is well below this; 1601 would be ~0.
    EXPECT_GT(filetime, 132000000000000000ull);

    const u32 teb_self_slot =
        memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + 0x18).value();
    EXPECT_NE(teb_self_slot, static_cast<u32>(pe::SHARED_USER_DATA_ADDRESS));
}

TEST(KuserSharedDataTest, RefreshAdvancesTickCount) {
    GuestMemory memory;
    ASSERT_TRUE(install_kuser_shared_data(memory));
    const u64 t1 =
        memory.read_u64(pe::SHARED_USER_DATA_ADDRESS + 0x320).value();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    refresh_kuser_shared_data();
    const u64 t2 =
        memory.read_u64(pe::SHARED_USER_DATA_ADDRESS + 0x320).value();
    EXPECT_GE(t2, t1);
    EXPECT_GT(t2, t1);
}

namespace {

u64 read_ksystem_time(const GuestMemory& memory, u32 offset) {
    // KSYSTEM_TIME reader protocol: High1Time, LowPart, High2Time; retry
    // until the two high halves agree.
    for (;;) {
        const u32 high1 =
            memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + offset + 4).value();
        const u32 low =
            memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + offset).value();
        const u32 high2 =
            memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + offset + 8).value();
        if (high1 == high2) return (static_cast<u64>(high1) << 32) | low;
    }
}

u64 kuser_tick_count_ms(const GuestMemory& memory) {
    const u64 ticks =
        memory.read_u64(pe::SHARED_USER_DATA_ADDRESS + 0x320).value();
    const u32 multiplier =
        memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + 0x004).value();
    return (ticks * multiplier) >> 24;
}

}  // namespace

// Field encoding measured on Windows 11 x86 and x64
// (tools/oracle_probe/kuser_clock_probe.expected): TickCount counts 15.625 ms
// clock ticks, TickCountMultiplier is 15.625 * 2^24, the deprecated 32-bit
// TickCountLow is zero, and (TickCount * multiplier) >> 24 is GetTickCount64.
TEST(KuserSharedDataTest, TickCountUsesTheWindowsTickEncoding) {
    GuestMemory memory;
    ASSERT_TRUE(install_kuser_shared_data(memory));
    EXPECT_EQ(memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + 0x004).value(),
              0x0FA00000u);
    EXPECT_EQ(memory.read_u32(pe::SHARED_USER_DATA_ADDRESS + 0x000).value(),
              0u);
    const u64 kuser_ms = kuser_tick_count_ms(memory);
    const u64 host_ms = tick_count_ms();
    EXPECT_LT(kuser_ms > host_ms ? kuser_ms - host_ms : host_ms - kuser_ms,
              32u);
}

// The kernel advances the clocks from its own timer: a reader that never
// calls into the system still sees them move, in steps of at most ~16 ms
// (kuser_clock_probe.expected: interrupt_time_advances_without_api_calls,
// interrupt_time_step_at_most_16ms, interrupt_time_advances_across_sleep100).
// Nothing here calls refresh_kuser_shared_data().
TEST(KuserSharedDataTest, ClocksAdvanceWithoutAnyRefreshCall) {
    GuestMemory memory;
    ASSERT_TRUE(install_kuser_shared_data(memory));
    const u64 i0 = read_ksystem_time(memory, 0x008);
    const u64 s0 = read_ksystem_time(memory, 0x014);
    const u64 k0 = kuser_tick_count_ms(memory);

    u64 last = i0, max_step = 0, changes = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        const u64 now = read_ksystem_time(memory, 0x008);
        if (now != last) {
            max_step = std::max(max_step, now - last);
            last = now;
            ++changes;
        }
    }
    const u64 i1 = read_ksystem_time(memory, 0x008);
    const u64 s1 = read_ksystem_time(memory, 0x014);
    const u64 k1 = kuser_tick_count_ms(memory);

    EXPECT_GE(i1 - i0, 2'300'000u) << "InterruptTime froze";
    EXPECT_GT(changes, 0u);
    EXPECT_LE(max_step, 160'000u) << "InterruptTime stepped more than 16 ms";
    const u64 di = i1 - i0, ds = s1 - s0, dk = (k1 - k0) * 10000;
    EXPECT_LT(di > ds ? di - ds : ds - di, 200'000u)
        << "SystemTime does not track InterruptTime";
    EXPECT_LT(di > dk ? di - dk : dk - di, 200'000u)
        << "TickCount does not track InterruptTime";
}

// Address spaces come and go in tests; the clock source must never write
// into one that has been destroyed.
TEST(KuserSharedDataTest, ClockSourceOutlivesItsAddressSpaces) {
    for (int round = 0; round < 8; ++round) {
        auto memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(install_kuser_shared_data(*memory));
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
        memory.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    GuestMemory last;
    ASSERT_TRUE(install_kuser_shared_data(last));
    const u64 before = read_ksystem_time(last, 0x008);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_GT(read_ksystem_time(last, 0x008), before);
}
