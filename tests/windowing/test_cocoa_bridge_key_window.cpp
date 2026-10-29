/// @file test_cocoa_bridge_key_window.cpp
/// @brief Tests for CocoaBridge app activation, key-window and keyboard-focus ownership (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

class ScopedProcessRoleEnv {
public:
    explicit ScopedProcessRoleEnv(const char* value) {
        const char* prev = std::getenv("VITREO_MACOS_PROCESS_ROLE");
        if (prev) {
            m_prev = prev;
        }
        if (value) {
            ::setenv("VITREO_MACOS_PROCESS_ROLE", value, 1);
        } else {
            ::unsetenv("VITREO_MACOS_PROCESS_ROLE");
        }
    }
    ~ScopedProcessRoleEnv() {
        if (m_prev) {
            ::setenv("VITREO_MACOS_PROCESS_ROLE", m_prev->c_str(), 1);
        } else {
            ::unsetenv("VITREO_MACOS_PROCESS_ROLE");
        }
    }
    ScopedProcessRoleEnv(const ScopedProcessRoleEnv&) = delete;
    ScopedProcessRoleEnv& operator=(const ScopedProcessRoleEnv&) = delete;

private:
    std::optional<std::string> m_prev;
};

TEST_F(CocoaBridgeTest, AppActivationDoesNotRestoreHiddenWindows) {
    // Windows generates WM_SYSCOMMAND SC_RESTORE from the system menu or the
    // taskbar for a *minimized* window; activating an application never
    // restores an ordinary hidden window. Battle.net creates its CEF `#49158`
    // host and Qt's popup save-bits window without WS_VISIBLE, so broadcasting
    // SC_RESTORE on activation popped both up as empty 640x480 windows.
    MessageQueue queue;
    bridge.set_message_queue(queue);

    WindowData hidden;
    hidden.handle = 0x49158;
    hidden.style = WS::OVERLAPPEDWINDOW | WS::CLIPCHILDREN;
    hidden.rect = {100, 100, 100 + 640, 100 + 480};
    hidden.client_rect = {0, 0, 624, 434};
    hidden.visible = false;
    ASSERT_TRUE(bridge.create_window(0x49158, hidden).is_ok());
    ASSERT_FALSE(bridge.native_window_is_visible(0x49158));

    bridge.on_app_did_become_active();

    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(bridge.native_window_is_visible(0x49158));
}

TEST_F(CocoaBridgeTest, ShowingAChildOfAHiddenTopLevelShowsAndActivatesNothing) {
    // Spotify (CEF), 2026-10-22: ShowWindow(SW_SHOW) on its "Chrome Legacy
    // Window" child while the top-level was still hidden put the top-level's
    // NSWindow on screen and made it key, and USER32 then activated the
    // hidden window before Chromium had built its widget: a NULL dereference
    // in the WM_SETFOCUS handler, after which crashpad terminated the browser
    // process. Learn IsWindowVisible: a window is visible only when it and
    // all its parents are. ShowWindow on a WS_CHILD window activates nothing
    // (SWP_NOACTIVATE, measured setparent_child_to_desktop).
    MessageQueue queue;
    bridge.set_message_queue(queue);
    std::vector<std::pair<HWND, bool>> transitions;
    bridge.set_native_application_activation_callback(
        [&](HWND hwnd, bool active, bool) {
            transitions.emplace_back(hwnd, active);
        });

    WindowData top;
    top.handle = 0x10c46;
    top.style = WS::OVERLAPPEDWINDOW | WS::CLIPCHILDREN;
    top.rect = {0, 0, 1584, 854};
    top.client_rect = top.rect;
    top.visible = false;
    ASSERT_TRUE(bridge.create_window(0x10c46, top).is_ok());

    WindowData child;
    child.handle = 0x10c47;
    child.parent = 0x10c46;
    child.style = WS::CHILD;
    child.rect = {0, 0, 0, 0};
    child.client_rect = child.rect;
    bridge.add_child_view(0x10c46, 0x10c47, child, 0, L"Chrome Legacy Window", L"");
    bridge.set_visible(0x10c47, true);

    EXPECT_FALSE(bridge.native_window_is_visible(0x10c46))
        << "a child's show put its hidden top-level on screen";
    EXPECT_TRUE(transitions.empty()) << "a child's show activated its top-level";
    EXPECT_EQ(bridge.active_hwnd(), NULL_HWND);

    // Showing the top-level shows it, child and all.
    bridge.set_visible(0x10c46, true);
    EXPECT_TRUE(bridge.native_window_is_visible(0x10c46));
}

TEST_F(CocoaBridgeTest, AppActivationReordersVisibleWindowWithoutSysCommand) {
    // A WS_VISIBLE HWND that AppKit ordered out for lack of activation must be
    // put back on screen by the visibility sync, not by a synthesized restore.
    MessageQueue queue;
    bridge.set_message_queue(queue);

    WindowData shown;
    shown.handle = 0x49159;
    shown.style = WS::OVERLAPPEDWINDOW;
    shown.rect = {80, 60, 80 + 400, 60 + 300};
    shown.client_rect = {0, 0, 400, 300};
    shown.visible = true;
    ASSERT_TRUE(bridge.create_window(0x49159, shown).is_ok());
    // The first show of a visible top-level may activate it; without a USER32
    // activation callback the bridge queues that WM_ACTIVATEAPP/WM_SETFOCUS.
    // Those belong to the show, not to the app activation under test.
    MSG creation_msg{};
    while (queue.peek(creation_msg, NULL_HWND, 0, 0, PM::REMOVE)) {
        EXPECT_NE(creation_msg.message, WM::SYSCOMMAND);
    }

    bridge.on_app_did_become_active();

    EXPECT_TRUE(bridge.native_window_is_visible(0x49159));
    EXPECT_TRUE(queue.empty());
}

TEST_F(CocoaBridgeTest, ActivateWindowOrdersVisibleWindowOnScreen) {
    WindowData data;
    data.handle = 241;
    data.rect = {80, 60, 80 + 400, 60 + 300};
    data.client_rect = {0, 0, 400, 300};
    data.visible = false;
    ASSERT_TRUE(bridge.create_window(241, data).is_ok());
    bridge.set_visible(241, true);
    ASSERT_TRUE(bridge.native_window_is_visible(241));
    bridge.set_visible(241, false);
    EXPECT_FALSE(bridge.native_window_is_visible(241));
    bridge.set_visible(241, true);
    // Learn SetForegroundWindow: nonzero only when the window was brought to
    // the foreground, which a background process may not do; the shown
    // window stays on screen either way.
    EXPECT_EQ(bridge.activate_window(241), bridge.process_may_set_foreground());
    EXPECT_TRUE(bridge.native_window_is_visible(241));
}

TEST_F(CocoaBridgeTest,
       NativeActivationAndFocusUseSentCallbacks) {
    MessageQueue queue;
    bridge.set_message_queue(queue);
    std::vector<std::pair<HWND, bool>> transitions;
    bridge.set_native_application_activation_callback(
        [&](HWND hwnd, bool active, bool) {
            transitions.emplace_back(hwnd, active);
        });
    std::vector<std::pair<HWND, bool>> focus_transitions;
    bridge.set_native_keyboard_focus_callback(
        [&](HWND hwnd, bool focused) {
            focus_transitions.emplace_back(hwnd, focused);
        });

    bridge.on_native_window_became_key(0x1234);
    bridge.on_native_window_resigned_key(0x1234);

    EXPECT_EQ(transitions,
              (std::vector<std::pair<HWND, bool>>{
                  {0x1234, true}, {0x1234, false}}));
    // USER32 activation owns focus (DefWindowProc WM_ACTIVATE -> SetFocus).
    EXPECT_TRUE(focus_transitions.empty());
    EXPECT_TRUE(queue.empty());
}

TEST_F(CocoaBridgeTest, StaleResignIgnoredAfterSiblingClaimsKey) {
    MessageQueue queue;
    bridge.set_message_queue(queue);
    std::vector<std::pair<HWND, bool>> transitions;
    bridge.set_native_application_activation_callback(
        [&](HWND hwnd, bool active, bool) {
            transitions.emplace_back(hwnd, active);
        });
    std::vector<std::pair<HWND, bool>> focus_transitions;
    bridge.set_native_keyboard_focus_callback(
        [&](HWND hwnd, bool focused) {
            focus_transitions.emplace_back(hwnd, focused);
        });

    constexpr HWND first = 0x105f3;
    constexpr HWND second = 0x105fb;
    bridge.on_native_window_became_key(first);
    bridge.on_native_window_became_key(second);
    // Delayed resign for the previous key window must not KillFocus the new
    // key owner or post a second ACTIVATEAPP(0) gap.
    bridge.on_native_window_resigned_key(first);

    EXPECT_EQ(bridge.active_hwnd(), second);
    EXPECT_EQ(transitions,
              (std::vector<std::pair<HWND, bool>>{
                  {first, true}, {second, true}}));
    EXPECT_TRUE(focus_transitions.empty());
}

TEST_F(CocoaBridgeTest, AlreadyVisiblePreferredDoesNotRemakeKeyOnSync) {
    MessageQueue queue;
    bridge.set_message_queue(queue);
    int became_key_count = 0;
    bridge.set_native_keyboard_focus_callback(
        [&](HWND, bool focused) {
            if (focused) {
                ++became_key_count;
            }
        });

    WindowData primary;
    primary.handle = 0x300;
    primary.visible = true;
    primary.rect = {0, 0, 800, 600};
    primary.client_rect = primary.rect;
    ASSERT_TRUE(bridge.create_window(0x300, primary).is_ok());
    if (bridge.active_hwnd() != 0x300) {
        bridge.on_native_window_became_key(0x300);
    }
    const int after_first = became_key_count;
    ASSERT_GE(after_first, 1);

    // Simulate host app taking key away; preferred remains.
    bridge.on_native_window_resigned_key(0x300);
    bridge.clear_active_hwnd(0x300);

    // Re-show / visibility sync of an already-visible window must not
    // activateIgnoringOtherApps + makeKeyAndOrderFront.
    bridge.set_visible(0x300, true);
    EXPECT_EQ(became_key_count, after_first);
    EXPECT_EQ(bridge.active_hwnd(), NULL_HWND);
}

TEST_F(CocoaBridgeTest, MacosSessionChildEnvIsDetected) {
    {
        ScopedProcessRoleEnv child("child");
        EXPECT_TRUE(CocoaBridge::macos_session_is_child_process());
    }
    {
        ScopedProcessRoleEnv other("other");
        EXPECT_FALSE(CocoaBridge::macos_session_is_child_process());
    }
}

TEST_F(CocoaBridgeTest, NonactivatingChildAppWindowDoesNotActivateSession) {
    ScopedProcessRoleEnv child("child");

    MessageQueue queue;
    bridge.set_message_queue(queue);
    int became_key_count = 0;
    bridge.set_native_keyboard_focus_callback(
        [&](HWND, bool focused) {
            if (focused) {
                ++became_key_count;
            }
        });

    WindowData primary;
    primary.handle = 0x400;
    primary.ex_style = WS_EX::NOACTIVATE | WS_EX::APPWINDOW;
    primary.visible = true;
    primary.rect = {0, 0, 624, 434};
    primary.client_rect = primary.rect;
    ASSERT_TRUE(bridge.create_window(0x400, primary).is_ok());

    // APPWINDOW may promote a descendant to a Dock application, but must not
    // override NOACTIVATE (Learn: Extended Window Styles).
    EXPECT_EQ(became_key_count, 0);
    EXPECT_EQ(bridge.active_hwnd(), NULL_HWND);
}

TEST_F(CocoaBridgeTest, SiblingShowDoesNotStealPreferredKeyWindow) {
    MessageQueue queue;
    bridge.set_message_queue(queue);
    std::vector<HWND> became_key;
    bridge.set_native_keyboard_focus_callback(
        [&](HWND hwnd, bool focused) {
            if (focused) {
                became_key.push_back(hwnd);
            }
        });

    WindowData primary;
    primary.handle = 0x200;
    primary.visible = true;
    primary.rect = {0, 0, 800, 600};
    primary.client_rect = primary.rect;
    ASSERT_TRUE(bridge.create_window(0x200, primary).is_ok());
    // Ensure preferred/active name the primary even if AppKit deferred key.
    if (bridge.active_hwnd() != 0x200) {
        bridge.on_native_window_became_key(0x200);
    }
    ASSERT_EQ(bridge.active_hwnd(), 0x200u);

    // Clear active to mimic a resign gap; preferred must still block steal.
    bridge.clear_active_hwnd(0x200);
    ASSERT_EQ(bridge.active_hwnd(), NULL_HWND);

    WindowData sibling;
    sibling.handle = 0x201;
    sibling.visible = true;
    sibling.rect = {100, 100, 400, 300};
    sibling.client_rect = sibling.rect;
    ASSERT_TRUE(bridge.create_window(0x201, sibling).is_ok());
    // Visibility sync may orderFront the sibling but must not claim key /
    // focus while another top-level remains the preferred foreground.
    EXPECT_EQ(bridge.active_hwnd(), NULL_HWND);
    EXPECT_TRUE(std::find(became_key.begin(), became_key.end(), 0x200) !=
                became_key.end());
    EXPECT_TRUE(std::find(became_key.begin(), became_key.end(), 0x201) ==
                became_key.end());
}

TEST_F(CocoaBridgeTest, KeyboardTargetPrefersFocusedChildAndFallsBackToTopLevel) {
    constexpr HWND top_level = 0x100;
    constexpr HWND child = 0x101;

    EXPECT_EQ(bridge.keyboard_target(top_level), top_level);
    bridge.set_keyboard_focus_hwnd(child);
    EXPECT_EQ(bridge.keyboard_target(top_level), child);
    bridge.set_keyboard_focus_hwnd(NULL_HWND);
    EXPECT_EQ(bridge.keyboard_target(top_level), top_level);
}
