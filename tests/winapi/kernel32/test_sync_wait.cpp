/// @file test_sync_wait.cpp
/// @brief SyncManager wait-function edge cases: WaitForSingle/MultipleObjects, mixed local and shared waits, parking, shutdown cancellation and wait causality (THREAD-002)

#include "sync_test_fixture.hpp"

// =============================================================================
// Wait Functions — Edge Cases
// =============================================================================

TEST_F(SyncTest, WaitForSingleObjectInvalidHandleReturnsFailed) {
    u32 result = m_mgr->wait_for_single_object(0xDEAD, 0);
    EXPECT_EQ(result, WAIT_FAILED);
}

TEST_F(SyncTest, WaitForSingleObjectNullHandleReturnsFailed) {
    u32 result = m_mgr->wait_for_single_object(NULL_HANDLE, 0);
    EXPECT_EQ(result, WAIT_FAILED);
}

TEST_F(SyncTest, WaitForMultipleObjectsFirstAvailable) {
    Handle e1 = m_mgr->create_event(true, false, "e1");
    Handle e2 = m_mgr->create_event(true, true, "e2"); // signaled
    Handle handles[] = {e1, e2};

    u32 result = m_mgr->wait_for_multiple_objects(2, handles, false, 0);
    // e2 (index 1) should satisfy first
    EXPECT_EQ(result, WAIT_OBJECT_0 + 1);
}

TEST_F(SyncTest, WaitForMultipleObjectsAllTimeout) {
    Handle e1 = m_mgr->create_event(true, false, "e1");
    Handle e2 = m_mgr->create_event(true, false, "e2");
    Handle handles[] = {e1, e2};

    u32 result = m_mgr->wait_for_multiple_objects(2, handles, false, 0);
    EXPECT_EQ(result, WAIT_TIMEOUT);
}

TEST_F(SyncTest, WaitForMultipleObjectsInvalidHandleReturnsFailed) {
    Handle e1 = m_mgr->create_event(true, false, "e1");
    Handle handles[] = {e1, NULL_HANDLE};

    u32 result = m_mgr->wait_for_multiple_objects(2, handles, false, 0);
    EXPECT_EQ(result, WAIT_FAILED);
}

TEST_F(SyncTest, ExtraWaitableHandleIsAcceptedByWaitForSingleObject) {
    // ConsoleInput-style: ntsync event registered outside m_events.
    const u64 nt = ntsync::global().create_event(true, false);
    const Handle h = m_handles->allocate(HandleType::ConsoleInput, nullptr);
    m_mgr->register_waitable(h, nt);
    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_TIMEOUT);
    ntsync::global().set_event(nt);
    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_OBJECT_0);
}

TEST_F(SyncTest, MemoryResourceNotificationHighSignaledLowNot) {
    // Windows: HighMemory → WFSO(0)=WAIT_OBJECT_0; LowMemory → WAIT_TIMEOUT.
    Handle low = m_mgr->create_event(true, false, "");
    Handle high = m_mgr->create_event(true, true, "");
    EXPECT_EQ(m_mgr->wait_for_single_object(low, 0), WAIT_TIMEOUT);
    EXPECT_EQ(m_mgr->wait_for_single_object(high, 0), WAIT_OBJECT_0);
}

TEST_F(SyncTest, WaitForMultipleObjectsExExportRegistered) {
    const auto exports = create_sync_thunks(*m_mgr);
    bool found = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "WaitForMultipleObjectsEx") {
            found = true;
            EXPECT_EQ(entry.stdcall_bytes, 20u);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SyncTest, MixedLocalAndPromotedSharedWaitAnyObservesSharedSignal) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC";

    const Handle local = m_mgr->create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    const Handle promoted = m_mgr->create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "");
    ASSERT_NE(local, NULL_HANDLE);
    ASSERT_NE(promoted, NULL_HANDLE);

    constexpr u32 kSynchronizeAndModifyState = 0x00100002;
    const auto descriptor = m_mgr->prepare_transferred_handle(
        promoted, kSynchronizeAndModifyState, /*inheritable=*/false);
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(m_mgr->is_waitable_sync_handle(promoted));

    Handle handles[] = {local, promoted};
    std::atomic<u32> result{WAIT_TIMEOUT};
    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(3);
        result.store(
            m_mgr->wait_for_multiple_objects(2, handles, false, 1000),
            std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(m_mgr->set_event(promoted));
    waiter.join();
    EXPECT_EQ(result.load(std::memory_order_acquire), WAIT_OBJECT_0 + 1);
}

TEST_F(SyncTest, MixedWaitParkingDoesNotConsumeLocalRelease) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC";
    // Learn WaitForMultipleObjects: only the object satisfying the final
    // wait may change state. Parking between scans is not a guest wait.
    for (bool wait_all : {false, true}) {
        for (bool semaphore : {false, true}) {
            const Handle local = semaphore
                ? m_mgr->create_semaphore(0, 1, "")
                : m_mgr->create_event(false, false, "");
            const Handle shared = m_mgr->create_event(true, false, "");
            ASSERT_NE(local, NULL_HANDLE);
            ASSERT_NE(shared, NULL_HANDLE);
            ASSERT_TRUE(m_mgr->prepare_transferred_handle(
                shared, sync_access::EVENT_ALL_ACCESS, false).has_value());
            const Handle handles[] = {shared, local};
            const auto parked_before = ntsync::global().wake_counters().waits;
            std::atomic<u32> result{WAIT_FAILED};
            std::thread waiter([&] {
                ThreadManager::set_current_thread_id(39);
                result.store(m_mgr->wait_for_multiple_objects(
                    2, handles, wait_all, 250), std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(100);
            while (ntsync::global().wake_counters().waits == parked_before &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            const bool parked =
                ntsync::global().wake_counters().waits > parked_before;
            const bool signaled = semaphore
                ? m_mgr->release_semaphore(local, 1, nullptr)
                : m_mgr->set_event(local);
            // In wait-all the local unit must survive until the other
            // backend is signaled too; a partial wait cannot take it away.
            if (wait_all) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                EXPECT_TRUE(m_mgr->set_event(shared));
            }
            waiter.join();
            EXPECT_TRUE(parked);
            EXPECT_TRUE(signaled);
            EXPECT_EQ(result.load(std::memory_order_acquire),
                      wait_all ? WAIT_OBJECT_0 : WAIT_OBJECT_0 + 1)
                << "wait_all=" << wait_all << " semaphore=" << semaphore;
            EXPECT_EQ(m_mgr->wait_for_single_object(local, 0), WAIT_TIMEOUT);
            EXPECT_TRUE(m_handles->close(local));
            EXPECT_TRUE(m_handles->close(shared));
        }
    }
}

TEST_F(SyncTest, InheritsAnonymousEventAtSameHandleValue) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC";

    const Handle parent_event = m_mgr->create_event(
        /*manual_reset=*/true, /*initial_state=*/false, "",
        /*inheritable=*/true);
    ASSERT_NE(parent_event, NULL_HANDLE);

    const std::array<Handle, 1> selected{parent_event};
    const std::string descriptors =
        m_mgr->prepare_inherited_handles(selected);
    ASSERT_FALSE(descriptors.empty());

    const std::array<Handle, 1> inherited_values{parent_event};
    HandleTable child_handles(inherited_values);
    for (int i = 0; i < 32; ++i) {
        ASSERT_NE(child_handles.allocate(HandleType::Event, nullptr), parent_event);
    }
    SyncManager child_manager(child_handles);
    ASSERT_TRUE(child_manager.import_inherited_handles(descriptors));
    EXPECT_EQ(child_handles.get_type(parent_event), HandleType::Event);
    EXPECT_NE(
        child_handles.get_flags(parent_event) & HandleFlags::Inherit, 0u);

    EXPECT_EQ(
        child_manager.wait_for_single_object(parent_event, 0),
        WAIT_TIMEOUT);

    EXPECT_TRUE(m_mgr->set_event(parent_event));
    EXPECT_EQ(
        child_manager.wait_for_single_object(parent_event, 0),
        WAIT_OBJECT_0);
}

TEST_F(SyncTest, BeginShutdownCancelsInfiniteLocalWait) {
    const Handle event = m_mgr->create_event(
        /*manual_reset=*/false, /*initial_state=*/false, "");
    ASSERT_NE(event, NULL_HANDLE);

    std::atomic<bool> started{false};
    std::atomic<u32> result{WAIT_TIMEOUT};
    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(2);
        started.store(true, std::memory_order_release);
        result.store(
            m_mgr->wait_for_single_object(event, INFINITE_WAIT),
            std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    m_mgr->begin_shutdown();
    waiter.join();
    EXPECT_EQ(result.load(std::memory_order_acquire), WAIT_FAILED);
}

TEST_F(SyncTest, BeginShutdownCancelsInfiniteSharedCancellableWait) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC";
    const Handle event = m_mgr->create_event(false, false, "", true);
    ASSERT_NE(event, NULL_HANDLE);
    const std::array<Handle, 1> selected{event};
    ASSERT_FALSE(m_mgr->prepare_inherited_handles(selected).empty());
    std::atomic<bool> entered{false}, finished{false};
    std::atomic<bool> cancelled{false};
    std::atomic<u32> result{WAIT_TIMEOUT};
    std::thread waiter([&] {
        ThreadManager::set_current_thread_id(2);
        entered.store(true, std::memory_order_release);
        result.store(m_mgr->wait_for_single_object_cancellable(event, INFINITE_WAIT, cancelled));
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    // Let the worker enter the blocking shared-object path, past its entry
    // shutdown check. Signaling below bounds cleanup on the old implementation.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    m_mgr->begin_shutdown();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(finished.load(std::memory_order_acquire));
    if (!finished.load(std::memory_order_acquire)) m_mgr->set_event(event);
    waiter.join();
    EXPECT_EQ(result.load(), WAIT_FAILED);
}

TEST_F(SyncTest, BeginShutdownCancelsInfiniteConditionVariableSrwWait) {
    constexpr GuestAddr cv_addr = 0x24000;
    constexpr GuestAddr srw_addr = 0x25000;
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::atomic<bool> result{true};

    std::thread waiter([&]() {
        ThreadManager::set_current_thread_id(2);
        m_mgr->acquire_srw_lock_exclusive(srw_addr);
        entered.store(true, std::memory_order_release);
        result.store(m_mgr->sleep_condition_variable_srw(
                         cv_addr, srw_addr, INFINITE_WAIT, 0),
                     std::memory_order_release);
        returned.store(true, std::memory_order_release);
        m_mgr->release_srw_lock_exclusive(srw_addr);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    m_mgr->begin_shutdown();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool cancelled = returned.load(std::memory_order_acquire);
    if (!cancelled) {
        m_mgr->wake_all_condition_variable(cv_addr);
    }
    waiter.join();

    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(result.load(std::memory_order_acquire));
}

TEST(SyncCausalityTest, InfiniteEventWaitReportsCreationAndLastTransition) {
    ASSERT_EQ(::setenv("VITREO_SYNC_CAUSAL_TRACE", "1", 1), 0);

    HandleTable handles;
    SyncManager manager{handles};
    ThreadManager::set_current_thread_id(7);

    testing::internal::CaptureStderr();
    SyncTraceContext created_trace{"CreateEventW", 0x11111111};
    created_trace.parents[0] = 0x11112222;
    created_trace.parent_count = 1;
    const Handle event = manager.create_event(
        /*manual_reset=*/false, /*initial_state=*/false, "",
        /*inheritable=*/false, created_trace);
    EXPECT_NE(event, NULL_HANDLE);
    EXPECT_TRUE(manager.set_event(
        event, SyncTraceContext{"SetEvent", 0x22222222}));
    EXPECT_EQ(
        manager.wait_for_single_object(
            event, INFINITE_WAIT,
            SyncTraceContext{"WaitForSingleObject", 0x33333333}),
        WAIT_OBJECT_0);
    const std::string output = testing::internal::GetCapturedStderr();

    EXPECT_NE(output.find("[sync-cause]"), std::string::npos);
    EXPECT_NE(output.find("state=signaled"), std::string::npos);
    EXPECT_NE(output.find(
                  "created={seq=1 api=CreateEventW tid=7 caller=0x11111111"),
              std::string::npos);
    EXPECT_NE(output.find("parents=[0x11112222]"), std::string::npos);
    EXPECT_NE(output.find("op=set api=SetEvent tid=7 caller=0x22222222"),
              std::string::npos);
    EXPECT_NE(output.find(
                  "wait={api=WaitForSingleObject tid=7 caller=0x33333333"),
              std::string::npos);

    EXPECT_EQ(::unsetenv("VITREO_SYNC_CAUSAL_TRACE"), 0);
}
