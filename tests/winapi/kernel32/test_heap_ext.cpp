/// @file test_heap_ext.cpp
/// @brief Unit tests for extended heap operations (WIN32-061)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/memory/heap_ext.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/handle.hpp>
#include <core/memory/guest_memory.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class HeapExtTest : public ::testing::Test {
protected:
    GuestMemory mem;
    VirtualMemoryManager vmm{mem};
    HandleTable handles;
    HeapManager hm{vmm, mem, handles};
    HeapExtended ext{hm, handles};
};

// =============================================================================
// HeapReAlloc Tests
// =============================================================================

TEST_F(HeapExtTest, ReAllocGrowPreservesData) {
    Handle h = hm.get_process_heap();
    u64 addr = hm.heap_alloc(h, 0, 100);
    ASSERT_NE(addr, 0u);

    // Write pattern: 0, 1, 2, … 99
    u8 pattern[100];
    for (int i = 0; i < 100; ++i) {
        pattern[i] = static_cast<u8>(i);
    }
    auto wr = mem.write_bytes(static_cast<GuestAddr>(addr), pattern, 100);
    ASSERT_FALSE(wr.is_err());

    // Grow to 200 bytes
    u64 new_addr = hm.heap_realloc(h, 0, addr, 200);
    ASSERT_NE(new_addr, 0u);

    // Verify original 100 bytes preserved
    u8 buf[100];
    auto rd = mem.read_bytes(static_cast<GuestAddr>(new_addr), buf, 100);
    ASSERT_FALSE(rd.is_err());
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(buf[i], static_cast<u8>(i)) << "byte " << i;
    }
}

TEST_F(HeapExtTest, ReAllocShrinkPreservesPrefix) {
    Handle h = hm.get_process_heap();
    u64 addr = hm.heap_alloc(h, 0, 200);
    ASSERT_NE(addr, 0u);

    u8 pattern[200];
    for (int i = 0; i < 200; ++i) {
        pattern[i] = static_cast<u8>(i & 0xFF);
    }
    auto wr = mem.write_bytes(static_cast<GuestAddr>(addr), pattern, 200);
    ASSERT_FALSE(wr.is_err());

    // Shrink to 100 bytes
    u64 new_addr = hm.heap_realloc(h, 0, addr, 100);
    ASSERT_NE(new_addr, 0u);

    // Verify first 100 bytes preserved
    u8 buf[100];
    auto rd = mem.read_bytes(static_cast<GuestAddr>(new_addr), buf, 100);
    ASSERT_FALSE(rd.is_err());
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(buf[i], static_cast<u8>(i)) << "byte " << i;
    }
}

TEST_F(HeapExtTest, ReAllocZeroMemoryZerosGrown) {
    Handle h = hm.get_process_heap();
    u64 addr = hm.heap_alloc(h, 0, 64);
    ASSERT_NE(addr, 0u);

    // Fill original 64 bytes
    u8 data[64];
    for (int i = 0; i < 64; ++i) {
        data[i] = 0xAA;
    }
    auto wr = mem.write_bytes(static_cast<GuestAddr>(addr), data, 64);
    ASSERT_FALSE(wr.is_err());

    // Grow to 128 with HEAP_ZERO_MEMORY
    u64 new_addr = hm.heap_realloc(h, HEAP_ZERO_MEMORY, addr, 128);
    ASSERT_NE(new_addr, 0u);

    // The grown portion (bytes 64–127) must be zero
    u8 buf[128];
    auto rd = mem.read_bytes(static_cast<GuestAddr>(new_addr), buf, 128);
    ASSERT_FALSE(rd.is_err());
    for (int i = 64; i < 128; ++i) {
        EXPECT_EQ(buf[i], 0u) << "byte " << i;
    }
}

TEST_F(HeapExtTest, ReAllocInPlaceOnlyReturnsZero) {
    Handle h = hm.get_process_heap();
    u64 addr = hm.heap_alloc(h, 0, 64);
    ASSERT_NE(addr, 0u);

    // Per-alloc VirtualAlloc strategy can never resize in-place
    u64 new_addr = hm.heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, addr, 128);
    EXPECT_EQ(new_addr, 0u);
}

TEST_F(HeapExtTest, ReAllocNullPtrReturnsZero) {
    Handle h = hm.get_process_heap();
    // Address 0 is not in the allocations map → must fail
    u64 result = hm.heap_realloc(h, 0, 0, 64);
    EXPECT_EQ(result, 0u);
}

// =============================================================================
// HeapSize Tests
// =============================================================================

TEST_F(HeapExtTest, SizeReturnsCorrectSize) {
    Handle h = hm.get_process_heap();
    u64 addr = hm.heap_alloc(h, 0, 256);
    ASSERT_NE(addr, 0u);

    u64 sz = hm.heap_size(h, 0, addr);
    EXPECT_EQ(sz, 256u);
}

TEST_F(HeapExtTest, SizeInvalidPointerReturnsError) {
    Handle h = hm.get_process_heap();
    u64 sz = hm.heap_size(h, 0, 0xDEADBEEF);
    EXPECT_EQ(sz, ~0ULL);
}

// =============================================================================
// HeapValidate Tests
// =============================================================================

TEST_F(HeapExtTest, ValidateReturnsTrueForValidHeap) {
    Handle h = hm.get_process_heap();
    // address == 0 means validate entire heap
    EXPECT_TRUE(ext.heap_validate(h, 0, 0));
}

TEST_F(HeapExtTest, ValidateReturnsTrueForValidAllocation) {
    Handle h = hm.get_process_heap();
    u64 addr = hm.heap_alloc(h, 0, 128);
    ASSERT_NE(addr, 0u);
    EXPECT_TRUE(ext.heap_validate(h, 0, addr));
}

TEST_F(HeapExtTest, ValidateReturnsFalseForBadPointer) {
    Handle h = hm.get_process_heap();
    EXPECT_FALSE(ext.heap_validate(h, 0, 0xBADBAD));
}

// =============================================================================
// HeapCompact Test
// =============================================================================

TEST_F(HeapExtTest, CompactReturnsNonZero) {
    Handle h = hm.get_process_heap();
    u64 largest = ext.heap_compact(h, 0);
    EXPECT_GT(largest, 0u);
}

// =============================================================================
// HeapCreate / HeapDestroy Tests
// =============================================================================

TEST_F(HeapExtTest, CreateCreatesIndependentHeap) {
    Handle h1 = hm.heap_create(0, 0x10000, 0);
    Handle h2 = hm.heap_create(0, 0x10000, 0);
    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);
    EXPECT_NE(h1, h2);

    // Allocate on h1, verify not visible from h2
    u64 addr = hm.heap_alloc(h1, 0, 64);
    ASSERT_NE(addr, 0u);
    u64 sz_h2 = hm.heap_size(h2, 0, addr);
    EXPECT_EQ(sz_h2, ~0ULL); // not tracked by h2
}

TEST_F(HeapExtTest, DestroyFreesHeap) {
    Handle h = hm.heap_create(0, 0x10000, 0);
    ASSERT_NE(h, INVALID_HANDLE);
    EXPECT_TRUE(hm.heap_destroy(h));
    // Double destroy must fail
    EXPECT_FALSE(hm.heap_destroy(h));
}

// =============================================================================
// GetProcessHeaps Test
// =============================================================================

TEST_F(HeapExtTest, GetProcessHeapsReturnsAtLeastOne) {
    // Force process heap creation
    Handle ph = hm.get_process_heap();
    ASSERT_NE(ph, INVALID_HANDLE);

    GuestAddr heaps[4] = {};
    u32 count = ext.get_process_heaps(4, heaps);
    EXPECT_GE(count, 1u);
    EXPECT_EQ(heaps[0], ph);
}

// =============================================================================
// Thunk Creation Test
// =============================================================================

TEST_F(HeapExtTest, ThunksCreatedWithExpectedNames) {
    DllExports exports = create_heap_ext_thunks(ext, hm);
    ASSERT_EQ(exports.entries.size(), 11u);

    EXPECT_EQ(exports.entries[0].name, "HeapReAlloc");
    EXPECT_EQ(exports.entries[1].name, "HeapSize");
    EXPECT_EQ(exports.entries[2].name, "HeapValidate");
    EXPECT_EQ(exports.entries[3].name, "HeapCompact");
    EXPECT_EQ(exports.entries[4].name, "HeapCreate");
    EXPECT_EQ(exports.entries[5].name, "HeapDestroy");
    EXPECT_EQ(exports.entries[6].name, "GetProcessHeaps");
    EXPECT_EQ(exports.entries[7].name, "HeapSetInformation");
    EXPECT_EQ(exports.entries[8].name, "HeapAlloc");
    EXPECT_EQ(exports.entries[9].name, "HeapFree");
    EXPECT_EQ(exports.entries[10].name, "GetProcessHeap");
}
