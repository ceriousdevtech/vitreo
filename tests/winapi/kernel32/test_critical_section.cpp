/// @file test_critical_section.cpp
/// @brief Tests for CriticalSectionManager with guest memory layout (WIN32-065)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/synchronization/critical_section.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/memory/guest_memory.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class CriticalSectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        mem = std::make_unique<GuestMemory>();
        auto r = mem->map_region(0x10000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(r.is_ok());
        mgr = std::make_unique<CriticalSectionManager>(*mem);
    }

    void TearDown() override {
        ThreadManager::set_current_thread_id(1);
    }

    std::unique_ptr<GuestMemory> mem;
    std::unique_ptr<CriticalSectionManager> mgr;

    static constexpr GuestAddr CS1 = 0x10000;
    static constexpr GuestAddr CS2 = 0x10100;
};

// =============================================================================
// Initialize Tests
// =============================================================================

TEST_F(CriticalSectionTest, Initialize_SetsLockCountMinusOne) {
    mgr->initialize(CS1);
    auto val = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), cs_layout::UNLOCKED);
}

// Windows gives a section no debug record until its first contended wait:
// DebugInfo reads (PVOID)-1 after InitializeCriticalSection
// (handles_waits_probe.expected:151 "init dbg=ffffffff").
TEST_F(CriticalSectionTest, Initialize_DebugInfoIsMinusOne) {
    ASSERT_FALSE(mgr->initialize(CS1).has_value());
    auto val = mem->read_u32(CS1 + cs_layout::OFF_DEBUG_INFO);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 0xFFFFFFFFu);
}

TEST_F(CriticalSectionTest, Initialize_RecursionCountIsZero) {
    mgr->initialize(CS1);
    auto val = mem->read_u32(CS1 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 0u);
}

// =============================================================================
// Enter / Leave Tests
// =============================================================================

TEST_F(CriticalSectionTest, EnterThenLeave) {
    mgr->initialize(CS1);
    mgr->enter(CS1);
    mgr->leave(CS1);
    // Should not crash or deadlock

    // After leave, LockCount should be back to UNLOCKED
    auto val = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), cs_layout::UNLOCKED);
}

TEST_F(CriticalSectionTest, RecursiveEnter) {
    mgr->initialize(CS1);
    mgr->enter(CS1);
    mgr->enter(CS1);

    auto rec = mem->read_u32(CS1 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(rec.is_ok());
    EXPECT_EQ(rec.value(), 2u);

    // LockCount should be -2 (SP1+: locked, no waiters)
    auto lc = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(lc.is_ok());
    EXPECT_EQ(lc.value(), cs_layout::LOCKED_NO_WAITERS);

    mgr->leave(CS1);
    mgr->leave(CS1);
}

TEST_F(CriticalSectionTest, RecursiveLeaveFullyReleasesForAnotherThread) {
    ThreadManager::set_current_thread_id(1);
    mgr->initialize(CS1);
    mgr->enter(CS1);
    mgr->enter(CS1);
    mgr->leave(CS1);
    mgr->leave(CS1);

    bool acquired = false;
    std::thread contender([&] {
        ThreadManager::set_current_thread_id(2);
        acquired = mgr->try_enter(CS1);
        if (acquired) {
            mgr->leave(CS1);
        }
    });
    contender.join();

    EXPECT_TRUE(acquired);
}

// =============================================================================
// TryEnter Tests
// =============================================================================

TEST_F(CriticalSectionTest, TryEnter_Succeeds) {
    mgr->initialize(CS1);
    EXPECT_TRUE(mgr->try_enter(CS1));
    mgr->leave(CS1);
}

TEST_F(CriticalSectionTest, TryEnter_RecursiveSucceeds) {
    mgr->initialize(CS1);
    mgr->enter(CS1);
    EXPECT_TRUE(mgr->try_enter(CS1));

    auto rec = mem->read_u32(CS1 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(rec.is_ok());
    EXPECT_EQ(rec.value(), 2u);

    mgr->leave(CS1);
    mgr->leave(CS1);
}

// =============================================================================
// Delete Tests
// =============================================================================

TEST_F(CriticalSectionTest, Delete_ZeroesStructure) {
    mgr->initialize(CS1);
    mgr->delete_cs(CS1);

    auto lc = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(lc.is_ok());
    EXPECT_EQ(lc.value(), 0u);

    auto rec = mem->read_u32(CS1 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(rec.is_ok());
    EXPECT_EQ(rec.value(), 0u);

    auto owner = mem->read_u64(CS1 + cs_layout::OFF_OWNING_THREAD);
    ASSERT_TRUE(owner.is_ok());
    EXPECT_EQ(owner.value(), 0u);
}

// Learn LeaveCriticalSection: leave_wake must not resurrect host state for a
// deleted section (no get_or_create after ownership is released).
TEST_F(CriticalSectionTest, LeaveWakeAfterDeleteIsNoOp) {
    mgr->initialize(CS1);
    mgr->enter(CS1);
    mgr->leave(CS1);
    mgr->delete_cs(CS1);
    mgr->leave_wake(CS1);
    EXPECT_EQ(mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT).value_or(1), 0u);
}

// =============================================================================
// SpinCount / InitEx Tests
// =============================================================================

// handles_waits_probe.expected:186-187 "spin_4000 ... spin=0000000000000fa0".
TEST_F(CriticalSectionTest, InitAndSpinCount_StoresSpinCount) {
    EXPECT_FALSE(mgr->initialize_and_spin_count(CS1, 4000).has_value());

    auto sc = mem->read_u32(CS1 + cs_layout::OFF_SPIN_COUNT);
    ASSERT_TRUE(sc.is_ok());
    EXPECT_EQ(sc.value(), 4000u);
}

// handles_waits_probe.expected:161-162 "ex_0 ... spin=0000000000000fa0".
TEST_F(CriticalSectionTest, InitEx_ReturnsTrue) {
    const CsInitExResult r = mgr->initialize_ex(CS1, 4000, 0);
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.fault.has_value());

    auto sc = mem->read_u32(CS1 + cs_layout::OFF_SPIN_COUNT);
    ASSERT_TRUE(sc.is_ok());
    EXPECT_EQ(sc.value(), 4000u);
}

// =============================================================================
// Leave / Recursion Tests
// =============================================================================

TEST_F(CriticalSectionTest, LeaveAfterRecursive_DecrementCorrectly) {
    mgr->initialize(CS1);
    mgr->enter(CS1);
    mgr->enter(CS1);
    mgr->enter(CS1);

    // 3 entries
    auto rec = mem->read_u32(CS1 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(rec.is_ok());
    EXPECT_EQ(rec.value(), 3u);

    mgr->leave(CS1);

    rec = mem->read_u32(CS1 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(rec.is_ok());
    EXPECT_EQ(rec.value(), 2u);

    // Still locked (SP1+: -2)
    auto lc = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(lc.is_ok());
    EXPECT_EQ(lc.value(), cs_layout::LOCKED_NO_WAITERS);

    mgr->leave(CS1);
    mgr->leave(CS1);

    // Fully unlocked
    lc = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(lc.is_ok());
    EXPECT_EQ(lc.value(), cs_layout::UNLOCKED);
}

// =============================================================================
// Multiple Independent CS Tests
// =============================================================================

TEST_F(CriticalSectionTest, MultipleCsIndependent) {
    mgr->initialize(CS1);
    mgr->initialize(CS2);

    mgr->enter(CS1);

    // CS2 should still be unlocked
    auto lc2 = mem->read_u32(CS2 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(lc2.is_ok());
    EXPECT_EQ(lc2.value(), cs_layout::UNLOCKED);

    auto rec2 = mem->read_u32(CS2 + cs_layout::OFF_RECURSION_COUNT);
    ASSERT_TRUE(rec2.is_ok());
    EXPECT_EQ(rec2.value(), 0u);

    // CS1 should be locked (SP1+: -2)
    auto lc1 = mem->read_u32(CS1 + cs_layout::OFF_LOCK_COUNT);
    ASSERT_TRUE(lc1.is_ok());
    EXPECT_EQ(lc1.value(), cs_layout::LOCKED_NO_WAITERS);

    mgr->leave(CS1);
}

TEST_F(CriticalSectionTest, OwningThread_SetOnEnterClearedOnLeave) {
    ThreadManager::set_current_thread_id(7);
    mgr->initialize(CS1);
    mgr->enter(CS1);

    auto owner = mem->read_u64(CS1 + cs_layout::OFF_OWNING_THREAD);
    ASSERT_TRUE(owner.is_ok());
    EXPECT_EQ(owner.value(), 7u);
    EXPECT_EQ(mgr->owning_thread(CS1), 7u);
    EXPECT_EQ(mgr->recursion_count(CS1), 1u);

    mgr->leave(CS1);

    owner = mem->read_u64(CS1 + cs_layout::OFF_OWNING_THREAD);
    ASSERT_TRUE(owner.is_ok());
    EXPECT_EQ(owner.value(), 0u);
    EXPECT_EQ(mgr->owning_thread(CS1), 0u);
    EXPECT_EQ(mgr->recursion_count(CS1), 0u);
}

// =============================================================================
// Spin count (Learn, InitializeCriticalSectionAndSpinCount: on a
// multiprocessor, a thread that finds the section owned "spins dwSpinCount
// times before performing a wait operation on a semaphore that is associated
// with the critical section. If the critical section becomes free during the
// spin operation, the calling thread avoids the wait operation.")
// =============================================================================

class CriticalSectionSpinTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (std::thread::hardware_concurrency() < 2) {
            GTEST_SKIP() << "the spin count applies on multiprocessors only";
        }
        mem = std::make_unique<GuestMemory>();
        // The production model: the lock word lives in the linear
        // reservation, where the contended path parks on it.
        ASSERT_TRUE(mem->enable_linear_reservation(usize{1} << 30));
        ASSERT_TRUE(mem->map_region(0x10000, 0x10000,
                                    MemoryProtection::ReadWrite).is_ok());
        mgr = std::make_unique<CriticalSectionManager>(*mem);
    }
    void TearDown() override { ThreadManager::set_current_thread_id(1); }

    /// The owner holds CS1, a second thread enters it, and the owner leaves
    /// after `hold`. Returns how many contended waits the entry took.
    u64 waits_for_entry_while_held(std::chrono::milliseconds hold) {
        ThreadManager::set_current_thread_id(1);
        mgr->enter(CS1);
        const u64 before = mgr->contended_waits();
        std::atomic<bool> entered{false};
        std::thread waiter([&] {
            ThreadManager::set_current_thread_id(2);
            mgr->enter(CS1);
            entered.store(true);
            mgr->leave(CS1);
        });
        std::this_thread::sleep_for(hold);
        EXPECT_FALSE(entered.load()) << "entered a section that was held";
        mgr->leave(CS1);
        waiter.join();
        EXPECT_TRUE(entered.load());
        return mgr->contended_waits() - before;
    }

    std::unique_ptr<GuestMemory> mem;
    std::unique_ptr<CriticalSectionManager> mgr;
    static constexpr GuestAddr CS1 = 0x10000;
};

TEST_F(CriticalSectionSpinTest, ASectionFreedDuringTheSpinIsTakenWithoutWaiting) {
    // 0x00FFFFFF iterations spin far longer than the 2 ms hold.
    ASSERT_FALSE(mgr->initialize_and_spin_count(CS1, 0x00FFFFFFu).has_value());
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(2)), 0u);
}

// The flag byte is not part of the count: NO_DEBUG_INFO | 0x00FFFFFF
// (stored 0x01FFFFFF) still spins 0x00FFFFFF times, then takes the section.
TEST_F(CriticalSectionSpinTest, TheFlagByteIsNotSpun) {
    ASSERT_TRUE(mgr->initialize_ex(CS1, 0x00FFFFFFu,
                                   cs_flags::CRITICAL_SECTION_NO_DEBUG_INFO).ok);
    ASSERT_EQ(mem->read_u32(CS1 + cs_layout::OFF_SPIN_COUNT).value(), 0x01FFFFFFu);
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(2)), 0u);
}

// A default section spins its 2000 (handles_waits_probe.expected:151), which
// is over long before a 20 ms hold ends; the waiter then waits.
TEST_F(CriticalSectionSpinTest, TheDefaultSpinRunsOutAndWaits) {
    ASSERT_FALSE(mgr->initialize(CS1).has_value());
    ASSERT_EQ(mem->read_u32(CS1 + cs_layout::OFF_SPIN_COUNT).value(), 0x020007D0u);
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(20)), 1u);
}

// SetCriticalSectionSpinCount(cs, 0) leaves no iterations (0x02000000,
// cs_init_flags_probe default_set_0): the waiter waits at once.
TEST_F(CriticalSectionSpinTest, AZeroCountWaitsWithoutSpinning) {
    ASSERT_FALSE(mgr->initialize(CS1).has_value());
    ASSERT_EQ(mgr->set_spin_count(CS1, 0).previous, 2000u);
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(2)), 1u);
}

TEST_F(CriticalSectionSpinTest, ASpinThatRunsOutWaits) {
    // A short spin is over long before a 20 ms hold ends.
    ASSERT_FALSE(mgr->initialize_and_spin_count(CS1, 64).has_value());
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(20)), 1u);
}

// The same spin contract on the x64 layout (SpinCount is a ULONG_PTR at +32):
// a long count takes a briefly held section without waiting, the default
// count (0x020007D0, handles_waits_probe.expected:151) runs out and waits.
TEST_F(CriticalSectionSpinTest, X64LayoutSpinsItsOwnSpinCountField) {
    mgr->note_bitness(true);
    ASSERT_FALSE(mgr->initialize_and_spin_count(CS1, 0x00FFFFFFu).has_value());
    ASSERT_EQ(mem->read_u64(CS1 + cs_layout_64.off_spin_count).value(), 0x00FFFFFFull);
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(2)), 0u);
}

TEST_F(CriticalSectionSpinTest, X64LayoutDefaultSpinRunsOutAndWaits) {
    mgr->note_bitness(true);
    ASSERT_FALSE(mgr->initialize(CS1).has_value());
    ASSERT_EQ(mem->read_u64(CS1 + cs_layout_64.off_spin_count).value(), 0x020007D0ull);
    EXPECT_EQ(waits_for_entry_while_held(std::chrono::milliseconds(20)), 1u);
}
