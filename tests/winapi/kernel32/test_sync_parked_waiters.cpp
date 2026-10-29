/// @file test_sync_parked_waiters.cpp
/// @brief Wake-one semantics through the kernel32 adapter: SetEvent, ReleaseSemaphore and PulseEvent with parked waiters (THREAD-002)

#include "sync_test_fixture.hpp"

// =============================================================================
// Wake-one through the kernel32 adapter.
//
// Learn SetEvent: an auto-reset event "remains signaled until a single waiting
// thread is released, at which time the system automatically sets the state to
// nonsignaled". Learn ReleaseSemaphore: one release per count. Learn
// PulseEvent: releases the threads waiting at that instant (auto-reset: one),
// then nonsignaled. The signaller hands the release to one parked waiter; the
// other parked waiters are not woken (ntsync::WakeCounters::spurious_wakes).
// =============================================================================

namespace {
struct ParkedWaiters {
    SyncManager& mgr;
    Handle stop;
    Handle obj;
    std::vector<std::thread> threads;
    std::atomic<int> released{0};
    std::atomic<int> returned{0};
    ParkedWaiters(SyncManager& m, Handle s, Handle o) : mgr(m), stop(s), obj(o) {}
    void park(int n) {
        const auto before = ntsync::global().wake_counters().waits;
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([this] {
                const Handle hs[2] = {stop, obj};
                const u32 r = mgr.wait_for_multiple_objects(2, hs, false,
                                                           INFINITE_WAIT);
                if (r == WAIT_OBJECT_0 + 1) released.fetch_add(1);
                returned.fetch_add(1);
            });
        }
        const auto target = before + static_cast<std::uint64_t>(n);
        const auto t0 = std::chrono::steady_clock::now();
        while (ntsync::global().wake_counters().waits < target &&
               std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
            std::this_thread::yield();
        }
        ASSERT_EQ(ntsync::global().wake_counters().waits, target);
    }
    void finish() {
        mgr.set_event(stop);
        for (auto& t : threads) t.join();
    }
};
} // namespace

TEST_F(SyncTest, SetEventReleasesExactlyOneOfManyParkedWaiters) {
    Handle stop = m_mgr->create_event(true, false, "");
    Handle ev   = m_mgr->create_event(false, false, "");
    ParkedWaiters herd(*m_mgr, stop, ev);
    herd.park(6);
    const auto c0 = ntsync::global().wake_counters();

    for (int s = 1; s <= 3; ++s) {
        EXPECT_TRUE(m_mgr->set_event(ev));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_EQ(herd.released.load(), s);
        EXPECT_EQ(m_mgr->wait_for_single_object(ev, 0), WAIT_TIMEOUT)
            << "the event must be nonsignaled after releasing one thread";
    }
    const auto c1 = ntsync::global().wake_counters();
    EXPECT_EQ(c1.wakes - c0.wakes, 3u) << "only released threads are woken";
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    herd.finish();
    EXPECT_EQ(herd.released.load(), 3);
}

TEST_F(SyncTest, ReleaseSemaphoreReleasesOnePerCountAcrossParkedWaiters) {
    Handle stop = m_mgr->create_event(true, false, "");
    Handle sem  = m_mgr->create_semaphore(0, 10, "");
    ParkedWaiters herd(*m_mgr, stop, sem);
    herd.park(5);
    const auto c0 = ntsync::global().wake_counters();

    i32 prev = -1;
    EXPECT_TRUE(m_mgr->release_semaphore(sem, 2, &prev));
    EXPECT_EQ(prev, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(herd.released.load(), 2);
    EXPECT_EQ(m_mgr->wait_for_single_object(sem, 0), WAIT_TIMEOUT)
        << "both counts went to parked waiters";
    const auto c1 = ntsync::global().wake_counters();
    EXPECT_EQ(c1.wakes - c0.wakes, 2u);
    EXPECT_EQ(c1.spurious_wakes - c0.spurious_wakes, 0u);
    herd.finish();
    EXPECT_EQ(herd.released.load(), 2);
}

TEST_F(SyncTest, PulseEventAutoResetReleasesOneParkedWaiterAndClears) {
    Handle stop = m_mgr->create_event(true, false, "");
    Handle ev   = m_mgr->create_event(false, false, "");
    ParkedWaiters herd(*m_mgr, stop, ev);
    herd.park(3);

    EXPECT_TRUE(m_mgr->pulse_event(ev));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(herd.released.load(), 1);
    EXPECT_EQ(m_mgr->wait_for_single_object(ev, 0), WAIT_TIMEOUT);
    herd.finish();
    EXPECT_EQ(herd.released.load(), 1);
}

// The GUI thread's WaitForSingleObject (tid == main thread, AppKit park
// installed) waits on the object directly for one pump slice and parks in
// the host run loop only once the wait outlives it: a park costs a hop
// through the host main thread at both ends. The Win32 results are Learn's
// either way: WAIT_OBJECT_0 once signaled, WAIT_TIMEOUT once the interval
// elapses.
TEST_F(SyncTest, GuiThreadWaitParksOnlyAfterOnePumpSlice) {
    using clock = std::chrono::steady_clock;
    const auto ms_since = [](clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   clock::now() - t)
            .count();
    };
    ThreadManager threads{*m_handles};
    m_mgr->set_thread_manager(&threads);
    ThreadManager::set_current_thread_id(threads.main_thread_id());
    std::atomic<u32> parks{0};
    m_mgr->set_wait_pump([] {});
    m_mgr->set_wait_park([&](u32 ms) {
        parks.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min<u32>(ms, 2)));
    });
    m_mgr->set_wait_park_arm([](bool) {});
    const Handle ev = m_mgr->create_event(false, false, "");
    const auto signal_after = [&](clock::time_point start, u32 delay_ms,
                                  std::atomic<i64>& signalled_at) {
        return std::thread([&, start, delay_ms] {
            ThreadManager::set_current_thread_id(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            signalled_at.store(ms_since(start), std::memory_order_release);
            EXPECT_TRUE(m_mgr->set_event(ev));
        });
    };

    // Signaled inside the slice: no park, infinite or timed. A signaller
    // descheduled past the slice legitimately parks, so only then is the
    // count not asserted.
    for (const u32 timeout : {INFINITE_WAIT, 1000u}) {
        parks.store(0);
        std::atomic<i64> signalled_at{-1};
        const auto start = clock::now();
        std::thread signaller = signal_after(start, 2, signalled_at);
        EXPECT_EQ(m_mgr->wait_for_single_object(ev, timeout), WAIT_OBJECT_0);
        signaller.join();
        if (signalled_at.load() < 10) {
            EXPECT_EQ(parks.load(), 0u) << "timeout=" << timeout;
        }
    }

    // Shorter than a slice: the whole interval is a direct wait.
    parks.store(0);
    auto start = clock::now();
    EXPECT_EQ(m_mgr->wait_for_single_object(ev, 5), WAIT_TIMEOUT);
    EXPECT_GE(ms_since(start), 4);
    EXPECT_EQ(parks.load(), 0u);

    // Longer than a slice: parks for the rest, and still honours the interval
    // measured from the call, not from the end of the direct wait.
    parks.store(0);
    start = clock::now();
    EXPECT_EQ(m_mgr->wait_for_single_object(ev, 60), WAIT_TIMEOUT);
    const i64 waited = ms_since(start);
    EXPECT_GE(waited, 59);
    EXPECT_LT(waited, 60 + 16 + 200);
    EXPECT_GT(parks.load(), 0u);

    // Signaled after the slice: the parked wait still returns the object.
    for (const u32 timeout : {INFINITE_WAIT, 2000u}) {
        parks.store(0);
        std::atomic<i64> signalled_at{-1};
        start = clock::now();
        std::thread signaller = signal_after(start, 50, signalled_at);
        EXPECT_EQ(m_mgr->wait_for_single_object(ev, timeout), WAIT_OBJECT_0);
        signaller.join();
        EXPECT_GT(parks.load(), 0u) << "timeout=" << timeout;
        EXPECT_EQ(m_mgr->wait_for_single_object(ev, 0), WAIT_TIMEOUT)
            << "auto-reset event consumed once";
    }
    m_mgr->set_thread_manager(nullptr);
}
