/// @file test_cocoa_bridge_window_lifecycle.cpp
/// @brief Tests for CocoaBridge window create/destroy/visibility, guest quit and the event pump (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, RequestGuestQuitPostsWmClose) {
    MessageQueue queue;
    bridge.set_message_queue(queue);
    WindowData data;
    data.handle = 0x500;
    data.style = WS::OVERLAPPEDWINDOW;
    data.title = L"Quit Me";
    ASSERT_TRUE(bridge.create_window(0x500, data).is_ok());

    bridge.request_guest_quit();

    MSG msg{};
    ASSERT_TRUE(queue.peek(msg, 0x500, WM::CLOSE, WM::CLOSE, PM::REMOVE));
    EXPECT_EQ(msg.hwnd, 0x500);
    EXPECT_EQ(msg.message, WM::CLOSE);
    EXPECT_FALSE(queue.quit_posted());
}

TEST_F(CocoaBridgeTest, RequestGuestQuitWithoutWindowsPostsQuit) {
    MessageQueue queue;
    bridge.set_message_queue(queue);
    bridge.request_guest_quit();
    EXPECT_TRUE(queue.quit_posted());
}

TEST_F(CocoaBridgeTest, AutomaticTerminationGuardSurvivesWindowlessInterval) {
    WindowData data;
    data.handle = 0x5A0;
    data.style = WS::OVERLAPPEDWINDOW;
    data.title = L"Transient splash";
    ASSERT_TRUE(bridge.create_window(data.handle, data).is_ok());
    ASSERT_TRUE(bridge.automatic_termination_guard_active());

    bridge.destroy_window(data.handle);

    EXPECT_EQ(bridge.native_window_count(), 0u);
    EXPECT_TRUE(bridge.automatic_termination_guard_active())
        << "the guest process remains live after its last HWND is destroyed";
}

// ============================================================================
// Window Management Stub Tests
// ============================================================================

TEST_F(CocoaBridgeTest, CreateWindowStub) {
    WindowData data;
    data.handle = 1;
    data.title  = L"Test Window";
    data.rect   = {10, 20, 810, 620};

    auto result = bridge.create_window(1, data);
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(bridge.has_native_window(1));
}

TEST_F(CocoaBridgeTest, DestroyWindowStub) {
    WindowData data;
    data.handle = 2;
    bridge.create_window(2, data);
    EXPECT_TRUE(bridge.has_native_window(2));

    bridge.destroy_window(2);
    EXPECT_FALSE(bridge.has_native_window(2));
}

TEST_F(CocoaBridgeTest, SetVisibleDoesNotCrash) {
    WindowData data;
    data.handle = 3;
    bridge.create_window(3, data);
    bridge.set_visible(3, true);
    bridge.set_visible(3, false);
    // No crash = pass
}

TEST_F(CocoaBridgeTest, SetVisibleOrdersWindowOnScreen) {
    // ShowWindow must display the HWND (Learn SW_SHOW). An empty guest rect is
    // treated as off-monitor and stays orderOut; a real client rect must appear
    // even when makeKeyAndOrderFront cannot activate the process.
    WindowData data;
    data.handle = 240;
    data.rect = {80, 60, 80 + 400, 60 + 300};
    data.client_rect = {0, 0, 400, 300};
    data.visible = false;
    ASSERT_TRUE(bridge.create_window(240, data).is_ok());
    EXPECT_FALSE(bridge.native_window_is_visible(240));
    bridge.set_visible(240, true);
    EXPECT_TRUE(bridge.native_window_is_visible(240));
    bridge.set_visible(240, false);
    EXPECT_FALSE(bridge.native_window_is_visible(240));
}

TEST_F(CocoaBridgeTest, OffMonitorWindowIsNotPresented) {
    // Win32 does not display a HWND whose rect misses every monitor.
    // AppKit's setFrame:display:YES plus constrainFrameRect would otherwise
    // clamp parked installer pages onto the desktop as chromeless rectangles.
    WindowData data;
    data.handle = 260;
    data.rect = {80, 60, 80 + 400, 60 + 300};
    data.client_rect = {0, 0, 400, 300};
    data.visible = false;
    ASSERT_TRUE(bridge.create_window(260, data).is_ok());
    bridge.set_visible(260, true);
    ASSERT_TRUE(bridge.native_window_is_visible(260));

    bridge.set_frame(260, Rect{20000, 100, 20400, 400});
    EXPECT_FALSE(bridge.native_window_is_visible(260));
}

TEST_F(CocoaBridgeTest, NativeWindowCount) {
    EXPECT_EQ(bridge.native_window_count(), 0u);

    WindowData d1;
    d1.handle = 10;
    bridge.create_window(10, d1);
    EXPECT_EQ(bridge.native_window_count(), 1u);

    WindowData d2;
    d2.handle = 11;
    bridge.create_window(11, d2);
    EXPECT_EQ(bridge.native_window_count(), 2u);

    bridge.destroy_window(10);
    EXPECT_EQ(bridge.native_window_count(), 1u);
}

TEST_F(CocoaBridgeTest, PumpEventsNoOp) {
    MessageQueue queue;
    bridge.pump_events(queue);
    EXPECT_TRUE(queue.empty());
}

TEST_F(CocoaBridgeTest, ArmedWakeSurvivesInterveningEventPump) {
    WindowData data{};
    data.style = WS::OVERLAPPEDWINDOW;
    data.rect = {0, 0, 100, 100};
    ASSERT_TRUE(bridge.create_window(0x510, data).is_ok());
    MessageQueue queue;
    bridge.pump_events(queue);
    bridge.set_main_thread_parked(true);
    bridge.wake_main_runloop();
    // The native thread can service another pump before the guest's queued
    // wait block starts. Dequeuing the event must not lose the armed wake.
    bridge.pump_events(queue);
    const auto start = std::chrono::steady_clock::now();
    // Incidental native startup events may end one read. The armed signal
    // must continue to prevent parking until the guest re-probes/disarms.
    for (int i = 0; i < 3; ++i) bridge.wait_events(queue, 500);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    bridge.set_main_thread_parked(false);
    EXPECT_LT(elapsed, std::chrono::milliseconds(150));
    EXPECT_FALSE(vtr_main_wake_pending());
    bridge.set_main_thread_parked(true);
    EXPECT_FALSE(vtr_main_wake_pending());
    bridge.set_main_thread_parked(false);
}

TEST_F(CocoaBridgeTest, NativeSendCompletionInterruptsEventWait) {
    WindowData data{};
    data.style = WS::OVERLAPPEDWINDOW;
    data.rect = {0, 0, 100, 100};
    ASSERT_TRUE(bridge.create_window(0x511, data).is_ok());
    MessageQueue queue;
    queue.set_host_wake([&] { bridge.wake_main_runloop(); });
    bridge.pump_events(queue);
    for (int round = 0; round < 5; ++round) {
        std::promise<void> waiting;
        auto started = waiting.get_future();
        std::thread receiver([&] {
            started.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            queue.dispatch_sent_messages(2);
        });
        bool notified = false;
        const auto start = std::chrono::steady_clock::now();
        const auto result = queue.send_to_thread(0, 2, [] { return 42u; }, [&](std::function<bool()> ready) {
            if (!notified) { notified = true; waiting.set_value(); }
            bridge.wait_events(queue, 500, std::move(ready));
        });
        receiver.join();
        EXPECT_EQ(result, 42u);
        EXPECT_LT(std::chrono::steady_clock::now() - start,
                  std::chrono::milliseconds(200));
    }
    const auto start = std::chrono::steady_clock::now();
    const auto result = queue.send_to_thread(0, 2, [] { return 73u; },
        [&](std::function<bool()> ready) {
            // Completion happens after the queue's first readiness check,
            // but before the native wait has registered wake interest.
            std::thread receiver([&] { queue.dispatch_sent_messages(2); });
            receiver.join();
            bridge.pump_events(queue);
            bridge.wait_events(queue, 500, std::move(ready));
        });
    EXPECT_EQ(result, 73u);
    EXPECT_LT(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds(200));
}
