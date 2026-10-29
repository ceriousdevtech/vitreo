/// @file test_sync_thunks.cpp
/// @brief Sync export thunks, Interlocked edge cases, event-count queue and FlushProcessWriteBuffers (THREAD-002)

#include "sync_test_fixture.hpp"

TEST_F(SyncTest, CreateSyncThunksOwnsSynchronizationButNotCriticalSections) {
    DllExports exports = create_sync_thunks(*m_mgr);
    // PulseEvent must be a real export, not a missing-export stub: the generic
    // stub pops 0 stdcall bytes, so its 4 argument bytes would be left on the
    // guest stack and the caller would return through a corrupted frame.
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(),
                           [](const auto& entry) {
                               return entry.name == "PulseEvent";
                           }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "CreateMutexA";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "OpenEventA";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "OpenEventW";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "OpenSemaphoreW";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "OpenSemaphoreA";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "WaitForMultipleObjectsEx";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "WaitOnAddress";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "WakeByAddressSingle";
    }), exports.entries.end());
    EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "WakeByAddressAll";
    }), exports.entries.end());
    EXPECT_EQ(std::find_if(exports.entries.begin(), exports.entries.end(), [](const auto& entry) {
        return entry.name == "InitializeCriticalSection";
    }), exports.entries.end());
}

TEST_F(SyncTest, InterlockedDecrementFromZeroReturnsMinusOne) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kData, 0).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_sync_thunks(*m_mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "InterlockedDecrement"; });
    ASSERT_NE(it, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, static_cast<u32>(kData)).is_ok());
    it->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(kData).value_or(0), 0xFFFFFFFFu);
}

TEST_F(SyncTest, InterlockedIncrementFromMinusOneReturnsZero) {
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kData, 0xFFFFFFFFu).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_sync_thunks(*m_mgr);
    auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "InterlockedIncrement"; });
    ASSERT_NE(it, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(kStack + 0x404, static_cast<u32>(kData)).is_ok());
    it->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(kData).value_or(1), 0u);
}

TEST_F(SyncTest, EventCountQueueWakesAfterIncrementAndSet) {
    // Decrement-then-WFSO-if-negative, Increment+SetEvent on the other side.
    const Handle event = m_mgr->create_event(
        /*manual_reset=*/false, /*initial_state=*/false, "");
    ASSERT_NE(event, NULL_HANDLE);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kData, 0).is_ok());

    auto exports = create_sync_thunks(*m_mgr);
    auto find = [&](const char* name) -> ExportEntry* {
        auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
        return it == exports.entries.end() ? nullptr : &*it;
    };
    auto* dec = find("InterlockedDecrement");
    auto* inc = find("InterlockedIncrement");
    ASSERT_NE(dec, nullptr);
    ASSERT_NE(inc, nullptr);

    std::atomic<bool> started{false};
    std::atomic<u32> wait_result{WAIT_FAILED};
    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(2);
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.rsp = kStack + 0x400;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(memory.write_u32(kStack + 0x404, static_cast<u32>(kData)).is_ok());
        dec->handler(marshaler);
        const auto neu = static_cast<i32>(cpu.gpr.rax);
        started.store(true, std::memory_order_release);
        if (neu < 0) {
            wait_result.store(m_mgr->wait_for_single_object(event, INFINITE_WAIT),
                              std::memory_order_release);
        } else {
            wait_result.store(WAIT_OBJECT_0, std::memory_order_release);
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    cpu::CpuState prod;
    prod.init_protected_mode();
    prod.gpr.rsp = kStack + 0x200;
    Marshaler prod_m(memory);
    prod_m.attach_cpu(prod);
    ASSERT_TRUE(memory.write_u32(kStack + 0x204, static_cast<u32>(kData)).is_ok());
    inc->handler(prod_m);
    EXPECT_EQ(static_cast<u32>(prod.gpr.rax), 0u);
    EXPECT_TRUE(m_mgr->set_event(event));
    waiter.join();
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), WAIT_OBJECT_0);
}

// Audit 01 FlushProcessWriteBuffers #1. Learn: the call "flushes the write
// queue of each processor that is running a thread of the current process".
// Every other thread of the task must be driven through the kernel's
// serialising stop, including threads spinning without any barrier.
TEST(FlushProcessWriteBuffersTest, SerialisesEveryOtherThreadOfTheProcess) {
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    std::vector<std::thread> spinners;
    for (int i = 0; i < 3; ++i) {
        spinners.emplace_back([&] {
            started.fetch_add(1);
            while (!stop.load(std::memory_order_relaxed)) {}
        });
    }
    while (started.load() != 3) {}
    EXPECT_GE(flush_process_write_buffers(), 3u);
    stop.store(true);
    for (auto& t : spinners) t.join();
}

// Asymmetric store-buffering: the fast side has no barrier, the slow side
// calls FlushProcessWriteBuffers. Forbidden outcome: both loads read 0.
// (A caller-local fence alone produced this outcome 59 times in 2M rounds on
// Apple Silicon; a bounded smoke run here.)
TEST(FlushProcessWriteBuffersTest, AsymmetricStoreBufferingOutcomeForbidden) {
    std::atomic<int> x{0}, y{0};
    std::atomic<long> epoch{-1}, done{-1};
    int fast_read = 0;
    constexpr long kRounds = 300;
    std::thread fast([&] {
        for (long i = 0; i < kRounds; ++i) {
            while (epoch.load(std::memory_order_acquire) != i) {}
            x.store(1, std::memory_order_relaxed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            fast_read = y.load(std::memory_order_relaxed);
            done.store(i, std::memory_order_release);
        }
    });
    long violations = 0;
    for (long i = 0; i < kRounds; ++i) {
        x.store(0, std::memory_order_relaxed);
        y.store(0, std::memory_order_relaxed);
        epoch.store(i, std::memory_order_seq_cst);
        y.store(1, std::memory_order_relaxed);
        (void)flush_process_write_buffers();
        const int slow_read = x.load(std::memory_order_relaxed);
        while (done.load(std::memory_order_acquire) != i) {}
        if (fast_read == 0 && slow_read == 0) ++violations;
    }
    fast.join();
    EXPECT_EQ(violations, 0);
}
