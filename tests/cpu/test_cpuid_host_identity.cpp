/// @file test_cpuid_host_identity.cpp
/// @brief CPUID identity, cache, topology and clock leaves describe the host
/// machine (Intel SDM Vol. 2A, CPUID).
///
/// The profile is a fixed Apple M4 Pro, as macOS reports it (sysctl
/// hw.perflevel*, IODeviceTree:/cpus, pmgr): 4 Efficiency cores (CPUs 0-3, one
/// cluster) and 10 Performance cores in two clusters of 5 (CPUs 4-8, 9-13).
/// Expected register values are the SDM encodings of those facts.

#include "core/cpu/cpuid_synth.hpp"
#include "platform/host_hardware.hpp"
#include "platform/host_hardware_fixtures.hpp"

#include <gtest/gtest.h>

#include <vitreo/virtual_tsc.hpp>

#include <cstring>
#include <map>
#include <set>
#include <string>

namespace vitreo::cpu {
namespace {

struct Regs { u32 eax = 0, ebx = 0, ecx = 0, edx = 0; };

class CpuidHostIdentityTest : public ::testing::Test {
protected:
    void SetUp() override { platform::set_host_hardware_for_testing(&m_host); }
    void TearDown() override { platform::set_host_hardware_for_testing(nullptr); }

    Regs cpuid(u32 processor, u32 leaf, u32 subleaf = 0) const {
        Regs r;
        ops::synth_cpuid_on(processor, leaf, subleaf, true, r.eax, r.ebx, r.ecx, r.edx);
        return r;
    }

    /// Leaf 4 size: (Ways + 1) * (Partitions + 1) * (Line + 1) * (Sets + 1).
    static u64 cache_size(const Regs& r) {
        return u64{((r.ebx >> 22) & 0x3FF) + 1} * u64{((r.ebx >> 12) & 0x3FF) + 1} *
               u64{(r.ebx & 0xFFF) + 1} * (u64{r.ecx} + 1);
    }

    platform::HostHardwareProfile m_host = test::m4_pro_profile();
};

TEST_F(CpuidHostIdentityTest, HighestLeavesCoverTheHostLeaves) {
    EXPECT_EQ(cpuid(0, 0).eax, 0x1Fu);
    EXPECT_EQ(cpuid(0, 0x80000000).eax, 0x80000007u);
}

TEST_F(CpuidHostIdentityTest, BrandStringIsTheHostProcessor) {
    char brand[49] = {};
    for (u32 i = 0; i < 3; ++i) {
        const Regs r = cpuid(0, 0x80000002 + i);
        const u32 words[4] = {r.eax, r.ebx, r.ecx, r.edx};
        std::memcpy(brand + 16 * i, words, 16);
    }
    EXPECT_STREQ(brand, "Apple M4 Pro");
}

TEST_F(CpuidHostIdentityTest, ApicIdsGiveEachClusterAnAlignedBlock) {
    // Clusters of at most 5 cores get 8-ID blocks: E 0-3, P 8-12, P 16-20.
    const u32 expected[14] = {0, 1, 2, 3, 8, 9, 10, 11, 12, 16, 17, 18, 19, 20};
    for (u32 cpu = 0; cpu < 14; ++cpu) {
        SCOPED_TRACE(cpu);
        const Regs leaf1 = cpuid(cpu, 1);
        EXPECT_EQ(leaf1.ebx >> 24, expected[cpu]);           // initial APIC ID
        EXPECT_EQ((leaf1.ebx >> 16) & 0xFF, 32u);            // addressable IDs
        EXPECT_EQ((leaf1.ebx >> 8) & 0xFF, 16u);             // CLFLUSH: 16 x 8 = 128 bytes
        EXPECT_NE(leaf1.edx & (1u << 28), 0u);               // HTT
        EXPECT_EQ(leaf1.eax, 0x000306C3u);
        EXPECT_EQ(cpuid(cpu, 0x0B, 0).edx, expected[cpu]);   // x2APIC ID
        EXPECT_EQ(cpuid(cpu, 0x1F, 2).edx, expected[cpu]);
    }
}

TEST_F(CpuidHostIdentityTest, ExtendedTopologyLevels) {
    // Leaf 0xB: SMT (1 per core, shift 0), Core (14 in the package, shift 5).
    Regs r = cpuid(5, 0x0B, 0);
    EXPECT_EQ(r.eax, 0u); EXPECT_EQ(r.ebx, 1u); EXPECT_EQ(r.ecx, 0x100u);
    r = cpuid(5, 0x0B, 1);
    EXPECT_EQ(r.eax, 5u); EXPECT_EQ(r.ebx, 14u); EXPECT_EQ(r.ecx, 0x201u);
    r = cpuid(5, 0x0B, 2);
    EXPECT_EQ(r.eax, 0u); EXPECT_EQ(r.ebx, 0u); EXPECT_EQ(r.ecx, 2u);  // invalid level
    // Leaf 0x1F adds Module: cores of one cluster (at most 5, shift 3), then
    // the package (shift 5).
    r = cpuid(5, 0x1F, 1);
    EXPECT_EQ(r.eax, 3u); EXPECT_EQ(r.ebx, 5u); EXPECT_EQ(r.ecx, 0x201u);
    r = cpuid(5, 0x1F, 2);
    EXPECT_EQ(r.eax, 5u); EXPECT_EQ(r.ebx, 14u); EXPECT_EQ(r.ecx, 0x302u);
    r = cpuid(5, 0x1F, 3);
    EXPECT_EQ(r.eax, 0u); EXPECT_EQ(r.ebx, 0u); EXPECT_EQ(r.ecx, 3u);
}

TEST_F(CpuidHostIdentityTest, Leaf4ReportsTheRunningCoresCaches) {
    struct Expect { u32 cpu; u64 l1d, l1i, l2; };
    for (const Expect e : {Expect{6, 131072, 196608, 16777216}, Expect{2, 65536, 131072, 4194304}}) {
        SCOPED_TRACE(e.cpu);
        const Regs l1d = cpuid(e.cpu, 4, 0);
        EXPECT_EQ(l1d.eax & 0x1F, 1u);                // data
        EXPECT_EQ((l1d.eax >> 5) & 7, 1u);            // level 1
        EXPECT_NE(l1d.eax & (1u << 8), 0u);           // self-initializing
        EXPECT_EQ(l1d.eax & (1u << 9), 0u);           // set associative
        EXPECT_EQ(((l1d.ebx >> 22) & 0x3FF) + 1, 8u);  // reported 8-way
        EXPECT_EQ(((l1d.ebx >> 12) & 0x3FF), 0u);      // one partition
        EXPECT_EQ((l1d.eax >> 14) & 0xFFF, 0u);       // private to the core
        EXPECT_EQ(l1d.eax >> 26, 31u);                // 32 addressable core IDs
        EXPECT_EQ((l1d.ebx & 0xFFF) + 1, 128u);       // line size
        EXPECT_EQ(cache_size(l1d), e.l1d);
        const Regs l1i = cpuid(e.cpu, 4, 1);
        EXPECT_EQ(l1i.eax & 0x1F, 2u);                // instruction
        EXPECT_EQ(cache_size(l1i), e.l1i);
        const Regs l2 = cpuid(e.cpu, 4, 2);
        EXPECT_EQ(l2.eax & 0x1F, 3u);                 // unified
        EXPECT_EQ((l2.eax >> 5) & 7, 2u);
        EXPECT_EQ((l2.eax >> 14) & 0xFFF, 7u);        // shared by an 8-ID cluster block
        EXPECT_EQ(((l2.ebx >> 22) & 0x3FF) + 1, 16u);  // reported 16-way
        EXPECT_EQ(cache_size(l2), e.l2);
        EXPECT_EQ(cpuid(e.cpu, 4, 3).eax, 0u);        // no L3 on Apple silicon
        const Regs ext = cpuid(e.cpu, 0x80000006);
        EXPECT_EQ(ext.ecx & 0xFF, 128u);
        EXPECT_EQ((ext.ecx >> 12) & 0xF, 0x8u);       // 16 ways
        EXPECT_EQ(ext.ecx >> 16, e.l2 / 1024);
    }
}

TEST_F(CpuidHostIdentityTest, L2SharingMaskSelectsExactlyTheHostClusters) {
    // Processors whose APIC IDs agree above the leaf 4 L2 sharing bits are
    // the ones the host says share an L2.
    std::map<u32, std::set<u32>> by_l2;
    for (u32 cpu = 0; cpu < 14; ++cpu) {
        const u32 sharing = ((cpuid(cpu, 4, 2).eax >> 14) & 0xFFF) + 1;
        by_l2[(cpuid(cpu, 1).ebx >> 24) / sharing].insert(cpu);
    }
    ASSERT_EQ(by_l2.size(), 3u);
    EXPECT_EQ(by_l2[0], (std::set<u32>{0, 1, 2, 3}));
    EXPECT_EQ(by_l2[1], (std::set<u32>{4, 5, 6, 7, 8}));
    EXPECT_EQ(by_l2[2], (std::set<u32>{9, 10, 11, 12, 13}));
}

TEST_F(CpuidHostIdentityTest, ClockLeavesDescribeTheActualTsc) {
    // Leaf 0x15: TSC = crystal x EBX / EAX, and it is the rate RDTSC runs at.
    const Regs tsc = cpuid(0, 0x15);
    ASSERT_NE(tsc.eax, 0u);
    EXPECT_EQ(tsc.ecx, 24'000'000u);
    EXPECT_EQ(u64{tsc.ecx} * tsc.ebx / tsc.eax, guest_tsc_hz());
    // Leaf 0x16: base = the TSC rate, maximum = the fastest core, reference
    // = the crystal.
    const Regs mhz = cpuid(0, 0x16);
    EXPECT_EQ(mhz.eax, guest_tsc_hz() / 1'000'000);
    EXPECT_EQ(mhz.ebx, 4512u);
    EXPECT_EQ(mhz.ecx, 24u);
    EXPECT_NE(cpuid(0, 0x80000007).edx & (1u << 8), 0u);  // invariant TSC
}

TEST_F(CpuidHostIdentityTest, LeafTwoDefersToLeafFour) {
    const Regs r = cpuid(0, 2);
    EXPECT_EQ(r.eax, 0x0000FF01u);
    EXPECT_EQ(r.ebx | r.ecx | r.edx, 0u);
}

TEST(CpuidHostClock, GuestTscIsTheHostCoreClock) {
    const u64 host = platform::host_max_core_hz();
    EXPECT_EQ(guest_tsc_hz(), host != 0 ? host : kFallbackGuestTscHz);
}

} // namespace
} // namespace vitreo::cpu
