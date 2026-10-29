/// @file test_vmem_host_memory_floor.cpp
/// @brief Unit tests for the host-memory floor: commit limit, GPU admission, live readings and the memory brake (WIN32-020)

#include "test_vmem_fixture.hpp"
#include "runtime/diagnostics/memory_brake.hpp"
#include "core/memory/host_memory_floor.hpp"
#include <winapi/kernel32/error.hpp>

TEST_F(VirtualMemoryTest, CommitPastTheHostMemoryFloorFailsWithCommitmentLimit) {
    // Learn (VirtualAlloc): a commit the system cannot back fails, and
    // winerror.h names the commit-limit case ERROR_COMMITMENT_LIMIT (1455).
    // Windows charges commit against RAM + pagefile; Vitreo's commit limit
    // is reached when the kernel reports critical memory pressure. Pin that
    // so every bulk commit is refused, and check a small one still passes.
    using vitreo::memory::HostMemoryPressure;
    using vitreo::memory::HostMemoryReadings;
    vitreo::memory::set_host_memory_floor_bytes(u64{1} << 30);
    const HostMemoryReadings plenty{u64{64} << 30, HostMemoryPressure::warn};
    vitreo::memory::set_host_memory_readings_override(&plenty);
    EXPECT_NE(vmm.virtual_alloc(0, 0x400000, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE), 0u)
        << "warn pressure does not exhaust commit";
    const HostMemoryReadings floor_only{0, HostMemoryPressure::normal};
    vitreo::memory::set_host_memory_readings_override(&floor_only);
    EXPECT_NE(vmm.virtual_alloc(0, 0x400000, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE), 0u)
        << "pageable commit is not held to the GPU floor";
    const HostMemoryReadings critical{u64{64} << 30,
                                      HostMemoryPressure::critical};
    vitreo::memory::set_host_memory_readings_override(&critical);
    const u64 refused =
        vmm.virtual_alloc(0, 0x400000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    EXPECT_EQ(refused, 0u);
    EXPECT_EQ(vmm.take_last_alloc_error(), error_codes::ERROR_COMMITMENT_LIMIT);
    // A reservation charges nothing against the commit limit.
    const u64 reserved =
        vmm.virtual_alloc(0, 0x400000, MEM_RESERVE, PAGE_READWRITE);
    EXPECT_NE(reserved, 0u);
    // Neither does a commit under the checked size (stack guard pages etc.).
    EXPECT_NE(vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE), 0u);
    vitreo::memory::set_host_memory_floor_bytes(0);  // disabled
    EXPECT_NE(vmm.virtual_alloc(0, 0x400000, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE), 0u);
    vitreo::memory::set_host_memory_floor_bytes(~u64{0});  // default again
    vitreo::memory::set_host_memory_readings_override(nullptr);
}

TEST(HostMemoryFloor, PageableCommitIsRefusedOnlyUnderCriticalPressure) {
    using namespace vitreo::memory;
    set_host_memory_floor_bytes(u64{3} << 30);
    for (const auto pressure :
         {HostMemoryPressure::normal, HostMemoryPressure::warn}) {
        const HostMemoryReadings readings{0, pressure};
        set_host_memory_readings_override(&readings);
        EXPECT_TRUE(host_can_commit(u64{1} << 40, "test"));
        EXPECT_FALSE(take_host_memory_refusal());
    }
    const HostMemoryReadings critical{u64{64} << 30,
                                      HostMemoryPressure::critical};
    set_host_memory_readings_override(&critical);
    EXPECT_FALSE(host_can_commit(u64{1} << 20, "test"));
    EXPECT_TRUE(take_host_memory_refusal());
    set_host_memory_floor_bytes(0);
    EXPECT_TRUE(host_can_commit(u64{1} << 20, "test"))
        << "a zero floor disables admission";
    set_host_memory_floor_bytes(~u64{0});
    set_host_memory_readings_override(nullptr);
}

TEST(HostMemoryFloor, GpuAllocationHonoursWorkingSetFloorAndPressure) {
    using namespace vitreo::memory;
    constexpr u64 MiB = u64{1} << 20;
    constexpr u64 GiB = u64{1} << 30;
    set_host_memory_floor_bytes(3 * GiB);
    // The RV There Yet refusal: a 511 MiB texture with ~10 GiB obtainable
    // through the compressor, a 3 GiB floor, and an idle device.
    const HostMemoryReadings roomy{10 * GiB, HostMemoryPressure::warn};
    set_host_memory_readings_override(&roomy);
    const GpuWorkingSet idle{0, 16 * GiB};
    EXPECT_TRUE(host_can_allocate_gpu(511 * MiB, idle, "test"));
    EXPECT_EQ(host_gpu_budget(idle), 7 * GiB);
    // Up to the floor exactly, and not a byte past it.
    EXPECT_TRUE(host_can_allocate_gpu(7 * GiB, idle, "test"));
    EXPECT_FALSE(host_can_allocate_gpu(7 * GiB + 1, idle, "test"));
    EXPECT_TRUE(take_host_memory_refusal());
    // Metal's recommended working set is the reported budget, not a limit:
    // Learn QueryVideoMemoryInfo treats over-budget as a performance hazard.
    const GpuWorkingSet busy{15 * GiB + 512 * MiB, 16 * GiB};
    EXPECT_TRUE(host_can_allocate_gpu(512 * MiB + 1, busy, "test"));
    EXPECT_EQ(host_gpu_budget(busy), 16 * GiB);
    const GpuWorkingSet over{17 * GiB, 16 * GiB};
    EXPECT_TRUE(host_can_allocate_gpu(1, over, "test"));
    EXPECT_FALSE(take_host_memory_refusal());
    // Below the floor nothing new fits; an unknown working set skips that rule.
    const HostMemoryReadings tight{2 * GiB, HostMemoryPressure::normal};
    set_host_memory_readings_override(&tight);
    EXPECT_FALSE(host_can_allocate_gpu(MiB, {0, 0}, "test"));
    EXPECT_EQ(host_gpu_budget({GiB, 16 * GiB}), GiB);
    // Critical pressure refuses even with room everywhere.
    const HostMemoryReadings critical{64 * GiB, HostMemoryPressure::critical};
    set_host_memory_readings_override(&critical);
    EXPECT_FALSE(host_can_allocate_gpu(MiB, idle, "test"));
    EXPECT_TRUE(take_host_memory_refusal());
    EXPECT_EQ(host_gpu_budget({GiB, 16 * GiB}), GiB);
    set_host_memory_floor_bytes(0);
    EXPECT_TRUE(host_can_allocate_gpu(u64{1} << 40, over, "test"))
        << "a zero floor disables admission";
    EXPECT_EQ(host_gpu_budget(idle), 16 * GiB);
    set_host_memory_floor_bytes(~u64{0});
    set_host_memory_readings_override(nullptr);
}

TEST(HostMemoryFloor, LiveReadingsAreKnown) {
    vitreo::memory::set_host_memory_readings_override(nullptr);
    const auto readings = vitreo::memory::host_memory_readings();
    EXPECT_GT(readings.obtainable_bytes, 0u);
    EXPECT_NE(readings.obtainable_bytes, ~u64{0}) << "host_statistics64 failed";
}

TEST(HostMemoryFloor, BrakeFiresOnlyForTheBigConsumerBelowHalfTheFloor) {
    constexpr u64 GiB = u64{1} << 30;
    const u64 physical = 24 * GiB;
    const u64 floor = 3 * GiB;
    // Host short of memory and this process holds 10 GiB: terminate it.
    EXPECT_TRUE(vitreo::diagnostics::memory_brake_should_fire(
        1 * GiB, floor, 10 * GiB, physical));
    // A small bystander (a Steam helper) is left alone.
    EXPECT_FALSE(vitreo::diagnostics::memory_brake_should_fire(
        1 * GiB, floor, 2 * GiB, physical));
    // Above half the floor the floor itself is still refusing allocations.
    EXPECT_FALSE(vitreo::diagnostics::memory_brake_should_fire(
        2 * GiB, floor, 10 * GiB, physical));
    // A disabled floor disables the brake.
    EXPECT_FALSE(vitreo::diagnostics::memory_brake_should_fire(
        0, 0, 10 * GiB, physical));
}
