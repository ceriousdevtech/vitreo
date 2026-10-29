/// @file test_sync_wait_on_address_sleep.cpp
/// @brief WaitOnAddress wake semantics and Sleep behaviour (THREAD-002)

#include "sync_test_fixture.hpp"

// =============================================================================
// Sleep Tests
// =============================================================================

TEST(WaitOnAddressTest, WakeByAddressSingleUnblocksMatchingWaiter) {
    constexpr GuestAddr kAddr = 0x4000;
    constexpr GuestAddr kCompare = 0x4010;
    constexpr GuestAddr kWaitStack = 0x5000;
    constexpr GuestAddr kWakeStack = 0x6000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kAddr, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(
        memory.map_region(kWaitStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(
        memory.map_region(kWakeStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kAddr, 7).is_ok());
    ASSERT_TRUE(memory.write_u32(kCompare, 7).is_ok());

    std::atomic<u32> result{0};
    std::thread waiter([&]() {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.rsp = kWaitStack + 0x800;
        Marshaler marshaler{memory};
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x804, kAddr).is_ok());
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x808, kCompare).is_ok());
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x80c, 4).is_ok());
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x810, 1000).is_ok());
        impl_WaitOnAddress(marshaler);
        result.store(static_cast<u32>(cpu.gpr.rax), std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(memory.write_u32(kAddr, 8).is_ok());
    cpu::CpuState wake_cpu;
    wake_cpu.init_protected_mode();
    wake_cpu.gpr.rsp = kWakeStack + 0x800;
    Marshaler wake_marshaler{memory};
    wake_marshaler.attach_cpu(wake_cpu);
    ASSERT_TRUE(memory.write_u32(kWakeStack + 0x804, kAddr).is_ok());
    impl_WakeByAddressSingle(wake_marshaler);
    waiter.join();
    EXPECT_EQ(result.load(std::memory_order_acquire), 1u);
}

// Learn: WaitOnAddress may return for reasons other than a visible value
// change (spurious wake / not-yet-visible store). Callers recheck. Vitreo
// must not re-block forever when WakeByAddress* fired but the word still
// compares equal.
TEST(WaitOnAddressTest, WakeWithoutVisibleValueChangeStillReturns) {
    constexpr GuestAddr kAddr = 0x7000;
    constexpr GuestAddr kCompare = 0x7010;
    constexpr GuestAddr kWaitStack = 0x8000;
    constexpr GuestAddr kWakeStack = 0x9000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kAddr, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(
        memory.map_region(kWaitStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(
        memory.map_region(kWakeStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kAddr, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(kCompare, 3).is_ok());

    std::atomic<u32> result{0};
    std::atomic<bool> started{false};
    std::thread waiter([&]() {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.rsp = kWaitStack + 0x800;
        Marshaler marshaler{memory};
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x804, kAddr).is_ok());
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x808, kCompare).is_ok());
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x80c, 4).is_ok());
        // INFINITE — without the wake-return fix this hangs the test.
        ASSERT_TRUE(memory.write_u32(kWaitStack + 0x810, 0xFFFFFFFFu).is_ok());
        started.store(true, std::memory_order_release);
        impl_WaitOnAddress(marshaler);
        result.store(static_cast<u32>(cpu.gpr.rax), std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Deliberately do NOT change *Address — only wake. Retry briefly so the
    // wake is not lost if it races ahead of cv.wait entry.
    cpu::CpuState wake_cpu;
    wake_cpu.init_protected_mode();
    wake_cpu.gpr.rsp = kWakeStack + 0x800;
    Marshaler wake_marshaler{memory};
    wake_marshaler.attach_cpu(wake_cpu);
    ASSERT_TRUE(memory.write_u32(kWakeStack + 0x804, kAddr).is_ok());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (result.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        impl_WakeByAddressSingle(wake_marshaler);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    waiter.join();
    EXPECT_EQ(result.load(std::memory_order_acquire), 1u);
}

// Learn WakeByAddressSingle: "If another thread has previously called
// WaitOnAddress for this address, the system wakes the waiting thread."
// Vitreo parks on the 8-byte word containing the address; with a second
// waiter on a NEIGHBOURING address in that word, a kernel wake-one could
// pick the neighbour and leave the addressed waiter parked forever, because
// its kernel compare had already passed. The neighbour parks first so a
// FIFO wake-one picks it.
TEST(WaitOnAddressTest, WakeSingleReachesTheAddressedWaiterNotItsNeighbour) {
    constexpr GuestAddr kWord = 0xA000;        // 8-aligned
    constexpr GuestAddr kAddrA = kWord;        // addressed waiter
    constexpr GuestAddr kAddrB = kWord + 4;    // neighbour in the same word
    constexpr GuestAddr kCompare = 0xA100;
    constexpr GuestAddr kStacks = 0xB000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kWord, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStacks, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kAddrA, 5).is_ok());
    ASSERT_TRUE(memory.write_u32(kAddrB, 5).is_ok());
    ASSERT_TRUE(memory.write_u32(kCompare, 5).is_ok());

    auto wait_on = [&](GuestAddr stack, GuestAddr addr, u32 ms,
                       std::atomic<int>& result) {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.rsp = stack + 0x800;
        Marshaler marshaler{memory};
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(memory.write_u32(stack + 0x804, addr).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 0x808, kCompare).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 0x80c, 4).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 0x810, ms).is_ok());
        impl_WaitOnAddress(marshaler);
        result.store(static_cast<int>(cpu.gpr.rax), std::memory_order_release);
    };

    std::atomic<int> result_b{-1};
    std::atomic<int> result_a{-1};
    std::thread neighbour([&] { wait_on(kStacks, kAddrB, 1500, result_b); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::thread addressed([&] { wait_on(kStacks + 0x1000, kAddrA, 1500, result_a); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    ASSERT_TRUE(memory.write_u32(kAddrA, 6).is_ok());
    cpu::CpuState wake_cpu;
    wake_cpu.init_protected_mode();
    wake_cpu.gpr.rsp = kStacks + 0x2800;
    Marshaler wake_marshaler{memory};
    wake_marshaler.attach_cpu(wake_cpu);
    ASSERT_TRUE(memory.write_u32(kStacks + 0x2804, kAddrA).is_ok());
    const auto t0 = std::chrono::steady_clock::now();
    impl_WakeByAddressSingle(wake_marshaler);

    while (result_a.load(std::memory_order_acquire) < 0 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    addressed.join();
    EXPECT_EQ(result_a.load(), 1) << "the addressed waiter timed out";
    EXPECT_LT(ms, 500) << "the wake went to the neighbour";
    neighbour.join();   // it may return spuriously or time out; both are legal
}

TEST_F(SyncTest, SleepDoesNotCrash) {
    m_mgr->sleep(1);
    // If we get here, it didn't crash
}

TEST_F(SyncTest, SleepZeroYields) {
    m_mgr->sleep(0);
    // Should return immediately (yield)
}

// Learn Sleep: INFINITE "indicates that the suspension should not time out."
// Casting 0xFFFFFFFF to chrono::milliseconds returns after ~49.7 days; park
// until begin_shutdown instead (02_threads.md Sleep LOW).
TEST_F(SyncTest, SleepInfiniteParksUntilShutdown) {
    std::atomic<bool> returned{false};
    std::thread sleeper([&] {
        m_mgr->sleep(INFINITE_WAIT);
        returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(returned.load(std::memory_order_acquire));
    m_mgr->begin_shutdown();
    sleeper.join();
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST_F(SyncTest, SleepCompletesIntervalDespiteSigchld) {
    // Learn Sleep: the thread stays unrunnable until the interval elapses.
    // CreateProcess children deliver SIGCHLD; a host nanosleep that returns
    // on EINTR must not cut the Windows wait short.
    std::atomic<bool> stop{false};
    std::thread storm{[&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const pid_t pid = ::fork();
            if (pid == 0) {
                _exit(0);
            }
            if (pid > 0) {
                int status = 0;
                (void)::waitpid(pid, &status, 0);
            }
        }
    }};
    const auto started = std::chrono::steady_clock::now();
    m_mgr->sleep(80);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    stop.store(true, std::memory_order_relaxed);
    storm.join();
    EXPECT_GE(elapsed_ms, 50);
}
