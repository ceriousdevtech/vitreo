/// @file test_thread_suspend.cpp
/// @brief Tests for ThreadSuspendRegistry: live-state publish, cooperative
///        suspend/park with consistent snapshot capture, and SetThreadContext
///        application on resume. Backs the real GetThreadContext contract that
///        Chromium's sampling/watchdog threads depend on inside the Steam
///        webhelper (previously a zeroed-CONTEXT fail-stub).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/thread/thread_suspend.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
// Unique per-test TIDs so tests do not collide in the process-global registry.
std::atomic<u32> g_next_test_tid{0x51000000u};
u32 fresh_tid() { return g_next_test_tid.fetch_add(1); }
}  // namespace

// A published thread's live registers are readable via capture().
TEST(ThreadSuspendRegistry, CaptureReadsPublishedLiveState) {
    const u32 tid = fresh_tid();
    cpu::CpuState live{};
    live.init_long_mode();
    live.rip = 0x1400abcde;
    live.gpr.rax = 0x1122334455667788ull;
    live.gpr.rsp = 0x7ff0000;

    auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
    cpu::CpuState out{};
    ASSERT_TRUE(ThreadSuspendRegistry::instance().capture(tid, out));
    EXPECT_EQ(out.rip, 0x1400abcdeull);
    EXPECT_EQ(out.gpr.rax, 0x1122334455667788ull);
    EXPECT_EQ(out.gpr.rsp, 0x7ff0000ull);

    const auto samples = ThreadSuspendRegistry::instance().sample_threads();
    bool found = false;
    for (const auto& sample : samples) {
        if (sample.tid != tid) continue;
        found = true;
        EXPECT_EQ(sample.rip, 0x1400abcdeull);
        EXPECT_EQ(sample.eax, 0x1122334455667788ull);
        EXPECT_EQ(sample.sp, 0x7ff0000ull);
    }
    EXPECT_TRUE(found);
}

// After the publishing scope ends the thread is untracked.
TEST(ThreadSuspendRegistry, UnpublishRemovesTracking) {
    const u32 tid = fresh_tid();
    cpu::CpuState live{};
    live.init_long_mode();
    {
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        cpu::CpuState out{};
        EXPECT_TRUE(ThreadSuspendRegistry::instance().capture(tid, out));
    }
    cpu::CpuState out{};
    EXPECT_FALSE(ThreadSuspendRegistry::instance().capture(tid, out));
}

// Separate host callback threads can temporarily execute under the same guest
// TID. One publisher exiting must not erase or destroy the shared Slot while
// the other guard still uses its host-call boundary.
TEST(ThreadSuspendRegistry, DuplicateTidPublisherKeepsSlotAlive) {
    const u32 tid = fresh_tid();
    std::atomic<bool> first_published{false};
    std::atomic<bool> second_published{false};
    std::atomic<bool> release_second{false};
    std::atomic<bool> second_finished{false};
    std::atomic<bool> release_first{false};

    std::thread first([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        live.rip = 0x11111111;
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        first_published.store(true, std::memory_order_release);
        while (!second_finished.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // This locked a destroyed mutex in the original raw-pointer design.
        guard.enter_host_call();
        guard.leave_host_call();

        cpu::CpuState snapshot{};
        EXPECT_TRUE(ThreadSuspendRegistry::instance().capture(tid, snapshot));
        EXPECT_EQ(snapshot.rip, 0x11111111ull);
        while (!release_first.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!first_published.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread second([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        live.rip = 0x22222222;
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        second_published.store(true, std::memory_order_release);
        while (!release_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!second_published.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    release_second.store(true, std::memory_order_release);
    second.join();
    second_finished.store(true, std::memory_order_release);

    release_first.store(true, std::memory_order_release);
    first.join();

    cpu::CpuState snapshot{};
    EXPECT_FALSE(ThreadSuspendRegistry::instance().capture(tid, snapshot));
}

// A real running thread parks at its checkpoint on request_suspend, and the
// snapshot captured reflects the register values it published, frozen while
// parked; resume lets it finish.
TEST(ThreadSuspendRegistry, RunningThreadParksAndCapturesConsistentSnapshot) {
    const u32 tid = fresh_tid();
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<u64> spins{0};

    std::thread worker([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        live.rip = 0xdead0000;
        live.gpr.rax = 0x1000;
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        started.store(true);
        // Busy loop, advancing state each iteration and checkpointing so a
        // suspend request can park us.
        while (!stop.load(std::memory_order_acquire)) {
            guard.checkpoint();
            live.rip += 1;
            live.gpr.rax += 1;
            spins.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    while (!started.load()) std::this_thread::yield();

    auto& reg = ThreadSuspendRegistry::instance();
    ASSERT_TRUE(reg.request_suspend(tid));

    // While parked the captured snapshot must be stable across reads.
    cpu::CpuState a{}, b{};
    ASSERT_TRUE(reg.capture(tid, a));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(reg.capture(tid, b));
    EXPECT_EQ(a.rip, b.rip) << "snapshot changed while suspended";
    EXPECT_EQ(a.gpr.rax, b.gpr.rax);
    // rip advanced from the seed, proving the thread ran before parking.
    EXPECT_GE(a.rip, 0xdead0000ull);

    const u64 spins_at_park = spins.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(spins.load(), spins_at_park) << "thread kept running while suspended";

    reg.request_resume(tid);
    // After resume the thread makes progress again.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GT(spins.load(), spins_at_park);

    stop.store(true, std::memory_order_release);
    worker.join();
}

// A thread blocked in a host thunk cannot reach a guest basic-block
// checkpoint. SuspendThread must retain the suspend count and expose its live
// state without imposing the full bounded-park timeout on every sample.
TEST(ThreadSuspendRegistry, HostCallSuspensionReturnsPromptlyWithLiveSnapshot) {
    const u32 tid = fresh_tid();
    std::atomic<bool> in_host_call{false};
    std::atomic<bool> leave_host_call{false};

    std::thread worker([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        live.rip = 0x7ffa01234567;
        live.gpr.rax = 0x123456789abcdef0ull;
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        guard.enter_host_call();
        in_host_call.store(true, std::memory_order_release);
        while (!leave_host_call.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        guard.leave_host_call();
        guard.checkpoint();
    });

    while (!in_host_call.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto& reg = ThreadSuspendRegistry::instance();
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(reg.request_suspend(tid));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::milliseconds(50));

    cpu::CpuState snapshot{};
    ASSERT_TRUE(reg.capture(tid, snapshot));
    EXPECT_EQ(snapshot.rip, 0x7ffa01234567ull);
    EXPECT_EQ(snapshot.gpr.rax, 0x123456789abcdef0ull);

    reg.request_resume(tid);
    leave_host_call.store(true, std::memory_order_release);
    worker.join();
}

// A suspension requester can begin waiting immediately before the target
// crosses into a host call.  The atomic fast path must still notify that
// condition waiter once the host-call boundary becomes a valid snapshot.
TEST(ThreadSuspendRegistry, PendingSuspendIsNotifiedByHostCallEntry) {
    const u32 tid = fresh_tid();
    std::atomic<bool> published{false};
    std::atomic<bool> enter_host_call{false};
    std::atomic<bool> in_host_call{false};
    std::atomic<bool> leave_host_call{false};
    std::atomic<bool> suspend_started{false};
    std::atomic<bool> suspend_returned{false};

    std::thread worker([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        published.store(true, std::memory_order_release);
        while (!enter_host_call.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        guard.enter_host_call();
        in_host_call.store(true, std::memory_order_release);
        while (!leave_host_call.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        guard.leave_host_call();
        guard.checkpoint();
    });

    while (!published.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread suspender([&] {
        suspend_started.store(true, std::memory_order_release);
        if (ThreadSuspendRegistry::instance().request_suspend(tid)) {
            suspend_returned.store(true, std::memory_order_release);
        }
    });
    while (!suspend_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    enter_host_call.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (!suspend_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(in_host_call.load(std::memory_order_acquire));
    EXPECT_TRUE(suspend_returned.load(std::memory_order_acquire));

    ThreadSuspendRegistry::instance().request_resume(tid);
    leave_host_call.store(true, std::memory_order_release);
    suspender.join();
    worker.join();
}

TEST(ThreadSuspendRegistry, SuspendWaitsPastFormerTimeoutUntilGuestParks) {
    const u32 tid = fresh_tid();
    std::atomic<bool> started{false};
    std::atomic<bool> suspend_returned{false};

    std::thread worker([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        started.store(true, std::memory_order_release);

        // Model a long guest/JIT run before its next basic-block checkpoint.
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        guard.checkpoint();
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto& reg = ThreadSuspendRegistry::instance();
    std::thread suspender([&] {
        EXPECT_TRUE(reg.request_suspend(tid));
        suspend_returned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(suspend_returned.load(std::memory_order_acquire))
        << "SuspendThread returned while guest user-mode code was still running";

    for (int i = 0;
         i < 500 && !suspend_returned.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(suspend_returned.load(std::memory_order_acquire));
    reg.request_resume(tid);

    suspender.join();
    worker.join();
}

// Nested suspends require a matching number of resumes before the thread runs.
TEST(ThreadSuspendRegistry, SuspendCountRequiresMatchingResumes) {
    const u32 tid = fresh_tid();
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<u64> spins{0};

    std::thread worker([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        started.store(true);
        while (!stop.load(std::memory_order_acquire)) {
            guard.checkpoint();
            spins.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    while (!started.load()) std::this_thread::yield();

    auto& reg = ThreadSuspendRegistry::instance();
    ASSERT_TRUE(reg.request_suspend(tid));
    ASSERT_TRUE(reg.request_suspend(tid));

    reg.request_resume(tid);  // depth 2 -> 1, still parked
    const u64 s1 = spins.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(spins.load(), s1) << "resumed too early (depth still > 0)";

    reg.request_resume(tid);  // depth 1 -> 0, runs
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GT(spins.load(), s1);

    stop.store(true, std::memory_order_release);
    worker.join();
}

// A SetThreadContext (apply) issued while parked is written into the live
// register file when the thread resumes.
TEST(ThreadSuspendRegistry, ApplyWhileParkedTakesEffectOnResume) {
    const u32 tid = fresh_tid();
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<u64> observed_rax{0};

    std::thread worker([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live{};
        live.init_long_mode();
        live.gpr.rax = 0x1;
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        started.store(true);
        while (!stop.load(std::memory_order_acquire)) {
            guard.checkpoint();
            observed_rax.store(live.gpr.rax, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    while (!started.load()) std::this_thread::yield();

    auto& reg = ThreadSuspendRegistry::instance();
    ASSERT_TRUE(reg.request_suspend(tid));

    cpu::CpuState ctx{};
    ASSERT_TRUE(reg.capture(tid, ctx));
    ctx.gpr.rax = 0xABCDEF;
    ASSERT_TRUE(reg.apply(tid, ctx));

    reg.request_resume(tid);
    // Give the thread time to resume and observe the applied value.
    for (int i = 0; i < 200 && observed_rax.load() != 0xABCDEF; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(observed_rax.load(), 0xABCDEFull);

    stop.store(true, std::memory_order_release);
    worker.join();
}

// TerminateThread on a thread running guest code: the terminator does not
// return until the target reached a checkpoint, and from then on the target's
// dispatch loop sees terminate_requested() (terminate_thread_probe SPIN
// stopped=1).
TEST(ThreadSuspendRegistry, TerminateWaitsForRunningTargetToAcknowledge) {
    const u32 tid = fresh_tid();
    cpu::CpuState live{};
    live.init_long_mode();
    std::atomic<bool> published{false};
    std::atomic<u64> blocks_after_terminate{0};
    std::atomic<bool> terminator_returned{false};
    std::thread target([&] {
        ThreadManager::set_current_thread_id(tid);
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        published = true;
        for (;;) {
            guard.checkpoint();
            if (guard.terminate_requested()) break;
            if (terminator_returned.load()) ++blocks_after_terminate;
        }
    });
    while (!published.load()) std::this_thread::yield();
    ThreadSuspendRegistry::instance().request_terminate(tid);
    terminator_returned = true;
    target.join();
    EXPECT_EQ(blocks_after_terminate.load(), 0u);
    EXPECT_TRUE(ThreadSuspendRegistry::instance().is_terminating(tid));
}

// A target blocked inside a host call cannot run guest code, so the
// terminator returns without waiting; the target leaves at its first
// checkpoint after the call returns.
TEST(ThreadSuspendRegistry, TerminateDoesNotWaitForTargetBlockedInHostCall) {
    const u32 tid = fresh_tid();
    cpu::CpuState live{};
    live.init_long_mode();
    std::atomic<bool> in_host_call{false};
    std::atomic<bool> release{false};
    std::atomic<bool> left_after_call{false};
    std::thread target([&] {
        ThreadManager::set_current_thread_id(tid);
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        guard.enter_host_call();
        in_host_call = true;
        while (!release.load()) std::this_thread::yield();
        guard.leave_host_call();
        guard.checkpoint();
        left_after_call = guard.terminate_requested();
    });
    while (!in_host_call.load()) std::this_thread::yield();
    ThreadSuspendRegistry::instance().request_terminate(tid);
    release = true;
    target.join();
    EXPECT_TRUE(left_after_call.load());
}

// A thread SuspendThread parked is still terminated (terminate_thread_probe
// SUSPENDED_RUNNING wait=0): the park ends and the loop leaves guest code.
TEST(ThreadSuspendRegistry, TerminateReleasesSuspendedThread) {
    const u32 tid = fresh_tid();
    cpu::CpuState live{};
    live.init_long_mode();
    std::atomic<bool> published{false};
    std::atomic<bool> terminated_seen{false};
    std::thread target([&] {
        ThreadManager::set_current_thread_id(tid);
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        published = true;
        for (;;) {
            guard.checkpoint();
            if (guard.terminate_requested()) break;
        }
        terminated_seen = true;
    });
    while (!published.load()) std::this_thread::yield();
    ASSERT_TRUE(ThreadSuspendRegistry::instance().request_suspend(tid));
    ThreadSuspendRegistry::instance().request_terminate(tid);
    target.join();
    EXPECT_TRUE(terminated_seen.load());
}

// Nested guest callbacks (a window procedure, a timer or APC routine) run on a
// nested PublishGuard. Windows suspends a thread wherever it is; Vitreo's
// cooperative SuspendThread treats a thread as suspendable when its innermost
// activation is a host call. These pin which activation is innermost.
namespace {

/// Runs `body(guard)` on a published worker; `suspend_returned_within` asks
/// for a suspend once the worker reaches `ready`, and reports whether it
/// returned within `budget`. The worker is always released afterwards.
struct NestedSuspendProbe {
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    std::atomic<bool> park_now{false};

    template <typename Body>
    bool suspend_returned_within(Body body, std::chrono::milliseconds budget) {
        const u32 tid = fresh_tid();
        std::thread worker([&] {
            ThreadManager::set_current_thread_id(tid);
            cpu::CpuState live{};
            live.init_long_mode();
            auto outer = ThreadSuspendRegistry::instance().publish(tid, &live);
            body(outer);
            outer.checkpoint();
        });
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        std::atomic<bool> returned{false};
        std::thread suspender([&] {
            if (ThreadSuspendRegistry::instance().request_suspend(tid)) {
                returned.store(true, std::memory_order_release);
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!returned.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const bool in_time = returned.load(std::memory_order_acquire);
        // Unwind: let a running callback reach its checkpoint and a blocked
        // one leave its host call; either way the worker parks, the suspend
        // returns, and the resume lets it finish.
        park_now.store(true, std::memory_order_release);
        release.store(true, std::memory_order_release);
        while (!returned.load(std::memory_order_acquire)) std::this_thread::yield();
        ThreadSuspendRegistry::instance().request_resume(tid);
        suspender.join();
        worker.join();
        return in_time;
    }

    void wait_released() {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    }
};

}  // namespace

// The hang: host code that was not itself a counted host call (a host-side
// message delivery, a fast-thunk path) ran a guest callback, and the callback
// blocked in WaitForSingleObject. Its counters read 1 host call / 1 override,
// which the old rule took for "running guest code", so SuspendThread waited
// out the whole wait.
TEST(ThreadSuspendRegistry, ACallbackThatBlocksIsSuspendableWhereverItWasEntered) {
    NestedSuspendProbe probe;
    const bool prompt = probe.suspend_returned_within(
        [&](ThreadSuspendRegistry::PublishGuard&) {
            auto callback = ThreadSuspendRegistry::instance().publish(
                ThreadManager::get_current_thread_id_static(), nullptr);
            callback.enter_host_call();
            probe.ready.store(true, std::memory_order_release);
            probe.wait_released();
            callback.leave_host_call();
        },
        std::chrono::milliseconds(200));
    EXPECT_TRUE(prompt) << "SuspendThread waited on a thread blocked in a host "
                           "call inside a guest callback";
}

// A callback still running guest code is not a suspend boundary, even inside
// an enclosing host call: SuspendThread waits for its next checkpoint.
TEST(ThreadSuspendRegistry, ACallbackRunningGuestCodeInsideAHostCallIsNotABoundary) {
    NestedSuspendProbe probe;
    const bool prompt = probe.suspend_returned_within(
        [&](ThreadSuspendRegistry::PublishGuard& outer) {
            outer.enter_host_call();
            {
                auto callback = ThreadSuspendRegistry::instance().publish(
                    ThreadManager::get_current_thread_id_static(), nullptr);
                probe.ready.store(true, std::memory_order_release);
                while (!probe.park_now.load(std::memory_order_acquire)) {
                    std::this_thread::yield();   // "guest code"
                }
                callback.checkpoint();           // parks here
            }
            outer.leave_host_call();
        },
        std::chrono::milliseconds(100));
    EXPECT_FALSE(prompt)
        << "SuspendThread returned while the callback was running guest code";
}

// The ordinary nesting: host call, callback, host call inside it.
TEST(ThreadSuspendRegistry, ACallbackBlockedInsideAHostCallIsSuspendable) {
    NestedSuspendProbe probe;
    const bool prompt = probe.suspend_returned_within(
        [&](ThreadSuspendRegistry::PublishGuard& outer) {
            outer.enter_host_call();
            {
                auto callback = ThreadSuspendRegistry::instance().publish(
                    ThreadManager::get_current_thread_id_static(), nullptr);
                callback.enter_host_call();
                probe.ready.store(true, std::memory_order_release);
                probe.wait_released();
                callback.leave_host_call();
            }
            outer.leave_host_call();
        },
        std::chrono::milliseconds(200));
    EXPECT_TRUE(prompt);
}
