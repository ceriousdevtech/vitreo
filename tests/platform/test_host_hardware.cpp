// SPDX-License-Identifier: LGPL-2.1-only
//
// The host hardware profile: the pure decoders against tables read from a
// real M4 Pro's IORegistry, and the live profile against the invariants every
// Mac satisfies. Identity values (serial, UUID) are checked for shape only and
// never printed.

#include "platform/host_hardware.hpp"

#include <gtest/gtest.h>

#include <sys/sysctl.h>

#include <cstring>
#include <map>
#include <vector>

namespace vitreo::platform {
namespace {

using namespace host_hardware_detail;

std::vector<u8> dvfs_table(std::initializer_list<u32> frequencies) {
    std::vector<u8> bytes;
    for (const u32 frequency : frequencies) {
        const u32 pair[2] = {frequency, 800'000};  // voltage word is ignored
        const auto* raw = reinterpret_cast<const u8*>(pair);
        bytes.insert(bytes.end(), raw, raw + sizeof(pair));
    }
    return bytes;
}

TEST(HostHardwareDecode, M4CpuTablesAreKilohertz) {
    // pmgr voltage-states1-sram on an M4 Pro (Efficiency cluster).
    EXPECT_EQ(decode_dvfs_mhz(dvfs_table({1020000, 1404000, 1788000, 2112000, 2352000, 2532000, 2592000})),
              (std::vector<u32>{1020, 1404, 1788, 2112, 2352, 2532, 2592}));
    // voltage-states5-sram, last three states (Performance cluster).
    EXPECT_EQ(decode_dvfs_mhz(dvfs_table({4104000, 4416000, 4512000})),
              (std::vector<u32>{4104, 4416, 4512}));
}

TEST(HostHardwareDecode, HertzTablesAndOffStates) {
    // voltage-states9-sram on an M4 Pro (GPU): Hz, with a leading off state.
    EXPECT_EQ(decode_dvfs_mhz(dvfs_table({0, 338000000, 618000000, 1470000000, 1578000000})),
              (std::vector<u32>{338, 618, 1470, 1578}));
    EXPECT_TRUE(decode_dvfs_mhz(dvfs_table({0, 0})).empty());
    EXPECT_TRUE(decode_dvfs_mhz({}).empty());
    // A trailing partial pair is not a state.
    std::vector<u8> partial = dvfs_table({2592000});
    partial.resize(partial.size() + 4, 0xFF);
    EXPECT_EQ(decode_dvfs_mhz(partial), (std::vector<u32>{2592}));
}

TEST(HostHardwareDecode, ProductFamilyAndChassis) {
    EXPECT_EQ(product_family("MacBook Pro (14-inch, Nov 2024)"), "MacBook Pro");
    EXPECT_EQ(product_family("Mac mini (2024)"), "Mac mini");
    EXPECT_EQ(product_family("Mac Studio"), "Mac Studio");
    EXPECT_EQ(product_family(""), "");
    EXPECT_EQ(chassis_of("MacBook Pro"), HostChassis::Notebook);
    EXPECT_EQ(chassis_of("MacBook Air"), HostChassis::Notebook);
    EXPECT_EQ(chassis_of("iMac"), HostChassis::AllInOne);
    EXPECT_EQ(chassis_of("Mac mini"), HostChassis::MiniPc);
    EXPECT_EQ(chassis_of("Mac Studio"), HostChassis::Desktop);
    EXPECT_EQ(chassis_of("Mac Pro"), HostChassis::Tower);
    EXPECT_EQ(chassis_of(""), HostChassis::Unknown);
}

TEST(HostHardwareDecode, DeviceTreeStrings) {
    const u8 padded[] = {'M', 'i', 'c', 'r', 'o', 'n', 0, 0, 0, 0};
    EXPECT_EQ(device_tree_string(padded), "Micron");
    const u8 unterminated[] = {'L', 'P', 'D', 'D', 'R', '5'};
    EXPECT_EQ(device_tree_string(unterminated), "LPDDR5");
    EXPECT_EQ(device_tree_string({}), "");
}

TEST(HostHardwareDecode, ReportedCacheWaysKeepTheExactSize) {
    // 8-way L1 and 16-way L2 where the line count allows.
    EXPECT_EQ(reported_cache_ways(1, 131072, 128), 8u);    // 1024 lines
    EXPECT_EQ(reported_cache_ways(1, 196608, 128), 8u);    // 1536 lines: 192 sets
    EXPECT_EQ(reported_cache_ways(2, 16777216, 128), 16u);
    EXPECT_EQ(reported_cache_ways(2, 12582912, 128), 16u); // 12 MB: 6144 sets
    // Otherwise the next divisor, so ways x sets x line stays exact.
    EXPECT_EQ(reported_cache_ways(1, 9 * 128, 128), 9u);
    EXPECT_EQ(reported_cache_ways(2, 18 * 128, 128), 18u);
    EXPECT_EQ(reported_cache_ways(1, 4 * 128, 128), 4u);   // fewer lines than ways
    EXPECT_EQ(reported_cache_ways(1, 0, 128), 0u);
    EXPECT_EQ(reported_cache_ways(1, 1000, 128), 0u);
}

TEST(HostHardwareDecode, FirmwareDateFromTheDeviceTreeTimeStamp) {
    EXPECT_EQ(firmware_date("Wed Jan 28 20:42:19 PST 2026"), "01/28/2026");
    EXPECT_EQ(firmware_date("Tue Sep  2 01:02:03 PDT 2025"), "09/02/2025");  // padded day
    EXPECT_EQ(firmware_date("Wed Foo 28 20:42:19 PST 2026"), "");
    EXPECT_EQ(firmware_date("garbage"), "");
    EXPECT_EQ(firmware_date(""), "");
}

u64 sysctl_number(const char* name) {
    u64 value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return 0;
    if (size == sizeof(u32)) {
        u32 narrow = 0;
        std::memcpy(&narrow, &value, sizeof(narrow));
        return narrow;
    }
    return value;
}

TEST(HostHardwareLive, ProfileDescribesThisMac) {
    const HostHardwareProfile& host = host_hardware();
    EXPECT_FALSE(host.cpu.brand.empty());
    EXPECT_GT(host.cpu.cache_line_bytes, 0u);
    EXPECT_EQ(host.memory.total_bytes, sysctl_number("hw.memsize"));
    EXPECT_FALSE(host.machine.model_identifier.empty());

    // Every logical CPU macOS schedules on is placed, in one class each.
    ASSERT_EQ(host.cpu.processors.size(), sysctl_number("hw.logicalcpu"));
    ASSERT_FALSE(host.cpu.core_classes.empty());
    std::vector<u32> per_class(host.cpu.core_classes.size(), 0);
    std::map<std::pair<u32, u32>, u32> per_cluster;
    for (const HostLogicalProcessor& processor : host.cpu.processors) {
        ASSERT_LT(processor.core_class, host.cpu.core_classes.size());
        ++per_class[processor.core_class];
        ++per_cluster[{processor.core_class, processor.cluster}];
    }
    for (usize i = 0; i < host.cpu.core_classes.size(); ++i) {
        const HostCoreClass& core_class = host.cpu.core_classes[i];
        SCOPED_TRACE(core_class.name);
        EXPECT_EQ(per_class[i], core_class.logical_processors);
        EXPECT_GT(core_class.l1d_bytes, 0u);
        EXPECT_GT(core_class.l2_bytes, 0u);
        EXPECT_GE(core_class.max_mhz, core_class.min_mhz);
    }
    for (const auto& [cluster, count] : per_cluster) {
        EXPECT_LE(count, host.cpu.core_classes[cluster.first].cores_per_l2 *
                             std::max(1u, host.cpu.core_classes[cluster.first].logical_processors /
                                              std::max(1u, host.cpu.core_classes[cluster.first].physical_cores)));
    }

    if (sysctl_number("hw.nperflevels") > 1) {
        // Apple silicon: hybrid, clocks from pmgr, memory and GPU from the
        // device tree and the GPU driver.
        const auto performance = std::find_if(host.cpu.core_classes.begin(), host.cpu.core_classes.end(),
            [](const HostCoreClass& c) { return !c.efficiency; });
        const auto efficiency = std::find_if(host.cpu.core_classes.begin(), host.cpu.core_classes.end(),
            [](const HostCoreClass& c) { return c.efficiency; });
        ASSERT_NE(performance, host.cpu.core_classes.end());
        ASSERT_NE(efficiency, host.cpu.core_classes.end());
        EXPECT_GT(efficiency->max_mhz, 0u);
        EXPECT_GT(performance->max_mhz, efficiency->max_mhz);
        EXPECT_FALSE(host.memory.type.empty());
        EXPECT_FALSE(host.memory.manufacturer.empty());
        EXPECT_GT(host.gpu.core_count, 0u);
        EXPECT_FALSE(host.gpu.name.empty());
        EXPECT_GT(host.gpu.max_mhz, 0u);
        EXPECT_FALSE(host.machine.product_family.empty());
        EXPECT_NE(host.machine.chassis, HostChassis::Unknown);
        EXPECT_FALSE(host.machine.firmware_version.empty());
        EXPECT_EQ(host.machine.firmware_date.size(), 10u);
        EXPECT_EQ(host.machine.firmware_vendor, host.machine.manufacturer);
    }
    // Shape only: the identity itself is the user's.
    if (!host.machine.hardware_uuid.empty()) {
        ASSERT_EQ(host.machine.hardware_uuid.size(), 36u);
        for (const usize dash : {8u, 13u, 18u, 23u}) EXPECT_EQ(host.machine.hardware_uuid[dash], '-');
    }
}

TEST(HostHardwareLive, TestProfileReplacesTheHost) {
    HostHardwareProfile fixture;
    fixture.cpu.brand = "Fixture CPU";
    set_host_hardware_for_testing(&fixture);
    EXPECT_EQ(host_hardware().cpu.brand, "Fixture CPU");
    set_host_hardware_for_testing(nullptr);
    EXPECT_NE(host_hardware().cpu.brand, "Fixture CPU");
}

} // namespace
} // namespace vitreo::platform
