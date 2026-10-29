#include <gtest/gtest.h>

#include <winapi/common/timer_resolution.hpp>
#include <winapi/media/winmm/timer_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace std::chrono_literals;

// -----------------------------------------------------------------------------
// Timer resolution (timeBeginPeriod / timeEndPeriod)
// -----------------------------------------------------------------------------

class TimerResolutionTest : public ::testing::Test {
protected:
    void SetUp() override { timer_resolution_reset_for_testing(); }
    void TearDown() override { timer_resolution_reset_for_testing(); }
};

TEST_F(TimerResolutionTest, DefaultsToLegacyTickWhenNobodyAsks) {
    EXPECT_EQ(timer_resolution_effective(), TIMER_PERIOD_DEFAULT);
    EXPECT_FALSE(timer_resolution_is_high());
}

TEST_F(TimerResolutionTest, EffectiveIsTheSmallestOutstandingRequest) {
    ASSERT_TRUE(timer_resolution_acquire(10));
    EXPECT_EQ(timer_resolution_effective(), 10u);
    ASSERT_TRUE(timer_resolution_acquire(2));
    EXPECT_EQ(timer_resolution_effective(), 2u);
    // A coarser request must not raise the effective resolution back up.
    ASSERT_TRUE(timer_resolution_acquire(15));
    EXPECT_EQ(timer_resolution_effective(), 2u);
    EXPECT_TRUE(timer_resolution_is_high());
}

TEST_F(TimerResolutionTest, ReleasingTheFinestRestoresThePrevious) {
    ASSERT_TRUE(timer_resolution_acquire(10));
    ASSERT_TRUE(timer_resolution_acquire(2));
    ASSERT_TRUE(timer_resolution_release(2));
    // This is the case a no-op implementation gets wrong: dropping the 2 ms
    // holder must fall back to 10 ms, not to the default and not stay at 2.
    EXPECT_EQ(timer_resolution_effective(), 10u);
    ASSERT_TRUE(timer_resolution_release(10));
    EXPECT_EQ(timer_resolution_effective(), TIMER_PERIOD_DEFAULT);
}

TEST_F(TimerResolutionTest, RequestsAreReferenceCounted) {
    ASSERT_TRUE(timer_resolution_acquire(5));
    ASSERT_TRUE(timer_resolution_acquire(5));
    ASSERT_TRUE(timer_resolution_release(5));
    // Two acquires need two releases; one release must not drop the request.
    EXPECT_EQ(timer_resolution_effective(), 5u);
    ASSERT_TRUE(timer_resolution_release(5));
    EXPECT_EQ(timer_resolution_effective(), TIMER_PERIOD_DEFAULT);
}

TEST_F(TimerResolutionTest, RejectsOutOfRangeAndUnbalancedRelease) {
    EXPECT_FALSE(timer_resolution_acquire(0));
    EXPECT_FALSE(timer_resolution_acquire(TIMER_PERIOD_MAX + 1));
    EXPECT_FALSE(timer_resolution_release(7));  // never acquired
    EXPECT_EQ(timer_resolution_effective(), TIMER_PERIOD_DEFAULT);
}

// -----------------------------------------------------------------------------
// Multimedia timer scheduling
// -----------------------------------------------------------------------------

/// Collects callback invocations with their arrival times.
class Recorder {
public:
    void record(u32 id, u64 user) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_fires.push_back({id, user, std::chrono::steady_clock::now()});
        m_cv.notify_all();
    }

    bool wait_for_count(usize count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [&] { return m_fires.size() >= count; });
    }

    usize count() const {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_fires.size();
    }

    std::vector<u32> ids() const {
        std::lock_guard<std::mutex> guard(m_mutex);
        std::vector<u32> out;
        for (const auto& fire : m_fires) out.push_back(fire.id);
        return out;
    }

    std::vector<u64> users() const {
        std::lock_guard<std::mutex> guard(m_mutex);
        std::vector<u64> out;
        for (const auto& fire : m_fires) out.push_back(fire.user);
        return out;
    }

private:
    struct Fire {
        u32 id;
        u64 user;
        std::chrono::steady_clock::time_point at;
    };
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Fire> m_fires;
};

class TimerSchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        scheduler.set_proc_invoker(
            [this](GuestAddr, u32 id, u64 user) { recorder.record(id, user); });
    }

    MultimediaTimerScheduler scheduler;
    Recorder recorder;
};

TEST_F(TimerSchedulerTest, OneShotFiresExactlyOnce) {
    const u32 id = scheduler.create(5, 1, 0x1000, 0xABCD, TIME_ONESHOT);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(recorder.wait_for_count(1, 2s));
    EXPECT_EQ(recorder.ids().front(), id);
    EXPECT_EQ(recorder.users().front(), 0xABCDu);

    // A one-shot must retire itself; nothing further may arrive.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(recorder.count(), 1u);
    EXPECT_EQ(scheduler.active_count(), 0u);
}

TEST_F(TimerSchedulerTest, PeriodicFiresRepeatedly) {
    const u32 id = scheduler.create(5, 1, 0x2000, 7, TIME_PERIODIC);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(recorder.wait_for_count(4, 3s));
    for (u32 fired_id : recorder.ids()) EXPECT_EQ(fired_id, id);
    EXPECT_TRUE(scheduler.destroy(id));
}

TEST_F(TimerSchedulerTest, RejectsNullCallback) {
    EXPECT_EQ(scheduler.create(5, 1, 0, 0, TIME_ONESHOT), 0u);
}

TEST_F(TimerSchedulerTest, RejectsDelayOutsideAdvertisedRange) {
    // Learn timeSetEvent: uDelay not in [timeGetDevCaps min, max] fails.
    EXPECT_EQ(scheduler.create(0, 1, 0x1000, 0, TIME_PERIODIC), 0u);
    EXPECT_EQ(scheduler.create(TIMER_PERIOD_MAX + 1, 1, 0x1000, 0, TIME_ONESHOT),
              0u);
}

TEST_F(TimerSchedulerTest, MinDelayStillFires) {
    const u32 id = scheduler.create(TIMER_PERIOD_MIN, 1, 0x1000, 0x11,
                                    TIME_ONESHOT);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(recorder.wait_for_count(1, 2s));
    EXPECT_EQ(recorder.users().front(), 0x11u);
}

TEST_F(TimerSchedulerTest, IdsAreDistinctAndNonZero) {
    const u32 first = scheduler.create(10'000, 1, 0x3000, 0, TIME_ONESHOT);
    const u32 second = scheduler.create(10'000, 1, 0x3000, 0, TIME_ONESHOT);
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);
    EXPECT_TRUE(scheduler.destroy(first));
    EXPECT_TRUE(scheduler.destroy(second));
}

TEST_F(TimerSchedulerTest, CancelBeforeFiringSuppressesTheCallback) {
    const u32 id = scheduler.create(500, 1, 0x4000, 0, TIME_ONESHOT);
    ASSERT_NE(id, 0u);
    EXPECT_TRUE(scheduler.destroy(id));
    // The whole point of timeKillEvent: nothing may arrive afterwards.
    std::this_thread::sleep_for(700ms);
    EXPECT_EQ(recorder.count(), 0u);
}

TEST_F(TimerSchedulerTest, DestroyRejectsUnknownAndDoubleKill) {
    EXPECT_FALSE(scheduler.destroy(99999));
    const u32 id = scheduler.create(10'000, 1, 0x5000, 0, TIME_ONESHOT);
    ASSERT_NE(id, 0u);
    EXPECT_TRUE(scheduler.destroy(id));
    EXPECT_FALSE(scheduler.destroy(id));
}

TEST_F(TimerSchedulerTest, DestroyWaitsOutAnInFlightCallback) {
    // A callback that is slow on purpose: destroy() must not return while it
    // is still running, or a caller freeing user data races the callback.
    std::atomic<bool> inside{false};
    std::atomic<bool> finished{false};
    scheduler.set_proc_invoker([&](GuestAddr, u32, u64) {
        inside.store(true);
        std::this_thread::sleep_for(300ms);
        finished.store(true);
    });

    const u32 id =
        scheduler.create(5, 1, 0x6000, 0, TIME_ONESHOT | TIME_KILL_SYNCHRONOUS);
    ASSERT_NE(id, 0u);
    while (!inside.load()) std::this_thread::sleep_for(1ms);

    EXPECT_FALSE(scheduler.destroy(id));  // already retired by firing
    EXPECT_TRUE(finished.load()) << "destroy returned while the callback was running";
}

TEST_F(TimerSchedulerTest, DestroyOfPeriodicWaitsOutCallback) {
    // Learn timeSetEvent TIME_KILL_SYNCHRONOUS: timeKillEvent must not return
    // while the TIMEPROC for that id is still running. Periodic timers stay in
    // the map until killed, so destroy() reports success and still waits.
    std::atomic<bool> inside{false};
    std::atomic<bool> finished{false};
    scheduler.set_proc_invoker([&](GuestAddr, u32, u64) {
        inside.store(true);
        std::this_thread::sleep_for(300ms);
        finished.store(true);
    });

    const u32 id =
        scheduler.create(5, 1, 0x6000, 0, TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
    ASSERT_NE(id, 0u);
    while (!inside.load()) std::this_thread::sleep_for(1ms);

    EXPECT_TRUE(scheduler.destroy(id));
    EXPECT_TRUE(finished.load()) << "destroy returned while the callback was running";
}

TEST_F(TimerSchedulerTest, DestroyWithoutKillSynchronousDoesNotWait) {
    // Learn timeSetEvent: only TIME_KILL_SYNCHRONOUS makes timeKillEvent wait
    // for an in-flight TIMEPROC; otherwise a caller holding a lock its
    // TIMEPROC takes must not deadlock.
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    scheduler.set_proc_invoker([&](GuestAddr, u32, u64) {
        inside.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
        finished.store(true);
    });
    const u32 id = scheduler.create(5, 1, 0x6000, 0, TIME_PERIODIC);
    ASSERT_NE(id, 0u);
    while (!inside.load()) std::this_thread::sleep_for(1ms);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(scheduler.destroy(id));  // returns while the callback still runs
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 50ms)
        << "destroy without TIME_KILL_SYNCHRONOUS must not wait out the TIMEPROC";
    release.store(true);
    while (!finished.load()) std::this_thread::sleep_for(1ms);
}

TEST_F(TimerSchedulerTest, UserDataKeepsFullPointerWidth) {
    // dwUser is DWORD_PTR (winmm_timer_probe user_roundtrip on x64).
    const u32 id = scheduler.create(5, 1, 0x1000, 0x1122334455667788ull, TIME_ONESHOT);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(recorder.wait_for_count(1, 2s));
    EXPECT_EQ(recorder.users().front(), 0x1122334455667788ull);
}

TEST_F(TimerSchedulerTest, TimeProcMayKillSelfWithoutDeadlock) {
    // Learn TimeProc remarks: the callback may call timeKillEvent. That must
    // return without waiting for the current invocation, or the single
    // scheduler thread deadlocks and later ticks never run.
    std::atomic<bool> returned{false};
    std::atomic<bool> killed{false};
    std::atomic<u32> fires{0};
    scheduler.set_proc_invoker([&](GuestAddr, u32 id, u64) {
        fires.fetch_add(1);
        killed.store(scheduler.destroy(id));
        returned.store(true);
    });

    const u32 id = scheduler.create(5, 1, 0x1000, 0, TIME_PERIODIC);
    ASSERT_NE(id, 0u);
    for (int i = 0; i < 200 && !returned.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(returned.load()) << "timeKillEvent from TIMEPROC deadlocked";
    EXPECT_TRUE(killed.load());
    EXPECT_EQ(fires.load(), 1u);

    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(fires.load(), 1u);
    EXPECT_EQ(scheduler.active_count(), 0u);
}

TEST_F(TimerSchedulerTest, MultipleTimersAllFire) {
    Recorder shared;
    scheduler.set_proc_invoker(
        [&](GuestAddr, u32 id, u64 user) { shared.record(id, user); });

    const u32 a = scheduler.create(5, 1, 0x7000, 1, TIME_ONESHOT);
    const u32 b = scheduler.create(10, 1, 0x7000, 2, TIME_ONESHOT);
    const u32 c = scheduler.create(15, 1, 0x7000, 3, TIME_ONESHOT);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    ASSERT_NE(c, 0u);
    ASSERT_TRUE(shared.wait_for_count(3, 3s));

    auto users = shared.users();
    std::sort(users.begin(), users.end());
    EXPECT_EQ(users, (std::vector<u64>{1, 2, 3}));
}

TEST_F(TimerSchedulerTest, EventModeSignalsRatherThanCallingGuestCode) {
    std::atomic<u32> signalled_handle{0};
    std::atomic<bool> was_pulse{false};
    std::atomic<bool> proc_called{false};
    scheduler.set_proc_invoker([&](GuestAddr, u32, u64) { proc_called.store(true); });
    scheduler.set_event_signaler([&](u32 handle, bool pulse) {
        signalled_handle.store(handle);
        was_pulse.store(pulse);
    });

    // In event mode lpTimeProc carries a HANDLE, not a function pointer, so
    // invoking it as guest code would jump into a handle value.
    const u32 id = scheduler.create(5, 1, 0x1234, 0,
                                    TIME_ONESHOT | TIME_CALLBACK_EVENT_PULSE);
    ASSERT_NE(id, 0u);
    for (int i = 0; i < 200 && signalled_handle.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(signalled_handle.load(), 0x1234u);
    EXPECT_TRUE(was_pulse.load());
    EXPECT_FALSE(proc_called.load());
}

TEST_F(TimerSchedulerTest, ShutdownStopsDeliveryAndIsIdempotent) {
    const u32 id = scheduler.create(5, 1, 0x8000, 0, TIME_PERIODIC);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(recorder.wait_for_count(1, 2s));

    scheduler.shutdown();
    const usize after_shutdown = recorder.count();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(recorder.count(), after_shutdown);

    scheduler.shutdown();  // must not hang or crash
    EXPECT_EQ(scheduler.active_count(), 0u);
}

TEST_F(TimerSchedulerTest, PeriodicDoesNotStormAfterAStall) {
    // A periodic timer whose callback overruns its period must not accumulate
    // a backlog and then fire it all at once; it should re-anchor to cadence.
    std::atomic<int> fires{0};
    scheduler.set_proc_invoker([&](GuestAddr, u32, u64) {
        fires.fetch_add(1);
        std::this_thread::sleep_for(60ms);
    });

    const u32 id = scheduler.create(10, 1, 0x9000, 0, TIME_PERIODIC);
    ASSERT_NE(id, 0u);
    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(scheduler.destroy(id));

    // ~400ms of 60ms callbacks is at most ~7 fires. A storming implementation
    // would show far more as it replays every missed 10ms deadline.
    EXPECT_LE(fires.load(), 10) << "periodic timer replayed missed deadlines";
    EXPECT_GE(fires.load(), 2);
}
