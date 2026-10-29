// SPDX-License-Identifier: LGPL-2.1-only
//
// SRWLOCK / CONDITION_VARIABLE kept in the guest word and parked with the
// kernel address wait. Contracts under test (Microsoft Learn, Slim
// Reader/Writer locks and Condition Variables): exclusive holders exclude
// everyone, shared holders run concurrently, Try* never blocks and fails
// against any exclusive holder, SleepConditionVariable* releases the lock
// while parked and holds it again on return, a wake ends the sleep, and a
// timeout returns FALSE.
#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/synchronization/srw_word.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class SrwWordTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.enable_linear_reservation(0x100000));
        ASSERT_TRUE(memory.map_region(kBase, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(srw_word::resolve(memory, kLock, /*is_64bit=*/true, lock));
        ASSERT_TRUE(srw_word::resolve(memory, kCv, /*is_64bit=*/true, cv));
    }
    static constexpr GuestAddr kBase = 0x10000;
    static constexpr GuestAddr kLock = kBase + 0x100;
    static constexpr GuestAddr kCv = kBase + 0x200;
    GuestMemory memory;
    srw_word::Word lock;
    srw_word::Word cv;
};

} // namespace

TEST_F(SrwWordTest, ZeroWordIsFreeAndUnalignedOrUnbackedWordsAreRejected) {
    EXPECT_EQ(srw_word::load(lock), 0u);
    srw_word::Word w;
    EXPECT_FALSE(srw_word::resolve(memory, kBase + 0x104, true, w));   // 8-byte word, 4-aligned
    EXPECT_FALSE(srw_word::resolve(memory, 0x900000, true, w));        // outside the reservation
    EXPECT_TRUE(srw_word::resolve(memory, kBase + 0x104, false, w));   // fine as a 4-byte word
}

TEST_F(SrwWordTest, ExclusiveExcludesEveryoneAndWakesAWaiter) {
    srw_word::acquire_exclusive(lock, 1);
    EXPECT_TRUE(srw_word::owned_exclusively_by(lock, 1));
    EXPECT_FALSE(srw_word::try_acquire_exclusive(lock, 2));
    EXPECT_FALSE(srw_word::try_acquire_shared(lock, 2));
    EXPECT_FALSE(srw_word::try_acquire_exclusive(lock, 1));  // not recursive

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        srw_word::acquire_exclusive(lock, 2);
        acquired.store(true);
        srw_word::release_exclusive(lock, 2);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(acquired.load());
    const auto t0 = std::chrono::steady_clock::now();
    srw_word::release_exclusive(lock, 1);
    waiter.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(acquired.load());
    EXPECT_LT(ms, 50) << "waiter woke by the release, not by a poll";
    EXPECT_EQ(srw_word::load(lock), 0u);
}

TEST_F(SrwWordTest, SharedHoldersRunConcurrentlyAndBlockAWriterUntilTheLast) {
    srw_word::acquire_shared(lock, 1);
    srw_word::acquire_shared(lock, 2);
    EXPECT_TRUE(srw_word::try_acquire_shared(lock, 3));
    EXPECT_FALSE(srw_word::try_acquire_exclusive(lock, 4));

    std::atomic<bool> writer_in{false};
    std::thread writer([&] {
        srw_word::acquire_exclusive(lock, 4);
        writer_in.store(true);
        srw_word::release_exclusive(lock, 4);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(srw_word::release_shared(lock, 1));
    EXPECT_FALSE(srw_word::release_shared(lock, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(writer_in.load()) << "two shared holders remain";
    EXPECT_TRUE(srw_word::release_shared(lock, 3));   // the last one
    writer.join();
    EXPECT_TRUE(writer_in.load());
    EXPECT_EQ(srw_word::load(lock), 0u);
}

TEST_F(SrwWordTest, ManyThreadsContendingLeaveTheWordFreeAndCountExactly) {
    constexpr int kThreads = 8;
    constexpr int kIters = 2000;
    u64 counter = 0;   // protected by the lock
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            const u32 tid = static_cast<u32>(10 + t);
            for (int i = 0; i < kIters; ++i) {
                if ((i & 3) == 0) {
                    srw_word::acquire_shared(lock, tid);
                    volatile u64 sink = counter; (void)sink;
                    (void)srw_word::release_shared(lock, tid);
                } else {
                    srw_word::acquire_exclusive(lock, tid);
                    ++counter;
                    srw_word::release_exclusive(lock, tid);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(counter, static_cast<u64>(kThreads) * (kIters - kIters / 4));
    EXPECT_EQ(srw_word::load(lock), 0u);
}

TEST_F(SrwWordTest, ConditionVariableSleepReleasesTheLockAndAWakeEndsIt) {
    srw_word::acquire_exclusive(lock, 1);
    std::atomic<bool> slept{false};
    std::atomic<bool> woke{false};
    std::thread sleeper([&] {
        // Runs as tid 1's continuation: the sleep must give the lock away.
        slept.store(true);
        const bool ok = srw_word::cv_sleep_srw(cv, lock, /*shared=*/false,
                                               0xFFFFFFFFu, 1, nullptr);
        woke.store(ok);
        EXPECT_TRUE(srw_word::owned_exclusively_by(lock, 1));
        srw_word::release_exclusive(lock, 1);
    });
    while (!slept.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // The lock is free while the sleeper is parked.
    srw_word::acquire_exclusive(lock, 2);
    srw_word::cv_wake(cv, /*all=*/false);
    srw_word::release_exclusive(lock, 2);
    sleeper.join();
    EXPECT_TRUE(woke.load());
}

TEST_F(SrwWordTest, ConditionVariableTimeoutReturnsFalseAndReacquires) {
    srw_word::acquire_exclusive(lock, 1);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = srw_word::cv_sleep_srw(cv, lock, false, 30, 1, nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(ok);
    EXPECT_GE(ms, 25);
    EXPECT_TRUE(srw_word::owned_exclusively_by(lock, 1));
    srw_word::release_exclusive(lock, 1);
}

TEST_F(SrwWordTest, WakeAllReleasesEverySleeperAndSharedModeSleepWorks) {
    std::atomic<int> woke{0};
    std::vector<std::thread> sleepers;
    for (int i = 0; i < 4; ++i) {
        sleepers.emplace_back([&, i] {
            const u32 tid = static_cast<u32>(20 + i);
            srw_word::acquire_shared(lock, tid);
            if (srw_word::cv_sleep_srw(cv, lock, /*shared=*/true, 2000, tid,
                                       nullptr)) {
                woke.fetch_add(1);
            }
            (void)srw_word::release_shared(lock, tid);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    srw_word::cv_wake(cv, /*all=*/true);
    for (auto& th : sleepers) th.join();
    EXPECT_EQ(woke.load(), 4);
    EXPECT_EQ(srw_word::load(lock), 0u);
}

// ---------------------------------------------------------------------------
// Satisfactory (UE5) deadlock: GameThread parked forever in
// AcquireSRWLockExclusive with every other thread idle. Each test below is a
// way the word could stay held after the guest had released it.
// ---------------------------------------------------------------------------

// Windows keeps no owner in an SRWLOCK, so ReleaseSRWLockExclusive releases
// it whichever thread calls it. Vitreo used to ignore a release whose tid
// did not match the owner recorded in the word, leaving the lock held with
// nobody left to release it -- the next acquirer parked forever.
TEST_F(SrwWordTest, ReleaseFromAnotherThreadFreesTheLockAndWakesWaiters) {
    std::thread owner([&] { srw_word::acquire_exclusive(lock, 1); });
    owner.join();   // the acquiring thread is gone; the hold stays

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        srw_word::acquire_exclusive(lock, 3);
        acquired.store(true);
        srw_word::release_exclusive(lock, 3);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(acquired.load());

    srw_word::release_exclusive(lock, 2);   // a different thread releases
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!acquired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool ok = acquired.load();
    EXPECT_TRUE(ok) << "a foreign release left the lock held (word=0x"
                    << std::hex << srw_word::load(lock) << ")";
    if (!ok) srw_word::release_exclusive(lock, 1);   // unstick the waiter
    waiter.join();
    EXPECT_EQ(srw_word::load(lock), 0u);
}

// Two host threads presenting the same guest tid (a host thread that never
// set its tid defaults to 1, the GameThread) leave a recursion count on one
// of them that no release consumes. It must not turn that thread's NEXT,
// fresh, exclusive hold into one whose release is a no-op.
TEST_F(SrwWordTest, StaleRecursionCountDoesNotSwallowAFreshRelease) {
    std::atomic<int> step{0};
    std::thread h1([&] {
        srw_word::acquire_exclusive(lock, 7);
        step.store(1);
        while (step.load() != 2) std::this_thread::yield();
        srw_word::release_exclusive(lock, 7);
        step.store(3);
    });
    std::thread h2([&] {
        while (step.load() != 1) std::this_thread::yield();
        srw_word::acquire_shared(lock, 7);   // "ours" by tid: counted as recursion
        step.store(2);
        while (step.load() != 3) std::this_thread::yield();
        (void)srw_word::release_shared(lock, 7);   // word already free
        // A fresh, legitimate exclusive hold on this thread.
        srw_word::acquire_exclusive(lock, 7);
        srw_word::release_exclusive(lock, 7);
    });
    h1.join();
    h2.join();
    EXPECT_EQ(srw_word::load(lock), 0u) << "the release was swallowed";
    EXPECT_TRUE(srw_word::try_acquire_exclusive(lock, 9));
    srw_word::release_exclusive(lock, 9);
}

// A 4-byte (x86 guest) word has 30 owner bits. Vitreo's service threads use
// tids 0xF000000x; the owner was stored truncated, so the owner's own
// release read as "not ours" and the lock leaked.
TEST_F(SrwWordTest, FourByteWordOwnerRoundTripsAHighTid) {
    srw_word::Word w32;
    ASSERT_TRUE(srw_word::resolve(memory, kBase + 0x304, /*is_64bit=*/false, w32));
    constexpr u32 kServiceTid = 0xF0000001u;
    srw_word::acquire_exclusive(w32, kServiceTid);
    EXPECT_TRUE(srw_word::owned_exclusively_by(w32, kServiceTid));
    EXPECT_FALSE(srw_word::try_acquire_exclusive(w32, kServiceTid));
    srw_word::release_exclusive(w32, kServiceTid);
    EXPECT_EQ(srw_word::load(w32), 0u);
}

// Hammer every path at once -- exclusive, shared, both Try forms, CV sleeps
// in both lock modes with and without timeouts, wake-one and wake-all, and
// holds handed from one thread to another for release -- and require that
// everything finishes, the protected counter is exact and the word ends
// free. A lost release or lost wake shows up as no progress; the watchdog
// aborts rather than hanging the suite.
TEST_F(SrwWordTest, StressMixedOperationsNeverWedge) {
    constexpr int kWorkers = 12;
    constexpr int kIters = 20000;
    std::atomic<u64> progress{0};
    std::atomic<bool> done{false};
    u64 counter = 0;              // protected by the lock (exclusive)
    u64 queue = 0;                // protected by the lock; CV predicate
    std::atomic<u64> consumed{0};
    std::atomic<u64> exclusive_ops{0};

    std::thread watchdog([&] {
        u64 last = progress.load();
        auto last_change = std::chrono::steady_clock::now();
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const u64 now = progress.load();
            if (now != last) {
                last = now;
                last_change = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - last_change >
                       std::chrono::seconds(10)) {
                std::fprintf(stderr, "stress wedged: word=0x%llx cv=0x%llx\n",
                             static_cast<unsigned long long>(srw_word::load(lock)),
                             static_cast<unsigned long long>(srw_word::load(cv)));
                std::abort();
            }
        }
    });

    // Hand-off: a hold taken on one thread and released by another.
    std::atomic<int> handoff{0};   // 0 idle, 1 held-awaiting-release

    std::atomic<bool> workers_done{false};
    std::thread releaser([&] {
        while (!workers_done.load()) {
            if (handoff.load() == 1) {
                srw_word::release_exclusive(lock, 999);   // not the acquirer
                handoff.store(0);
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::vector<std::thread> threads;
    for (int t = 0; t < kWorkers; ++t) {
        threads.emplace_back([&, t] {
            const u32 tid = static_cast<u32>(100 + t);
            u32 rng = 0x9E3779B9u * static_cast<u32>(t + 1);
            auto next = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
            for (int i = 0; i < kIters; ++i) {
                switch (next() % 9) {
                case 0:
                case 1:
                    srw_word::acquire_exclusive(lock, tid);
                    ++counter;
                    exclusive_ops.fetch_add(1);
                    srw_word::release_exclusive(lock, tid);
                    break;
                case 2:
                    srw_word::acquire_shared(lock, tid);
                    { volatile u64 sink = counter; (void)sink; }
                    (void)srw_word::release_shared(lock, tid);
                    break;
                case 3:
                    if (srw_word::try_acquire_exclusive(lock, tid)) {
                        ++counter;
                        exclusive_ops.fetch_add(1);
                        srw_word::release_exclusive(lock, tid);
                    }
                    break;
                case 4:
                    if (srw_word::try_acquire_shared(lock, tid)) {
                        (void)srw_word::release_shared(lock, tid);
                    }
                    break;
                case 5: {   // producer
                    srw_word::acquire_exclusive(lock, tid);
                    ++queue;
                    ++counter;
                    exclusive_ops.fetch_add(1);
                    srw_word::cv_wake(cv, (next() & 1) != 0);
                    srw_word::release_exclusive(lock, tid);
                    break;
                }
                case 6: {   // consumer, exclusive mode, short timeout
                    srw_word::acquire_exclusive(lock, tid);
                    if (queue == 0) {
                        (void)srw_word::cv_sleep_srw(cv, lock, false,
                                                     1 + next() % 3, tid, nullptr);
                    }
                    if (!srw_word::owned_exclusively_by(lock, tid)) {
                        ADD_FAILURE() << "CV sleep returned without the lock";
                    }
                    if (queue != 0) { --queue; consumed.fetch_add(1); }
                    srw_word::release_exclusive(lock, tid);
                    break;
                }
                case 7: {   // consumer, shared mode: only observes
                    srw_word::acquire_shared(lock, tid);
                    (void)srw_word::cv_sleep_srw(cv, lock, true,
                                                 1 + next() % 2, tid, nullptr);
                    (void)srw_word::release_shared(lock, tid);
                    break;
                }
                case 8: {   // hand the hold to the releaser thread
                    int expected = 0;
                    if (handoff.compare_exchange_strong(expected, 2)) {
                        srw_word::acquire_exclusive(lock, tid);
                        ++counter;
                        exclusive_ops.fetch_add(1);
                        handoff.store(1);   // released by another thread
                        while (handoff.load() != 0) std::this_thread::yield();
                    }
                    break;
                }
                }
                progress.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    workers_done.store(true);
    releaser.join();
    done.store(true);
    watchdog.join();
    EXPECT_EQ(counter, exclusive_ops.load());
    EXPECT_EQ(srw_word::load(lock) & ~u64{2}, 0u) << "word left held";
    EXPECT_TRUE(srw_word::try_acquire_exclusive(lock, 1));
    srw_word::release_exclusive(lock, 1);
}
