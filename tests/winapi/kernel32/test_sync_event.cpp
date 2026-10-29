/// @file test_sync_event.cpp
/// @brief SyncManager event tests: manual/auto-reset events, SignalObjectAndWait, named events and OpenEvent (THREAD-002)

#include "sync_test_fixture.hpp"

// =============================================================================
// Event Tests
// =============================================================================

TEST_F(SyncTest, CreateEventManualResetStaysSignaled) {
    Handle h = m_mgr->create_event(true, true, "manual_event");
    EXPECT_NE(h, INVALID_HANDLE);

    // Already signaled — wait should succeed immediately
    u32 r1 = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(r1, WAIT_OBJECT_0);

    // Manual reset: should still be signaled after wait
    u32 r2 = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(r2, WAIT_OBJECT_0);
}

TEST_F(SyncTest, UnnamedManualResetEventStaysSignaledAcrossRepeatedWaits) {
    // Steam's CSteamEngine::BMainLoop waits on an UNNAMED manual-reset event
    // created unsignaled and set by another thread. Windows leaves a
    // manual-reset event signaled until ResetEvent, so every later wait must
    // still succeed; when one stops succeeding the engine asserts
    // "BMainLoop appears to have stalled > 15 seconds without event signalled"
    // and SteamAPI_Init never completes.
    Handle h = m_mgr->create_event(/*manual_reset=*/true,
                                   /*initial_state=*/false, "");
    ASSERT_NE(h, INVALID_HANDLE);

    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_TIMEOUT);

    EXPECT_TRUE(m_mgr->set_event(h));
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_OBJECT_0)
            << "wait " << i << " after SetEvent on a manual-reset event";
    }

    // Setting an already-signaled manual-reset event is a no-op, not a pulse.
    EXPECT_TRUE(m_mgr->set_event(h));
    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_OBJECT_0);

    // Only ResetEvent clears it.
    EXPECT_TRUE(m_mgr->reset_event(h));
    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_TIMEOUT);
}

TEST_F(SyncTest, ManualResetEventStaysSignaledAfterWakingAParkedWaiter) {
    // The shape Steam actually uses: a waiter is PARKED on the event when
    // another thread signals it. Waking a parked waiter must not consume a
    // manual-reset event — every later wait still has to succeed. Steam's
    // engine loop re-waits on the same event forever, so a wake that clears it
    // strands BMainLoop and SteamAPI_Init never completes.
    Handle h = m_mgr->create_event(/*manual_reset=*/true,
                                   /*initial_state=*/false, "");
    ASSERT_NE(h, INVALID_HANDLE);

    std::atomic<u32> parked_result{0xFFFFFFFFu};
    std::atomic<bool> entered{false};
    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        parked_result.store(m_mgr->wait_for_single_object(h, 5000),
                            std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(m_mgr->set_event(h));
    waiter.join();
    EXPECT_EQ(parked_result.load(std::memory_order_acquire), WAIT_OBJECT_0);

    // The event must still be signaled now that the parked waiter is through.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_OBJECT_0)
            << "wait " << i << " after a parked waiter was woken";
    }
}

TEST_F(SyncTest, CreateEventAutoResetResetsAfterWait) {
    Handle h = m_mgr->create_event(false, true, "auto_event");

    // First wait should succeed and auto-reset
    u32 r1 = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(r1, WAIT_OBJECT_0);

    // Second wait should time out since auto-reset cleared signal
    u32 r2 = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(r2, WAIT_TIMEOUT);
}

TEST_F(SyncTest, WaitForNonSignaledEventTimesOut) {
    Handle h = m_mgr->create_event(true, false, "nonsignaled");
    u32 result = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(result, WAIT_TIMEOUT);
}

TEST_F(SyncTest, WaitForNonsignaledAutoResetTimesOut) {
    Handle h = m_mgr->create_event(false, false, "nonsignaled_auto");
    u32 result = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(result, WAIT_TIMEOUT);
}

TEST_F(SyncTest, SetEventThenWaitSucceeds) {
    Handle h = m_mgr->create_event(true, false, "delayed");
    EXPECT_TRUE(m_mgr->set_event(h));
    u32 result = m_mgr->wait_for_single_object(h, 0);
    EXPECT_EQ(result, WAIT_OBJECT_0);
}

TEST_F(SyncTest, SignalObjectAndWaitSignalsThenWaits) {
    Handle to_signal = m_mgr->create_event(false, false, "soa_signal");
    Handle to_wait = m_mgr->create_event(false, true, "soa_wait");
    ASSERT_NE(to_signal, NULL_HANDLE);
    ASSERT_NE(to_wait, NULL_HANDLE);

    EXPECT_EQ(
        m_mgr->signal_object_and_wait(to_signal, to_wait, 0, false),
        WAIT_OBJECT_0);
    // Auto-reset signal event should now be signaled for another waiter.
    EXPECT_EQ(m_mgr->wait_for_single_object(to_signal, 0), WAIT_OBJECT_0);
}

TEST_F(SyncTest, SignalObjectAndWaitInvalidSignalFails) {
    Handle wait = m_mgr->create_event(false, true, "soa_wait_only");
    EXPECT_EQ(
        m_mgr->signal_object_and_wait(0, wait, 0, false),
        WAIT_FAILED);
}

TEST_F(SyncTest, OpenNamedEventSharesStateAndHasIndependentLifetime) {
    constexpr u32 EVENT_MODIFY_STATE = 0x0002;
    constexpr u32 SYNCHRONIZE = 0x00100000;
    const Handle created = m_mgr->create_event(false, false, "open_named_event");
    ASSERT_NE(created, NULL_HANDLE);

    const Handle opened =
        m_mgr->open_event(EVENT_MODIFY_STATE | SYNCHRONIZE,
                          "open_named_event", true);
    ASSERT_NE(opened, NULL_HANDLE);
    EXPECT_NE(opened, created);
    EXPECT_EQ(m_handles->get_flags(opened), HandleFlags::Inherit);
    EXPECT_EQ(m_handles->get_access(opened), EVENT_MODIFY_STATE | SYNCHRONIZE);

    EXPECT_TRUE(m_mgr->set_event(created));
    EXPECT_EQ(m_mgr->wait_for_single_object(opened, 0), WAIT_OBJECT_0);
    EXPECT_EQ(m_mgr->wait_for_single_object(created, 0), WAIT_TIMEOUT);

    EXPECT_TRUE(m_handles->close(created));
    EXPECT_TRUE(m_mgr->set_event(opened));
    EXPECT_EQ(m_mgr->wait_for_single_object(opened, 0), WAIT_OBJECT_0);
}

TEST_F(SyncTest, CreateNamedEventReturnsDistinctHandleAndAlreadyExists) {
    const Handle first = m_mgr->create_event(true, false, "existing_event");
    ASSERT_NE(first, NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    const Handle second = m_mgr->create_event(false, true, "existing_event");
    ASSERT_NE(second, NULL_HANDLE);
    EXPECT_NE(second, first);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ALREADY_EXISTS);

    // The second CreateEvent call must not replace the original object's
    // manual-reset or initial-state attributes.
    EXPECT_EQ(m_mgr->wait_for_single_object(second, 0), WAIT_TIMEOUT);
    EXPECT_TRUE(m_mgr->set_event(first));
    EXPECT_EQ(m_mgr->wait_for_single_object(second, 0), WAIT_OBJECT_0);
    EXPECT_EQ(m_mgr->wait_for_single_object(first, 0), WAIT_OBJECT_0);
}

TEST_F(SyncTest, NamedEventNameIsReleasedAfterLastHandleCloses) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";

    const std::string name =
        "named_event_last_close_" + std::to_string(::getpid());
    const Handle first = m_mgr->create_event(true, false, name);
    ASSERT_NE(first, NULL_HANDLE);
    ASSERT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    ASSERT_TRUE(m_handles->close(first));

    // Windows removes a named kernel object when its last handle closes. A
    // later CreateEvent with the same name creates a fresh object and must not
    // report ERROR_ALREADY_EXISTS.
    const Handle recreated = m_mgr->create_event(true, false, name);
    ASSERT_NE(recreated, NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_TRUE(m_handles->close(recreated));
}

// Native event_name_pipe_duplicate_probe: NULL=87, empty=6, missing=2.
TEST_F(SyncTest, OpenEventNameErrorsMatchWindowsBothBitnessesAndEncodings) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x2000,
        MemoryProtection::ReadWrite).is_ok());
    auto exports = create_sync_thunks(*m_mgr);
    for (bool is64 : {false, true}) {
        for (const char* export_name : {"OpenEventA", "OpenEventW"}) {
            auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                [&](const auto& e) { return e.name == export_name; });
            ASSERT_NE(entry, exports.entries.end());
            for (bool null_name : {false, true}) {
                cpu::CpuState cpu;
                if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
                cpu.set_sp(0x11000);
                const GuestAddr name = null_name ? 0 : 0x10100;
                ASSERT_TRUE(memory.write_u32(0x10100, 0).is_ok());
                if (is64) {
                    cpu.gpr.rcx = sync_access::SYNCHRONIZE;
                    cpu.gpr.rdx = 0;
                    cpu.gpr.r8 = name;
                } else {
                    ASSERT_TRUE(memory.write_u32(cpu.sp()+4, sync_access::SYNCHRONIZE).is_ok());
                    ASSERT_TRUE(memory.write_u32(cpu.sp()+8, 0).is_ok());
                    ASSERT_TRUE(memory.write_u32(cpu.sp()+12, name).is_ok());
                }
                Marshaler marshaler(memory);
                marshaler.attach_cpu(cpu);
                entry->handler(marshaler);
                EXPECT_EQ(cpu.gpr.rax, 0u);
                EXPECT_EQ(get_last_error(), null_name ?
                    error_codes::ERROR_INVALID_PARAMETER : error_codes::ERROR_INVALID_HANDLE);
            }
        }
    }
    EXPECT_EQ(m_mgr->open_event(sync_access::SYNCHRONIZE, "missing_event"), NULL_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(SyncTest, ProcessHandleTracksChildWithoutOptionalIpcGate) {
    int release_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(release_pipe), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(release_pipe[1]);
        char release = 0;
        ssize_t result = 0;
        do {
            result = ::read(release_pipe[0], &release, 1);
        } while (result < 0 && errno == EINTR);
        ::close(release_pipe[0]);
        _exit(result == 1 ? 7 : 8);
    }

    ::close(release_pipe[0]);
    const Handle process = m_mgr->create_process_handle(child);
    EXPECT_NE(process, NULL_HANDLE);

    u32 exit_code = 0;
    EXPECT_TRUE(m_mgr->process_exit_code(process, exit_code));
    EXPECT_EQ(exit_code, 259u);
    EXPECT_EQ(m_mgr->wait_for_single_object(process, 0), WAIT_TIMEOUT);

    const char release = 1;
    EXPECT_EQ(::write(release_pipe[1], &release, 1), 1);
    ::close(release_pipe[1]);

    EXPECT_EQ(
        m_mgr->wait_for_single_object(process, 5000),
        WAIT_OBJECT_0);
    EXPECT_TRUE(m_mgr->process_exit_code(process, exit_code));
    EXPECT_EQ(exit_code, 7u);

    const Handle unsignaled = m_mgr->create_event(true, false, "");
    const Handle handles[2] = {unsignaled, process};
    EXPECT_EQ(
        m_mgr->wait_for_multiple_objects(2, handles, false, 0),
        WAIT_OBJECT_0 + 1);

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 7);
}

TEST_F(SyncTest, ResetEventClearsSignal) {
    Handle h = m_mgr->create_event(true, true, "to_reset");
    // Signaled initially
    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_OBJECT_0);

    // Reset
    EXPECT_TRUE(m_mgr->reset_event(h));

    // Should now time out
    EXPECT_EQ(m_mgr->wait_for_single_object(h, 0), WAIT_TIMEOUT);
}

TEST_F(SyncTest, ResetEventInvalidHandleSetsLastError) {
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->reset_event(0));
    EXPECT_EQ(get_last_error(), 6u);  // ERROR_INVALID_HANDLE
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->reset_event(0x7FFFFFF0u));
    EXPECT_EQ(get_last_error(), 6u);
    Handle mtx = m_mgr->create_mutex(false, "");
    ASSERT_NE(mtx, NULL_HANDLE);
    set_last_error(0xDEAD);
    EXPECT_FALSE(m_mgr->reset_event(mtx));
    EXPECT_EQ(get_last_error(), 6u);
}
