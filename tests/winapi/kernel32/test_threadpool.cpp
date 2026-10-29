// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_threadpool.cpp
/// @brief Contract tests for the kernel32 thread-pool work objects.
///
/// `SubmitThreadpoolWork` returns void, so a guest cannot detect a dropped
/// submission: the callback simply never runs. These tests pin the behaviour
/// that matters — a submission reaches the callback with the arguments Windows
/// passes, a closed object survives until its callbacks finish, and the calls
/// that cannot be honoured say so rather than reporting success.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/thread/threadpool.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <chrono>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

/// One launch request the runtime would have turned into a guest thread.
struct Launch {
    u32 tid{0};
    GuestAddr func{0};
    GuestAddr param{0};
    u64 second{0};
    u64 third{0};
    usize arg_count{0};
};

} // namespace

class ThreadpoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x2000;
        marshaler.attach_cpu(cpu);

        // Capture launches instead of spawning real guest threads: the point
        // under test is what the pool asks the runtime to run.
        threads.set_run_callback(
            [this](u32 tid, GuestAddr func, GuestAddr param, u64 second,
                   u64 third, usize arg_count) {
                launches.push_back(Launch{tid, func, param, second, third,
                                          arg_count});
            });
        pool.set_thread_manager(&threads);
        threads.set_thread_exit_callback(
            [this](u32 tid, u32) { pool.on_thread_exit(tid); });
    }

    u32 call(void (ThreadpoolManager::*fn)(Marshaler&),
             std::initializer_list<u32> args) {
        usize index = 0;
        for (const u32 argument : args) {
            EXPECT_TRUE(memory.write_u32(
                cpu.gpr.rsp + 4 + index * sizeof(u32), argument).is_ok());
            ++index;
        }
        (pool.*fn)(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }

    u32 create_work(GuestAddr callback = CALLBACK, GuestAddr context = CONTEXT,
                    GuestAddr environment = 0) {
        return call(&ThreadpoolManager::impl_CreateThreadpoolWork,
                    {static_cast<u32>(callback), static_cast<u32>(context),
                     static_cast<u32>(environment)});
    }

    /// Finish the most recent launch, as a returning guest thread would.
    void complete_last_launch() {
        ASSERT_FALSE(launches.empty());
        threads.exit_thread(launches.back().tid, 0);
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;
    static constexpr GuestAddr CALLBACK   = 0x401000;
    static constexpr GuestAddr CONTEXT    = 0x1234;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HandleTable handle_table;
    ThreadManager threads{handle_table};
    ThreadpoolManager pool;
    std::vector<Launch> launches;
};

// =============================================================================
// Work object lifetime
// =============================================================================

TEST_F(ThreadpoolTest, CreateThreadpoolWorkReturnsDistinctNonNullObjects) {
    const u32 first = create_work();
    const u32 second = create_work();
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);
    EXPECT_EQ(pool.live_work_count(), 2u);
}

// Every callback used to leak its guest thread: the dispatch never closed the
// thread handle, so the handle slot, the GuestThread and its wait object
// lived until process exit (unbounded growth under CEF's PPL work). After
// N callbacks the handle table and thread count must be back where they were.
TEST_F(ThreadpoolTest, CallbacksDoNotLeakThreadHandlesOrThreads) {
    const usize handles_before = handle_table.count();
    const u32 threads_before = threads.active_thread_count();
    const u32 work = create_work();
    ASSERT_NE(work, 0u);
    for (int i = 0; i < 20; ++i) {
        call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
        complete_last_launch();
    }
    EXPECT_EQ(launches.size(), 20u);
    call(&ThreadpoolManager::impl_CloseThreadpoolWork, {work});
    EXPECT_EQ(pool.live_work_count(), 0u);
    EXPECT_EQ(handle_table.count(), handles_before);
    EXPECT_EQ(threads.active_thread_count(), threads_before);
}

TEST_F(ThreadpoolTest, CreateThreadpoolWorkRejectsANullCallback) {
    EXPECT_EQ(create_work(0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(pool.live_work_count(), 0u);
}

TEST_F(ThreadpoolTest, CreateThreadpoolWorkAcceptsDefaultCallbackEnvironment) {
    // InitializeThreadpoolEnvironment leaves Pool/CleanupGroup NULL; Chromium
    // still passes &env (non-null). That must create a work object.
    constexpr GuestAddr kEnv = DATA_BASE + 0x100;
    for (u32 i = 0; i < 40; ++i) {
        ASSERT_TRUE(memory.write_u32(kEnv + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kEnv + 0, 3).is_ok());   // Version
    ASSERT_TRUE(memory.write_u32(kEnv + 36, 40).is_ok()); // Size
    // Success leaves the caller's last error untouched (measured on Windows 11:
    // tools/oracle_probe/last_error_on_success_probe.expected).
    set_last_error(0xDEADu);
    const u32 work = create_work(CALLBACK, CONTEXT, kEnv);
    EXPECT_NE(work, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(pool.live_work_count(), 1u);
}

TEST_F(ThreadpoolTest, CreateThreadpoolWorkAcceptsPoolOnlyEnvironment) {
    constexpr GuestAddr kEnv = DATA_BASE + 0x200;
    for (u32 i = 0; i < 40; ++i) {
        ASSERT_TRUE(memory.write_u32(kEnv + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kEnv + 0, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(kEnv + 4, 0x7C000004).is_ok()); // Pool
    ASSERT_TRUE(memory.write_u32(kEnv + 36, 40).is_ok());
    const u32 work = create_work(CALLBACK, CONTEXT, kEnv);
    EXPECT_NE(work, 0u);
    EXPECT_EQ(pool.live_work_count(), 1u);
}

TEST_F(ThreadpoolTest, CreateThreadpoolWorkRefusesCleanupGroupEnvironment) {
    constexpr GuestAddr kEnv = DATA_BASE + 0x300;
    for (u32 i = 0; i < 40; ++i) {
        ASSERT_TRUE(memory.write_u32(kEnv + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kEnv + 0, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(kEnv + 8, 0xABCD).is_ok()); // CleanupGroup
    ASSERT_TRUE(memory.write_u32(kEnv + 36, 40).is_ok());
    EXPECT_EQ(create_work(CALLBACK, CONTEXT, kEnv), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_EQ(pool.live_work_count(), 0u);
}

// =============================================================================
// Submission actually reaches the callback
// =============================================================================

TEST_F(ThreadpoolTest, SubmitRunsTheCallbackWithTheWindowsArguments) {
    const u32 work = create_work();
    ASSERT_NE(work, 0u);

    call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});

    ASSERT_EQ(launches.size(), 1u);
    const Launch& launch = launches.front();
    EXPECT_EQ(launch.func, CALLBACK);
    // PTP_WORK_CALLBACK is (PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK).
    EXPECT_EQ(launch.arg_count, 3u);
    EXPECT_NE(launch.param, 0u);          // callback instance, never null
    EXPECT_EQ(launch.second, CONTEXT);
    EXPECT_EQ(launch.third, work);
}

TEST_F(ThreadpoolTest, EachSubmitRunsTheCallbackAgain) {
    const u32 work = create_work();
    for (int i = 0; i < 3; ++i) {
        call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
    }
    EXPECT_EQ(launches.size(), 3u);
}

TEST_F(ThreadpoolTest, SubmitOnAnUnknownObjectRunsNothing) {
    call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {0xBADF00D});
    EXPECT_TRUE(launches.empty());
}

// =============================================================================
// Close ordering
// =============================================================================

TEST_F(ThreadpoolTest, CloseFreesImmediatelyWhenNothingIsOutstanding) {
    const u32 work = create_work();
    ASSERT_EQ(pool.live_work_count(), 1u);
    call(&ThreadpoolManager::impl_CloseThreadpoolWork, {work});
    EXPECT_EQ(pool.live_work_count(), 0u);
}

TEST_F(ThreadpoolTest, CloseDefersUntilAnOutstandingCallbackReturns) {
    const u32 work = create_work();
    call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
    ASSERT_EQ(launches.size(), 1u);

    // Windows keeps the object alive until its callbacks finish.
    call(&ThreadpoolManager::impl_CloseThreadpoolWork, {work});
    EXPECT_EQ(pool.live_work_count(), 1u);

    complete_last_launch();
    EXPECT_EQ(pool.live_work_count(), 0u);
}

TEST_F(ThreadpoolTest, SubmitAfterCloseRunsNothing) {
    const u32 work = create_work();
    call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
    call(&ThreadpoolManager::impl_CloseThreadpoolWork, {work});
    launches.clear();

    call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
    EXPECT_TRUE(launches.empty());
}

TEST_F(ThreadpoolTest, WaitReturnsOnceOutstandingCallbacksHaveFinished) {
    const u32 work = create_work();
    call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
    ASSERT_EQ(launches.size(), 1u);

    // Complete first so the wait does not block this single-threaded test.
    complete_last_launch();
    call(&ThreadpoolManager::impl_WaitForThreadpoolWorkCallbacks, {work, 0});
    EXPECT_EQ(pool.live_work_count(), 1u);

    call(&ThreadpoolManager::impl_CloseThreadpoolWork, {work});
    EXPECT_EQ(pool.live_work_count(), 0u);
}

// =============================================================================
// Simple callbacks
// =============================================================================

TEST_F(ThreadpoolTest, TrySubmitRunsASimpleCallbackAndReportsSuccess) {
    const u32 result =
        call(&ThreadpoolManager::impl_TrySubmitThreadpoolCallback,
             {static_cast<u32>(CALLBACK), static_cast<u32>(CONTEXT), 0});
    EXPECT_EQ(result, 1u);

    ASSERT_EQ(launches.size(), 1u);
    const Launch& launch = launches.front();
    EXPECT_EQ(launch.func, CALLBACK);
    // PTP_SIMPLE_CALLBACK is (PTP_CALLBACK_INSTANCE, PVOID context).
    EXPECT_EQ(launch.arg_count, 2u);
    EXPECT_EQ(launch.second, CONTEXT);
    // A simple callback has no work object to track.
    EXPECT_EQ(pool.live_work_count(), 0u);
}

TEST_F(ThreadpoolTest, TrySubmitRejectsBadArgumentsWithoutRunningAnything) {
    EXPECT_EQ(call(&ThreadpoolManager::impl_TrySubmitThreadpoolCallback,
                   {0, static_cast<u32>(CONTEXT), 0}),
              0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    // CleanupGroup membership is still unimplemented — refuse honestly.
    constexpr GuestAddr kEnv = DATA_BASE + 0x500;
    for (u32 i = 0; i < 40; ++i) {
        ASSERT_TRUE(memory.write_u32(kEnv + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kEnv + 0, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(kEnv + 8, 0xABCD).is_ok()); // CleanupGroup
    ASSERT_TRUE(memory.write_u32(kEnv + 36, 40).is_ok());
    EXPECT_EQ(call(&ThreadpoolManager::impl_TrySubmitThreadpoolCallback,
                   {static_cast<u32>(CALLBACK), static_cast<u32>(CONTEXT),
                    static_cast<u32>(kEnv)}),
              0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);

    EXPECT_TRUE(launches.empty());
}

TEST_F(ThreadpoolTest, TrySubmitAcceptsDefaultCallbackEnvironment) {
    constexpr GuestAddr kEnv = DATA_BASE + 0x400;
    for (u32 i = 0; i < 40; ++i) {
        ASSERT_TRUE(memory.write_u32(kEnv + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kEnv + 0, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(kEnv + 36, 40).is_ok());
    EXPECT_EQ(call(&ThreadpoolManager::impl_TrySubmitThreadpoolCallback,
                   {static_cast<u32>(CALLBACK), static_cast<u32>(CONTEXT),
                    static_cast<u32>(kEnv)}),
              1u);
    ASSERT_EQ(launches.size(), 1u);
}

TEST_F(ThreadpoolTest, TrySubmitReportsFailureWhenNoThreadCanStart) {
    // With no runtime attached the pool cannot run anything, and
    // TrySubmitThreadpoolCallback returns BOOL, so it must say so.
    ThreadpoolManager detached;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4,
                                 static_cast<u32>(CALLBACK)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8,
                                 static_cast<u32>(CONTEXT)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    detached.impl_TrySubmitThreadpoolCallback(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// Export table
// =============================================================================

TEST_F(ThreadpoolTest, EveryExportHasTheStdcallCleanupOfItsSignature) {
    DllExports exports;
    add_threadpool_exports(exports, pool);

    const struct { const char* name; u16 bytes; } expected[] = {
        {"CreateThreadpoolWork", 12},
        {"SubmitThreadpoolWork", 4},
        {"CloseThreadpoolWork", 4},
        {"WaitForThreadpoolWorkCallbacks", 8},
        {"TrySubmitThreadpoolCallback", 12},
        {"FreeLibraryWhenCallbackReturns", 8},
        // threadpoolapiset.h prototypes.
        {"CreateThreadpoolTimer", 12},
        {"SetThreadpoolTimer", 16},
        {"SetThreadpoolTimerEx", 16},
        {"IsThreadpoolTimerSet", 4},
        {"WaitForThreadpoolTimerCallbacks", 8},
        {"CloseThreadpoolTimer", 4},
        {"CreateThreadpoolWait", 12},
        {"SetThreadpoolWait", 12},
        {"SetThreadpoolWaitEx", 16},
        {"WaitForThreadpoolWaitCallbacks", 8},
        {"CloseThreadpoolWait", 4},
        {"CallbackMayRunLong", 4},
        {"SetEventWhenCallbackReturns", 8},
        {"ReleaseSemaphoreWhenCallbackReturns", 12},
        {"ReleaseMutexWhenCallbackReturns", 8},
        {"LeaveCriticalSectionWhenCallbackReturns", 8},
        {"DisassociateCurrentThreadFromCallback", 4},
        {"CreateThreadpoolCleanupGroup", 0},
        {"CreateThreadpoolIo", 16},
    };
    for (const auto& want : expected) {
        bool found = false;
        for (const ExportEntry& entry : exports.entries) {
            if (entry.name != want.name) continue;
            found = true;
            EXPECT_EQ(entry.stdcall_bytes, want.bytes) << want.name;
            break;
        }
        EXPECT_TRUE(found) << want.name << " is not exported";
    }
}

// =============================================================================
// Timer and wait objects (tools/oracle_probe/s4_threadpool_probe.expected)
// =============================================================================

namespace {

/// Poll until @p pred holds (the pool's timer thread runs asynchronously).
template <typename Pred>
bool eventually(Pred pred) {
    for (int i = 0; i < 400; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

class ThreadpoolTimerWaitTest : public ThreadpoolTest {
protected:
    void SetUp() override {
        ThreadpoolTest::SetUp();
        // The capturing run callback is called from the pool's own threads.
        threads.set_run_callback(
            [this](u32 tid, GuestAddr func, GuestAddr param, u64 second,
                   u64 third, usize arg_count) {
                std::lock_guard<std::mutex> lock(launch_mutex);
                launches.push_back(Launch{tid, func, param, second, third,
                                          arg_count});
            });
        pool.set_sync_manager(&sync);
    }
    void TearDown() override { pool.begin_shutdown(); }

    usize launch_count() {
        std::lock_guard<std::mutex> lock(launch_mutex);
        return launches.size();
    }
    Launch launch_at(usize i) {
        std::lock_guard<std::mutex> lock(launch_mutex);
        return launches.at(i);
    }
    /// A relative FILETIME of @p ms at FT_ADDR.
    void relative_due(i64 ms) {
        ASSERT_TRUE(memory.write_u64(FT_ADDR, static_cast<u64>(-ms * 10000)).is_ok());
    }

    static constexpr GuestAddr FT_ADDR = DATA_BASE + 0x800;
    SyncManager sync{handle_table};
    std::mutex launch_mutex;
};

TEST_F(ThreadpoolTimerWaitTest, OneShotTimerFiresOnceAndStaysSet) {
    const u32 timer = call(&ThreadpoolManager::impl_CreateThreadpoolTimer,
                           {CALLBACK, CONTEXT, 0});
    ASSERT_NE(timer, 0u);                                       // tpt_create
    EXPECT_EQ(call(&ThreadpoolManager::impl_IsThreadpoolTimerSet, {timer}), 0u);
    relative_due(20);
    (void)call(&ThreadpoolManager::impl_SetThreadpoolTimer, {timer, FT_ADDR, 0, 0});
    EXPECT_EQ(call(&ThreadpoolManager::impl_IsThreadpoolTimerSet, {timer}), 1u);
    ASSERT_TRUE(eventually([&] { return launch_count() == 1; }));
    // PTP_TIMER_CALLBACK(instance, context, timer).
    const Launch l = launch_at(0);
    EXPECT_EQ(l.func, CALLBACK);
    EXPECT_NE(l.param, 0u);
    EXPECT_EQ(l.second, CONTEXT);
    EXPECT_EQ(l.third, timer);
    EXPECT_EQ(l.arg_count, 3u);
    // tpt_oneshot isset_after=1: still "set" after expiring.
    EXPECT_EQ(call(&ThreadpoolManager::impl_IsThreadpoolTimerSet, {timer}), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(launch_count(), 1u);
    threads.exit_thread(l.tid, 0);
    (void)call(&ThreadpoolManager::impl_WaitForThreadpoolTimerCallbacks, {timer, 0});
    // NULL due time cancels (tpt_far_cancel_isset r=0).
    (void)call(&ThreadpoolManager::impl_SetThreadpoolTimer, {timer, 0, 0, 0});
    EXPECT_EQ(call(&ThreadpoolManager::impl_IsThreadpoolTimerSet, {timer}), 0u);
    (void)call(&ThreadpoolManager::impl_CloseThreadpoolTimer, {timer});
}

TEST_F(ThreadpoolTimerWaitTest, PeriodicTimerRepeatsUntilCancelled) {
    const u32 timer = call(&ThreadpoolManager::impl_CreateThreadpoolTimer,
                           {CALLBACK, CONTEXT, 0});
    relative_due(5);
    (void)call(&ThreadpoolManager::impl_SetThreadpoolTimer, {timer, FT_ADDR, 20, 0});
    ASSERT_TRUE(eventually([&] { return launch_count() >= 3; }));
    (void)call(&ThreadpoolManager::impl_SetThreadpoolTimer, {timer, 0, 0, 0});
    for (usize i = 0; i < launch_count(); ++i) threads.exit_thread(launch_at(i).tid, 0);
    const usize fired = launch_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(launch_count(), fired);                            // tpt_after_cancel
    (void)call(&ThreadpoolManager::impl_CloseThreadpoolTimer, {timer});
}

TEST_F(ThreadpoolTimerWaitTest, WaitCallsBackOnceWithSignalOrTimeoutResult) {
    const Handle ev = sync.create_event(false, false, "");
    const u32 wait = call(&ThreadpoolManager::impl_CreateThreadpoolWait,
                          {CALLBACK, CONTEXT, 0});
    ASSERT_NE(wait, 0u);                                          // tpw_create
    (void)call(&ThreadpoolManager::impl_SetThreadpoolWait, {wait, ev, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(launch_count(), 0u);                                // tpw_unsignaled
    ASSERT_TRUE(sync.set_event(ev));
    ASSERT_TRUE(eventually([&] { return launch_count() == 1; }));
    // PTP_WAIT_CALLBACK(instance, context, wait, WaitResult = WAIT_OBJECT_0).
    Launch l = launch_at(0);
    EXPECT_EQ(l.second, CONTEXT);
    EXPECT_EQ(l.third, wait);
    EXPECT_EQ(l.arg_count, 4u);
    EXPECT_EQ(threads.get_thread(l.tid)->fourth_parameter, 0u);
    threads.exit_thread(l.tid, 0);
    // One-shot: a second signal without SetThreadpoolWait is not seen
    // (tpw_not_rearmed).
    ASSERT_TRUE(sync.set_event(ev));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(launch_count(), 1u);
    (void)sync.reset_event(ev);
    // Relative timeout: WAIT_TIMEOUT (tpw_timeout result=102).
    relative_due(20);
    (void)call(&ThreadpoolManager::impl_SetThreadpoolWait, {wait, ev, FT_ADDR});
    ASSERT_TRUE(eventually([&] { return launch_count() == 2; }));
    l = launch_at(1);
    EXPECT_EQ(threads.get_thread(l.tid)->fourth_parameter, 0x102u);
    threads.exit_thread(l.tid, 0);
    // A NULL handle cancels a pending wait (tpw_cancel_null).
    (void)call(&ThreadpoolManager::impl_SetThreadpoolWait, {wait, ev, 0});
    (void)call(&ThreadpoolManager::impl_SetThreadpoolWait, {wait, 0, 0});
    ASSERT_TRUE(sync.set_event(ev));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(launch_count(), 2u);
    (void)call(&ThreadpoolManager::impl_WaitForThreadpoolWaitCallbacks, {wait, 1});
    (void)call(&ThreadpoolManager::impl_CloseThreadpoolWait, {wait});
}

TEST_F(ThreadpoolTimerWaitTest, CallbackInstanceFunctionsActWhenTheCallbackReturns) {
    // callbackmayrunlong r=1
    EXPECT_EQ(call(&ThreadpoolManager::impl_CallbackMayRunLong, {0x1234}), 1u);
    const Handle done = sync.create_event(true, false, "");
    const u32 work = create_work();
    (void)call(&ThreadpoolManager::impl_SubmitThreadpoolWork, {work});
    ASSERT_EQ(launch_count(), 1u);
    const Launch l = launch_at(0);
    (void)call(&ThreadpoolManager::impl_SetEventWhenCallbackReturns,
               {static_cast<u32>(l.param), done});
    EXPECT_EQ(sync.wait_for_single_object(done, 0), WAIT_TIMEOUT);
    threads.exit_thread(l.tid, 0);
    // seteventwhencallbackreturns wait=0: set once the callback returned.
    EXPECT_EQ(sync.wait_for_single_object(done, 0), WAIT_OBJECT_0);
    (void)call(&ThreadpoolManager::impl_CloseThreadpoolWork, {work});
}
