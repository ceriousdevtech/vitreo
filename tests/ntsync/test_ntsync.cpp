/// @file test_ntsync.cpp
/// @brief Tests for the Vitreo NT synchronization object manager.

#include "core/sync/ntsync.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

using namespace vitreo::ntsync;

class NtsyncTest : public ::testing::Test {
protected:
    Manager& mgr() {
        // Use a fresh manager per test by leveraging unique IDs — each test
        // creates new objects so no cross-test interference.
        return global();
    }
    static constexpr std::uint32_t kT1 = 1001;
    static constexpr std::uint32_t kT2 = 1002;
};

TEST_F(NtsyncTest, EventManualResetStaysSignaled) {
    auto id = mgr().create_event(/*manual_reset=*/true, /*initial=*/false);
    EXPECT_TRUE(mgr().set_event(id));
    ObjectId ids[] = { id };
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0); // still signaled
    mgr().destroy(id);
}

TEST_F(NtsyncTest, EventAutoResetConsumesOnce) {
    auto id = mgr().create_event(/*manual_reset=*/false, /*initial=*/true);
    ObjectId ids[] = { id };
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_TIMEOUT); // consumed
    mgr().destroy(id);
}

TEST_F(NtsyncTest, NotificationParkingPreservesEveryLocalObjectType) {
    for (ObjType type : {ObjType::Event, ObjType::Timer,
                         ObjType::Semaphore, ObjType::Mutex}) {
        ObjectId id = 0;
        switch (type) {
        case ObjType::Event: id = mgr().create_event(false, false); break;
        case ObjType::Timer: id = mgr().create_timer(false); break;
        case ObjType::Semaphore: id = mgr().create_semaphore(0, 1); break;
        case ObjType::Mutex: id = mgr().create_mutex(true, kT1); break;
        }
        const auto before = mgr().wake_counters().waits;
        std::atomic<std::uint32_t> result{WAIT_FAILED};
        std::thread observer([&] {
            result = mgr().wait_for_notification(&id, 1, 1000, kT2,
                                                 nullptr, true);
        });
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
        while (mgr().wake_counters().waits == before &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const bool parked = mgr().wake_counters().waits > before;
        switch (type) {
        case ObjType::Event: EXPECT_TRUE(mgr().set_event(id)); break;
        case ObjType::Timer: mgr().signal_timer(id); break;
        case ObjType::Semaphore:
            EXPECT_TRUE(mgr().release_semaphore(id, 1, nullptr)); break;
        case ObjType::Mutex: EXPECT_TRUE(mgr().release_mutex(id, kT1)); break;
        }
        observer.join();
        EXPECT_TRUE(parked);
        EXPECT_EQ(result.load(), WAIT_OBJECT_0);
        // The observer cannot reserve the unit or take mutex ownership;
        // another thread must still be able to complete the actual wait.
        EXPECT_EQ(mgr().wait(&id, 1, false, 0, kT1), WAIT_OBJECT_0);
        if (type == ObjType::Mutex) {
            EXPECT_TRUE(mgr().release_mutex(id, kT1));
        } else {
            EXPECT_EQ(mgr().wait(&id, 1, false, 0, kT1), WAIT_TIMEOUT);
        }
        mgr().destroy(id);
    }
}

TEST_F(NtsyncTest, NotificationParkingCancellationAndErrorsDoNotConsume) {
    const ObjectId id = mgr().create_event(false, true);
    std::atomic<bool> cancelled{true};
    EXPECT_EQ(mgr().wait_for_notification(&id, 1, 0, kT1,
                                         &cancelled, true), WAIT_FAILED);
    cancelled = false;
    EXPECT_EQ(mgr().wait_for_notification(&id, 1, 0, kT1,
                                         &cancelled, true), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait(&id, 1, false, 0, kT2), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait_for_notification(&id, 1, 0, kT1,
                                         &cancelled, true), WAIT_TIMEOUT);
    EXPECT_EQ(mgr().wait_for_notification(nullptr, 1, 0, kT1,
                                         &cancelled, true), WAIT_FAILED);
    mgr().destroy(id);
    EXPECT_EQ(mgr().wait_for_notification(&id, 1, 0, kT1,
                                         &cancelled, true), WAIT_FAILED);
}

TEST_F(NtsyncTest, ImmediatelySatisfiedWaitReportsNoBlockedTime) {
    auto id = mgr().create_event(/*manual_reset=*/true, /*initial=*/true);
    ObjectId ids[] = {id};
    std::uint64_t blocked_ns = 0;
    EXPECT_EQ(mgr().wait(ids, 1, false, INFINITE_TIMEOUT, kT1, nullptr,
                         &blocked_ns),
              WAIT_OBJECT_0);
    EXPECT_EQ(blocked_ns, 0u);
    mgr().destroy(id);
}

TEST_F(NtsyncTest, ParkedWaitReportsBlockedTime) {
    auto id = mgr().create_event(/*manual_reset=*/true, /*initial=*/false);
    ObjectId ids[] = {id};
    std::thread signaler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_TRUE(mgr().set_event(id));
    });
    std::uint64_t blocked_ns = 0;
    EXPECT_EQ(mgr().wait(ids, 1, false, 1000, kT1, nullptr, &blocked_ns),
              WAIT_OBJECT_0);
    signaler.join();
    EXPECT_GT(blocked_ns, 0u);
    mgr().destroy(id);
}

TEST_F(NtsyncTest, SemaphoreCountsDownToZero) {
    auto id = mgr().create_semaphore(/*initial=*/2, /*max=*/3);
    ObjectId ids[] = { id };
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_TIMEOUT);
    std::int32_t prev = -1;
    EXPECT_TRUE(mgr().release_semaphore(id, 1, &prev));
    EXPECT_EQ(prev, 0);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    mgr().destroy(id);
}

TEST_F(NtsyncTest, SemaphoreOverReleaseFails) {
    auto id = mgr().create_semaphore(0, 1);
    EXPECT_FALSE(mgr().release_semaphore(id, 2, nullptr));
    mgr().destroy(id);
}

TEST_F(NtsyncTest, MutexRecursiveSameThread) {
    auto id = mgr().create_mutex(false, 0);
    ObjectId ids[] = { id };
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0); // recursive
    EXPECT_TRUE(mgr().release_mutex(id, kT1));
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT2), WAIT_TIMEOUT); // still held by kT1
    EXPECT_TRUE(mgr().release_mutex(id, kT1));
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT2), WAIT_OBJECT_0); // now kT2 owns
    mgr().destroy(id);
}

TEST_F(NtsyncTest, MutexAbandonedReturnsAbandoned) {
    auto id = mgr().create_mutex(false, 0);
    ObjectId ids[] = { id };
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    mgr().abandon_for_thread(kT1);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT2), WAIT_ABANDONED_0);
    EXPECT_TRUE(mgr().release_mutex(id, kT2));
    mgr().destroy(id);
}

TEST_F(NtsyncTest, WaitAllAtomicNoPartialConsume) {
    // Two auto-reset events: only the FIRST is signaled.  WaitAll must NOT
    // consume the first event because the second is not satisfiable.
    auto a = mgr().create_event(false, /*initial=*/true);
    auto b = mgr().create_event(false, /*initial=*/false);
    ObjectId ids[] = { a, b };
    EXPECT_EQ(mgr().wait(ids, 2, /*wait_all=*/true, 0, kT1), WAIT_TIMEOUT);
    // 'a' must still be signaled — verify by single-object wait.
    ObjectId one[] = { a };
    EXPECT_EQ(mgr().wait(one, 1, false, 0, kT1), WAIT_OBJECT_0);
    mgr().destroy(a);
    mgr().destroy(b);
}

TEST_F(NtsyncTest, WaitAnyReturnsFirstSatisfied) {
    auto a = mgr().create_event(false, false);
    auto b = mgr().create_event(false, true);
    auto c = mgr().create_event(false, true);
    ObjectId ids[] = { a, b, c };
    EXPECT_EQ(mgr().wait(ids, 3, false, 0, kT1), WAIT_OBJECT_0 + 1); // 'b'
    mgr().destroy(a); mgr().destroy(b); mgr().destroy(c);
}

TEST_F(NtsyncTest, WaitWakesOnSetEvent) {
    auto id = mgr().create_event(/*manual_reset=*/true, /*initial=*/false);
    std::atomic<bool> done{false};
    std::thread t([&]() {
        ObjectId ids[] = { id };
        std::uint32_t r = mgr().wait(ids, 1, false, 1000, kT2);
        EXPECT_EQ(r, WAIT_OBJECT_0);
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(done.load());
    mgr().set_event(id);
    t.join();
    EXPECT_TRUE(done.load());
    mgr().destroy(id);
}

TEST_F(NtsyncTest, WaitTimeoutReturnsTimeout) {
    auto id = mgr().create_event(true, false);
    ObjectId ids[] = { id };
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(mgr().wait(ids, 1, false, 30, kT1), WAIT_TIMEOUT);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(dt, 20);
    EXPECT_LT(dt, 500);
    mgr().destroy(id);
}

TEST_F(NtsyncTest, CancellationWakesInfiniteWaitWithoutConsumingObject) {
    auto id = mgr().create_event(/*manual_reset=*/false, /*initial=*/false);
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint32_t> result{WAIT_TIMEOUT};

    std::thread waiter([&]() {
        ObjectId ids[] = {id};
        started.store(true, std::memory_order_release);
        result.store(
            mgr().wait(ids, 1, false, INFINITE_TIMEOUT, kT2,
                       &cancel_requested),
            std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    cancel_requested.store(true, std::memory_order_release);
    mgr().notify_waiters();
    waiter.join();

    EXPECT_EQ(result.load(std::memory_order_acquire), WAIT_FAILED);
    ObjectId ids[] = {id};
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_TIMEOUT);
    mgr().destroy(id);
}

TEST_F(NtsyncTest, InvalidObjectReturnsWaitFailed) {
    ObjectId ids[] = { 999999999ULL };
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_FAILED);
}

TEST_F(NtsyncTest, DenseTableSurvivesCreateDestroyChurn) {
    // Regression for ccb29: unordered_map bucket pointer went null while
    // bucket_count stayed non-zero during MsgWait → host SIGSEGV at 0x358.
    constexpr int kN = 256;
    std::vector<ObjectId> live;
    live.reserve(static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        live.push_back(mgr().create_event(true, false));
    }
    const ObjectId destroyed_id = live[0];
    for (int i = 0; i < kN; i += 2) {
        mgr().destroy(live[static_cast<std::size_t>(i)]);
        live[static_cast<std::size_t>(i)] = kInvalidId;
    }
    // Re-create into higher ids; waits on surviving objects must still work.
    for (int i = 0; i < 64; ++i) {
        auto id = mgr().create_event(false, true);
        ObjectId ids[] = {id};
        EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
        mgr().destroy(id);
    }
    for (ObjectId id : live) {
        if (id == kInvalidId) continue;
        ObjectId ids[] = {id};
        EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_TIMEOUT);
        mgr().destroy(id);
    }
    ObjectId gone[] = {destroyed_id};
    EXPECT_EQ(mgr().wait(gone, 1, false, 0, kT1), WAIT_FAILED);
}

TEST_F(NtsyncTest, SetEventInvokesHostWakeOnlyForArmedObjects) {
    std::atomic<int> wakes{0};
    mgr().set_host_wake([&] { wakes.fetch_add(1, std::memory_order_relaxed); });
    auto id = mgr().create_event(true, false);
    const auto other = mgr().create_event(true, false);
    // The host parks on its armed wait set, not every signal in a process.
    EXPECT_TRUE(mgr().set_event(id));
    EXPECT_EQ(wakes.load(std::memory_order_relaxed), 0);
    mgr().arm_host_wake(kT1, &id, 1);
    EXPECT_TRUE(mgr().set_event(other));
    EXPECT_EQ(wakes.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(mgr().set_event(id));
    EXPECT_EQ(wakes.load(std::memory_order_relaxed), 1);
    mgr().disarm_host_wake(kT1);
    EXPECT_TRUE(mgr().set_event(id));
    EXPECT_EQ(wakes.load(std::memory_order_relaxed), 1);
    mgr().destroy(id);
    mgr().destroy(other);
    mgr().set_host_wake({});
}

// ============================================================================
// Targeted-wakeup behaviour.
//
// Every waiter used to park on one process-wide condition variable, so any
// signal woke every waiter in the process; each re-took the single global lock,
// re-scanned its handles, and all but one went back to sleep. With N guest
// threads that is O(N) wakeups and O(N) contended lock acquisitions per signal.
// ============================================================================

// A signal must release the thread waiting on THAT object and leave threads
// waiting on unrelated objects parked.
TEST_F(NtsyncTest, SignalDoesNotReleaseWaitersOnOtherObjects) {
    const ObjectId target = mgr().create_event(false, false);
    const ObjectId other  = mgr().create_event(false, false);
    std::atomic<int> other_woke{0};
    std::atomic<bool> stop{false};

    std::thread bystander([&] {
        // Auto-reset, never signalled: any return other than a timeout means
        // this thread was released by a signal that was not for it.
        while (!stop.load(std::memory_order_acquire)) {
            const ObjectId id = other;
            if (mgr().wait(&id, 1, false, 20, kT2) != WAIT_TIMEOUT) {
                other_woke.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (int i = 0; i < 200; ++i) {
        EXPECT_TRUE(mgr().set_event(target));
        const ObjectId id = target;
        EXPECT_EQ(mgr().wait(&id, 1, false, 1000, kT1), WAIT_OBJECT_0);
    }
    stop.store(true, std::memory_order_release);
    bystander.join();

    EXPECT_EQ(other_woke.load(), 0)
        << "a waiter on an unrelated object was released by another object's "
           "signal";
    mgr().destroy(target);
    mgr().destroy(other);
}

// Throughput of signal->wake->consume with many threads parked on their own
// objects. DISABLED: a measurement, not an assertion.
//   ./vitreo_ipc_tests --gtest_also_run_disabled_tests --gtest_filter='*WakeThroughput*'
//   VITREO_NTSYNC_NOTIFY_ALL=1 ...same...   (restores the old broadcast)
TEST_F(NtsyncTest, DISABLED_WakeThroughputWithManyWaiters) {
    const char* env = std::getenv("VITREO_BENCH_WAITERS");
    const int kWaiters = env != nullptr ? std::atoi(env) : 32;
    constexpr int kSignalsPerWaiter = 400;

    // Semaphores, not auto-reset events: an event COALESCES, so a signaller
    // running ahead of the waiters silently loses signals and the benchmark
    // measures its own timeout instead of wake throughput.
    std::vector<ObjectId> events;
    events.reserve(static_cast<std::size_t>(kWaiters));
    for (int i = 0; i < kWaiters; ++i) {
        events.push_back(mgr().create_semaphore(0, kSignalsPerWaiter + 1));
    }

    std::atomic<bool> stop{false};
    std::atomic<long> consumed{0};
    std::vector<std::thread> waiters;
    waiters.reserve(static_cast<std::size_t>(kWaiters));
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&, i] {
            const ObjectId id = events[static_cast<std::size_t>(i)];
            while (!stop.load(std::memory_order_acquire)) {
                if (mgr().wait(&id, 1, false, 20,
                               kT1 + static_cast<std::uint32_t>(i)) ==
                    WAIT_OBJECT_0) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    const auto start = std::chrono::steady_clock::now();
    const long target = static_cast<long>(kWaiters) * kSignalsPerWaiter;
    for (int round = 0; round < kSignalsPerWaiter; ++round) {
        for (int i = 0; i < kWaiters; ++i) {
            (void)mgr().release_semaphore(events[static_cast<std::size_t>(i)],
                                          1, nullptr);
        }
    }
    while (consumed.load(std::memory_order_relaxed) < target &&
           std::chrono::steady_clock::now() - start <
               std::chrono::seconds(20)) {
        std::this_thread::yield();
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    stop.store(true, std::memory_order_release);
    for (auto& t : waiters) t.join();

    std::printf("[bench] %d waiters, %ld signals consumed in %.1f ms "
                "= %.2f k signals/s, notify_all=%d\n",
                kWaiters, consumed.load(), ms,
                static_cast<double>(consumed.load()) / ms,
                std::getenv("VITREO_NTSYNC_NOTIFY_ALL") != nullptr ? 1 : 0);
    for (ObjectId id : events) mgr().destroy(id);
}

// ============================================================================
// Direct handoff (wake-one).
//
// Learn SetEvent: an auto-reset event "remains signaled until a single waiting
// thread is released"; a manual-reset event releases "any number of waiting
// threads". Learn ReleaseSemaphore: "each time a waiting thread is released
// ... the count of the semaphore is decreased by one". Learn
// WaitForMultipleObjects (bWaitAll): "does not modify the states of the
// specified objects until the states of all objects have been set to
// signaled". Learn PulseEvent: releases only the threads "waiting at the
// moment it is called"; manual-reset all, auto-reset one; then nonsignaled.
//
// Measured on Steam's browser process before this: every signal woke every
// waiter registered on the object, and 17 of 110 threads were blocked on the
// manager mutex at once. The signaller now satisfies the wait on the waiter's
// behalf and wakes only the threads it released; wake_counters().spurious_wakes
// counts any wake that re-parked unsatisfied, and must stay at zero.
// ============================================================================

namespace {

// A pool of threads parked on ONE wait-any {stop, obj} wait; each returns
// after consuming `obj` once (or when `stop` is set).
struct Herd {
    Manager& mgr;
    ObjectId stop;
    ObjectId obj;
    std::vector<std::thread> threads;
    std::atomic<int> consumed{0};
    std::atomic<int> returned{0};
    std::vector<std::uint32_t> results;
    std::mutex results_lock;
    // Release order, by waiter index, as each thread consumes.
    std::vector<int> order;

    Herd(Manager& m, ObjectId stop_, ObjectId obj_) : mgr(m), stop(stop_), obj(obj_) {}

    void park(int n, std::uint32_t base_tid, bool wait_all = false,
              int stagger_ms = 0) {
        const auto before = mgr.wake_counters().waits;
        results.resize(static_cast<std::size_t>(n), 0xDEADBEEFu);
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([this, i, base_tid, wait_all] {
                std::uint32_t r;
                if (wait_all) {
                    const ObjectId ids[1] = {obj};
                    r = mgr.wait(ids, 1, true, INFINITE_TIMEOUT,
                                 base_tid + static_cast<std::uint32_t>(i));
                    if (r == WAIT_OBJECT_0) {
                        std::lock_guard<std::mutex> lk(results_lock);
                        order.push_back(i);
                        consumed.fetch_add(1);
                    }
                } else {
                    const ObjectId ids[2] = {stop, obj};
                    r = mgr.wait(ids, 2, false, INFINITE_TIMEOUT,
                                 base_tid + static_cast<std::uint32_t>(i));
                    if (r == WAIT_OBJECT_0 + 1 || r == WAIT_ABANDONED_0 + 1) {
                        std::lock_guard<std::mutex> lk(results_lock);
                        order.push_back(i);
                        consumed.fetch_add(1);
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(results_lock);
                    results[static_cast<std::size_t>(i)] = r;
                }
                returned.fetch_add(1);
            });
            if (stagger_ms > 0) {
                // Registration order is release order; make it deterministic.
                const auto target = before + static_cast<std::uint64_t>(i + 1);
                while (mgr.wake_counters().waits < target) {
                    std::this_thread::yield();
                }
            }
        }
        // Every thread has parked once (waits counts first parks).
        const auto target = before + static_cast<std::uint64_t>(n);
        const auto t0 = std::chrono::steady_clock::now();
        while (mgr.wake_counters().waits < target &&
               std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
            std::this_thread::yield();
        }
        ASSERT_EQ(mgr.wake_counters().waits, target) << "herd did not park";
    }
    void settle() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    void finish() {
        mgr.set_event(stop);
        for (auto& t : threads) t.join();
        threads.clear();
    }
};

} // namespace

// N waiters on an auto-reset event, M < N sets: exactly M threads are
// released, each set releases one, and no thread wakes without consuming.
TEST_F(NtsyncTest, AutoResetSetReleasesExactlyOneWaiterPerSet) {
    constexpr int kN = 8, kM = 3;
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId ev   = mgr().create_event(false, false);
    Herd herd(mgr(), stop, ev);
    herd.park(kN, 0x5100);
    const auto c0 = mgr().wake_counters();

    for (int s = 0; s < kM; ++s) {
        ASSERT_TRUE(mgr().set_event(ev));
        herd.settle();
        EXPECT_EQ(herd.consumed.load(), s + 1) << "set " << s;
        EXPECT_EQ(mgr().probe(ev, 0x5FFF), false)
            << "a release must leave the auto-reset event nonsignaled";
    }
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), kM);
    EXPECT_EQ(herd.returned.load(), kM);
    const auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    EXPECT_EQ(c1.wakes - c0.wakes, static_cast<std::uint64_t>(kM))
        << "a set woke more threads than it released";
    EXPECT_EQ(c1.grants - c0.grants, static_cast<std::uint64_t>(kM));

    herd.finish();
    EXPECT_EQ(herd.consumed.load(), kM);
    mgr().destroy(ev);
    mgr().destroy(stop);
}

// ReleaseSemaphore(k) with N > k waiters releases exactly k, one per count.
TEST_F(NtsyncTest, ReleaseSemaphoreReleasesOneWaiterPerCount) {
    constexpr int kN = 7, kK = 4;
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId sem  = mgr().create_semaphore(0, 100);
    Herd herd(mgr(), stop, sem);
    herd.park(kN, 0x5200);
    const auto c0 = mgr().wake_counters();

    std::int32_t prev = -1;
    ASSERT_TRUE(mgr().release_semaphore(sem, kK, &prev));
    EXPECT_EQ(prev, 0);
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), kK);
    EXPECT_EQ(herd.returned.load(), kK);
    EXPECT_FALSE(mgr().probe(sem, 0x5FFF)) << "count must be back to 0";
    const auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    EXPECT_EQ(c1.wakes - c0.wakes, static_cast<std::uint64_t>(kK));

    // The remaining count-less waiters are released one per later release.
    ASSERT_TRUE(mgr().release_semaphore(sem, 1, &prev));
    EXPECT_EQ(prev, 0);
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), kK + 1);
    herd.finish();
    mgr().destroy(sem);
    mgr().destroy(stop);
}

// A manual-reset set releases every waiter and stays signaled.
TEST_F(NtsyncTest, ManualResetSetReleasesAllWaiters) {
    constexpr int kN = 6;
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId ev   = mgr().create_event(true, false);
    Herd herd(mgr(), stop, ev);
    herd.park(kN, 0x5300);
    const auto c0 = mgr().wake_counters();

    ASSERT_TRUE(mgr().set_event(ev));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), kN);
    EXPECT_TRUE(mgr().probe(ev, 0x5FFF)) << "manual-reset stays signaled";
    const auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    EXPECT_EQ(c1.wakes - c0.wakes, static_cast<std::uint64_t>(kN));
    herd.finish();
    mgr().destroy(ev);
    mgr().destroy(stop);
}

// Waiters are released in the order they began waiting (FIFO).
TEST_F(NtsyncTest, AutoResetReleasesWaitersInRegistrationOrder) {
    constexpr int kN = 5;
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId ev   = mgr().create_event(false, false);
    Herd herd(mgr(), stop, ev);
    herd.park(kN, 0x5400, /*wait_all=*/false, /*stagger_ms=*/1);
    for (int s = 0; s < kN; ++s) {
        ASSERT_TRUE(mgr().set_event(ev));
        herd.settle();
        ASSERT_EQ(herd.consumed.load(), s + 1);
    }
    herd.finish();
    ASSERT_EQ(herd.order.size(), static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        EXPECT_EQ(herd.order[static_cast<std::size_t>(i)], i)
            << "release " << i << " went to a later-registered waiter";
    }
    mgr().destroy(ev);
    mgr().destroy(stop);
}

// A wait-all waiter whose other object is not signaled must not consume the
// auto-reset event (Learn bWaitAll), and must not stop the release reaching
// the wait-any waiters behind it. When its other object is finally set, the
// signaller completes the whole wait-all on its behalf.
TEST_F(NtsyncTest, WaitAllWaiterDoesNotStealAutoResetFromWaitAnyWaiters) {
    const ObjectId stop  = mgr().create_event(true, false);
    const ObjectId ev    = mgr().create_event(false, false);
    const ObjectId other = mgr().create_event(false, false);

    // Registered FIRST: a wait-all on {ev, other}.
    std::atomic<std::uint32_t> all_result{0xDEADBEEFu};
    const auto w0 = mgr().wake_counters().waits;
    std::thread all_waiter([&] {
        const ObjectId ids[2] = {ev, other};
        all_result.store(mgr().wait(ids, 2, true, INFINITE_TIMEOUT, 0x5500));
    });
    while (mgr().wake_counters().waits < w0 + 1) std::this_thread::yield();

    // Then two wait-any waiters on ev.
    Herd herd(mgr(), stop, ev);
    herd.park(2, 0x5510);
    const auto c0 = mgr().wake_counters();

    ASSERT_TRUE(mgr().set_event(ev));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), 1) << "the release skipped the wait-all "
                                           "and went to the first wait-any";
    EXPECT_EQ(all_result.load(), 0xDEADBEEFu) << "wait-all must still be parked";
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF));
    auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    EXPECT_EQ(c1.wakes - c0.wakes, 1u) << "the wait-all must not be woken";

    // Now make the wait-all completable: set other, then ev. The second set
    // goes to the wait-all (registered first), consuming BOTH objects.
    ASSERT_TRUE(mgr().set_event(other));
    herd.settle();
    EXPECT_EQ(all_result.load(), 0xDEADBEEFu) << "ev is clear: not yet";
    ASSERT_TRUE(mgr().set_event(ev));
    all_waiter.join();
    EXPECT_EQ(all_result.load(), WAIT_OBJECT_0);
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF));
    EXPECT_FALSE(mgr().probe(other, 0x5FFF));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), 1) << "the second wait-any must not have "
                                           "been released by the wait-all's set";
    c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);

    herd.finish();
    mgr().destroy(ev);
    mgr().destroy(other);
    mgr().destroy(stop);
}

// PulseEvent, Learn: manual-reset releases every thread waiting at that
// instant; auto-reset releases exactly one; the event is nonsignaled after,
// so a later arrival misses it; a wait-all whose other objects are not
// signaled is not released.
TEST_F(NtsyncTest, PulseManualResetReleasesPresentWaitersOnly) {
    constexpr int kN = 4;
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId ev   = mgr().create_event(true, false);
    Herd herd(mgr(), stop, ev);
    herd.park(kN, 0x5600);
    const auto c0 = mgr().wake_counters();

    ASSERT_TRUE(mgr().pulse_event(ev));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), kN);
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF)) << "nonsignaled after the pulse";
    const auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    herd.finish();

    // A late arrival misses the pulse.
    const ObjectId ids[1] = {ev};
    EXPECT_EQ(mgr().wait(ids, 1, false, 20, 0x5FFF), WAIT_TIMEOUT);
    mgr().destroy(ev);
    mgr().destroy(stop);
}

TEST_F(NtsyncTest, PulseAutoResetReleasesExactlyOneWaiter) {
    constexpr int kN = 4;
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId ev   = mgr().create_event(false, false);
    Herd herd(mgr(), stop, ev);
    herd.park(kN, 0x5700);
    const auto c0 = mgr().wake_counters();

    ASSERT_TRUE(mgr().pulse_event(ev));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), 1);
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF));
    const auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.wakes - c0.wakes, 1u);
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);

    // Pulse with nobody waiting: just nonsignaled.
    herd.finish();
    ASSERT_TRUE(mgr().set_event(ev));
    ASSERT_TRUE(mgr().pulse_event(ev));
    const ObjectId ids[1] = {ev};
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, 0x5FFF), WAIT_TIMEOUT);
    mgr().destroy(ev);
    mgr().destroy(stop);
}

TEST_F(NtsyncTest, PulseDoesNotReleaseUnsatisfiableWaitAll) {
    const ObjectId ev    = mgr().create_event(true, false);
    const ObjectId other = mgr().create_event(false, false);
    std::atomic<std::uint32_t> result{0xDEADBEEFu};
    const auto w0 = mgr().wake_counters().waits;
    std::thread waiter([&] {
        const ObjectId ids[2] = {ev, other};
        result.store(mgr().wait(ids, 2, true, 2000, 0x5800));
    });
    while (mgr().wake_counters().waits < w0 + 1) std::this_thread::yield();

    ASSERT_TRUE(mgr().pulse_event(ev));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(result.load(), 0xDEADBEEFu) << "other is clear: no release";
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF));

    // With `other` signaled the next pulse completes the wait-all, consuming
    // `other` and leaving the pulsed event clear.
    ASSERT_TRUE(mgr().set_event(other));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(result.load(), 0xDEADBEEFu);
    ASSERT_TRUE(mgr().pulse_event(ev));
    waiter.join();
    EXPECT_EQ(result.load(), WAIT_OBJECT_0);
    EXPECT_FALSE(mgr().probe(other, 0x5FFF));
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF));
    mgr().destroy(ev);
    mgr().destroy(other);
}

// ReleaseMutex hands ownership straight to the first waiter; an abandoned
// mutex hands it over with WAIT_ABANDONED_0.
TEST_F(NtsyncTest, MutexReleaseHandsOwnershipToFirstWaiter) {
    const ObjectId stop = mgr().create_event(true, false);
    const ObjectId mx   = mgr().create_mutex(true, 0x5900);
    Herd herd(mgr(), stop, mx);
    herd.park(3, 0x5910, false, 1);
    const auto c0 = mgr().wake_counters();

    ASSERT_TRUE(mgr().release_mutex(mx, 0x5900));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), 1);
    ASSERT_EQ(herd.order.size(), 1u);
    EXPECT_EQ(herd.order[0], 0);
    EXPECT_EQ(herd.results[0], WAIT_OBJECT_0 + 1);
    // Owned by waiter 0 now: nobody else can take it.
    EXPECT_FALSE(mgr().probe(mx, 0x5FFF));
    EXPECT_TRUE(mgr().probe(mx, 0x5910)) << "recursive entry for the owner";
    auto c1 = mgr().wake_counters();
    EXPECT_EQ(c1.wakes - c0.wakes, 1u);

    // Waiter 0's thread "dies" owning it: waiter 1 gets WAIT_ABANDONED_0+1.
    mgr().abandon_for_thread(0x5910);
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), 2);
    EXPECT_EQ(herd.results[1], WAIT_ABANDONED_0 + 1);
    EXPECT_FALSE(mgr().probe(mx, 0x5FFF));
    c1 = mgr().wake_counters();
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    EXPECT_TRUE(mgr().release_mutex(mx, 0x5911));
    herd.settle();
    EXPECT_EQ(herd.consumed.load(), 3);
    herd.finish();
    mgr().destroy(mx);
    mgr().destroy(stop);
}

// A set that lands while a timed waiter's deadline is passing must not be
// swallowed: every release is consumed by somebody, and a waiter that was
// granted reports success even if its deadline had elapsed.
TEST_F(NtsyncTest, TimeoutRacingASetNeverSwallowsTheRelease) {
    const ObjectId ev  = mgr().create_event(false, false);
    const ObjectId ack = mgr().create_semaphore(0, 1 << 20);
    std::atomic<bool> stop{false};
    std::atomic<int> released{0};
    std::atomic<int> timeouts{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 4; ++i) {
        waiters.emplace_back([&, i] {
            const ObjectId ids[1] = {ev};
            while (!stop.load(std::memory_order_acquire)) {
                // 1 ms waits: deadlines expire constantly while sets land.
                const std::uint32_t r =
                    mgr().wait(ids, 1, false, 1, 0x5A00 + static_cast<std::uint32_t>(i));
                if (r == WAIT_OBJECT_0) {
                    released.fetch_add(1);
                    mgr().release_semaphore(ack, 1, nullptr);
                } else {
                    EXPECT_EQ(r, WAIT_TIMEOUT);
                    timeouts.fetch_add(1);
                }
            }
        });
    }
    constexpr int kSets = 3000;
    const ObjectId ack_ids[1] = {ack};
    for (int s = 0; s < kSets; ++s) {
        ASSERT_TRUE(mgr().set_event(ev));
        // Each set must be consumed by exactly one waiter (the event coalesces,
        // so the next set waits for the ack).
        ASSERT_EQ(mgr().wait(ack_ids, 1, false, 5000, 0x5AFF), WAIT_OBJECT_0)
            << "set " << s << " was swallowed";
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : waiters) t.join();
    EXPECT_EQ(released.load(), kSets);
    EXPECT_FALSE(mgr().probe(ev, 0x5FFF));
    EXPECT_GT(timeouts.load(), 0) << "the race was never exercised";
    mgr().destroy(ev);
    mgr().destroy(ack);
}

// Destroying an object under a parked waiter is not a release: the waiter
// is woken to observe the disappearance and reports WAIT_FAILED.
TEST_F(NtsyncTest, DestroyUnderParkedWaiterReportsWaitFailed) {
    const ObjectId ev = mgr().create_event(false, false);
    std::atomic<std::uint32_t> result{0xDEADBEEFu};
    const auto w0 = mgr().wake_counters().waits;
    std::thread waiter([&] {
        const ObjectId ids[1] = {ev};
        result.store(mgr().wait(ids, 1, false, INFINITE_TIMEOUT, 0x5B00));
    });
    while (mgr().wake_counters().waits < w0 + 1) std::this_thread::yield();
    mgr().destroy(ev);
    waiter.join();
    EXPECT_EQ(result.load(), WAIT_FAILED);
}

// TerminateThread: a thread parked in a wait is released without consuming the
// object (terminate_thread_probe BLOCKED_WAIT count_left=1), and it can never
// wait again.
TEST_F(NtsyncTest, InterruptThreadEndsParkedWaitWithoutConsuming) {
    constexpr std::uint32_t kVictim = 0x7E570001u;
    auto sem = mgr().create_semaphore(0, 1);
    ObjectId ids[] = { sem };
    std::atomic<std::uint32_t> result{0};
    std::thread waiter([&] {
        result = mgr().wait(ids, 1, false, INFINITE_TIMEOUT, kVictim);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mgr().interrupt_thread(kVictim);
    waiter.join();
    EXPECT_EQ(result.load(), WAIT_FAILED);
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kVictim), WAIT_FAILED);
    ASSERT_TRUE(mgr().release_semaphore(sem, 1, nullptr));
    EXPECT_EQ(mgr().wait(ids, 1, false, 0, kT1), WAIT_OBJECT_0);
    mgr().destroy(sem);
}
