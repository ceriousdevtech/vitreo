/// @file test_memory.cpp
/// @brief Unit tests for memory management

#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <set>
#include <thread>
#include <unistd.h>

#include "../../src/core/memory/memory_region.hpp"
#include "../../src/core/memory/guest_memory.hpp"

using namespace vitreo;

// Helper to get system page size
static usize get_page_size() {
    return static_cast<usize>(getpagesize());
}

// =============================================================================
// MemoryRegion Tests
// =============================================================================

TEST(MemoryRegionTest, AllocateSuccess) {
    auto result = MemoryRegion::allocate(4096, MemoryProtection::ReadWrite);
    ASSERT_TRUE(result.is_ok());
    
    auto& region = result.value();
    EXPECT_TRUE(region.is_valid());
    EXPECT_GE(region.size(), 4096u);
    EXPECT_NE(region.data(), nullptr);
}

TEST(MemoryRegionTest, AllocateZeroSizeFails) {
    auto result = MemoryRegion::allocate(0);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::InvalidArgument);
}

TEST(MemoryRegionTest, AllocateRoundsUpToPageSize) {
    auto result = MemoryRegion::allocate(1);
    ASSERT_TRUE(result.is_ok());
    
    // Should be at least one page
    EXPECT_GE(result.value().size(), get_page_size());
}

TEST(MemoryRegionTest, MoveConstructor) {
    auto result = MemoryRegion::allocate(4096);
    ASSERT_TRUE(result.is_ok());
    
    Byte* original_ptr = result.value().data();
    MemoryRegion moved(std::move(result.value()));
    
    EXPECT_TRUE(moved.is_valid());
    EXPECT_EQ(moved.data(), original_ptr);
}

TEST(MemoryRegionTest, MoveAssignment) {
    auto result1 = MemoryRegion::allocate(4096);
    auto result2 = MemoryRegion::allocate(8192);
    ASSERT_TRUE(result1.is_ok());
    ASSERT_TRUE(result2.is_ok());
    
    Byte* ptr2 = result2.value().data();
    result1.value() = std::move(result2.value());
    
    EXPECT_TRUE(result1.value().is_valid());
    EXPECT_EQ(result1.value().data(), ptr2);
}

TEST(MemoryRegionTest, ContainsOffset) {
    auto result = MemoryRegion::allocate(4096);
    ASSERT_TRUE(result.is_ok());
    
    auto& region = result.value();
    EXPECT_TRUE(region.contains(0));
    EXPECT_TRUE(region.contains(region.size() - 1));
    EXPECT_FALSE(region.contains(region.size()));
}

TEST(MemoryRegionTest, ContainsRange) {
    auto result = MemoryRegion::allocate(4096);
    ASSERT_TRUE(result.is_ok());
    
    auto& region = result.value();
    usize size = region.size();  // May be larger than 4096 due to page rounding
    
    EXPECT_TRUE(region.contains(0, size));
    EXPECT_TRUE(region.contains(100, 100));
    EXPECT_FALSE(region.contains(size - 100, 200));  // Exceeds end
}

TEST(MemoryRegionTest, SetProtection) {
    auto result = MemoryRegion::allocate(4096, MemoryProtection::ReadWrite);
    ASSERT_TRUE(result.is_ok());
    
    auto& region = result.value();
    EXPECT_EQ(region.protection(), MemoryProtection::ReadWrite);
    
    auto prot_result = region.set_protection(MemoryProtection::Read);
    EXPECT_TRUE(prot_result.is_ok());
    EXPECT_EQ(region.protection(), MemoryProtection::Read);
}

TEST(MemoryRegionTest, WriteReadData) {
    auto result = MemoryRegion::allocate(4096, MemoryProtection::ReadWrite);
    ASSERT_TRUE(result.is_ok());
    
    auto& region = result.value();
    
    // Write data
    region.data()[0] = 0xAB;
    region.data()[1] = 0xCD;
    
    // Read back
    EXPECT_EQ(region.data()[0], 0xAB);
    EXPECT_EQ(region.data()[1], 0xCD);
}

// =============================================================================
// GuestMemory Tests
// =============================================================================

TEST(GuestMemoryTest, MapRegion) {
    GuestMemory mem;
    
    auto result = mem.map_region(0x10000, 4096, MemoryProtection::ReadWrite);
    EXPECT_TRUE(result.is_ok());
    
    EXPECT_TRUE(mem.is_mapped(0x10000));
    EXPECT_TRUE(mem.is_mapped(0x10FFF));
    EXPECT_FALSE(mem.is_mapped(0x11000));
    EXPECT_EQ(mem.region_count(), 1u);
}

TEST(GuestMemoryTest, MapOverlappingFails) {
    GuestMemory mem;
    
    // Map a 8KB region starting at 0x10000 (range: 0x10000-0x12000)
    EXPECT_TRUE(mem.map_region(0x10000, 8192, MemoryProtection::ReadWrite).is_ok());
    
    // Try to map at 0x11000 (4KB aligned) which overlaps with 0x10000-0x12000
    auto result = mem.map_region(0x11000, 4096, MemoryProtection::ReadWrite);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::AlreadyExists);
}

TEST(GuestMemoryTest, MapUnalignedFails) {
    GuestMemory mem;
    
    // Not page-aligned
    auto result = mem.map_region(0x10001, 4096, MemoryProtection::ReadWrite);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::InvalidArgument);
}

TEST(GuestMemoryTest, UnmapRegion) {
    GuestMemory mem;
    
    EXPECT_TRUE(mem.map_region(0x10000, 4096, MemoryProtection::ReadWrite).is_ok());
    EXPECT_TRUE(mem.is_mapped(0x10000));
    
    // Note: unmap requires aligned_up size (4096)
    EXPECT_TRUE(mem.unmap_region(0x10000, 4096).is_ok());
    EXPECT_FALSE(mem.is_mapped(0x10000));
}

TEST(GuestMemoryTest, TranslateAddress) {
    GuestMemory mem;
    
    EXPECT_TRUE(mem.map_region(0x10000, 4096, MemoryProtection::ReadWrite).is_ok());
    
    auto result = mem.translate(0x10100);
    EXPECT_TRUE(result.is_ok());
    EXPECT_NE(result.value(), nullptr);
    
    // Unmapped address
    auto bad_result = mem.translate(0x20000);
    EXPECT_TRUE(bad_result.is_err());
    EXPECT_EQ(bad_result.error(), Error::InvalidAddress);
}

TEST(GuestMemoryTest, ReadWriteU8) {
    GuestMemory mem;
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    
    EXPECT_TRUE(mem.write_u8(0x1000, 0xAB).is_ok());
    
    auto result = mem.read_u8(0x1000);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 0xAB);
}

TEST(GuestMemoryTest, ReadWriteU16) {
    GuestMemory mem;
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    
    EXPECT_TRUE(mem.write_u16(0x1000, 0xABCD).is_ok());
    
    auto result = mem.read_u16(0x1000);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 0xABCD);
    
    // Verify little-endian storage
    auto byte0 = mem.read_u8(0x1000);
    auto byte1 = mem.read_u8(0x1001);
    EXPECT_EQ(byte0.value(), 0xCD);  // Low byte first
    EXPECT_EQ(byte1.value(), 0xAB);  // High byte second
}

TEST(GuestMemoryTest, ReadWriteU32) {
    GuestMemory mem;
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    
    EXPECT_TRUE(mem.write_u32(0x1000, 0x12345678).is_ok());
    
    auto result = mem.read_u32(0x1000);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 0x12345678u);
}

TEST(GuestMemoryTest, ReadWriteU64) {
    GuestMemory mem;
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    
    EXPECT_TRUE(mem.write_u64(0x1000, 0x123456789ABCDEF0ULL).is_ok());
    
    auto result = mem.read_u64(0x1000);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 0x123456789ABCDEF0ULL);
}

TEST(GuestMemoryTest, ReadWriteBytes) {
    GuestMemory mem;
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    
    Byte data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_TRUE(mem.write_bytes(0x1000, data, sizeof(data)).is_ok());
    
    Byte buffer[5] = {};
    EXPECT_TRUE(mem.read_bytes(0x1000, buffer, sizeof(buffer)).is_ok());
    
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(buffer[i], data[i]);
    }
}

TEST(GuestMemoryTest, WritesNotifyObserverOnlyForExecutableMappings) {
    GuestMemory mem;
    constexpr GuestAddr data_base = 0x1000;
    constexpr GuestAddr code_base = 0x3000;
    ASSERT_TRUE(mem.map_region(
        data_base, 4096, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.map_region(
        code_base, 4096, MemoryProtection::ReadWriteExecute).is_ok());

    u32 notify_count = 0;
    GuestAddr seen_addr = 0;
    usize seen_size = 0;
        mem.set_thread_executable_write_observer(
        [&](GuestAddr addr, usize size, u64) {
            ++notify_count;
            seen_addr = addr;
            seen_size = size;
            return true;
        });

    const Byte bytes[] = {0x11, 0x22, 0x33, 0x44};
    ASSERT_TRUE(mem.write_u32(data_base, 0x12345678).is_ok());
    ASSERT_TRUE(mem.write_bytes(data_base + 8, bytes, sizeof(bytes)).is_ok());
    ASSERT_NE(mem.host_ptr_mut(data_base + 16, sizeof(u32)), nullptr);
    EXPECT_EQ(notify_count, 0u);

    ASSERT_TRUE(mem.write_u32(code_base, 0xAABBCCDD).is_ok());
    EXPECT_EQ(notify_count, 1u);
    EXPECT_EQ(seen_addr, code_base);
    EXPECT_EQ(seen_size, sizeof(u32));

    ASSERT_TRUE(mem.write_bytes(code_base + 8, bytes, sizeof(bytes)).is_ok());
    EXPECT_EQ(notify_count, 2u);
    EXPECT_EQ(seen_addr, code_base + 8);
    EXPECT_EQ(seen_size, sizeof(bytes));

    ASSERT_NE(mem.host_ptr_mut(code_base + 16, sizeof(u32)), nullptr);
    EXPECT_EQ(notify_count, 3u);
    EXPECT_EQ(seen_addr, code_base + 16);
    EXPECT_EQ(seen_size, sizeof(u32));

    mem.clear_thread_executable_write_observer();
}

// The complement of the test above. guest_accel.cpp and native32/native64
// _prepare patch code by dropping the page to ReadWrite FIRST, which makes
// every write silent -- write_bytes() only notifies when the resolved page is
// already executable, and the raw linear_host() stub-emit loops bypass the
// write path entirely. The transition back to executable is therefore the
// only signal the block cache can receive for a patched sled.
TEST(GuestMemoryTest, ProtectToExecutableNotifiesForPatchedRange) {
    GuestMemory mem;
    constexpr GuestAddr code_base = 0x3000;
    ASSERT_TRUE(mem.map_region(code_base, 4096,
                               MemoryProtection::ReadWrite).is_ok());
    const Byte sled[] = {0x90, 0x90, 0x90, 0x90, 0x90};
    ASSERT_TRUE(mem.write_bytes(code_base, sled, sizeof(sled)).is_ok());
    ASSERT_TRUE(mem.protect_region(code_base, 4096,
                                   MemoryProtection::ReadExecute).is_ok());

    u32 notify_count = 0;
    GuestAddr seen_addr = 0;
    usize seen_size = 0;
    mem.set_thread_executable_write_observer(
        [&](GuestAddr addr, usize size, u64) {
            ++notify_count;
            seen_addr = addr;
            seen_size = size;
            return true;
        });

    // Exactly install_guest_crt_accelerators' sequence.
    ASSERT_TRUE(mem.protect_region(code_base, 4096,
                                   MemoryProtection::ReadWrite).is_ok());
    EXPECT_EQ(notify_count, 0u) << "ReadWrite is not a publish point";
    const Byte jump[] = {0xE9, 0x11, 0x22, 0x33, 0x44};
    ASSERT_TRUE(mem.write_bytes(code_base, jump, sizeof(jump)).is_ok());
    EXPECT_EQ(notify_count, 0u) << "page is not executable during the patch";

    ASSERT_TRUE(mem.protect_region(code_base, 4096,
                                   MemoryProtection::ReadExecute).is_ok());
    EXPECT_EQ(notify_count, 1u) << "restoring execute must publish the patch";
    EXPECT_EQ(seen_addr, code_base);
    EXPECT_EQ(seen_size, 4096u);

    mem.clear_thread_executable_write_observer();
}

// A protect_region that does not grant Execute must stay silent, so the fix
// above cannot be satisfied by notifying unconditionally.
TEST(GuestMemoryTest, ProtectToNonExecutableDoesNotNotify) {
    GuestMemory mem;
    constexpr GuestAddr code_base = 0x3000;
    ASSERT_TRUE(mem.map_region(code_base, 4096,
                               MemoryProtection::ReadExecute).is_ok());

    u32 notify_count = 0;
    mem.set_thread_executable_write_observer(
        [&](GuestAddr, usize, u64) {
            ++notify_count;
            return true;
        });

    ASSERT_TRUE(mem.protect_region(code_base, 4096,
                                   MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.protect_region(code_base, 4096,
                                   MemoryProtection::Read).is_ok());
    EXPECT_EQ(notify_count, 0u);

    mem.clear_thread_executable_write_observer();
}

TEST(GuestMemoryTest, ReadUnmappedFails) {
    GuestMemory mem;
    
    auto result = mem.read_u32(0x1000);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::InvalidAddress);
}

TEST(GuestMemoryTest, WriteToReadOnlyFails) {
    GuestMemory mem;
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::Read).is_ok());
    
    auto result = mem.write_u8(0x1000, 0xAB);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::ProtectionFault);

    const Byte xmm[16] = {};
    auto bulk = mem.write_bytes(0x1000, xmm, sizeof(xmm));
    EXPECT_TRUE(bulk.is_err());
    EXPECT_EQ(bulk.error(), Error::ProtectionFault);
}

TEST(GuestMemoryTest, WriteBytesRejectsSpanIntoGuard) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x20000;
    ASSERT_TRUE(mem.map_region(
        base, 0x2000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.protect_region(
        base + 0x1000, 0x1000, MemoryProtection::None).is_ok());

    Byte block[16] = {};
    auto crossing = mem.write_bytes(base + 0x1000 - 8, block, sizeof(block));
    EXPECT_TRUE(crossing.is_err());
    EXPECT_EQ(crossing.error(), Error::ProtectionFault);

    EXPECT_TRUE(mem.write_bytes(base + 0x0FF0, block, sizeof(block)).is_ok());
}

TEST(GuestMemoryTest, WriteBytesSpansAdjacentRegionsAndRejectsHoles) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x140000000;
    ASSERT_TRUE(mem.map_region(base, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.map_region(base + 0x4000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    const Byte bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(mem.write_bytes(base + 0x3FFC, bytes, sizeof(bytes)).is_ok());
    for (usize i = 0; i < sizeof(bytes); ++i) {
        EXPECT_EQ(mem.read_u8(base + 0x3FFC + i).value(), bytes[i]);
    }
    EXPECT_TRUE(mem.write_bytes(base + 0x7FFC, bytes, sizeof(bytes)).is_err());
    EXPECT_EQ(mem.read_u8(base + 0x7FFC).value(), 0);
    ASSERT_TRUE(mem.protect_region(base + 0x4000, 0x4000, MemoryProtection::Read).is_ok());
    const Byte replacement[8] = {};
    EXPECT_EQ(mem.write_bytes(base + 0x3FFC, replacement, sizeof(replacement)).error(),
              Error::ProtectionFault);
    EXPECT_EQ(mem.read_u8(base + 0x3FFC).value(), 1);
}

TEST(GuestMemoryTest, ScalarAndBulkReadsCrossAdjacentRegions) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x140000000;
    ASSERT_TRUE(mem.map_region(base, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.map_region(base + 0x4000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.write_u64(base + 0x3FFC, 0x123456789ABCDEF0ULL).is_ok());
    EXPECT_EQ(mem.read_u64(base + 0x3FFC).value(), 0x123456789ABCDEF0ULL);
    ASSERT_TRUE(mem.write_u32(base + 0x3FFE, 0x87654321).is_ok());
    EXPECT_EQ(mem.read_u32(base + 0x3FFE).value(), 0x87654321u);
    ASSERT_TRUE(mem.write_u16(base + 0x3FFF, 0xABCD).is_ok());
    EXPECT_EQ(mem.read_u16(base + 0x3FFF).value(), 0xABCDu);
    Byte bytes[8] = {};
    ASSERT_TRUE(mem.read_bytes(base + 0x3FFC, bytes, sizeof(bytes)).is_ok());
    EXPECT_EQ(bytes[3], 0xCD);
    EXPECT_EQ(bytes[4], 0xAB);
    ASSERT_TRUE(mem.protect_region(base + 0x4000, 0x4000, MemoryProtection::None).is_ok());
    EXPECT_EQ(mem.read_u64(base + 0x3FFC).error(), Error::ProtectionFault);
    std::fill(std::begin(bytes), std::end(bytes), 0xCC);
    EXPECT_TRUE(mem.read_bytes(base + 0x3FFC, bytes, sizeof(bytes)).is_err());
    for (const auto byte : bytes) EXPECT_EQ(byte, 0xCC);
    ASSERT_TRUE(mem.unmap_region(base + 0x4000, 0x4000).is_ok());
    EXPECT_EQ(mem.read_u64(base + 0x3FFC).error(), Error::InvalidAddress);
}

TEST(GuestMemoryTest, FragmentedRangeUpdatesMatchPageModel) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x200000;
    constexpr usize page = 0x4000;
    constexpr usize count = 32;
    std::array<MemoryProtection, count> expected{};
    expected.fill(MemoryProtection::ReadWrite);
    ASSERT_TRUE(mem.map_region(base, count * page,
                               MemoryProtection::ReadWrite).is_ok());
    u32 seed = 0x12345678;
    const auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 16;
    };
    for (usize step = 0; step < 500; ++step) {
        const usize first = next() % count;
        const usize length = 1 + next() % (count - first);
        const unsigned operation = next() % 3;
        MemoryProtection protection;
        if (operation == 0) {
            protection = MemoryProtection::Read;
            ASSERT_TRUE(mem.protect_region(base + first * page,
                length * page, protection).is_ok());
        } else if (operation == 1) {
            protection = MemoryProtection::ReadWrite;
            ASSERT_TRUE(mem.commit_region(base + first * page,
                length * page, protection).is_ok());
        } else {
            protection = MemoryProtection::None;
            ASSERT_TRUE(mem.decommit_region(base + first * page,
                length * page).is_ok());
        }
        std::fill(expected.begin() + first,
                  expected.begin() + first + length, protection);
        // Internal empty updates must not introduce or remove boundaries.
        ASSERT_TRUE(mem.commit_region(base + first * page, 0,
                                       MemoryProtection::ReadWrite).is_ok());
        for (usize i = 0; i < count; ++i) {
            usize left = i, right = i + 1;
            while (left > 0 && expected[left - 1] == expected[i]) --left;
            while (right < count && expected[right] == expected[i]) ++right;
            GuestAddr actual_base = 0;
            usize actual_size = 0;
            MemoryProtection actual_protection{};
            ASSERT_TRUE(mem.query_region(base + i * page, actual_base,
                                          actual_size, actual_protection));
            ASSERT_EQ(actual_base, base + left * page) << "step " << step;
            ASSERT_EQ(actual_size, (right - left) * page) << "step " << step;
            ASSERT_EQ(actual_protection, expected[i]) << "step " << step;
        }
    }
}

TEST(GuestMemoryTest, PartialProtectionCreatesIndependentSpans) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x20000;
    ASSERT_TRUE(mem.map_region(
        base, 0x3000, MemoryProtection::ReadWrite).is_ok());

    ASSERT_TRUE(mem.protect_region(
        base + 0x1000, 0x1000, MemoryProtection::None).is_ok());

    EXPECT_TRUE(mem.write_u8(base, 0x11).is_ok());
    EXPECT_TRUE(mem.write_u8(base + 0x1000, 0x22).is_err());
    EXPECT_TRUE(mem.write_u8(base + 0x2000, 0x33).is_ok());

    GuestAddr span_base = 0;
    usize span_size = 0;
    MemoryProtection span_protection = MemoryProtection::None;
    ASSERT_TRUE(mem.query_region(
        base + 0x1000, span_base, span_size, span_protection));
    EXPECT_EQ(span_base, base + 0x1000);
    EXPECT_EQ(span_size, 0x1000u);
    EXPECT_EQ(span_protection, MemoryProtection::None);

    ASSERT_TRUE(mem.query_region(
        base + 0x2000, span_base, span_size, span_protection));
    EXPECT_EQ(span_base, base + 0x2000);
    EXPECT_EQ(span_size, 0x1000u);
    EXPECT_EQ(span_protection, MemoryProtection::ReadWrite);

    const auto first_info = mem.get_region_info(base);
    EXPECT_EQ(first_info.base, base);
    EXPECT_EQ(first_info.end, base + 0x1000);
    EXPECT_NE(first_info.host_ptr, nullptr);

    const auto inaccessible_info =
        mem.get_region_info_cached(base + 0x1000);
    EXPECT_EQ(inaccessible_info.host_ptr, nullptr);

    const auto last_info = mem.get_region_info(base + 0x2000);
    EXPECT_EQ(last_info.base, base + 0x2000);
    EXPECT_EQ(last_info.end, base + 0x3000);
    EXPECT_NE(last_info.host_ptr, nullptr);
}

TEST(GuestMemoryTest, ProtectDoesNotBumpGenerationOrFlushUnrelatedPages) {
    // VirtualProtect must fault the protected page without invalidating
    // translations for the rest of the mapping. A global generation bump
    // makes every JIT thread miss the TLB and convoy on m_regions_mutex.
    GuestMemory mem;
    constexpr GuestAddr base = 0x40000;
    ASSERT_TRUE(mem.map_region(
        base, 0x2000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.write_u8(base, 0x11).is_ok());
    ASSERT_TRUE(mem.write_u8(base + 0x1000, 0x22).is_ok());

    const u64 generation = mem.generation();
    const u64 protect_seq = mem.protect_sequence();
    ASSERT_TRUE(mem.protect_region(
        base, 0x1000, MemoryProtection::Read).is_ok());
    EXPECT_EQ(mem.generation(), generation);
    EXPECT_GT(mem.protect_sequence(), protect_seq);

    EXPECT_TRUE(mem.write_u8(base + 0x1000, 0x33).is_ok());
    EXPECT_TRUE(mem.write_u8(base, 0x44).is_err());
    EXPECT_EQ(mem.read_u8(base).value(), 0x11);
    EXPECT_EQ(mem.read_u8(base + 0x1000).value(), 0x33);

    GuestMemory::ProtectShootdown events[GuestMemory::kProtectShootdownLog];
    bool overflow = false;
    const usize n = mem.copy_protect_shootdowns_after(
        protect_seq, events, GuestMemory::kProtectShootdownLog, &overflow);
    EXPECT_FALSE(overflow);
    ASSERT_GE(n, 1u);
    EXPECT_EQ(events[n - 1].addr, base);
    EXPECT_EQ(events[n - 1].size, 0x1000u);
    EXPECT_EQ(events[n - 1].protection, MemoryProtection::Read);
}

TEST(GuestMemoryTest, AccessAcrossProtectionRunsUsesCombinedPermissions) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x30000;
    ASSERT_TRUE(mem.map_region(
        base, 0x2000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.write_u8(base + 0x0FFF, 0x34).is_ok());
    ASSERT_TRUE(mem.write_u8(base + 0x1000, 0x12).is_ok());

    ASSERT_TRUE(mem.protect_region(
        base + 0x1000, 0x1000, MemoryProtection::Read).is_ok());

    const auto cross_read = mem.read_u16(base + 0x0FFF);
    ASSERT_TRUE(cross_read.is_ok());
    EXPECT_EQ(cross_read.value(), 0x1234u);
    EXPECT_FALSE(mem.has_protection(
        base + 0x0FFF, 2, MemoryProtection::Write));
    EXPECT_TRUE(mem.write_u16(base + 0x0FFF, 0x5678).is_err());

    const auto probe = mem.probe_access(base + 0x0FFF, 2);
    EXPECT_NE(probe.host_ptr, nullptr);
    EXPECT_TRUE(probe.readable);
    EXPECT_FALSE(probe.writable);
    EXPECT_EQ(probe.region_base, base + 0x0FFF);
    EXPECT_EQ(probe.region_end, base + 0x1001);
}

TEST(GuestMemoryTest, MappingOverlapChecksBothOrderedNeighbors) {
    for (bool backed : {false, true}) {
        GuestMemory mem;
        std::vector<u8> backing(0x10000);
        const auto map = [&](GuestAddr addr, usize size) {
            return backed
                ? mem.map_region_backed(addr, size, backing.data(),
                                        MemoryProtection::ReadWrite)
                : mem.map_region(addr, size, MemoryProtection::ReadWrite);
        };
        ASSERT_TRUE(map(0x20000, 0x2000).is_ok());
        ASSERT_TRUE(map(0x28000, 0x2000).is_ok());
        EXPECT_TRUE(map(0x21000, 0x1000).is_err()); // predecessor overlap
        EXPECT_TRUE(map(0x27000, 0x2000).is_err()); // successor overlap
        EXPECT_TRUE(map(0x20000, 0x1000).is_err()); // identical start
        EXPECT_TRUE(map(0x1F000, 0xC000).is_err()); // encloses both
        const u64 generation = mem.generation();
        EXPECT_TRUE(map(0x22000, 0x6000).is_ok()); // exact gap
        EXPECT_TRUE(map(0x1F000, 0x1000).is_ok()); // adjacent predecessor
        EXPECT_TRUE(map(0x2A000, 0x1000).is_ok()); // adjacent successor
        EXPECT_EQ(mem.generation(), generation);
    }
}

TEST(GuestMemoryTest, NewMappingsPreserveExistingTranslations) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x100000000ull;
    ASSERT_TRUE(mem.map_region(base, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(mem.write_u32(base, 0x12345678).is_ok());
    const auto before = mem.probe_access(base, 4);
    const u64 generation = mem.generation();
    EXPECT_EQ(mem.probe_access(base + 0x10000, 4).host_ptr, nullptr);
    ASSERT_TRUE(mem.map_region(base + 0x10000, 0x1000, MemoryProtection::Read).is_ok());
    EXPECT_EQ(mem.generation(), generation);
    EXPECT_EQ(mem.probe_access(base, 4).host_ptr, before.host_ptr);
    EXPECT_EQ(mem.read_u32(base).value(), 0x12345678u);
    EXPECT_TRUE(mem.probe_access(base + 0x10000, 4).readable);
    EXPECT_FALSE(mem.probe_access(base + 0x10000, 4).writable);
    ASSERT_TRUE(mem.unmap_region(base, 0x1000).is_ok());
    EXPECT_NE(mem.generation(), generation);
    EXPECT_EQ(mem.probe_access(base, 4).host_ptr, nullptr);
    ASSERT_TRUE(mem.map_region(base, 0x1000, MemoryProtection::Read).is_ok());
    EXPECT_FALSE(mem.probe_access(base, 4).writable);
    EXPECT_EQ(mem.read_u32(base).value(), 0u);
}

// Alternate allocations and address widths, then change their lifetime and
// permissions after warming the translation cache. Cached lookup hints must
// never authorize an access using an evicted or invalidated entry.
TEST(GuestMemoryTest, AlternatingProbesObserveProtectionAndRemapping) {
    GuestMemory mem;
    const auto address = [](usize i) -> GuestAddr {
        return 0x10000 + i * 0x10000ull + ((i & 1) ? (1ull << 32) : 0);
    };
    for (usize i = 0; i < 320; ++i) {
        ASSERT_TRUE(mem.map_region(address(i), 0x2000,
                                   MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem.write_u32(address(i), static_cast<u32>(i)).is_ok());
    }
    // 192 live regions fit in the cache, while the second walk forces reuse
    // of slots and leaves hints pointing at their replacements.
    for (usize count : {192u, 320u}) {
        for (usize pass = 0; pass < 3; ++pass) {
            for (usize i = 0; i < count; ++i) {
                const auto info = mem.probe_access(address(i), sizeof(u32));
                ASSERT_NE(info.host_ptr, nullptr);
                EXPECT_TRUE(info.readable);
                EXPECT_TRUE(info.writable);
                EXPECT_EQ(mem.read_u32(address(i)).value(), i);
            }
        }
    }
    const GuestAddr target = address(319);
    ASSERT_TRUE(mem.protect_region(target, 0x1000,
                                   MemoryProtection::None).is_ok());
    EXPECT_FALSE(mem.probe_access(target, 4).readable);
    EXPECT_TRUE(mem.read_u32(target).is_err());
    EXPECT_TRUE(mem.probe_access(target + 0x1000, 4).writable);
    EXPECT_FALSE(mem.probe_access(target + 0xFFF, 2).readable);
    ASSERT_TRUE(mem.unmap_region(target, 0x2000).is_ok());
    EXPECT_EQ(mem.probe_access(target, 4).host_ptr, nullptr);
    ASSERT_TRUE(mem.map_region(target, 0x2000, MemoryProtection::Read).is_ok());
    const auto remapped = mem.probe_access(target, 4);
    EXPECT_TRUE(remapped.readable);
    EXPECT_FALSE(remapped.writable);
    EXPECT_TRUE(mem.write_u32(target, 1).is_err());

    GuestMemory other;
    ASSERT_TRUE(other.map_region(address(0), 0x2000,
                                 MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(other.write_u32(address(0), 1234).is_ok());
    EXPECT_EQ(other.read_u32(address(0)).value(), 1234u);
    EXPECT_EQ(mem.read_u32(address(0)).value(), 0u);
}

TEST(GuestMemoryTest, AlternatingProbeEntriesRespectProtectionAndRemapping) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x10000000;
    // More than a full TLB, repeatedly reusing previously populated slots.
    for (usize i = 0; i < 300; ++i) {
        const GuestAddr addr = base + i * 0x10000;
        ASSERT_TRUE(mem.map_region(addr, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(mem.write_u32(addr, static_cast<u32>(i)).is_ok());
    }
    for (usize round = 0; round < 3; ++round) {
        for (usize i = 0; i < 300; ++i) {
            const auto info = mem.probe_access(base + i * 0x10000, 4);
            ASSERT_NE(info.host_ptr, nullptr);
            EXPECT_TRUE(info.readable);
            EXPECT_TRUE(info.writable);
            EXPECT_EQ(*reinterpret_cast<const u32*>(info.host_ptr), i);
        }
    }
    ASSERT_TRUE(mem.protect_region(base + 0x1000, 0x1000, MemoryProtection::Read).is_ok());
    EXPECT_FALSE(mem.probe_access(base + 0x1000, 4).writable);
    EXPECT_TRUE(mem.probe_access(base, 4).writable);
    EXPECT_FALSE(mem.probe_access(base + 0xFFF, 2).writable);
    ASSERT_TRUE(mem.unmap_region(base, 0x2000).is_ok());
    EXPECT_EQ(mem.probe_access(base, 4).host_ptr, nullptr);
    ASSERT_TRUE(mem.map_region(base, 0x2000, MemoryProtection::Read).is_ok());
    EXPECT_FALSE(mem.probe_access(base, 4).writable);
    GuestMemory other;
    ASSERT_TRUE(other.map_region(base, 0x2000, MemoryProtection::ReadWrite).is_ok());
    EXPECT_TRUE(other.probe_access(base, 4).writable);
    EXPECT_FALSE(mem.probe_access(base, 4).writable);
}

// Explicit opt-in timing probe: no wall-clock assertion in the unit suite.
TEST(GuestMemoryTest, DISABLED_AlternatingProbeBenchmark) {
    GuestMemory mem;
    constexpr GuestAddr base = 0x10000000;
    constexpr usize count = 192;
    constexpr usize iterations = 2000000;
    for (usize i = 0; i < count; ++i) {
        ASSERT_TRUE(mem.map_region(base + i * 0x10000, 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());
    }
    for (usize i = 0; i < count; ++i) {
        ASSERT_NE(mem.probe_access(base + i * 0x10000, 4).host_ptr, nullptr);
    }
    usize hits = 0;
    const auto start = std::chrono::steady_clock::now();
    for (usize i = 0; i < iterations; ++i) {
        hits += mem.probe_access(base + (i % count) * 0x10000, 4).readable;
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(hits, iterations);
    RecordProperty("ns_per_probe", elapsed / iterations);
    std::printf("alternating probe: %.2f ns/access\n", elapsed / iterations);
}

TEST(GuestMemoryTest, MultipleRegions) {
    GuestMemory mem;
    
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    EXPECT_TRUE(mem.map_region(0x10000, 8192, MemoryProtection::ReadWrite).is_ok());
    EXPECT_TRUE(mem.map_region(0x100000, 4096, MemoryProtection::Read).is_ok());
    
    EXPECT_EQ(mem.region_count(), 3u);
    
    // Write to different regions
    EXPECT_TRUE(mem.write_u32(0x1000, 0x11111111).is_ok());
    EXPECT_TRUE(mem.write_u32(0x10000, 0x22222222).is_ok());
    
    // Verify isolation
    EXPECT_EQ(mem.read_u32(0x1000).value(), 0x11111111u);
    EXPECT_EQ(mem.read_u32(0x10000).value(), 0x22222222u);
}

TEST(GuestMemoryTest, TotalMappedSize) {
    GuestMemory mem;
    
    EXPECT_EQ(mem.total_mapped_size(), 0u);
    
    EXPECT_TRUE(mem.map_region(0x1000, 4096, MemoryProtection::ReadWrite).is_ok());
    EXPECT_GE(mem.total_mapped_size(), 4096u);
    
    EXPECT_TRUE(mem.map_region(0x10000, 8192, MemoryProtection::ReadWrite).is_ok());
    EXPECT_GE(mem.total_mapped_size(), 4096u + 8192u);
}

TEST(GuestMemoryTest, FindFreeGapHonorsNonLargeAddressAwareCeiling) {
    // Learn GetSystemInfo: lpMaximumApplicationAddress is 0x7FFEFFFF for
    // 32-bit processes without LARGEADDRESSAWARE. Opportunistic allocators
    // (CRT arenas, D3D Map) must not hand out 0x80000000+.
    GuestMemory mem;
    mem.set_maximum_user_address(0x7FFEFFFFULL);
    ASSERT_TRUE(mem.map_region(
        0x70000000ull, 0x10000000ull, MemoryProtection::ReadWrite).is_ok());

    const GuestAddr gap = mem.find_free_gap(0x70000000ull, 0x1000);
    EXPECT_EQ(gap, 0u);

    const GuestAddr low = mem.find_free_gap(0x10000000ull, 0x1000);
    ASSERT_NE(low, 0u);
    EXPECT_LE(low, 0x7FFEFFFFULL - 0xFFFull);
}

// A gap search must step over a long run of adjacent regions in one ordered
// walk. Scanning from the front of the map on every candidate move made this
// quadratic, and a guest that allocates per thread (Windows static-DLL TLS is
// replayed per thread) grows the map until the search dominates its runtime.
TEST(GuestMemoryTest, FindFreeGapSkipsLongRunOfAdjacentRegions) {
    GuestMemory mem;
    constexpr GuestAddr kBase = 0x10000000ull;
    constexpr usize kCount = 4000;
    for (usize i = 0; i < kCount; ++i) {
        ASSERT_TRUE(mem.map_region(kBase + i * 0x1000ull, 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());
    }
    // The only free address at or above kBase is past the whole run.
    EXPECT_EQ(mem.find_free_gap(kBase, 0x1000), kBase + kCount * 0x1000ull);
    // A request that must also clear the run's tail lands on the same edge.
    EXPECT_EQ(mem.find_free_gap(kBase + 0x1000ull, 0x4000),
              kBase + kCount * 0x1000ull);
    // A hole punched mid-run is found rather than skipped.
    ASSERT_TRUE(mem.unmap_region(kBase + 100 * 0x1000ull, 0x1000).is_ok());
    EXPECT_EQ(mem.find_free_gap(kBase, 0x1000), kBase + 100 * 0x1000ull);
    ASSERT_TRUE(mem.map_region(kBase + 100 * 0x1000ull, 0x1000,
                               MemoryProtection::ReadWrite).is_ok());

    // Cost guard, not a microbenchmark: the quadratic scan needed roughly
    // kCount^2 map-node visits per call, so 200 calls took minutes. An ordered
    // walk does them in milliseconds; the bound is loose by three orders of
    // magnitude so it cannot flake on a loaded machine.
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(mem.find_free_gap(kBase, 0x1000), kBase + kCount * 0x1000ull);
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              2000);
}

TEST(GuestMemoryTest, SamePageExecutableWritesCoalesceInLog) {
    GuestMemory memory;
    const u64 start = memory.executable_generation();
    for (int i = 0; i < 100; ++i) {
        memory.notify_executable_write(
            0x20001000ull + static_cast<GuestAddr>(i), 1);
    }
    const auto events = memory.collect_executable_writes_after(start);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].addr, 0x20001000ull);
    EXPECT_EQ(events[0].size, 100u);
}

TEST(GuestMemoryTest, ConcurrentExecutableWritesPublishCompleteHistory) {
    GuestMemory memory;
    // Fewer than one ring of writes per round: loss of history is impossible.
    // Distinct pages prevent coalescing from concealing a torn record.
    constexpr unsigned kWriters = 8;
    constexpr unsigned kWrites = 128;
    for (unsigned round = 0; round < 40; ++round) {
        const u64 start = memory.executable_generation();
        std::atomic<bool> go{false};
        std::atomic<unsigned> done{0};
        std::vector<std::thread> writers;
        for (unsigned writer = 0; writer < kWriters; ++writer) {
            writers.emplace_back([&, writer] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (unsigned i = 0; i < kWrites; ++i) {
                    const unsigned id = writer * kWrites + i + 1;
                    memory.notify_executable_write(GuestAddr{id} * 0x1000, id);
                }
                done.fetch_add(1, std::memory_order_release);
            });
        }
        go.store(true, std::memory_order_release);
        unsigned incomplete = 0;
        do {
            for (const auto& event : memory.collect_executable_writes_after(start)) {
                if (event.addr == 0 || event.size != event.addr / 0x1000)
                    ++incomplete;
            }
        } while (done.load(std::memory_order_acquire) != kWriters);
        for (auto& writer : writers) writer.join();
        ASSERT_EQ(incomplete, 0u) << "round " << round;
        const auto events = memory.collect_executable_writes_after(start);
        ASSERT_EQ(events.size(), kWriters * kWrites);
        EXPECT_EQ(memory.executable_generation(), start + kWriters * kWrites);
    }
}

TEST(GuestMemoryTest, ExecutableWriteLogOverflowKeepsRecentRangedEvents) {
    GuestMemory memory;
    const u64 start = memory.executable_generation();
    constexpr int kWrites = 5000;
    for (int i = 0; i < kWrites; ++i) {
        memory.notify_executable_write(
            0x20000000ull + static_cast<GuestAddr>(i) * 0x1000, 16);
    }
    const u64 now = memory.executable_generation();
    const auto recent = memory.collect_executable_writes_after(now - 1);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_NE(recent[0].addr, 0u);
    EXPECT_NE(recent[0].size, 0u);

    const auto lagged = memory.collect_executable_writes_after(start);
    ASSERT_EQ(lagged.size(), 1u);
    EXPECT_EQ(lagged[0].addr, 0u);
    EXPECT_EQ(lagged[0].size, 0u);
}

TEST(GuestMemoryTest, ProcessFlushCompactsExecutableWriteLog) {
    GuestMemory memory;
    const u64 start = memory.executable_generation();
    for (int i = 0; i < 50; ++i) {
        memory.notify_executable_write(
            0x20000000ull + static_cast<GuestAddr>(i) * 0x1000, 16);
    }
    memory.notify_executable_write(0, 0);
    const auto events = memory.collect_executable_writes_after(start);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].addr, 0u);
    EXPECT_EQ(events[0].size, 0u);
}

// =============================================================================
// Protection pin
// =============================================================================
//
// native64 translates guest segment overrides in place: it opens a code range
// for writing with its own mprotect, then stores into it. A guest
// VirtualProtect landing in that window re-protects the host pages read-only
// and the next store dies as a host SIGBUS -- macOS reports a protection
// failure as SIGBUS, not SIGSEGV, so it kills the emulator rather than
// faulting the guest. Observed on the Intel iMac as
// `Fault region: 0x7ff40f001000+0x229000 prot=5 max=7` from inside rewrite_at,
// after that exact span had just been opened RWX.
//
// The pin is what makes that window closed rather than merely narrow, so the
// contract under test is precisely: protect_region cannot complete while a
// pin is held.

TEST(GuestMemoryProtectionPin, ProtectRegionCannotLandWhilePinned) {
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x10000, 8192,
                               MemoryProtection::ReadWrite).is_ok());

    std::atomic<bool> protect_done{false};
    std::atomic<bool> protect_started{false};

    {
        const auto pin = mem.pin_protection();

        std::thread writer([&] {
            protect_started.store(true, std::memory_order_release);
            (void)mem.protect_region(0x10000, 8192,
                                     MemoryProtection::ReadExecute);
            protect_done.store(true, std::memory_order_release);
        });

        while (!protect_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Not a race against the pin: the pin is still held for the whole of
        // this wait, so a protect that completes here completes *under* it.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(protect_done.load(std::memory_order_acquire))
            << "protect_region re-protected a pinned range";

        // Releasing the pin must let it through, or the pin is a deadlock
        // rather than an exclusion.
        const_cast<GuestMemory::ProtectionPin&>(pin).unlock();
        writer.join();
    }
    EXPECT_TRUE(protect_done.load(std::memory_order_acquire));
}

TEST(GuestMemoryProtectionPin, ConcurrentPinsDoNotExcludeEachOther) {
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x10000, 4096,
                               MemoryProtection::ReadWrite).is_ok());

    const auto first = mem.pin_protection();
    std::atomic<bool> second_taken{false};
    std::thread other([&] {
        const auto second = mem.pin_protection();
        second_taken.store(true, std::memory_order_release);
    });
    other.join();
    EXPECT_TRUE(second_taken.load(std::memory_order_acquire))
        << "the pin must be shared: it stops writers, not other readers";
}

// An aligned request must cross fragmentation in one walk. MapViewOfFile needs
// a 64 KiB-aligned base (dwAllocationGranularity); its caller-side loop gave
// up after 16 misaligned gaps. A process whose low VA is diced by one 4 KiB
// block per TLS-bearing DLL per thread left exactly this pattern, and a
// 16 KiB view of Steam3Master_SharedMemFile failed with terabytes free.
// find_free_gap remembers, per (start, size, alignment), the prefix a search
// proved full, because VirtualAlloc's bottom-up first-fit otherwise re-walks
// every region above 1 MiB on every call (17% of Satisfactory's game thread).
// The cache must be invisible: after any sequence of maps and unmaps each
// search returns exactly the first fit a brute-force scan finds. Unmapping
// merges the freed range with the free space before it, so a fit may start
// below the freed base; the random sizes and 64 KiB alignment exercise that.
TEST(GuestMemoryTest, FindFreeGapCacheMatchesBruteForceFirstFit) {
    GuestMemory mem;
    constexpr GuestAddr kBase = 0x10000000ull;
    constexpr GuestAddr kSpan = 0x02000000ull;  // 32 MiB playground
    std::map<GuestAddr, usize> live;
    const auto brute = [&](GuestAddr start, usize size, usize align) {
        const usize sz = (size + 0xFFFu) & ~usize{0xFFFu};
        const GuestAddr a = align < 0x1000u ? 0x1000u : align;
        GuestAddr cand = (start + a - 1) & ~(a - 1);
        const auto extents = mem.region_extents();
        for (;;) {
            bool moved = false;
            for (const auto& [lo, hi] : extents) {
                if (cand < hi && lo < cand + sz) {
                    cand = (hi + a - 1) & ~(a - 1);
                    moved = true;
                }
            }
            if (!moved) return cand;
        }
    };
    u64 state = 0x243F6A8885A308D3ull;
    const auto next = [&](u64 bound) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return state % bound;
    };
    const usize sizes[] = {0x1000, 0x3000, 0x10000, 0x11000, 0x40000};
    const usize aligns[] = {0x1000, 0x10000};
    for (int step = 0; step < 4000; ++step) {
        const usize size = sizes[next(5)];
        const usize align = aligns[next(2)];
        const GuestAddr start = next(4) == 0 ? kBase + next(kSpan) : kBase;
        const GuestAddr expected = brute(start, size, align);
        ASSERT_EQ(mem.find_free_gap(start, size, align), expected)
            << "step " << step;
        if (next(3) != 0 && expected + size <= kBase + kSpan) {
            ASSERT_TRUE(mem.map_region(expected, (size + 0xFFFu) & ~usize{0xFFFu},
                                       MemoryProtection::ReadWrite).is_ok());
            live[expected] = (size + 0xFFFu) & ~usize{0xFFFu};
        } else if (!live.empty()) {
            auto victim = live.begin();
            std::advance(victim, static_cast<long>(next(live.size())));
            ASSERT_TRUE(mem.unmap_region(victim->first, victim->second).is_ok());
            live.erase(victim);
        }
    }
}

TEST(GuestMemoryTest, FindFreeGapWithAlignmentCrossesFragmentation) {
    GuestMemory mem;
    constexpr GuestAddr kBase = 0x10000000ull;
    constexpr usize kBlocks = 300;
    // One page at every 64 KiB boundary: 60 KiB of misaligned space follows
    // each, so every page-aligned gap is misaligned for a 64 KiB request.
    for (usize i = 0; i < kBlocks; ++i) {
        ASSERT_TRUE(mem.map_region(kBase + i * 0x10000ull, 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());
    }
    // Page alignment still finds the first hole.
    EXPECT_EQ(mem.find_free_gap(kBase, 0x4000), kBase + 0x1000ull);
    // 64 KiB alignment lands on the first boundary past the run.
    const GuestAddr aligned = mem.find_free_gap(kBase, 0x4000, 0x10000);
    EXPECT_EQ(aligned, kBase + kBlocks * 0x10000ull);
    EXPECT_EQ(aligned % 0x10000ull, 0u);
    // A misaligned start is rounded up, and a free aligned slot mid-run is
    // found rather than skipped.
    ASSERT_TRUE(mem.unmap_region(kBase + 150 * 0x10000ull, 0x1000).is_ok());
    EXPECT_EQ(mem.find_free_gap(kBase + 0x1234ull, 0x4000, 0x10000),
              kBase + 150 * 0x10000ull);
    // Alignment below the page size means page alignment.
    EXPECT_EQ(mem.find_free_gap(kBase, 0x4000, 16), kBase + 0x1000ull);
}

// The linear proof lease is taken on every JIT dispatcher entry of a
// proof-bearing block, by every guest thread at once. A writer (map, unmap,
// protect) must still see every lease: while a thread holds one, the
// mapping it validated cannot change, and a lease cannot be taken while a
// writer is active. Readers must not serialize on one shared cache line --
// a single CAS'd counter was 3.7% of all busy CPU across UE's task workers.
TEST(GuestMemoryTest, LinearProofLeaseExcludesWritersAcrossThreads) {
    GuestMemory mem;
    constexpr GuestAddr kToggle = 0x20000000ull;
    constexpr int kReaders = 8;
    constexpr int kIters = 200000;
    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};
    std::atomic<u64> entered{0};
    std::thread writer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            ASSERT_TRUE(mem.map_region(kToggle, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            ASSERT_TRUE(mem.unmap_region(kToggle, 0x1000).is_ok());
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            u64 mine = 0;
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(150);
            for (int i = 0; i < kIters ||
                            std::chrono::steady_clock::now() < until;
                 ++i) {
                if (!mem.try_enter_linear_proof_lease()) continue;
                const bool before = mem.is_mapped(kToggle, 0x1000);
                for (int spin = 0; spin < (i & 15); ++spin) {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                }
                if (mem.is_mapped(kToggle, 0x1000) != before) {
                    violations.fetch_add(1, std::memory_order_relaxed);
                }
                mem.exit_linear_proof_lease();
                ++mine;
            }
            entered.fetch_add(mine, std::memory_order_relaxed);
        });
    }
    for (auto& t : readers) t.join();
    const auto t1 = std::chrono::steady_clock::now();
    stop.store(true);
    writer.join();
    EXPECT_EQ(violations.load(), 0)
        << "a mapping changed under a live proof lease";
    EXPECT_GT(entered.load(), 0u);
    // Uncontended enter/exit pairs, all threads at once, no writer.
    std::vector<std::thread> fast;
    const auto f0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReaders; ++r) {
        fast.emplace_back([&] {
            for (int i = 0; i < 2000000; ++i) {
                if (mem.try_enter_linear_proof_lease()) {
                    mem.exit_linear_proof_lease();
                }
            }
        });
    }
    for (auto& t : fast) t.join();
    const auto f1 = std::chrono::steady_clock::now();
    std::printf("[proof-lease] contended %lld ms (%llu leases); "
                "%d threads x 2M enter/exit: %lld ms\n",
                static_cast<long long>(std::chrono::duration_cast<
                    std::chrono::milliseconds>(t1 - t0).count()),
                static_cast<unsigned long long>(entered.load()), kReaders,
                static_cast<long long>(std::chrono::duration_cast<
                    std::chrono::milliseconds>(f1 - f0).count()));
}

// First-fit placement races: every searcher converges on the same lowest gap,
// so a thread can lose the gap between its search and its map. That is not
// out-of-memory (Windows fails only when no space is left); map_first_fit
// retries against the updated table. Found as a UE "Out of video memory"
// abort: D3D11 Map lost the race to a concurrent VirtualAlloc and returned
// E_OUTOFMEMORY with plenty of address space free.
TEST(GuestMemoryTest, MapFirstFitSurvivesConcurrentPlacementRaces) {
    GuestMemory mem;
    constexpr GuestAddr kBase = 0x20000000ull;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 400;
    std::vector<std::vector<GuestAddr>> got(kThreads);
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                Error error = Error::None;
                const GuestAddr base = mem.map_first_fit(
                    kBase, 0x1000, MemoryProtection::ReadWrite, true, 0,
                    &error);
                if (base == 0) {
                    failures.fetch_add(1);
                } else {
                    got[t].push_back(base);
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(failures.load(), 0);
    std::set<GuestAddr> unique;
    for (const auto& list : got) unique.insert(list.begin(), list.end());
    EXPECT_EQ(unique.size(), static_cast<usize>(kThreads * kPerThread))
        << "two placements returned the same base";
    // First fit: the whole run is packed from kBase with no holes.
    EXPECT_EQ(*unique.begin(), kBase);
    EXPECT_EQ(*unique.rbegin(),
              kBase + static_cast<GuestAddr>(kThreads * kPerThread - 1) * 0x1000);
}

// Bounded startup diagnostic: quantify unrelated commit churn forcing guest
// readers back through the region-map lock. Opt-in, never a timing assertion.
TEST(GuestMemoryTest, DISABLED_TranslationCommitChurnBenchmark) {
    GuestMemory mem;
    const bool linear = std::getenv("VITREO_BENCH_LINEAR") != nullptr;
    if (linear) {
        ASSERT_TRUE(mem.enable_linear_reservation(usize{8} << 30));
        ASSERT_TRUE(mem.enable_linear_hardware_protection());
    }
    constexpr int readers = 12;
    constexpr int rounds = 300;
    constexpr int regions = 64;
    constexpr GuestAddr base = 0x100000;
    constexpr GuestAddr commit = 0x10000000;
    constexpr usize stride = 0x10000;
    for (int i = 0; i < regions; ++i) {
        ASSERT_TRUE(mem.map_region(base + i * stride, 0x4000,
                                   MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.write_u8(base + i * stride, 0xA5));
    }
    ASSERT_TRUE(mem.map_region(commit, stride, MemoryProtection::ReadWrite));
    const bool churn = std::getenv("VITREO_BENCH_COMMIT_CHURN") != nullptr;
    std::atomic<int> ready{0};
    std::atomic<int> epoch{0};
    std::atomic<int> done{0};
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < readers; ++thread) {
        workers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            for (int round = 1; round <= rounds; ++round) {
                while (epoch.load(std::memory_order_acquire) != round)
                    std::this_thread::yield();
                for (int i = 0; i < regions; ++i) {
                    const auto byte = mem.read_u8(base + i * stride);
                    if (byte.is_err() || byte.value() != 0xA5)
                        valid.store(false, std::memory_order_relaxed);
                }
                done.fetch_add(1, std::memory_order_release);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != readers)
        std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    for (int round = 1; round <= rounds; ++round) {
        if (churn && mem.commit_region(commit, stride,
                                       MemoryProtection::ReadWrite).is_err())
            valid.store(false, std::memory_order_relaxed);
        epoch.store(round, std::memory_order_release);
        while (done.load(std::memory_order_acquire) != round * readers)
            std::this_thread::yield();
    }
    for (auto& worker : workers) worker.join();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(valid.load());
    std::printf("TRANSLATION_BENCH churn=%d linear=%d seconds=%.6f reads=%d ns_per_read=%.3f\n",
                churn, linear, seconds, readers * rounds * regions,
                seconds * 1e9 / (readers * rounds * regions));
}

TEST(GuestMemoryTest, RangedCommitInvalidatesPermissionsAndKeepsJitEpoch) {
    // The software TLB must observe foreign-thread permission changes even
    // when it no longer follows the JIT's coarse backing generation.
    for (const bool linear : {false, true}) {
        for (const GuestAddr base : {GuestAddr{0x400000}, GuestAddr{0x140000000}}) {
            SCOPED_TRACE(linear);
            SCOPED_TRACE(base);
            GuestMemory mem;
            if (linear) {
                ASSERT_TRUE(mem.enable_linear_reservation(usize{8} << 30));
                ASSERT_TRUE(mem.enable_linear_hardware_protection());
            }
            ASSERT_TRUE(mem.map_region(base, 0x4000, MemoryProtection::ReadWrite));
            ASSERT_TRUE(mem.map_region(base + 0x10000, 0x4000,
                                       MemoryProtection::ReadWrite));
            ASSERT_TRUE(mem.write_u8(base, 0x11));
            ASSERT_TRUE(mem.write_u8(base + 0x1000, 0x22));
            ASSERT_TRUE(mem.write_u8(base + 0x10000, 0x33));
            const u64 jit_epoch = mem.generation();
            std::thread writer([&] {
                EXPECT_TRUE(mem.commit_region(base, 0x1000, MemoryProtection::Read));
            });
            writer.join();
            EXPECT_GT(mem.generation(), jit_epoch);
            EXPECT_EQ(mem.read_u8(base).value(), 0x11);
            EXPECT_EQ(mem.write_u8(base, 0x44).error(), Error::ProtectionFault);
            // The changed guest page shares a host page with this RW neighbor.
            EXPECT_TRUE(mem.write_u8(base + 0x1000, 0x55));
            EXPECT_EQ(mem.read_u8(base + 0x1000).value(), 0x55);
            EXPECT_EQ(mem.read_u8(base + 0x10000).value(), 0x33);
            EXPECT_FALSE(mem.probe_access(base, 1).writable);

            ASSERT_TRUE(mem.commit_region(base, 0x1000, MemoryProtection::None));
            EXPECT_EQ(mem.read_u8(base).error(), Error::ProtectionFault);
            EXPECT_EQ(mem.get_region_info_cached(base).host_ptr, nullptr);
            ASSERT_TRUE(mem.commit_region(base, 0x1000, MemoryProtection::ReadWrite));
            EXPECT_EQ(mem.read_u8(base).value(), 0x11);
            ASSERT_TRUE(mem.decommit_region(base, 0x1000));
            EXPECT_EQ(mem.read_u8(base).error(), Error::ProtectionFault);
            EXPECT_TRUE(mem.write_u8(base + 0x1000, 0x66));
            ASSERT_TRUE(mem.commit_region(base, 0x1000, MemoryProtection::ReadWrite));
            EXPECT_TRUE(mem.write_u8(base, 0x77));
            EXPECT_EQ(mem.read_u8(base).value(), 0x77);

            ASSERT_TRUE(mem.unmap_region(base, 0x4000));
            EXPECT_EQ(mem.read_u8(base).error(), Error::InvalidAddress);
            ASSERT_TRUE(mem.map_region(base, 0x4000, MemoryProtection::ReadWrite));
            EXPECT_EQ(mem.read_u8(base).value(), 0);
        }
    }
}

namespace {
/// One thread that runs each job it is given, so that its software TLB
/// persists from one job to the next.
class PersistentThread {
public:
    PersistentThread() : m_thread([this] { loop(); }) {}
    ~PersistentThread() {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    void run(std::function<void()> job) {
        std::unique_lock<std::mutex> lock(m_mu);
        m_job = std::move(job);
        m_cv.notify_all();
        m_cv.wait(lock, [&] { return !m_job; });
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(m_mu);
        for (;;) {
            m_cv.wait(lock, [&] { return m_quit || static_cast<bool>(m_job); });
            if (m_quit) return;
            m_job();
            m_job = nullptr;
            m_cv.notify_all();
        }
    }
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::function<void()> m_job;
    bool m_quit{false};
    std::thread m_thread;
};
}  // namespace

// Unmapping a region removes it from every thread's software TLB: a thread
// that had it cached gets InvalidAddress at once, and a remap at the same
// address reads as new memory. In the linear model a translation's host
// pointer outlives its region, so only the unmapped range is discarded; the
// other thread keeps its translations for everything else (a whole-TLB
// discard per VirtualFree sent every thread back to the region table).
TEST(GuestMemoryTest, UnmapDiscardsOnlyTheUnmappedRangeFromOtherThreadsTlbs) {
    for (const bool linear : {false, true}) {
        SCOPED_TRACE(linear);
        GuestMemory mem;
        if (linear) {
            ASSERT_TRUE(mem.enable_linear_reservation(usize{8} << 30));
            ASSERT_TRUE(mem.enable_linear_hardware_protection());
        }
        constexpr GuestAddr kept = 0x140000000;
        constexpr GuestAddr gone = 0x140100000;
        ASSERT_TRUE(mem.map_region(kept, 0x4000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.map_region(gone, 0x4000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.write_u8(kept, 0x5A));
        ASSERT_TRUE(mem.write_u8(gone, 0xA5));
        PersistentThread reader;
        reader.run([&] {
            EXPECT_EQ(mem.read_u8(kept).value(), 0x5A);
            EXPECT_EQ(mem.read_u8(gone).value(), 0xA5);
        });
        const u64 translations = mem.translation_generation();
        ASSERT_TRUE(mem.unmap_region(gone, 0x4000));
        reader.run([&] {
            EXPECT_EQ(mem.read_u8(gone).error(), Error::InvalidAddress);
            EXPECT_EQ(mem.write_u8(gone, 0x11).error(), Error::InvalidAddress);
            EXPECT_EQ(mem.read_u8(kept).value(), 0x5A);
        });
        if (linear) {
            EXPECT_EQ(mem.translation_generation(), translations)
                << "the unmap discarded every thread's translations";
        }
        ASSERT_TRUE(mem.map_region(gone, 0x4000, MemoryProtection::ReadWrite));
        reader.run([&] {
            EXPECT_EQ(mem.read_u8(gone).value(), 0) << "a stale translation";
            EXPECT_TRUE(mem.write_u8(gone, 0x22));
        });
        EXPECT_EQ(mem.read_u8(gone).value(), 0x22);
    }
}

TEST(GuestMemoryTest, RangedCommitLogOverflowCannotRetainOldTranslation) {
    for (const bool linear : {false, true}) {
        GuestMemory mem;
        if (linear) {
            ASSERT_TRUE(mem.enable_linear_reservation(usize{8} << 30));
            ASSERT_TRUE(mem.enable_linear_hardware_protection());
        }
        constexpr GuestAddr base = 0x140000000;
        constexpr GuestAddr unrelated = base + 0x10000;
        ASSERT_TRUE(mem.map_region(base, 0x4000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.map_region(unrelated, 0x4000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.write_u8(base, 0xA5));
        ASSERT_EQ(mem.read_u8(base).value(), 0xA5);
        std::thread writer([&] {
            EXPECT_TRUE(mem.commit_region(base, 0x4000, MemoryProtection::None));
            for (usize i = 0; i <= GuestMemory::kProtectShootdownLog; ++i)
                EXPECT_TRUE(mem.commit_region(unrelated, 0x4000,
                                              MemoryProtection::ReadWrite));
        });
        writer.join();
        EXPECT_EQ(mem.read_u8(base).error(), Error::ProtectionFault);
        EXPECT_EQ(mem.write_u8(base, 0x33).error(), Error::ProtectionFault);
        EXPECT_FALSE(mem.probe_access(base, 1).readable);
    }
}
