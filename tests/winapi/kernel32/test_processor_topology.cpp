/// @file test_processor_topology.cpp
/// @brief The processor topology kernel32 reports (GetLogicalProcessorInformation,
/// GetLogicalProcessorInformationEx, GetSystemCpuSetInformation), byte for byte.
///
/// Record layouts are winnt.h's; sizes, record order and the relationship
/// rules are tools/oracle_probe/processor_topology_probe{32,64}.expected
/// (Windows 11). Values come from a fixed M4 Pro profile.

#include <gtest/gtest.h>

#include "platform/host_hardware_fixtures.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

u32 rd32(const std::vector<u8>& b, usize at) { u32 v = 0; std::memcpy(&v, b.data() + at, 4); return v; }
u16 rd16(const std::vector<u8>& b, usize at) { u16 v = 0; std::memcpy(&v, b.data() + at, 2); return v; }
u64 rdptr(const std::vector<u8>& b, usize at, bool wide) {
    if (!wide) return rd32(b, at);
    u64 v = 0; std::memcpy(&v, b.data() + at, 8); return v;
}

struct Record { u32 relationship, size; usize at; };

std::vector<Record> walk(const std::vector<u8>& bytes) {
    std::vector<Record> records;
    for (usize at = 0; at + 8 <= bytes.size();) {
        records.push_back({rd32(bytes, at), rd32(bytes, at + 4), at});
        if (records.back().size == 0) break;
        at += records.back().size;
    }
    return records;
}

class ProcessorTopologyTest : public ::testing::TestWithParam<bool> {
protected:
    bool wide() const { return GetParam(); }
    u32 processor_size() const { return wide() ? 48 : 44; }
    u32 cache_size() const { return wide() ? 56 : 52; }
    u32 group_size() const { return wide() ? 80 : 76; }
    test::ScopedHostHardware m_host{test::m4_pro_profile()};
};

TEST_P(ProcessorTopologyTest, ModelMatchesTheHost) {
    const auto& t = processor_topology::topology();
    ASSERT_EQ(t.count, 14u);
    EXPECT_EQ(t.mask, 0x3FFFu);
    for (u32 p = 0; p < 14; ++p) {
        SCOPED_TRACE(p);
        EXPECT_EQ(t.efficiency_class[p], p < 4 ? 0u : 1u);  // P cores outrank E cores
        EXPECT_EQ(t.module_mask[p], p < 4 ? 0xFu : p < 9 ? 0x1F0u : 0x3E00u);
        EXPECT_EQ(t.max_mhz[p], p < 4 ? 2592u : 4512u);
        EXPECT_EQ(t.last_level_cache[p], t.module_mask[p]);  // no L3: the L2 is last
    }
    ASSERT_EQ(t.caches.size(), 14u * 2 + 3);
}

TEST_P(ProcessorTopologyTest, LegacyRecords) {
    const auto bytes = processor_topology::logical_processor_information(wide());
    const usize size = wide() ? 32 : 24, relation_at = wide() ? 8 : 4, union_at = wide() ? 16 : 8;
    ASSERT_EQ(bytes.size() % size, 0u);
    ASSERT_EQ(bytes.size() / size, 14u + 31u + 2u);  // cores, caches, package, NUMA node
    // Record 0: core 0. Then its L1D, L1I, and the E cluster's L2.
    EXPECT_EQ(rdptr(bytes, 0, wide()), 1u);
    EXPECT_EQ(rd32(bytes, relation_at), 0u);
    EXPECT_EQ(bytes[union_at], 0u);  // Flags: no SMT
    auto cache = [&](usize index, u64 mask, u8 level, u8 assoc, u32 cache_bytes, u32 type) {
        SCOPED_TRACE(index);
        const usize o = index * size;
        EXPECT_EQ(rdptr(bytes, o, wide()), mask);
        EXPECT_EQ(rd32(bytes, o + relation_at), 2u);
        EXPECT_EQ(bytes[o + union_at], level);
        EXPECT_EQ(bytes[o + union_at + 1], assoc);
        EXPECT_EQ(rd16(bytes, o + union_at + 2), 128u);
        EXPECT_EQ(rd32(bytes, o + union_at + 4), cache_bytes);
        EXPECT_EQ(rd32(bytes, o + union_at + 8), type);
    };
    cache(1, 0x1, 1, 8, 65536, 2);
    cache(2, 0x1, 1, 8, 131072, 1);
    cache(3, 0xF, 2, 16, 4194304, 0);
    const usize last = bytes.size() - size;
    EXPECT_EQ(rd32(bytes, last - size + relation_at), 3u);  // package
    EXPECT_EQ(rdptr(bytes, last - size, wide()), 0x3FFFu);
    EXPECT_EQ(rd32(bytes, last + relation_at), 1u);         // NUMA node 0
    EXPECT_EQ(rd32(bytes, last + union_at), 0u);
}

TEST_P(ProcessorTopologyTest, ExtendedRecordsPerRelationship) {
    struct Expect { u32 relationship; u32 count; u32 record_relationship; };
    for (const Expect e : {Expect{0, 14, 0}, Expect{1, 1, 1}, Expect{3, 1, 3}, Expect{5, 1, 5},
                           Expect{6, 1, 1}, Expect{7, 3, 7}}) {
        SCOPED_TRACE(e.relationship);
        const auto bytes = processor_topology::logical_processor_information_ex(e.relationship, wide());
        ASSERT_TRUE(bytes.has_value());
        const auto records = walk(*bytes);
        ASSERT_EQ(records.size(), e.count);
        for (const Record& r : records) {
            EXPECT_EQ(r.relationship, e.record_relationship);
            EXPECT_EQ(r.size, processor_size());
            EXPECT_EQ(rd16(*bytes, r.at + 30), 1u);  // GroupCount
            EXPECT_EQ(rd16(*bytes, r.at + 32 + (wide() ? 8 : 4)), 0u);  // group 0
        }
    }
    // Cores carry their efficiency class; modules are the clusters.
    const auto cores = *processor_topology::logical_processor_information_ex(0, wide());
    for (u32 p = 0; p < 14; ++p) {
        const usize at = p * processor_size();
        EXPECT_EQ(cores[at + 8], 0u);                      // Flags: no SMT
        EXPECT_EQ(cores[at + 9], p < 4 ? 0u : 1u);         // EfficiencyClass
        EXPECT_EQ(rdptr(cores, at + 32, wide()), 1ull << p);
    }
    const auto modules = *processor_topology::logical_processor_information_ex(7, wide());
    EXPECT_EQ(rdptr(modules, 32, wide()), 0xFu);
    EXPECT_EQ(rdptr(modules, processor_size() + 32, wide()), 0x1F0u);
    EXPECT_EQ(rdptr(modules, 2 * processor_size() + 32, wide()), 0x3E00u);
    // Caches: CACHE_RELATIONSHIP with GroupCount at 38 and the mask at 40.
    const auto caches = *processor_topology::logical_processor_information_ex(2, wide());
    const auto cache_records = walk(caches);
    ASSERT_EQ(cache_records.size(), 31u);
    const Record& p_l2 = cache_records[4 * 2 + 1 + 2];  // four E cores' L1s, the E L2, core 4's L1s
    EXPECT_EQ(p_l2.size, cache_size());
    EXPECT_EQ(caches[p_l2.at + 8], 2u);
    EXPECT_EQ(caches[p_l2.at + 9], 16u);
    EXPECT_EQ(rd16(caches, p_l2.at + 10), 128u);
    EXPECT_EQ(rd32(caches, p_l2.at + 12), 16777216u);
    EXPECT_EQ(rd32(caches, p_l2.at + 16), 0u);  // CacheUnified
    EXPECT_EQ(rd16(caches, p_l2.at + 38), 1u);
    EXPECT_EQ(rdptr(caches, p_l2.at + 40, wide()), 0x1F0u);
    // Group: one group of 14.
    const auto group = *processor_topology::logical_processor_information_ex(4, wide());
    ASSERT_EQ(group.size(), group_size());
    EXPECT_EQ(rd16(group, 8), 1u);
    EXPECT_EQ(rd16(group, 10), 1u);
    EXPECT_EQ(group[32], 14u);
    EXPECT_EQ(group[33], 14u);
    EXPECT_EQ(rdptr(group, 72, wide()), 0x3FFFu);
    // Anything else is refused (ERROR_GEN_FAILURE at the export).
    EXPECT_FALSE(processor_topology::logical_processor_information_ex(8, wide()).has_value());
    EXPECT_FALSE(processor_topology::logical_processor_information_ex(0x100, wide()).has_value());
}

TEST_P(ProcessorTopologyTest, RelationAllOrder) {
    // Package; then per core: Core, Module where one starts, caches first seen
    // there; then the NUMA node and the group. No Die record.
    const auto bytes = *processor_topology::logical_processor_information_ex(0xFFFF, wide());
    std::string order;
    for (const Record& r : walk(bytes)) order += static_cast<char>('0' + r.relationship);
    std::string expected = "3";
    for (u32 p = 0; p < 14; ++p) {
        expected += "0";
        if (p == 0 || p == 4 || p == 9) expected += "7";
        expected += "22";
        if (p == 0 || p == 4 || p == 9) expected += "2";
    }
    expected += "14";
    EXPECT_EQ(order, expected);
    EXPECT_EQ(bytes.size(), processor_size() * (1 + 14 + 3 + 1) + cache_size() * 31 + group_size());
}

TEST_P(ProcessorTopologyTest, CpuSets) {
    const auto bytes = processor_topology::cpu_set_information();
    ASSERT_EQ(bytes.size(), 14u * 32);
    const u8 llc[14] = {0, 0, 0, 0, 4, 4, 4, 4, 4, 9, 9, 9, 9, 9};
    for (u32 p = 0; p < 14; ++p) {
        SCOPED_TRACE(p);
        const usize o = p * 32;
        EXPECT_EQ(rd32(bytes, o + 0), 32u);
        EXPECT_EQ(rd32(bytes, o + 4), 0u);         // CpuSetInformation
        EXPECT_EQ(rd32(bytes, o + 8), 256u + p);   // Id
        EXPECT_EQ(rd16(bytes, o + 12), 0u);        // Group
        EXPECT_EQ(bytes[o + 14], p);               // LogicalProcessorIndex
        EXPECT_EQ(bytes[o + 15], p);               // CoreIndex
        EXPECT_EQ(bytes[o + 16], llc[p]);          // LastLevelCacheIndex
        EXPECT_EQ(bytes[o + 17], 0u);              // NumaNodeIndex
        EXPECT_EQ(bytes[o + 18], p < 4 ? 0u : 1u); // EfficiencyClass
        EXPECT_EQ(bytes[o + 19], 0u);              // flags: nothing parked or allocated
    }
}

INSTANTIATE_TEST_SUITE_P(BothAbis, ProcessorTopologyTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
