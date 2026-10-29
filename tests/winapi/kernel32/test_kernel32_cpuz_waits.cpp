/// @file test_kernel32_cpuz_waits.cpp
/// @brief Unit tests for KERNEL32 handle duplication, registered waits, semaphores, APCs and alertable waits

#include "kernel32_cpuz_fixture.hpp"
#include <atomic>
#include <chrono>
#include <winapi/kernel32/error.hpp>
#include <ipc/ipc_gate.hpp>
#include <unistd.h>
#include <thread>

TEST_F(Kernel32CpuzTest,
       UnregisterWaitExSignalsCompletionEventAfterWaitCancellation) {
    constexpr GuestAddr wait_out = DATA_BASE + 0x100;
    constexpr GuestAddr callback = 0x00401000;
    constexpr u32 execute_only_once = 0x00000008;
    const GuestAddr stack = cpu.sp();

    const Handle observed = sync.create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    const Handle completion = sync.create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    ASSERT_NE(observed, NULL_HANDLE);
    ASSERT_NE(completion, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, observed).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, callback).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, INFINITE_WAIT).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, execute_only_once).is_ok());
    mgr.impl_RegisterWaitForSingleObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle wait_handle =
        memory.read_u32(wait_out).value_or(NULL_HANDLE);
    ASSERT_NE(wait_handle, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, completion).is_ok());
    mgr.impl_UnregisterWaitEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(
        sync.wait_for_single_object(completion, 1000),
        WAIT_OBJECT_0);

    threads.join_host_threads();
}

TEST_F(Kernel32CpuzTest, RegisterWaitCallbackFiresOnSetEvent) {
    constexpr GuestAddr wait_out = DATA_BASE + 0x100;
    constexpr GuestAddr callback = 0x00401000;
    constexpr u32 execute_only_once = 0x00000008;
    const GuestAddr stack = cpu.sp();

    std::atomic<int> callback_launches{0};
    threads.set_run_callback(
        [&](u32 tid, GuestAddr, GuestAddr, u64, u64, usize) {
            callback_launches.fetch_add(1, std::memory_order_release);
            // Mimic a guest thread returning so UnregisterWaitEx can join.
            threads.exit_thread(tid, 0);
        });

    const Handle observed = sync.create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    ASSERT_NE(observed, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, observed).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, callback).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, INFINITE_WAIT).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, execute_only_once).is_ok());
    mgr.impl_RegisterWaitForSingleObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle wait_handle =
        memory.read_u32(wait_out).value_or(NULL_HANDLE);
    ASSERT_NE(wait_handle, NULL_HANDLE);

    ASSERT_TRUE(sync.set_event(observed));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (callback_launches.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(callback_launches.load(std::memory_order_acquire), 1);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_UnregisterWaitEx(marshaler);
    threads.join_host_threads();
}

// tools/oracle_probe/handles_waits_probe.expected: the runtime-attach call
// DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
// &h, THREAD_ALL_ACCESS, FALSE, 0) succeeds with a real thread handle, and so
// does a SYNCHRONIZE-only duplicate -- no DUPLICATE_SAME_ACCESS needed.
TEST_F(Kernel32CpuzTest, DuplicateCurrentThreadWithRequestedAccess) {
    constexpr GuestAddr dup_out = DATA_BASE + 0x120;
    constexpr u32 CURRENT_PROCESS = 0xFFFFFFFFu;
    constexpr u32 CURRENT_THREAD = 0xFFFFFFFEu;
    const GuestAddr stack = cpu.sp();
    for (const u32 access : {0x001FFFFFu /*THREAD_ALL_ACCESS*/, 0x00100000u /*SYNCHRONIZE*/}) {
        ASSERT_TRUE(memory.write_u32(dup_out, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 4, CURRENT_PROCESS).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, CURRENT_THREAD).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 12, CURRENT_PROCESS).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 16, dup_out).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 20, access).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 28, 0).is_ok());  // no SAME_ACCESS
        mgr.impl_DuplicateHandle(marshaler);
        ASSERT_EQ(cpu.gpr.rax, 1u) << "access 0x" << std::hex << access;
        const Handle dup = memory.read_u32(dup_out).value_or(NULL_HANDLE);
        ASSERT_NE(dup, NULL_HANDLE);
        EXPECT_NE(dup, CURRENT_THREAD);
        EXPECT_EQ(handles.get_type(dup), HandleType::Thread);
        EXPECT_EQ(handles.get_access(dup), access);
        EXPECT_TRUE(handles.close(dup));
    }
}

// Learn: hTargetProcessHandle may be NULL with DUPLICATE_CLOSE_SOURCE, which
// closes the source and returns TRUE (handles_waits_probe).
TEST_F(Kernel32CpuzTest, DuplicateHandleNullTargetClosesSource) {
    constexpr u32 CURRENT_PROCESS = 0xFFFFFFFFu;
    constexpr u32 DUPLICATE_CLOSE_SOURCE = 1;
    const GuestAddr stack = cpu.sp();
    const Handle ev = sync.create_event(true, false, "");
    ASSERT_NE(ev, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(stack + 4, CURRENT_PROCESS).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, ev).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 28, DUPLICATE_CLOSE_SOURCE).is_ok());
    mgr.impl_DuplicateHandle(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_FALSE(handles.is_valid(ev));
}

// Learn / handles_waits_probe: an alertable wait with a zero timeout still
// tests the object -- a signaled event is WAIT_OBJECT_0, a semaphore is taken.
TEST_F(Kernel32CpuzTest, AlertableZeroTimeoutWaitTestsTheObject) {
    const GuestAddr stack = cpu.sp();
    const Handle signaled = sync.create_event(true, true, "");
    const Handle sem = sync.create_semaphore(1, 1, "");
    ASSERT_NE(signaled, NULL_HANDLE);
    ASSERT_NE(sem, NULL_HANDLE);
    for (const Handle h : {signaled, sem}) {
        ASSERT_TRUE(memory.write_u32(stack + 4, h).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());   // dwMilliseconds
        ASSERT_TRUE(memory.write_u32(stack + 12, 1).is_ok());  // bAlertable
        mgr.impl_WaitForSingleObjectEx(marshaler);
        EXPECT_EQ(cpu.gpr.rax, WAIT_OBJECT_0);
    }
    // The semaphore's single count was consumed by that wait.
    EXPECT_EQ(sync.wait_for_single_object(sem, 0), WAIT_TIMEOUT);
}

TEST_F(Kernel32CpuzTest,
       SameProcessDuplicateHandleEventAliasWakesRegisterWait) {
    // Chromium WaitableEventWatcher: Dup with SYNCHRONIZE|EVENT_MODIFY_STATE
    // and options=0, then RegisterWait on the duplicate; Signal sets the
    // original. Both handles must alias one Event object.
    constexpr GuestAddr wait_out = DATA_BASE + 0x100;
    constexpr GuestAddr dup_out = DATA_BASE + 0x110;
    constexpr GuestAddr callback = 0x00401000;
    constexpr u32 execute_only_once = 0x00000008;
    constexpr u32 kSynchronizeAndModifyState = 0x00100002;
    constexpr u32 CURRENT_PROCESS = 0xFFFFFFFFu;
    const GuestAddr stack = cpu.sp();

    std::atomic<int> callback_launches{0};
    threads.set_run_callback(
        [&](u32 tid, GuestAddr, GuestAddr, u64, u64, usize) {
            callback_launches.fetch_add(1, std::memory_order_release);
            threads.exit_thread(tid, 0);
        });

    const Handle original = sync.create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    ASSERT_NE(original, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(stack + 4, CURRENT_PROCESS).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, original).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, CURRENT_PROCESS).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, dup_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, kSynchronizeAndModifyState).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 28, 0).is_ok()); // options: not SAME_ACCESS
    mgr.impl_DuplicateHandle(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle duplicate =
        memory.read_u32(dup_out).value_or(NULL_HANDLE);
    ASSERT_NE(duplicate, NULL_HANDLE);
    ASSERT_NE(duplicate, original);
    EXPECT_EQ(handles.get_type(duplicate), HandleType::Event);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, duplicate).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, callback).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, INFINITE_WAIT).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, execute_only_once).is_ok());
    mgr.impl_RegisterWaitForSingleObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle wait_handle =
        memory.read_u32(wait_out).value_or(NULL_HANDLE);
    ASSERT_NE(wait_handle, NULL_HANDLE);

    ASSERT_TRUE(sync.set_event(original));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (callback_launches.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(callback_launches.load(std::memory_order_acquire), 1);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_UnregisterWaitEx(marshaler);
    threads.join_host_threads();
}

TEST_F(Kernel32CpuzTest,
       RegisterWaitSurvivesDuplicateHandlePromoteThenSetEvent) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC";

    constexpr GuestAddr wait_out = DATA_BASE + 0x100;
    constexpr GuestAddr callback = 0x00401000;
    constexpr u32 execute_only_once = 0x00000008;
    constexpr u32 kSynchronizeAndModifyState = 0x00100002;
    const GuestAddr stack = cpu.sp();

    std::atomic<int> callback_launches{0};
    threads.set_run_callback(
        [&](u32 tid, GuestAddr, GuestAddr, u64, u64, usize) {
            callback_launches.fetch_add(1, std::memory_order_release);
            threads.exit_thread(tid, 0);
        });

    const Handle observed = sync.create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    ASSERT_NE(observed, NULL_HANDLE);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, observed).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, callback).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, INFINITE_WAIT).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, execute_only_once).is_ok());
    mgr.impl_RegisterWaitForSingleObject(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const Handle wait_handle =
        memory.read_u32(wait_out).value_or(NULL_HANDLE);
    ASSERT_NE(wait_handle, NULL_HANDLE);

    // Let the host waiter park on the local ntsync object first.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto descriptor = sync.prepare_transferred_handle(
        observed, kSynchronizeAndModifyState, /*inheritable=*/false);
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(sync.is_waitable_sync_handle(observed));
    ASSERT_TRUE(sync.set_event(observed));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (callback_launches.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(callback_launches.load(std::memory_order_acquire), 1);

    ASSERT_TRUE(memory.write_u32(stack + 4, wait_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_UnregisterWaitEx(marshaler);
    threads.join_host_threads();
}

// =============================================================================
// Interlocked / Memory
// =============================================================================

TEST_F(Kernel32CpuzTest, InterlockedExchangeAdd_ReturnsOldWritesSum) {
    constexpr GuestAddr target = DATA_BASE + 0x900;
    (void)memory.write_u32(target, 100);  // initial value

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), target);  // pAddend
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 50);     // Value
    mgr.impl_InterlockedExchangeAdd(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 100u);  // old value
    auto r = memory.read_u32(target);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 150u);  // 100 + 50
}

// =============================================================================
// Sync / Mutex / Semaphore
// =============================================================================

TEST_F(Kernel32CpuzTest, CreateSemaphoreW_ReturnsNonZero) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  0);  // lpAttributes
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  1);  // lInitialCount
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 10); // lMaximumCount
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 0);  // lpName
    mgr.impl_CreateSemaphoreW(marshaler);
    EXPECT_NE(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, ReleaseSemaphore_ReturnsTRUE) {
    const Handle semaphore = sync.create_semaphore(0, 10, "");
    ASSERT_NE(semaphore, NULL_HANDLE);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), semaphore);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  1);     // lReleaseCount
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0);     // lpPreviousCount
    mgr.impl_ReleaseSemaphore(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(Kernel32CpuzTest, QueueUserAPC_QueuesOnCurrentThread) {
    ThreadManager::set_current_thread_id(1);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  0x1000);  // pfnAPC
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),
                           ThreadManager::get_current_thread());  // hThread
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0x42);     // dwData
    mgr.impl_QueueUserAPC(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_TRUE(threads.has_pending_user_apcs(1));
}

TEST_F(Kernel32CpuzTest, QueueUserAPC_InvalidHandleFails) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  0x1000);  // pfnAPC
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  0xBB);    // hThread
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0);       // dwData
    mgr.impl_QueueUserAPC(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(Kernel32CpuzTest, WaitForSingleObjectEx_WithoutSyncManagerReturnsWaitFailed) {
    Kernel32ExtManager isolated{memory, vfs};
    isolated.set_handle_table(&handles);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x1234);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0);
    isolated.impl_WaitForSingleObjectEx(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), WAIT_FAILED);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(Kernel32CpuzTest, WaitForSingleObjectEx_NonsignaledAutoResetTimesOut) {
    Handle event = sync.create_event(false, false, "");
    ASSERT_NE(event, 0u);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), event);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0);
    mgr.impl_WaitForSingleObjectEx(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), WAIT_TIMEOUT);
}

TEST_F(Kernel32CpuzTest, WaitForSingleObjectEx_AlertableDeliversApc) {
    ThreadManager::set_current_thread_id(1);
    Handle event = sync.create_event(false, false, "");
    ASSERT_NE(event, 0u);

    std::atomic<u64> seen_data{0};
    mgr.set_guest_call([&](GuestAddr pfn, const u64* args, usize count,
                           bool /*is_64bit*/) -> u64 {
        EXPECT_EQ(pfn, 0x1000u);
        EXPECT_EQ(count, 1u);
        seen_data.store(args[0], std::memory_order_relaxed);
        return 0;
    });

    ASSERT_TRUE(threads.queue_user_apc(ThreadManager::get_current_thread(),
                                       0x1000, 0xABCDEF01ull));

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), event);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 50);  // ms
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 1);  // alertable
    mgr.impl_WaitForSingleObjectEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, WAIT_IO_COMPLETION);
    EXPECT_EQ(seen_data.load(std::memory_order_relaxed), 0xABCDEF01ull);
    EXPECT_FALSE(threads.has_pending_user_apcs(1));
}

TEST_F(Kernel32CpuzTest, WaitForSingleObjectEx_AlertableDeliversIoCompletion) {
    ThreadManager::set_current_thread_id(1);
    Handle event = sync.create_event(false, false, "");
    ASSERT_NE(event, 0u);

    std::atomic<u64> seen_error{0xFFFFFFFFull};
    std::atomic<u64> seen_bytes{0};
    std::atomic<u64> seen_ov{0};
    mgr.set_guest_call([&](GuestAddr pfn, const u64* args, usize count,
                           bool /*is_64bit*/) -> u64 {
        EXPECT_EQ(pfn, 0x2000u);
        EXPECT_EQ(count, 3u);
        seen_error.store(args[0], std::memory_order_relaxed);
        seen_bytes.store(args[1], std::memory_order_relaxed);
        seen_ov.store(args[2], std::memory_order_relaxed);
        return 0;
    });

    ASSERT_TRUE(threads.queue_io_completion_apc(0x2000, 0, 16, 0x3000));

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), event);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 50);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 1);
    mgr.impl_WaitForSingleObjectEx(marshaler);
    EXPECT_EQ(cpu.gpr.rax, WAIT_IO_COMPLETION);
    EXPECT_EQ(seen_error.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(seen_bytes.load(std::memory_order_relaxed), 16u);
    EXPECT_EQ(seen_ov.load(std::memory_order_relaxed), 0x3000u);
    EXPECT_FALSE(threads.has_pending_io_completion_apcs(1));
}

TEST_F(Kernel32CpuzTest, GetQueuedCompletionStatusEx_AlertableDeliversApc) {
    // Learn: fAlertable TRUE + pending QueueUserAPC → FALSE /
    // WAIT_IO_COMPLETION without dequeuing packets.
    ThreadManager::set_current_thread_id(1);
    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr entries = DATA_BASE + 0x300;
    constexpr GuestAddr num_out = DATA_BASE + 0x400;

    ASSERT_TRUE(memory.write_u32(stack + 4, INVALID_HANDLE).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    mgr.impl_CreateIoCompletionPort(marshaler);
    const Handle port = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(port, NULL_HANDLE);

    std::atomic<u64> seen_data{0};
    mgr.set_guest_call([&](GuestAddr pfn, const u64* args, usize count,
                           bool /*is_64bit*/) -> u64 {
        EXPECT_EQ(pfn, 0x1000u);
        EXPECT_EQ(count, 1u);
        seen_data.store(args[0], std::memory_order_relaxed);
        return 0;
    });
    ASSERT_TRUE(threads.queue_user_apc(ThreadManager::get_current_thread(),
                                       0x1000, 0xABCDEF02ull));

    ASSERT_TRUE(memory.write_u32(stack + 4, port).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, entries).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 1).is_ok());       // ulCount
    ASSERT_TRUE(memory.write_u32(stack + 16, num_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 50).is_ok());      // timeout
    ASSERT_TRUE(memory.write_u32(stack + 24, 1).is_ok());       // fAlertable
    mgr.impl_GetQueuedCompletionStatusEx(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), WAIT_IO_COMPLETION);
    EXPECT_EQ(memory.read_u32(num_out).value_or(1), 0u);
    EXPECT_EQ(seen_data.load(std::memory_order_relaxed), 0xABCDEF02ull);
    EXPECT_FALSE(threads.has_pending_user_apcs(1));
}
