/// @file test_global_local.cpp
/// @brief Tests for GlobalLocalManager (WIN32-082)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>

#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class GlobalLocalTest : public ::testing::Test {
protected:
    GuestMemory mem;
    VirtualMemoryManager vmm{mem};
    HandleTable handles;
    HeapManager heap_mgr{vmm, mem, handles};
    GlobalLocalManager mgr{heap_mgr};
};

// =============================================================================
// GlobalAlloc Tests
// =============================================================================

TEST_F(GlobalLocalTest, GlobalAlloc_Fixed_ReturnsNonNull) {
    u64 h = mgr.global_alloc(GMEM_FIXED, 64);
    EXPECT_NE(h, 0u);
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, GlobalAlloc_GPTR_ReturnsZeroedMemory) {
    u64 h = mgr.global_alloc(GPTR, 128);
    ASSERT_NE(h, 0u);
    // GPTR = GMEM_FIXED | GMEM_ZEROINIT — handle IS the guest pointer
    u8 buf[128];
    auto rd = mem.read_bytes(static_cast<GuestAddr>(h), buf, 128);
    ASSERT_FALSE(rd.is_err());
    for (u64 i = 0; i < 128; ++i) {
        EXPECT_EQ(buf[i], 0u) << "byte " << i << " not zeroed";
    }
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, GlobalAlloc_Moveable_ReturnsHandle) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 256);
    EXPECT_NE(h, 0u);
    // global_local_probe.expected: a moveable handle is the address of a
    // slot holding the data pointer; x64 handles end in 8 (x86 in 4).
    EXPECT_EQ(h & 0xF, 8u);
    const u64 p = mgr.global_lock(h);
    EXPECT_EQ(mem.read_u64(h).value_or(0), p);
    EXPECT_EQ(mgr.global_handle(p), h);
    mgr.global_unlock(h);
    mgr.global_free(h);
}

// =============================================================================
// GlobalLock / GlobalUnlock Tests
// =============================================================================

TEST_F(GlobalLocalTest, GlobalLock_Moveable_ReturnsPointer) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 64);
    ASSERT_NE(h, 0u);
    u64 ptr = mgr.global_lock(h);
    EXPECT_NE(ptr, 0u);
    // The returned pointer should differ from the synthetic handle
    EXPECT_NE(ptr, h);
    mgr.global_unlock(h);
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, GlobalUnlock_DecrementsLockCount) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 32);
    ASSERT_NE(h, 0u);

    mgr.global_lock(h);
    mgr.global_lock(h);
    // Two locks — unlock should return true (still locked)
    EXPECT_TRUE(mgr.global_unlock(h));
    // Second unlock brings count to 0 — returns false
    EXPECT_FALSE(mgr.global_unlock(h));

    mgr.global_free(h);
}

// =============================================================================
// GlobalFree Tests
// =============================================================================

TEST_F(GlobalLocalTest, GlobalFree_Fixed_Succeeds) {
    u64 h = mgr.global_alloc(GMEM_FIXED, 64);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(mgr.global_free(h), 0u);
}

TEST_F(GlobalLocalTest, GlobalFree_Moveable_Succeeds) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 64);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(mgr.global_free(h), 0u);
}

// =============================================================================
// GlobalSize Tests
// =============================================================================

TEST_F(GlobalLocalTest, GlobalSize_ReturnsCorrectSize) {
    u64 h = mgr.global_alloc(GMEM_FIXED, 512);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(mgr.global_size(h), 512u);
    mgr.global_free(h);
}

// =============================================================================
// GlobalReAlloc Tests
// =============================================================================

TEST_F(GlobalLocalTest, GlobalReAlloc_GrowsAllocation) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 64);
    ASSERT_NE(h, 0u);

    u64 h2 = mgr.global_realloc(h, 256, 0);
    EXPECT_NE(h2, 0u);
    EXPECT_EQ(mgr.global_size(h2), 256u);

    mgr.global_free(h2);
}

TEST_F(GlobalLocalTest, GlobalReAlloc_ShrinksAllocation) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 256);
    ASSERT_NE(h, 0u);

    u64 h2 = mgr.global_realloc(h, 32, 0);
    EXPECT_NE(h2, 0u);
    EXPECT_EQ(mgr.global_size(h2), 32u);

    mgr.global_free(h2);
}

// =============================================================================
// GlobalFlags Tests
// =============================================================================

TEST_F(GlobalLocalTest, GlobalFlags_ReturnsLockCount) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 128);
    ASSERT_NE(h, 0u);

    mgr.global_lock(h);
    mgr.global_lock(h);
    u32 f = mgr.global_flags(h);
    EXPECT_EQ(f & 0xFF, 2u);  // lock count in low byte

    mgr.global_unlock(h);
    mgr.global_unlock(h);
    mgr.global_free(h);
}

// =============================================================================
// Local* API Tests (mirrors Global*)
// =============================================================================

TEST_F(GlobalLocalTest, LocalAlloc_MirrorsGlobalAlloc) {
    // LocalAlloc is implemented via the same code path
    u64 h = mgr.global_alloc(GMEM_FIXED, 100);
    EXPECT_NE(h, 0u);
    EXPECT_EQ(mgr.global_size(h), 100u);
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, LocalFree_MirrorsGlobalFree) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 64);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(mgr.global_free(h), 0u);
    // Double-free should return the handle (failure)
    EXPECT_NE(mgr.global_free(h), 0u);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(GlobalLocalTest, ZeroSizeAllocation_Succeeds) {
    // global_local_probe.expected zero_fixed: globalsize=0x1 localsize=0x0.
    u64 h = mgr.global_alloc(GMEM_FIXED, 0);
    EXPECT_NE(h, 0u);
    EXPECT_EQ(mgr.global_size(h), 1u);
    mgr.global_free(h);
    u64 l = mgr.global_alloc(GMEM_FIXED, 0, /*local=*/true);
    EXPECT_NE(l, 0u);
    EXPECT_EQ(mgr.global_size(l), 0u);
    mgr.global_free(l);
}

TEST_F(GlobalLocalTest, GlobalFree_NullHandle_Succeeds) {
    // Freeing NULL is a no-op — returns 0 (success)
    EXPECT_EQ(mgr.global_free(0), 0u);
}

TEST_F(GlobalLocalTest, GlobalLock_Fixed_ReturnsSameHandle) {
    u64 h = mgr.global_alloc(GMEM_FIXED, 64);
    ASSERT_NE(h, 0u);
    // For FIXED allocations, lock returns the handle itself
    EXPECT_EQ(mgr.global_lock(h), h);
    mgr.global_unlock(h);
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, GlobalReAlloc_ZeroinitExtendsCleared) {
    u64 h = mgr.global_alloc(GMEM_MOVEABLE, 16);
    ASSERT_NE(h, 0u);

    // Write a pattern into the first 16 bytes via guest memory
    u64 ptr = mgr.global_lock(h);
    u8 pattern[16];
    std::memset(pattern, 0xAB, 16);
    (void)mem.write_bytes(static_cast<GuestAddr>(ptr), pattern, 16);
    mgr.global_unlock(h);

    // Grow with GMEM_ZEROINIT
    u64 h2 = mgr.global_realloc(h, 64, GMEM_ZEROINIT);
    ASSERT_NE(h2, 0u);

    u64 ptr2 = mgr.global_lock(h2);
    u8 buf[64];
    auto rd = mem.read_bytes(static_cast<GuestAddr>(ptr2), buf, 64);
    ASSERT_FALSE(rd.is_err());
    // Extended region (bytes 16..63) should be zeroed
    for (u64 i = 16; i < 64; ++i) {
        EXPECT_EQ(buf[i], 0u) << "byte " << i << " not zeroed after realloc";
    }
    mgr.global_unlock(h2);
    mgr.global_free(h2);
}

TEST_F(GlobalLocalTest, ConcurrentAllocationsRemainUniqueAndFreeSafely) {
    constexpr usize thread_count = 8;
    constexpr usize allocations_per_thread = 128;
    std::vector<std::vector<u64>> allocations(
        thread_count, std::vector<u64>(allocations_per_thread));
    std::atomic<usize> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;

    for (usize thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (usize i = 0; i < allocations_per_thread; ++i) {
                const u32 flags = (i & 1u) != 0 ? GMEM_MOVEABLE : GMEM_FIXED;
                const u64 size = 32u + i;
                const u64 handle = mgr.global_alloc(flags, size);
                allocations[thread][i] = handle;
                if (handle == 0 || mgr.global_size(handle) != size) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    ASSERT_FALSE(failed.load(std::memory_order_acquire));
    std::unordered_set<u64> unique;
    for (const auto& thread_allocations : allocations) {
        unique.insert(thread_allocations.begin(), thread_allocations.end());
    }
    ASSERT_EQ(unique.size(), thread_count * allocations_per_thread);

    ready.store(0, std::memory_order_release);
    start.store(false, std::memory_order_release);
    workers.clear();
    for (usize thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&, thread] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (u64 handle : allocations[thread]) {
                if (mgr.global_free(handle) != 0) {
                    failed.store(true, std::memory_order_release);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    for (u64 handle : unique) EXPECT_EQ(mgr.global_size(handle), 0u);
}

// =============================================================================
// Audit 01 GlobalAlloc/GlobalFree/GlobalReAlloc/LocalFree findings.
// Expected values: tools/oracle_probe/global_local_probe.expected (Windows 11
// x64) and the flag probe summarised in global_local.cpp.
// =============================================================================

namespace {
void fill(GuestMemory& mem, u64 p, u32 n, u8 seed) {
    for (u32 i = 0; i < n; ++i) (void)mem.write_u8(p + i, static_cast<u8>(seed + i));
}
bool check(GuestMemory& mem, u64 p, u32 n, u8 seed) {
    for (u32 i = 0; i < n; ++i) {
        if (mem.read_u8(p + i).value_or(0) != static_cast<u8>(seed + i)) return false;
    }
    return true;
}
}  // namespace

TEST_F(GlobalLocalTest, ModifyChangesAttributesOnlyAndConvertsFixedToMoveable) {
    // modify_fixed_to_moveable: new handle, same size, data copied.
    const u64 p = mgr.global_alloc(GMEM_FIXED, 0x5000);
    fill(mem, p, 32, 7);
    set_last_error(0xDEAD);
    const u64 h = mgr.global_realloc(p, 0, GMEM_MODIFY | GMEM_MOVEABLE);
    ASSERT_NE(h, 0u);
    EXPECT_NE(h, p);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(mgr.global_size(h), 0x5000u);
    const u64 q = mgr.global_lock(h);
    EXPECT_TRUE(check(mem, q, 32, 7));
    EXPECT_EQ(mgr.global_handle(q), h);
    mgr.global_unlock(h);
    EXPECT_EQ(mgr.global_free(h), 0u);

    // modify_fixed_plain: fixed stays fixed, dwBytes ignored.
    const u64 f = mgr.global_alloc(GMEM_FIXED, 0x5000);
    EXPECT_EQ(mgr.global_realloc(f, 0x9000, GMEM_MODIFY), f);
    EXPECT_EQ(mgr.global_size(f), 0x5000u);
    EXPECT_EQ(mgr.global_flags(f), 0u);
    // l_modify_same: LocalReAlloc(fixed, 0, LMEM_MODIFY|LMEM_MOVEABLE) keeps it.
    EXPECT_EQ(mgr.global_realloc(f, 0, GMEM_MODIFY | GMEM_MOVEABLE, true), f);

    // modify_moveable: size kept; DISCARDABLE set then cleared.
    const u64 m = mgr.global_alloc(GMEM_MOVEABLE, 0x5000);
    EXPECT_EQ(mgr.global_realloc(m, 0x9000, GMEM_MODIFY | GMEM_MOVEABLE), m);
    EXPECT_EQ(mgr.global_size(m), 0x5000u);
    EXPECT_EQ(mgr.global_realloc(m, 0, GMEM_MODIFY | GMEM_DISCARDABLE | GMEM_MOVEABLE), m);
    EXPECT_EQ(mgr.global_flags(m), GMEM_DISCARDABLE);
    EXPECT_EQ(mgr.global_flags(m, true), LMEM_DISCARDABLE);
    EXPECT_EQ(mgr.global_realloc(m, 0, GMEM_MODIFY), m);
    EXPECT_EQ(mgr.global_flags(m), 0u);
    mgr.global_free(m);
    mgr.global_free(f);
}

TEST_F(GlobalLocalTest, ReAllocInPlaceRuleForFixedAndLockedObjects) {
    const u64 p = mgr.global_alloc(GMEM_FIXED, 0x5000);
    fill(mem, p, 64, 3);
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_realloc(p, 0x100000, 0), 0u);          // fixed_grow_noflag
    EXPECT_EQ(get_last_error(), 8u);
    EXPECT_EQ(mgr.global_size(p), 0x5000u);
    EXPECT_TRUE(check(mem, p, 64, 3));
    EXPECT_EQ(mgr.global_realloc(p, 0x4000, 0), p);             // fixed_shrink_noflag
    EXPECT_EQ(mgr.global_realloc(p, 0x4800, 0), p);             // fixed_shrink48_noflag
    EXPECT_EQ(mgr.global_realloc(p, 0x4000, GMEM_MOVEABLE), p); // stays FIXED
    EXPECT_EQ(mgr.global_flags(p), 0u);
    const u64 c = mgr.global_realloc(p, 0x100000, GMEM_MOVEABLE); // fixed_grow_moveable
    ASSERT_NE(c, 0u);
    EXPECT_EQ(mgr.global_lock(c), c);
    EXPECT_TRUE(check(mem, c, 64, 3));
    mgr.global_free(c);

    const u64 h = mgr.global_alloc(GMEM_MOVEABLE, 0x5000);
    const u64 hp = mgr.global_lock(h);
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_realloc(h, 0x100000, 0), 0u);          // locked_grow_noflag
    EXPECT_EQ(get_last_error(), 8u);
    EXPECT_EQ(mgr.global_realloc(h, 0x4000, 0), h);             // locked_shrink_noflag
    EXPECT_EQ(mgr.global_lock(h), hp);
    mgr.global_unlock(h);
    EXPECT_EQ(mgr.global_realloc(h, 0x100000, GMEM_MOVEABLE), h); // locked_grow_moveable
    EXPECT_EQ(mgr.global_size(h), 0x100000u);
    EXPECT_EQ(mgr.global_flags(h), 1u);
    EXPECT_EQ(mem.read_u64(h).value_or(0), mgr.global_lock(h));
    mgr.global_unlock(h);
    mgr.global_unlock(h);
    EXPECT_EQ(mgr.global_realloc(h, 0x200000, 0), h);           // unlocked_grow_noflag
    EXPECT_EQ(mgr.global_size(h), 0x200000u);
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, ZeroBytesDiscardAndFreeOfLockedObject) {
    // moveable0: discarded object; lock fails with ERROR_DISCARDED; revive.
    const u64 h = mgr.global_alloc(GMEM_MOVEABLE, 0);
    ASSERT_NE(h, 0u);
    EXPECT_EQ(mgr.global_flags(h), GMEM_DISCARDED);
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_size(h), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(mgr.global_lock(h), 0u);
    EXPECT_EQ(get_last_error(), 157u);
    EXPECT_EQ(mgr.global_realloc(h, 32, GMEM_MOVEABLE), h);
    EXPECT_EQ(mgr.global_flags(h), 0u);
    EXPECT_EQ(mgr.global_size(h), 32u);
    // realloc0_moveable_flag / _noflag / _locked.
    EXPECT_EQ(mgr.global_realloc(h, 0, GMEM_MOVEABLE), h);
    EXPECT_EQ(mgr.global_flags(h), GMEM_DISCARDED);
    EXPECT_EQ(mem.read_u64(h).value_or(1), 0u);
    const u64 h2 = mgr.global_alloc(GMEM_MOVEABLE, 0x5000);
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_realloc(h2, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    (void)mgr.global_lock(h2);
    EXPECT_EQ(mgr.global_realloc(h2, 0, GMEM_MOVEABLE), 0u);
    EXPECT_EQ(mgr.global_size(h2), 0x5000u);
    // free_locked: "will free a locked memory object".
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_free(h2), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(mgr.global_free(h2), h2);                        // twice -> handle
    EXPECT_EQ(get_last_error(), 6u);
    // realloc0_fixed: size becomes 0 in place.
    const u64 p = mgr.global_alloc(GMEM_FIXED, 0x5000);
    EXPECT_EQ(mgr.global_realloc(p, 0, 0), p);
    EXPECT_EQ(mgr.global_size(p), 0u);
    mgr.global_free(p);
    mgr.global_free(h);
}

TEST_F(GlobalLocalTest, FlagValidationAndUnlockErrors) {
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_alloc(GMEM_MOVEABLE | 0x80000000u, 16), 0u);
    EXPECT_EQ(get_last_error(), 0x57u);
    EXPECT_EQ(mgr.global_alloc(GMEM_MOVEABLE | GMEM_DDESHARE, 16, true), 0u);
    const u64 h = mgr.global_alloc(GMEM_MOVEABLE | GMEM_DISCARDABLE | GMEM_DDESHARE, 16);
    EXPECT_EQ(mgr.global_flags(h), GMEM_DISCARDABLE | GMEM_DDESHARE);
    EXPECT_EQ(mgr.global_flags(h, true), LMEM_DISCARDABLE);
    EXPECT_EQ(mgr.global_realloc(h, 32, GMEM_MOVEABLE | GMEM_DISCARDABLE), 0u);
    EXPECT_EQ(get_last_error(), 0x57u);
    // unlock of an unlocked moveable object: FALSE, ERROR_NOT_LOCKED.
    EXPECT_FALSE(mgr.global_unlock(h));
    EXPECT_EQ(get_last_error(), 158u);
    (void)mgr.global_lock(h);
    set_last_error(0xDEAD);
    EXPECT_FALSE(mgr.global_unlock(h));
    EXPECT_EQ(get_last_error(), 0u);
    // Unknown handles.
    set_last_error(0xDEAD);
    EXPECT_EQ(mgr.global_flags(0x12345678), GMEM_INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(mgr.global_handle(h), 0u);
    mgr.global_free(h);
    // A plain process-heap block is a FIXED object (GlobalSize/GlobalFree).
    const u64 hp = heap_mgr.heap_alloc(heap_mgr.get_process_heap(), 0, 64);
    EXPECT_EQ(mgr.global_handle(hp), hp);
    EXPECT_EQ(mgr.global_size(hp), 64u);
    EXPECT_EQ(mgr.global_free(hp), 0u);
}
