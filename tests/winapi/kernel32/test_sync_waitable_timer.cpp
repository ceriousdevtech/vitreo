/// @file test_sync_waitable_timer.cpp
/// @brief Waitable-timer tests: SetWaitableTimer APC delivery, periods, cancellation and handle-type rejection (THREAD-002)

#include "sync_test_fixture.hpp"

// Learn: SetEvent/ResetEvent on a waitable-timer handle → ERROR_INVALID_HANDLE.
TEST_F(SyncTest, SetEventOnWaitableTimerFailsInvalidHandle) {
    Handle tmr = m_mgr->create_event(true, false, "", false, {}, true);
    ASSERT_NE(tmr, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->set_event(tmr));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->reset_event(tmr));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_TRUE(m_mgr->close_handle(tmr));
}

TEST_F(SyncTest, SetWaitableTimerQueuesApcOnIssuerAndHonorsPeriod) {
    // Learn SetWaitableTimer: due time signals the timer and queues
    // PTIMERAPCROUTINE on the setting thread; lPeriod > 0 repeats until cancel.
    ThreadManager threads{*m_handles};
    m_mgr->set_thread_manager(&threads);
    ThreadManager::set_current_thread_id(1);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(kStack + 0x404, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
    impl_CreateWaitableTimerW(marshaler, *m_mgr);
    const Handle timer = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(timer, 0u);

    constexpr GuestAddr due_addr = kData;
    constexpr GuestAddr pfn = 0x5000;
    constexpr GuestAddr arg = 0x1234;
    // Relative 1ms, period 15ms.
    ASSERT_TRUE(memory.write_u64(due_addr, static_cast<u64>(-10000)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, timer).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, static_cast<u32>(due_addr)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 15).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x410, static_cast<u32>(pfn)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x414, static_cast<u32>(arg)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x418, 0).is_ok());
    impl_SetWaitableTimer(marshaler, *m_mgr);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    EXPECT_EQ(m_mgr->wait_for_single_object(timer, 200), WAIT_OBJECT_0);
    EXPECT_TRUE(threads.has_pending_io_completion_apcs(1));

    GuestAddr seen_pfn = 0;
    u64 seen_arg = 0;
    EXPECT_EQ(threads.drain_io_completion_apcs(
                  1,
                  [&](GuestAddr queued_pfn, u64 a0, u64, u64, u64, u8) {
                      seen_pfn = queued_pfn;
                      seen_arg = a0;
                  }),
              1u);
    EXPECT_EQ(seen_pfn, pfn);
    EXPECT_EQ(seen_arg, arg);

    EXPECT_EQ(m_mgr->wait_for_single_object(timer, 200), WAIT_OBJECT_0);

    impl_CancelWaitableTimer(marshaler, *m_mgr);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    // A period that expired between the last wait and the cancel stays
    // signaled (Cancel does not change the state); consume it, then no
    // further period may fire.
    (void)m_mgr->wait_for_single_object(timer, 0);
    EXPECT_EQ(m_mgr->wait_for_single_object(timer, 40), WAIT_TIMEOUT);
    m_mgr->set_thread_manager(nullptr);
}

// s4_timer_apc_probe (Windows 11, x86 and x64): the PTIMERAPCROUTINE is
// already queued whenever a wait on the timer returns, and the issuer's
// alertable wait on the timer still returns WAIT_OBJECT_0. So the fire path
// queues the APC, signals, and only then wakes alertable waits: at the wake
// the timer must already be signaled, and the APC must already be queued the
// moment a waiter sees the signal. Repeated so the old signal-then-queue
// window (1 in 8 runs) cannot pass by luck.
TEST_F(SyncTest, WaitableTimerApcIsQueuedBeforeSignalAndWakeFollowsSignal) {
    ThreadManager threads{*m_handles};
    m_mgr->set_thread_manager(&threads);
    ThreadManager::set_current_thread_id(1);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // CreateWaitableTimerW(NULL, bManualReset=TRUE, NULL): a zero-timeout
    // wait from the wake hook must not consume the signal.
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
    impl_CreateWaitableTimerW(marshaler, *m_mgr);
    const Handle timer = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(timer, 0u);

    // Fired synchronously inside the timer's signal (ntsync host wake armed
    // on this one object): the APC must already be queued at that instant.
    // Signal-then-queue fails this on every round (negative control).
    std::atomic<int> signals{0};
    std::atomic<int> signals_without_apc{0};
    const auto main_id = std::this_thread::get_id();
    std::atomic<int> wakes{0};
    const ntsync::ObjectId timer_id = m_mgr->resolve_nt_id(timer);
    ASSERT_NE(timer_id, ntsync::kInvalidId);
    ntsync::global().set_host_wake([&] {
        // The host wake also fires on process-wide notify_waiters() and on
        // this thread's own waits; only the timer thread's signal counts.
        if (std::this_thread::get_id() == main_id) return;
        if (!threads.has_pending_io_completion_apcs(1)) {
            signals_without_apc.fetch_add(1);
        }
        signals.fetch_add(1);
    });
    constexpr u32 kHostArmTid = 0x7FFF0001u;
    ntsync::global().arm_host_wake(kHostArmTid, &timer_id, 1);

    std::atomic<int> wakes_before_signal{0};
    threads.set_apc_wake([&](u32 tid) {
        if (tid != 1) return;
        // Check before counting: the test re-arms (and so resets) the timer
        // as soon as it sees the count move.
        if (m_mgr->wait_for_single_object(timer, 0) != WAIT_OBJECT_0) {
            wakes_before_signal.fetch_add(1);
        }
        wakes.fetch_add(1);
    });

    constexpr int kRounds = 40;
    for (int round = 0; round < kRounds; ++round) {
        ASSERT_TRUE(memory.write_u64(kData, static_cast<u64>(-10000)).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x404, timer).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x408, static_cast<u32>(kData)).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x410, 0x5000).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x414, 0x1234).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x418, 0).is_ok());
        impl_SetWaitableTimer(marshaler, *m_mgr);
        ASSERT_EQ(cpu.gpr.rax, 1u);
        ASSERT_EQ(m_mgr->wait_for_single_object(timer, 500), WAIT_OBJECT_0);
        EXPECT_TRUE(threads.has_pending_io_completion_apcs(1)) << "round " << round;
        // The wake follows the signal on the timer thread; let it land.
        for (int spin = 0; spin < 200 && wakes.load() <= round; ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(threads.drain_io_completion_apcs(
                      1, [](GuestAddr, u64, u64, u64, u64, u8) {}),
                  1u);
    }
    ntsync::global().disarm_host_wake(kHostArmTid);
    ntsync::global().set_host_wake(nullptr);
    EXPECT_GE(signals.load(), kRounds);
    EXPECT_EQ(signals_without_apc.load(), 0);
    EXPECT_EQ(wakes.load(), kRounds);
    EXPECT_EQ(wakes_before_signal.load(), 0);
    threads.set_apc_wake(nullptr);
    m_mgr->set_thread_manager(nullptr);
}

// Learn SetWaitableTimer: a one-shot synchronization timer signals once at
// its due time and never before it. Learn CancelWaitableTimer: the call does
// not change the signaled state of the timer.
TEST_F(SyncTest, OneShotWaitableTimerSignalsAtDueTimeAndCancelKeepsState) {
    ThreadManager::set_current_thread_id(1);
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // CreateWaitableTimerW(NULL, bManualReset=FALSE, NULL)
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
    impl_CreateWaitableTimerW(marshaler, *m_mgr);
    const Handle timer = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(timer, 0u);

    const auto set_timer = [&](i64 due_100ns) {
        ASSERT_TRUE(memory.write_u64(kData, static_cast<u64>(due_100ns)).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x404, timer).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x408, kData).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x410, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x414, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(kStack + 0x418, 0).is_ok());
        impl_SetWaitableTimer(marshaler, *m_mgr);
        EXPECT_EQ(cpu.gpr.rax, 1u);
    };

    // Relative 19.5 ms, period 0, no completion routine.
    const auto armed = std::chrono::steady_clock::now();
    set_timer(-195000);
    EXPECT_EQ(m_mgr->wait_for_single_object(timer, 1000), WAIT_OBJECT_0);
    EXPECT_GE(std::chrono::steady_clock::now() - armed,
              std::chrono::microseconds(19500));
    // Synchronization timer: the satisfied wait reset it; no period re-arms.
    EXPECT_EQ(m_mgr->wait_for_single_object(timer, 40), WAIT_TIMEOUT);

    // Expire again, then cancel: the signaled state survives the cancel.
    set_timer(-10000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, timer).is_ok());
    impl_CancelWaitableTimer(marshaler, *m_mgr);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(m_mgr->wait_for_single_object(timer, 0), WAIT_OBJECT_0);
    EXPECT_TRUE(m_mgr->close_handle(timer));
}

// SetWaitableTimer/CancelWaitableTimer on an event handle is an object-type
// mismatch: FALSE with ERROR_INVALID_HANDLE, and the event is untouched.
TEST_F(SyncTest, WaitableTimerApisRejectEventHandles) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const Handle event = m_mgr->create_event(true, true, "", false);
    ASSERT_NE(event, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u64(kData, static_cast<u64>(-10000)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, event).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, kData).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x410, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x414, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x418, 0).is_ok());
    set_last_error(0xDEAD);
    impl_SetWaitableTimer(marshaler, *m_mgr);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    set_last_error(0xDEAD);
    impl_CancelWaitableTimer(marshaler, *m_mgr);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    EXPECT_EQ(m_mgr->wait_for_single_object(event, 0), WAIT_OBJECT_0);
    EXPECT_TRUE(m_mgr->close_handle(event));
}

TEST_F(SyncTest, SetWaitableTimerRejectsNegativePeriod) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(kStack + 0x404, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, 0).is_ok());
    impl_CreateWaitableTimerW(marshaler, *m_mgr);
    const Handle timer = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(timer, 0u);
    ASSERT_TRUE(memory.write_u64(kData, static_cast<u64>(-10000)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, timer).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x408, static_cast<u32>(kData)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x40C, static_cast<u32>(-1)).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x410, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x414, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kStack + 0x418, 0).is_ok());
    set_last_error(error_codes::ERROR_SUCCESS);
    impl_SetWaitableTimer(marshaler, *m_mgr);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}
