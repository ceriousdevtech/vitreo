/// @file test_sync.cpp
/// @brief Tests for SyncManager critical sections, condition variables, SRW locks and mutexes (THREAD-002)

#include "sync_test_fixture.hpp"

// =============================================================================
// Critical Section Tests
// =============================================================================

TEST_F(SyncTest, InitializeCriticalSectionEnterLeaveSucceeds) {
    constexpr GuestAddr cs_addr = 0x1000;
    m_mgr->initialize_critical_section(cs_addr);
    m_mgr->enter_critical_section(cs_addr);
    m_mgr->leave_critical_section(cs_addr);
    // Should not deadlock or crash
}

TEST_F(SyncTest, TryEnterCriticalSectionSucceedsWhenUnlocked) {
    constexpr GuestAddr cs_addr = 0x2000;
    m_mgr->initialize_critical_section(cs_addr);
    EXPECT_TRUE(m_mgr->try_enter_critical_section(cs_addr));
    m_mgr->leave_critical_section(cs_addr);
}

TEST_F(SyncTest, CriticalSectionRecursiveEnter) {
    constexpr GuestAddr cs_addr = 0x3000;
    m_mgr->initialize_critical_section(cs_addr);
    m_mgr->enter_critical_section(cs_addr);
    // Recursive entry should succeed (backed by recursive_mutex)
    EXPECT_TRUE(m_mgr->try_enter_critical_section(cs_addr));
    m_mgr->leave_critical_section(cs_addr);
    m_mgr->leave_critical_section(cs_addr);
}

TEST_F(SyncTest, DeleteCriticalSectionIsSafe) {
    constexpr GuestAddr cs_addr = 0x4000;
    m_mgr->initialize_critical_section(cs_addr);
    m_mgr->delete_critical_section(cs_addr);
    // Deleting again should be a no-op
    m_mgr->delete_critical_section(cs_addr);
}

TEST_F(SyncTest, SleepConditionVariableCSWaitsForWake) {
    constexpr GuestAddr cs_addr = 0x5000;
    constexpr GuestAddr cv_addr = 0x6000;

    m_mgr->initialize_critical_section(cs_addr);

    std::atomic<bool> sleeping{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(2);
        m_mgr->enter_critical_section(cs_addr);
        sleeping.store(true, std::memory_order_release);
        woke.store(m_mgr->sleep_condition_variable_cs(cv_addr, cs_addr, INFINITE_WAIT),
                   std::memory_order_release);
        m_mgr->leave_critical_section(cs_addr);
    });

    while (!sleeping.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    ThreadManager::set_current_thread_id(1);
    m_mgr->enter_critical_section(cs_addr);
    m_mgr->wake_all_condition_variable(cv_addr);
    m_mgr->leave_critical_section(cs_addr);
    waiter.join();

    EXPECT_TRUE(woke.load(std::memory_order_acquire));
}

TEST_F(SyncTest, WakeConditionVariableWakesWaiter) {
    constexpr GuestAddr cs_addr = 0x6100;
    constexpr GuestAddr cv_addr = 0x6200;

    m_mgr->initialize_critical_section(cs_addr);

    std::atomic<bool> sleeping{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(2);
        m_mgr->enter_critical_section(cs_addr);
        sleeping.store(true, std::memory_order_release);
        woke.store(m_mgr->sleep_condition_variable_cs(cv_addr, cs_addr, INFINITE_WAIT),
                   std::memory_order_release);
        m_mgr->leave_critical_section(cs_addr);
    });

    while (!sleeping.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    ThreadManager::set_current_thread_id(1);
    // Take the CRITICAL_SECTION before waking, exactly as
    // SleepConditionVariableCSWaitsForWake does. `sleeping` is set BEFORE the
    // waiter enters SleepConditionVariableCS, so on its own it only means
    // "about to wait". Waking in that gap is a no-op on Windows
    // (WakeConditionVariable releases nobody when no thread is waiting), and
    // the waiter then samples the already-advanced generation as its own
    // baseline and blocks forever on the INFINITE wait. Acquiring the CS the
    // waiter still holds is what orders the two: it cannot be released until
    // the waiter is inside the call and has sampled the generation.
    m_mgr->enter_critical_section(cs_addr);
    m_mgr->wake_condition_variable(cv_addr);
    m_mgr->leave_critical_section(cs_addr);
    waiter.join();

    EXPECT_TRUE(woke.load(std::memory_order_acquire));
}

TEST_F(SyncTest, SleepConditionVariableCSTimeoutReturnsFalse) {
    constexpr GuestAddr cs_addr = 0x7000;
    constexpr GuestAddr cv_addr = 0x8000;

    m_mgr->initialize_critical_section(cs_addr);
    m_mgr->enter_critical_section(cs_addr);
    EXPECT_FALSE(m_mgr->sleep_condition_variable_cs(cv_addr, cs_addr, 1));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_TIMEOUT);
    m_mgr->leave_critical_section(cs_addr);
}

TEST_F(SyncTest, WaitConditionVariableReleasesCriticalSectionManagerLock) {
    // Mirrors production SleepConditionVariableCS: Enter via CriticalSectionManager,
    // wait via SyncManager without holding that CS, so another thread can enter.
    constexpr GuestAddr cs_addr = 0x10000;
    constexpr GuestAddr cv_addr = 0x11000;

    GuestMemory memory;
    auto cs_region = memory.map_region(cs_addr, 0x1000, MemoryProtection::ReadWrite);
    ASSERT_TRUE(cs_region.is_ok());
    CriticalSectionManager cs_mgr{memory};
    cs_mgr.initialize(cs_addr);

    std::atomic<bool> sleeping{false};
    std::atomic<bool> other_acquired{false};
    std::atomic<bool> woke{false};

    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(2);
        cs_mgr.enter(cs_addr);
        sleeping.store(true, std::memory_order_release);
        woke.store(m_mgr->sleep_condition_variable_cs(
                       cs_mgr, cv_addr, cs_addr, INFINITE_WAIT),
                   std::memory_order_release);
        cs_mgr.leave(cs_addr);
    });

    while (!sleeping.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // A blocking contender removes the scheduling race between the waiter
    // publishing `sleeping` and the condition-variable path releasing the CS.
    std::thread contender([&]() {
        ThreadManager::set_current_thread_id(1);
        cs_mgr.enter(cs_addr);
        other_acquired.store(true, std::memory_order_release);
        cs_mgr.leave(cs_addr);
    });
    contender.join();
    m_mgr->wake_condition_variable(cv_addr);
    waiter.join();

    EXPECT_TRUE(other_acquired.load(std::memory_order_acquire));
    EXPECT_TRUE(woke.load(std::memory_order_acquire));
}

TEST_F(SyncTest, WaitConditionVariableRecognizesAcceleratedTryEnterOwner) {
    constexpr GuestAddr cs_addr = 0x10000;
    constexpr GuestAddr cv_addr = 0x11000;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(0x100000));
    ASSERT_TRUE(memory.map_region(
        cs_addr, 0x1000, MemoryProtection::ReadWrite).is_ok());
    CriticalSectionManager cs_mgr{memory};
    cs_mgr.note_bitness(false);
    cs_mgr.initialize(cs_addr);

    ThreadManager::set_current_thread_id(9);
    ASSERT_TRUE(cs_mgr.try_enter(cs_addr));
    EXPECT_EQ(cs_mgr.owning_thread(cs_addr), 9u);
    EXPECT_EQ(cs_mgr.recursion_count(cs_addr), 1u);

    // A real timeout proves SleepConditionVariableCS accepted ownership,
    // released the section, waited, and reacquired it.  The stale host-state
    // bug returned ERROR_INVALID_PARAMETER immediately at this point.
    EXPECT_FALSE(m_mgr->sleep_condition_variable_cs(
        cs_mgr, cv_addr, cs_addr, 1));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_TIMEOUT);
    EXPECT_EQ(cs_mgr.owning_thread(cs_addr), 9u);
    EXPECT_EQ(cs_mgr.recursion_count(cs_addr), 1u);
    cs_mgr.leave(cs_addr);
}

TEST_F(SyncTest, SrwExclusiveLockSupportsTryAcquire) {
    constexpr GuestAddr lock_addr = 0x9000;
    m_mgr->initialize_srw_lock(lock_addr);

    ThreadManager::set_current_thread_id(1);
    m_mgr->acquire_srw_lock_exclusive(lock_addr);

    std::atomic<bool> acquired{true};
    std::thread contender([&]() {
        ThreadManager::set_current_thread_id(2);
        acquired.store(m_mgr->try_acquire_srw_lock_exclusive(lock_addr),
                       std::memory_order_release);
        if (acquired.load(std::memory_order_acquire)) {
            m_mgr->release_srw_lock_exclusive(lock_addr);
        }
    });
    contender.join();

    EXPECT_FALSE(acquired.load(std::memory_order_acquire));
    m_mgr->release_srw_lock_exclusive(lock_addr);
}

// =============================================================================
// Mutex Tests
// =============================================================================

TEST_F(SyncTest, CreateMutexReturnsValidHandle) {
    Handle h = m_mgr->create_mutex(false, "");
    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(h, NULL_HANDLE);
}

TEST_F(SyncTest, ReopeningNamedMutexReturnsDistinctClosableHandle) {
    Handle first = m_mgr->create_mutex(false, "shared_mutex");
    Handle second = m_mgr->create_mutex(false, "shared_mutex");

    ASSERT_NE(first, second);
    EXPECT_TRUE(m_mgr->close_handle(first));
    EXPECT_TRUE(m_mgr->close_handle(second));
}

TEST_F(SyncTest, RecreatingClosedNamedMutexReturnsValidHandle) {
    Handle first = m_mgr->create_mutex(false, "recreated_mutex");
    ASSERT_TRUE(m_mgr->close_handle(first));

    Handle recreated = m_mgr->create_mutex(false, "recreated_mutex");
    EXPECT_NE(recreated, NULL_HANDLE);
    EXPECT_NE(recreated, first);
    EXPECT_TRUE(m_mgr->close_handle(recreated));
}

TEST_F(SyncTest, WaitForSingleObjectMutexReturnsWaitObject0) {
    Handle h = m_mgr->create_mutex(false, "test_mutex");
    u32 result = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(result, WAIT_OBJECT_0);
}

TEST_F(SyncTest, ReleaseMutexAfterAcquireSucceeds) {
    Handle h = m_mgr->create_mutex(false, "");
    u32 wait_result = m_mgr->wait_for_single_object(h, 0);
    ASSERT_EQ(wait_result, WAIT_OBJECT_0);
    EXPECT_TRUE(m_mgr->release_mutex(h));
}

TEST_F(SyncTest, ReleaseMutexWithoutAcquireFails) {
    Handle h = m_mgr->create_mutex(false, "");
    // Mutex is not locked, releasing should fail
    EXPECT_FALSE(m_mgr->release_mutex(h));
}

TEST_F(SyncTest, CreateMutexInitialOwnerLocksIt) {
    Handle h = m_mgr->create_mutex(true, "owned");
    // Mutex should already be locked — release should succeed
    EXPECT_TRUE(m_mgr->release_mutex(h));
}

TEST_F(SyncTest, CreatedMutexHandleSupportsInfiniteWaitAndRelease) {
    Handle h = m_mgr->create_mutex(false, "");
    EXPECT_EQ(m_mgr->wait_for_single_object(h, INFINITE_WAIT), WAIT_OBJECT_0);
    EXPECT_TRUE(m_mgr->release_mutex(h));
}

// AcquireSRWLockShared used to be implemented by taking the lock EXCLUSIVELY.
// That is semantically sound — exclusive is stronger than shared — and it is
// why no existing test caught it: every functional assertion still held while
// every reader serialised against every other reader. On a live Battle.net UI
// process that was 64% of all non-parked time blocked in
// acquire_srw_lock_exclusive, with the UI freezing for seconds.
//
// So assert the property that was missing: readers must be able to hold the
// lock AT THE SAME TIME. If shared degrades to exclusive again, no thread can
// observe a peer inside the lock and this times out.
TEST_F(SyncTest, SharedSrwLockHoldersRunConcurrently) {
    constexpr GuestAddr lock_addr = 0xA100;
    m_mgr->initialize_srw_lock(lock_addr);

    constexpr int kReaders = 4;
    std::atomic<int> inside{0};
    std::atomic<int> arrived{0};  // monotonic: a barrier must not count down
    std::atomic<int> peak{0};
    std::atomic<bool> timed_out{false};

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&, i]() {
            ThreadManager::set_current_thread_id(static_cast<u32>(10 + i));
            m_mgr->acquire_srw_lock_shared(lock_addr);
            const int now = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
            arrived.fetch_add(1, std::memory_order_acq_rel);
            int seen = peak.load(std::memory_order_relaxed);
            while (now > seen &&
                   !peak.compare_exchange_weak(seen, now,
                                               std::memory_order_relaxed)) {
            }
            // Wait for every reader to be inside — impossible if shared
            // acquisition excludes other readers.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (arrived.load(std::memory_order_acquire) < kReaders) {
                if (std::chrono::steady_clock::now() > deadline) {
                    timed_out.store(true, std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
            }
            inside.fetch_sub(1, std::memory_order_acq_rel);
            (void)m_mgr->release_srw_lock_shared(lock_addr);
        });
    }
    for (auto& t : readers) t.join();

    EXPECT_FALSE(timed_out.load())
        << "shared SRW holders could not be inside the lock simultaneously — "
           "shared acquisition is excluding other readers";
    EXPECT_EQ(peak.load(), kReaders);
}

// An exclusive holder must still exclude readers.
TEST_F(SyncTest, ExclusiveSrwLockStillExcludesSharedHolders) {
    constexpr GuestAddr lock_addr = 0xA200;
    m_mgr->initialize_srw_lock(lock_addr);

    ThreadManager::set_current_thread_id(1);
    m_mgr->acquire_srw_lock_exclusive(lock_addr);

    std::atomic<bool> reader_got_in{false};
    std::thread reader([&]() {
        ThreadManager::set_current_thread_id(2);
        if (m_mgr->try_acquire_srw_lock_shared(lock_addr)) {
            reader_got_in.store(true, std::memory_order_release);
            (void)m_mgr->release_srw_lock_shared(lock_addr);
        }
    });
    reader.join();
    EXPECT_FALSE(reader_got_in.load())
        << "a shared acquisition succeeded while the lock was held exclusively";

    m_mgr->release_srw_lock_exclusive(lock_addr);

    // ...and once released, a reader must get in.
    std::atomic<bool> after{false};
    std::thread reader2([&]() {
        ThreadManager::set_current_thread_id(3);
        if (m_mgr->try_acquire_srw_lock_shared(lock_addr)) {
            after.store(true, std::memory_order_release);
            (void)m_mgr->release_srw_lock_shared(lock_addr);
        }
    });
    reader2.join();
    EXPECT_TRUE(after.load());
}
