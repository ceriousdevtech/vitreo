/// @file test_critical_section_accel.cpp
/// @brief Contention tests for the accelerated critical-section protocol.
///
/// The accelerated path moves the authority for a critical section from a host
/// mutex to the guest's own LockCount word, so that emitted guest code and host
/// callers contend for the same thing. The only validation worth having for a
/// lock is that it still excludes under real contention, so these tests run
/// many host threads through the manager and check an unguarded counter.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/synchronization/critical_section.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kCsBase = 0x00400000;

class CriticalSectionAccelTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // The gate is read once per process; make sure it is the accel path.
        unsetenv("VITREO_NO_CS_ACCEL");   // accelerated path is the default
    }

    void SetUp() override {
        // The accelerated protocol reaches the guest's LockCount through
        // linear_host(), which is only meaningful when a linear model is
        // active. Without this the manager silently falls back to the legacy
        // host-mutex path and these tests validate the wrong implementation --
        // which is exactly what happened the first time they were written.
        ASSERT_TRUE(memory.enable_linear_reservation(0x1000000))
            << "accel path needs linear guest memory";
        ASSERT_TRUE(memory.map_region(kCsBase, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        mgr = std::make_unique<CriticalSectionManager>(memory);
        mgr->note_bitness(false);
    }

    GuestMemory memory;
    std::unique_ptr<CriticalSectionManager> mgr;
};

} // namespace

TEST_F(CriticalSectionAccelTest, InitializeLeavesSectionUnlocked) {
    mgr->initialize(kCsBase);
    const auto lock_count = memory.read_u32(kCsBase + 4);
    ASSERT_TRUE(lock_count.is_ok());
    EXPECT_EQ(lock_count.value(), cs_layout::UNLOCKED)
        << "a fresh section must read as unlocked, or every acquire hangs";
}

TEST_F(CriticalSectionAccelTest, UncontendedEnterLeaveRoundTrips) {
    mgr->initialize(kCsBase);
    mgr->enter(kCsBase);
    EXPECT_EQ(memory.read_u32(kCsBase + 8).value_or(0u), 1u) << "RecursionCount";
    mgr->leave(kCsBase);
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u), cs_layout::UNLOCKED);
    EXPECT_EQ(memory.read_u32(kCsBase + 0x0C).value_or(1u), 0u) << "OwningThread";
}

TEST_F(CriticalSectionAccelTest, RecursiveEnterNestsAndUnwinds) {
    mgr->initialize(kCsBase);
    mgr->enter(kCsBase);
    mgr->enter(kCsBase);
    mgr->enter(kCsBase);
    EXPECT_EQ(memory.read_u32(kCsBase + 8).value_or(0u), 3u);
    mgr->leave(kCsBase);
    mgr->leave(kCsBase);
    EXPECT_NE(memory.read_u32(kCsBase + 4).value_or(0u), cs_layout::UNLOCKED)
        << "still held after two of three leaves";
    mgr->leave(kCsBase);
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u), cs_layout::UNLOCKED);
}

// The accelerated protocol requires InitializeCriticalSection, as Windows does.
// It cannot lazily publish an unlocked word the way the legacy path did: with
// the guest fast path in play, the manager's first sight of a section may be
// while a fast-path owner holds it, and "uninitialized garbage" is
// indistinguishable from "held" by inspecting the word. Initializing clears
// whatever was there.
TEST_F(CriticalSectionAccelTest, InitializeClearsPreexistingGarbage) {
    const GuestAddr cs = kCsBase + 0x100;
    ASSERT_TRUE(memory.write_u32(cs + 4, 0xDEADBEEFu).is_ok());

    mgr->initialize(cs);
    EXPECT_EQ(memory.read_u32(cs + 4).value_or(0u), cs_layout::UNLOCKED);

    mgr->enter(cs);
    mgr->leave(cs);
    EXPECT_EQ(memory.read_u32(cs + 4).value_or(0u), cs_layout::UNLOCKED);
}

TEST_F(CriticalSectionAccelTest, TryEnterFailsWhileHeldByAnotherThread) {
    mgr->initialize(kCsBase);
    mgr->enter(kCsBase);

    bool acquired = true;
    std::thread other([&] {
        ThreadManager::set_current_thread_id(777u);
        acquired = mgr->try_enter(kCsBase);
    });
    other.join();
    EXPECT_FALSE(acquired);

    mgr->leave(kCsBase);
    std::thread after([&] {
        ThreadManager::set_current_thread_id(778u);
        acquired = mgr->try_enter(kCsBase);
    });
    after.join();
    EXPECT_TRUE(acquired) << "must be acquirable once released";
}

// The real test: if the protocol does not exclude, this counter loses updates.
TEST_F(CriticalSectionAccelTest, ExcludesUnderContention) {
    mgr->initialize(kCsBase);

    constexpr int kThreads = 8;
    constexpr int kIters = 4000;
    volatile long long counter = 0;      // deliberately unguarded
    std::atomic<int> max_inside{0};
    std::atomic<int> inside{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            // Every worker needs its OWN guest TID. The thread_local defaults
            // to 1 for all of them, and since the protocol decides recursion by
            // comparing OwningThread to the current TID, identical TIDs make
            // every thread believe it already owns the section and walk
            // straight in. That is a broken TEST, not a broken lock -- it is
            // what made this suite report five threads inside at once.
            ThreadManager::set_current_thread_id(static_cast<u32>(100 + t));
            for (int i = 0; i < kIters; ++i) {
                mgr->enter(kCsBase);
                const int now = inside.fetch_add(1) + 1;
                int prev = max_inside.load();
                while (now > prev &&
                       !max_inside.compare_exchange_weak(prev, now)) {}
                counter = counter + 1;
                inside.fetch_sub(1);
                mgr->leave(kCsBase);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(max_inside.load(), 1) << "two threads were inside at once";
    EXPECT_EQ(counter, static_cast<long long>(kThreads) * kIters)
        << "lost updates mean the section did not exclude";
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u), cs_layout::UNLOCKED)
        << "section must be left unlocked";
}

// Contention plus recursion, which is where a hand-rolled protocol usually
// drops a wakeup and hangs.
TEST_F(CriticalSectionAccelTest, ExcludesWithRecursionUnderContention) {
    mgr->initialize(kCsBase);

    constexpr int kThreads = 6;
    constexpr int kIters = 1500;
    volatile long long counter = 0;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ThreadManager::set_current_thread_id(static_cast<u32>(200 + t));
            for (int i = 0; i < kIters; ++i) {
                mgr->enter(kCsBase);
                mgr->enter(kCsBase);
                counter = counter + 1;
                mgr->leave(kCsBase);
                mgr->leave(kCsBase);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(counter, static_cast<long long>(kThreads) * kIters);
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u), cs_layout::UNLOCKED);
}

// Regression: with the emitted guest fast path enabled, an uncontended acquire
// never enters the host, so the FIRST time the manager sees a section can be
// inside enter_slow -- while that section is already held. Creating host
// bookkeeping must not disturb the live lock word. Getting this wrong reset a
// held lock to unlocked, let two threads own it, and deadlocked the guest.
TEST_F(CriticalSectionAccelTest, FirstHostSightOfHeldSectionDoesNotResetIt) {
    const GuestAddr cs = kCsBase + 0x200;   // never initialize()d here

    // Simulate what the guest fast path leaves behind on a successful acquire:
    // locked, no waiters (SP1+ LockCount = -2), owned by some other thread.
    ASSERT_TRUE(memory.write_u32(cs + 0x04, cs_layout::LOCKED_NO_WAITERS).is_ok());
    ASSERT_TRUE(memory.write_u32(cs + 0x08, 1u).is_ok());           // Recursion
    ASSERT_TRUE(memory.write_u32(cs + 0x0C, 0xABCDEF01u).is_ok());  // Owner

    // The manager's first ever contact with this section, from the contended
    // path. try_enter is the non-blocking way to reach the same bookkeeping.
    const bool got = mgr->try_enter(cs);

    EXPECT_FALSE(got) << "a held section must not be acquirable";
    EXPECT_EQ(memory.read_u32(cs + 0x04).value_or(9u), cs_layout::LOCKED_NO_WAITERS)
        << "first host sight must not reset a held lock word";
    EXPECT_EQ(memory.read_u32(cs + 0x0C).value_or(0u), 0xABCDEF01u)
        << "and must not clear the existing owner";
    EXPECT_EQ(memory.read_u32(cs + 0x08).value_or(0u), 1u);
}

// Regression: linear_host() is pure arithmetic -- base + addr, with no
// rejection of any address. So the accel path's lock word is whatever pointer
// the guest handed EnterCriticalSection, dereferenced by the HOST as a
// std::atomic<u32>. Two ways that kills the runtime instead of the guest:
//
//  * an unaligned CRITICAL_SECTION -- ARM64 atomics require natural alignment
//    and raise SIGBUS otherwise (observed under x64 guest-base memory, 34
//    identical crashes in one Steam run, all at an unaligned lock word);
//  * an address that is inside the reservation but not mapped -- present but
//    inaccessible, so the deref is a host SIGSEGV rather than a translation
//    failure.
//
// Both must fall back to the managed path, which is the general implementation
// the accel path optimizes. A guest bug must not be a host crash.
TEST_F(CriticalSectionAccelTest, UnalignedSectionDoesNotFaultTheHost) {
    const GuestAddr cs = kCsBase + 0x301;   // lock word lands at +0x305
    ASSERT_NE((cs + 4u) & 3u, 0u) << "the point of this test is the misalignment";

    mgr->initialize(cs);
    mgr->enter(cs);
    EXPECT_TRUE(mgr->try_enter(cs)) << "recursive acquire by the owner";
    mgr->leave(cs);
    mgr->leave(cs);
}

TEST_F(CriticalSectionAccelTest, UnmappedSectionDoesNotFaultTheHost) {
    // Inside the 16 MiB reservation, outside every mapped region.
    const GuestAddr cs = kCsBase + 0x400000;
    ASSERT_FALSE(memory.is_mapped(cs, 0x18));

    mgr->initialize(cs);
    mgr->enter(cs);
    mgr->leave(cs);
}

// Windows cs_lockcount_probe: held=-2, one blocked waiter → LockCount=-6 and
// LockSemaphore=(HANDLE)-1 (keyed-event sentinel).
TEST_F(CriticalSectionAccelTest, Sp1LockCountReflectsWaiterAndKeyedEvent) {
    mgr->initialize(kCsBase);
    mgr->enter(kCsBase);
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u),
              cs_layout::LOCKED_NO_WAITERS);
    EXPECT_EQ(memory.read_u32(kCsBase + 0x10).value_or(1u), 0u)
        << "LockSemaphore stays NULL until first contention";

    std::atomic<bool> waiter_inside{false};
    std::thread waiter([&] {
        ThreadManager::set_current_thread_id(4242u);
        mgr->enter(kCsBase);
        waiter_inside.store(true, std::memory_order_release);
        mgr->leave(kCsBase);
    });
    // Give the waiter time to park inside enter_slow.
    for (int i = 0; i < 200 && !waiter_inside.load(std::memory_order_acquire);
         ++i) {
        const u32 lc = memory.read_u32(kCsBase + 4).value_or(0u);
        if (lc == cs_layout::LOCKED_NO_WAITERS - cs_layout::WAITER_INC) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u),
              cs_layout::LOCKED_NO_WAITERS - cs_layout::WAITER_INC)
        << "one waiter → LockCount -6 (SP1+)";
    EXPECT_EQ(memory.read_u32(kCsBase + 0x10).value_or(0u), ~u32{0})
        << "LockSemaphore becomes (HANDLE)-1 on first contention";

    mgr->leave(kCsBase);
    waiter.join();
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value_or(0u), cs_layout::UNLOCKED);
    EXPECT_EQ(memory.read_u32(kCsBase + 0x10).value_or(0u), ~u32{0})
        << "keyed-event sentinel persists after contention";
}

// Learn LeaveCriticalSection: releasing a contended section wakes a waiter,
// which then acquires it. The wake must end the park itself; a waiter that
// only notices the release when its internal poll timeout (50 ms) expires
// turns every contended enter into a 50 ms stall.
TEST_F(CriticalSectionAccelTest, LeaveWakesParkedWaiterPromptly) {
    mgr->initialize(kCsBase);
    std::vector<double> handoff_ms;
    for (int round = 0; round < 9; ++round) {
        mgr->enter(kCsBase);
        std::atomic<bool> acquired{false};
        std::chrono::steady_clock::time_point acquired_at;
        std::thread waiter([&] {
            ThreadManager::set_current_thread_id(6000u + round);
            mgr->enter(kCsBase);
            acquired_at = std::chrono::steady_clock::now();
            acquired.store(true, std::memory_order_release);
            mgr->leave(kCsBase);
        });
        for (int i = 0; i < 500; ++i) {
            if (memory.read_u32(kCsBase + 4).value_or(0u) ==
                cs_layout::LOCKED_NO_WAITERS - cs_layout::WAITER_INC) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(memory.read_u32(kCsBase + 4).value_or(0u),
                  cs_layout::LOCKED_NO_WAITERS - cs_layout::WAITER_INC)
            << "waiter never parked";
        const auto released_at = std::chrono::steady_clock::now();
        mgr->leave(kCsBase);
        waiter.join();
        ASSERT_TRUE(acquired.load(std::memory_order_acquire));
        handoff_ms.push_back(
            std::chrono::duration<double, std::milli>(acquired_at - released_at)
                .count());
    }
    std::sort(handoff_ms.begin(), handoff_ms.end());
    EXPECT_LT(handoff_ms[handoff_ms.size() / 2], 20.0)
        << "median release-to-acquire handoff must not wait out the poll";
}

TEST_F(CriticalSectionAccelTest, BeginShutdownUnparksEnterSlowWaiter) {
    mgr->initialize(kCsBase);
    mgr->enter(kCsBase);

    std::atomic<bool> waiter_returned{false};
    std::thread waiter([&] {
        ThreadManager::set_current_thread_id(5150u);
        mgr->enter(kCsBase); // blocks in enter_slow
        waiter_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(waiter_returned.load(std::memory_order_acquire));

    mgr->begin_shutdown();
    waiter.join();
    EXPECT_TRUE(waiter_returned.load(std::memory_order_acquire))
        << "enter_slow must return on process-exit escape without owning";
    // Holder still owns; Leave is still valid for the owner.
    mgr->leave(kCsBase);
}

// Review finding 10 (use after free): once LeaveCriticalSection's releasing
// store lands, a peer may legally acquire the section, leave it and
// DeleteCriticalSection it. The releaser must then touch nothing that delete
// freed. Section states are type-stable (recycled, never freed), so the late
// releaser touches at worst a recycled state. The hook (a test-build-only
// instantiation, see critical_section_test_seam.cpp) runs exactly in that
// window and performs the peer's acquire / leave / delete / re-initialise.
namespace {
CriticalSectionManager* g_release_race_mgr = nullptr;
std::atomic<int> g_release_race_runs{0};
void delete_in_release_window(GuestAddr cs) {
    if (g_release_race_runs.fetch_add(1) != 0) return;   // the first leave only
    std::thread peer([cs] {
        ThreadManager::set_current_thread_id(4242u);
        EXPECT_TRUE(g_release_race_mgr->try_enter(cs));
        g_release_race_mgr->leave(cs);
        g_release_race_mgr->delete_cs(cs);
        g_release_race_mgr->initialize(cs);   // a new section at the same address
    });
    peer.join();
}
} // namespace

TEST_F(CriticalSectionAccelTest, LeaveSurvivesAPeerDeletingTheSectionAfterItsReleasingStore) {
    mgr->initialize(kCsBase);
    ThreadManager::set_current_thread_id(1u);
    mgr->enter(kCsBase);
    g_release_race_mgr = mgr.get();
    g_release_race_runs = 0;
    // The test build's hooked instantiation of the accelerated Leave; the
    // shipping leave() is the same template without the hook.
    mgr->leave_with_released_hook_for_test(kCsBase, delete_in_release_window);
    EXPECT_EQ(g_release_race_runs.load(), 1);
    // The deleted section's state was recycled for the new one, not freed.
    EXPECT_EQ(mgr->state_objects_for_test(), 1u);
    // The re-initialised section works as a fresh one.
    EXPECT_EQ(mgr->recursion_count(kCsBase), 0u);
    mgr->enter(kCsBase);
    EXPECT_EQ(mgr->recursion_count(kCsBase), 1u);
    mgr->leave(kCsBase);
    EXPECT_EQ(memory.read_u32(kCsBase + 4).value(), cs_layout::UNLOCKED);
}

// The same race without a hook, many times: a releaser hands the section to
// a waiter (parked, or spinning with a long count) that leaves and deletes
// it at once, while the releaser may still be inside its wake. State memory
// stays bounded by the peak number of live sections (here: one).
TEST_F(CriticalSectionAccelTest, StressReleaseAcquireDeleteRace) {
    constexpr int kRounds = 3000;
    for (int round = 0; round < kRounds; ++round) {
        const u32 spin = (round % 3 == 0) ? 0x00FFFFFFu : 1u;
        ASSERT_FALSE(mgr->initialize_and_spin_count(kCsBase, spin).has_value());
        ThreadManager::set_current_thread_id(1u);
        mgr->enter(kCsBase);
        std::atomic<bool> started{false};
        std::thread peer([&] {
            ThreadManager::set_current_thread_id(2u);
            started.store(true);
            mgr->enter(kCsBase);          // spins or parks
            mgr->leave(kCsBase);
            mgr->delete_cs(kCsBase);
        });
        while (!started.load()) std::this_thread::yield();
        if (round % 2 == 0) std::this_thread::yield();
        mgr->leave(kCsBase);
        peer.join();
    }
    EXPECT_EQ(mgr->state_objects_for_test(), 1u);
}

// A section deleted while its deleter still holds it (undefined per Learn,
// and common) must not hand a still-locked host mutex to the next section
// that reuses its state. Unaligned sections take the host-mutex path.
TEST_F(CriticalSectionAccelTest, LegacySectionDeletedWhileItsDeleterHoldsItIsRecycledUnlocked) {
    const GuestAddr a = kCsBase + 0x501;
    const GuestAddr b = kCsBase + 0x541;
    ThreadManager::set_current_thread_id(1u);
    ASSERT_FALSE(mgr->initialize(a).has_value());
    mgr->enter(a);
    mgr->enter(a);                     // held twice by this thread
    mgr->delete_cs(a);
    ASSERT_FALSE(mgr->initialize(b).has_value());
    EXPECT_EQ(mgr->state_objects_for_test(), 1u) << "b reuses a's state";
    bool got = false;
    std::thread other([&] {
        ThreadManager::set_current_thread_id(2u);
        got = mgr->try_enter(b);
        if (got) mgr->leave(b);
    });
    other.join();
    EXPECT_TRUE(got) << "the recycled state's host mutex was still held";
}

// ...and a section another thread still holds (or waits on) when it is
// deleted keeps its state out of reuse altogether: that thread may touch it
// again, and an unrelated section must not share its mutex.
TEST_F(CriticalSectionAccelTest, LegacySectionDeletedWhileAnotherThreadHoldsItIsRetired) {
    const GuestAddr a = kCsBase + 0x581;
    const GuestAddr b = kCsBase + 0x5C1;
    ASSERT_FALSE(mgr->initialize(a).has_value());
    std::atomic<bool> held{false};
    std::thread holder([&] {
        ThreadManager::set_current_thread_id(3u);
        mgr->enter(a);
        held.store(true);
        // Exits still holding the deleted section.
    });
    holder.join();
    ASSERT_TRUE(held.load());
    ThreadManager::set_current_thread_id(1u);
    mgr->delete_cs(a);
    ASSERT_FALSE(mgr->initialize(b).has_value());
    EXPECT_EQ(mgr->state_objects_for_test(), 2u) << "a's state must not be reused";
    EXPECT_TRUE(mgr->try_enter(b));
    mgr->leave(b);
}

TEST_F(CriticalSectionAccelTest, SectionDeletedWithAParkedWaiterIsRetired) {
    ThreadManager::set_current_thread_id(1u);
    mgr->initialize(kCsBase);
    mgr->enter(kCsBase);
    std::thread waiter([&] {
        ThreadManager::set_current_thread_id(7u);
        mgr->enter(kCsBase);   // parks; returns at begin_shutdown without owning
    });
    for (int i = 0; i < 2000; ++i) {
        if (memory.read_u32(kCsBase + 4).value_or(0u) ==
            cs_layout::LOCKED_NO_WAITERS - cs_layout::WAITER_INC) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(memory.read_u32(kCsBase + 4).value_or(0u),
              cs_layout::LOCKED_NO_WAITERS - cs_layout::WAITER_INC)
        << "waiter never parked";
    mgr->delete_cs(kCsBase);           // undefined per Learn: a thread waits
    ASSERT_FALSE(mgr->initialize(kCsBase + 0x100).has_value());
    EXPECT_EQ(mgr->state_objects_for_test(), 2u)
        << "a state with a registered waiter must not serve another section";
    mgr->begin_shutdown();
    waiter.join();
}

// The hang dump's host_owner= (review nit): an x86 guest fast-path Leave
// reaches the host only in leave_wake, after its releasing store. The
// releaser retires the host owner enter_slow installed for it, and never a
// newer owner's.
namespace {
std::string dump_of(CriticalSectionManager& mgr) {
    std::FILE* f = std::tmpfile();
    if (f == nullptr) return {};
    mgr.dump_locked_sections(f);
    std::fflush(f);
    std::rewind(f);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f) != nullptr) text += buf;
    std::fclose(f);
    return text;
}
// What the x86 guest fast path does on a non-recursive leave: clear
// RecursionCount and OwningThread, store -1, then jump to leave_wake.
void guest_fast_path_leave(GuestMemory& memory, CriticalSectionManager& mgr,
                           GuestAddr cs) {
    ASSERT_TRUE(memory.write_u32(cs + 8, 0u).is_ok());
    ASSERT_TRUE(memory.write_u32(cs + 0x0C, 0u).is_ok());
    ASSERT_TRUE(memory.write_u32(cs + 4, cs_layout::UNLOCKED).is_ok());
    mgr.leave_wake(cs);
}
} // namespace

TEST_F(CriticalSectionAccelTest, GuestFastPathLeaveRetiresTheReleasersHostOwner) {
    mgr->initialize(kCsBase);
    ThreadManager::set_current_thread_id(21u);
    mgr->enter_slow(kCsBase);          // the host half installs owner 21
    ASSERT_NE(dump_of(*mgr).find("host_owner=21\n"), std::string::npos);
    guest_fast_path_leave(memory, *mgr, kCsBase);
    EXPECT_NE(dump_of(*mgr).find("host_owner=0\n"), std::string::npos)
        << dump_of(*mgr);
}

TEST_F(CriticalSectionAccelTest, GuestFastPathLeaveKeepsANewOwnersHostOwner) {
    mgr->initialize(kCsBase);
    ThreadManager::set_current_thread_id(21u);
    mgr->enter_slow(kCsBase);
    // Released by the guest; a new owner takes it through the host half
    // before the releaser reaches leave_wake.
    ASSERT_TRUE(memory.write_u32(kCsBase + 8, 0u).is_ok());
    ASSERT_TRUE(memory.write_u32(kCsBase + 0x0C, 0u).is_ok());
    ASSERT_TRUE(memory.write_u32(kCsBase + 4, cs_layout::UNLOCKED).is_ok());
    std::thread next([&] {
        ThreadManager::set_current_thread_id(22u);
        mgr->enter_slow(kCsBase);
    });
    next.join();
    mgr->leave_wake(kCsBase);          // the releaser (21), late
    EXPECT_NE(dump_of(*mgr).find("host_owner=22\n"), std::string::npos)
        << dump_of(*mgr);
}
