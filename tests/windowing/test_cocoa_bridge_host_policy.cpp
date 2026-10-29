/// @file test_cocoa_bridge_host_policy.cpp
/// @brief Tests for the static CocoaBridge host policy: Space/fullscreen, cursor hide/clip, raise, key and foreground rules (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, SpaceFullscreenAllowsOverlappedAndGuestFrame) {
    EXPECT_TRUE(CocoaBridge::allows_space_fullscreen(
        WS::OVERLAPPEDWINDOW, 0, false, false));
    EXPECT_TRUE(CocoaBridge::allows_space_fullscreen(
        WS::CAPTION, 0, false, false));
    EXPECT_TRUE(CocoaBridge::allows_space_fullscreen(
        WS::THICKFRAME, 0, false, false));
    EXPECT_TRUE(CocoaBridge::allows_space_fullscreen(
        WS::POPUP, 0, false, true));
}

TEST_F(CocoaBridgeTest, SpaceFullscreenRejectsDialogToolChildPopup) {
    constexpr u32 kWsExToolwindow = 0x00000080u;
    EXPECT_FALSE(CocoaBridge::allows_space_fullscreen(
        WS::OVERLAPPEDWINDOW, 0, true, false));
    EXPECT_FALSE(CocoaBridge::allows_space_fullscreen(
        WS::OVERLAPPEDWINDOW, kWsExToolwindow, false, false));
    EXPECT_FALSE(CocoaBridge::allows_space_fullscreen(
        WS::CHILD | WS::CAPTION, 0, false, false));
    EXPECT_FALSE(CocoaBridge::allows_space_fullscreen(
        WS::POPUP, 0, false, false));
}

TEST_F(CocoaBridgeTest, ExclusiveFullscreenUsesSpaceCollection) {
    EXPECT_TRUE(CocoaBridge::uses_space_fullscreen_collection(
        true, WS::POPUP, 0, false, false));
    EXPECT_FALSE(CocoaBridge::uses_space_fullscreen_collection(
        false, WS::POPUP, 0, false, false));
    EXPECT_TRUE(CocoaBridge::uses_space_fullscreen_collection(
        false, WS::OVERLAPPEDWINDOW, 0, false, false));
}

TEST_F(CocoaBridgeTest, ExclusiveSpaceHidesSystemUiDocumentDoesNot) {
    EXPECT_TRUE(CocoaBridge::exclusive_space_hides_system_ui(true));
    EXPECT_FALSE(CocoaBridge::exclusive_space_hides_system_ui(false));
    bridge.apply_exclusive_space_system_ui(false);
}

TEST_F(CocoaBridgeTest, HostCursorHideKeepsInstalledGameCursor) {
    // A windowed SetCursor(NULL) is scoped by AppKit cursor rectangles; using
    // CGDisplayHideCursor here would also hide the pointer over other apps.
    EXPECT_FALSE(CocoaBridge::host_cursor_should_hide(true, false, false));
    EXPECT_TRUE(CocoaBridge::host_cursor_should_hide(true, true, true));
    EXPECT_TRUE(CocoaBridge::host_cursor_should_hide(true, true, false));
    EXPECT_FALSE(CocoaBridge::host_cursor_should_hide(false, true, false));
    EXPECT_FALSE(CocoaBridge::host_cursor_should_hide(false, true, true));
    EXPECT_FALSE(CocoaBridge::host_cursor_should_hide(false, false, false));
    EXPECT_FALSE(CocoaBridge::host_cursor_should_hide(false, false, true));
}

TEST_F(CocoaBridgeTest, HiddenCursorSurvivesExclusiveSpaceUi) {
    bridge.set_cursor_visible(false);
    EXPECT_TRUE(bridge.is_cursor_hidden());
    bridge.apply_exclusive_space_system_ui(true);
    EXPECT_TRUE(bridge.is_cursor_hidden());
    bridge.apply_host_cursor_visibility();
    EXPECT_TRUE(bridge.is_cursor_hidden());
    bridge.apply_exclusive_space_system_ui(false);
    bridge.set_cursor_visible(true);
    EXPECT_FALSE(bridge.is_cursor_hidden());
}

// Cmd-Tab away from a full-screen game: the host leaves the window alone.
// A ShowWindow re-sync of a window already on screen while another app is
// active neither raises nor activates it (Learn SetForegroundWindow: an app
// "cannot force a window to the foreground while the user is working with
// another window"); a first show, or one while the app is active, may.
TEST_F(CocoaBridgeTest, VisibilityResyncNeverRaisesOverTheActiveApp) {
    EXPECT_FALSE(CocoaBridge::visibility_sync_may_raise(false, false));
    EXPECT_TRUE(CocoaBridge::visibility_sync_may_raise(true, false));
    EXPECT_TRUE(CocoaBridge::visibility_sync_may_raise(false, true));
    EXPECT_TRUE(CocoaBridge::visibility_sync_may_raise(true, true));
}

// DXGI_STATUS_OCCLUDED follows AppKit's occlusion state
// (windowDidChangeOcclusionState / NSWindowOcclusionStateVisible): a window
// in a Space the user left is occluded, and visible again on return.
TEST_F(CocoaBridgeTest, OcclusionStateTracksAppKitVisibility) {
    constexpr HWND kGame = 0x7a0;
    constexpr HWND kOther = 0x7a1;
    EXPECT_FALSE(bridge.window_occluded(kGame)) << "unknown is not occluded";
    bridge.on_native_occlusion_changed(kGame, false);
    EXPECT_TRUE(bridge.window_occluded(kGame));
    EXPECT_FALSE(bridge.window_occluded(kOther));
    bridge.on_native_occlusion_changed(kGame, false);
    bridge.on_native_occlusion_changed(kGame, true);
    EXPECT_FALSE(bridge.window_occluded(kGame));
}

TEST_F(CocoaBridgeTest, SpaceReconcileWaitsForTransitionsAndVisibility) {
    using A = CocoaBridge::SpaceAction;
    // args: desired, in_space, in_transition, visible, app_active
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, false, false, true, true),
              A::Enter);
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, true, true, true, true),
              A::None) << "exit of the old Space still running";
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, false, true, true, true),
              A::None);
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, false, false, false, true),
              A::None) << "an ordered-out window forms no Space";
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, false, false, true, false),
              A::None) << "a background app does not pull the user to it";
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, true, false, true, true),
              A::None);
    EXPECT_EQ(CocoaBridge::space_reconcile_action(true, true, false, true, false),
              A::None) << "Cmd-Tab away keeps the Space untouched";
    EXPECT_EQ(CocoaBridge::space_reconcile_action(false, true, false, true, true),
              A::Exit);
    EXPECT_EQ(CocoaBridge::space_reconcile_action(false, true, false, false, false),
              A::Exit);
    EXPECT_EQ(CocoaBridge::space_reconcile_action(false, false, false, true, true),
              A::None);
}

// Cmd-Tab away from an exclusive-input game (ClipCursor, raw input, hidden
// cursor, re-focusing its viewport): nothing of the game stays above the app
// the user switched to, and the Space is left alone.
// - Learn SetForegroundWindow: a background process cannot take the
//   foreground (Windows flashes the taskbar button), unless it holds the
//   start grant.
// - Learn ClipCursor: the clip is released when the app is not foreground
//   and re-applied on reactivation; the display-wide cursor hide likewise.
// - A window in a Space is never raised; raising an owned window above a
//   full-screen sibling applies only while active.
// Learn (Raw Input overview, RAWINPUTDEVICE; IDirectInputDevice8::
// SetCooperativeLevel): raw input without RIDEV_INPUTSINK and DISCL_FOREGROUND
// devices deliver to the foreground application wherever the cursor is.
// Mouse look in a full-screen game kept stopping once the cursor crossed onto
// a second display, because only events over a guest window carried motion.
TEST_F(CocoaBridgeTest, MouseMotionOffGuestWindowsFollowsForeground) {
    EXPECT_TRUE(CocoaBridge::offwindow_motion_reaches_guest(true, true))
        << "cursor over another display while the game is foreground";
    EXPECT_FALSE(CocoaBridge::offwindow_motion_reaches_guest(false, true))
        << "another app is foreground: its motion is not the guest's";
    EXPECT_FALSE(CocoaBridge::offwindow_motion_reaches_guest(true, false))
        << "no guest window to receive it";
}

TEST_F(CocoaBridgeTest, DeactivatedExclusiveInputGameReleasesTheHost) {
    EXPECT_FALSE(CocoaBridge::foreground_request_allowed(false, false));
    EXPECT_TRUE(CocoaBridge::foreground_request_allowed(false, true));
    EXPECT_TRUE(CocoaBridge::foreground_request_allowed(true, false));

    EXPECT_TRUE(CocoaBridge::guest_clip_should_apply(true, true, true));
    EXPECT_FALSE(CocoaBridge::guest_clip_should_apply(true, false, true))
        << "released on deactivation";
    EXPECT_TRUE(CocoaBridge::guest_clip_should_apply(true, true, true))
        << "re-applied on reactivation (the guest's rectangle is kept)";

    EXPECT_TRUE(CocoaBridge::host_cursor_should_hide(true, true, false, true));
    EXPECT_FALSE(CocoaBridge::host_cursor_should_hide(true, true, false, false));

    using L = CocoaBridge::HostLevel;
    // args: topmost, in_space, above_fullscreen_sibling, app_active
    EXPECT_EQ(CocoaBridge::host_window_level(false, true, false, true), L::Normal);
    EXPECT_EQ(CocoaBridge::host_window_level(true, true, false, true), L::Normal)
        << "WS_EX_TOPMOST does not float a Space window";
    EXPECT_EQ(CocoaBridge::host_window_level(true, true, true, false), L::Normal);
    EXPECT_EQ(CocoaBridge::host_window_level(false, false, true, true),
              L::AboveFullscreen);
    EXPECT_EQ(CocoaBridge::host_window_level(false, false, true, false),
              L::Normal) << "dropped while another app is active";
    EXPECT_EQ(CocoaBridge::host_window_level(true, false, false, false),
              L::Floating);
    EXPECT_EQ(CocoaBridge::host_window_level(false, false, false, true),
              L::Normal);
}

// Learn WM_NCHITTEST / DefWindowProc WM_NCLBUTTONDOWN: only HTCAPTION moves a
// window. HTCLIENT (1), HTNOWHERE (0), HTTRANSPARENT (-1), HTERROR (-2) and
// the sizing borders are not a move; a window in a Space or in exclusive
// full-screen never moves.
TEST_F(CocoaBridgeTest, OnlyCaptionHitStartsAWindowDrag) {
    for (const i32 hit : {1, 0, -1, -2, 10, 12, 17}) {
        EXPECT_FALSE(CocoaBridge::guest_frame_drag_allowed(hit, true, false,
                                                           false))
            << hit;
    }
    EXPECT_TRUE(CocoaBridge::guest_frame_drag_allowed(2, true, false, false));
    EXPECT_FALSE(CocoaBridge::guest_frame_drag_allowed(2, false, false, false))
        << "native chrome drags through AppKit's own title bar";
    EXPECT_FALSE(CocoaBridge::guest_frame_drag_allowed(2, true, true, false))
        << "exclusive full-screen";
    EXPECT_FALSE(CocoaBridge::guest_frame_drag_allowed(2, true, false, true))
        << "in a Space";
}

// Learn ShowWindow (SW_SHOW activates) and SetForegroundWindow (the
// foreground process, or one it started, may take the foreground).
TEST_F(CocoaBridgeTest, FirstShowTakesKeyByForegroundRules) {
    // args: first_show, first_show_grant, show_without_activation, activating,
    //       session_child, foreground_process, owns_taskbar, preferred_ok
    // Session root, ordinary first show.
    EXPECT_TRUE(CocoaBridge::first_show_takes_key(
        true, false, false, true, false, true, true, true));
    // SW_SHOWNOACTIVATE / a non-activating recipe / a re-sync never key.
    EXPECT_FALSE(CocoaBridge::first_show_takes_key(
        true, false, true, true, false, true, true, true));
    EXPECT_FALSE(CocoaBridge::first_show_takes_key(
        true, false, false, false, false, true, true, true));
    EXPECT_FALSE(CocoaBridge::first_show_takes_key(
        false, false, false, true, false, true, true, true));
    // A background child (not foreground, no grant used) does not.
    EXPECT_FALSE(CocoaBridge::first_show_takes_key(
        true, false, false, true, true, false, true, true));
    // A child using its first-show grant does, while staying accessory.
    EXPECT_TRUE(CocoaBridge::first_show_takes_key(
        true, true, false, true, true, false, true, true));
    // The RV case: a child that is already the active application shows its
    // game window after the splash consumed the first-show grant, while the splash
    // is still the preferred key window -- the activating ShowWindow takes
    // activation from it.
    EXPECT_TRUE(CocoaBridge::first_show_takes_key(
        true, false, false, true, true, true, true, false));
    // ...but not for a window without taskbar identity (owned / tool).
    EXPECT_FALSE(CocoaBridge::first_show_takes_key(
        true, false, false, true, true, true, false, false));
    // The session root keeps its sticky preferred key window.
    EXPECT_FALSE(CocoaBridge::first_show_takes_key(
        true, false, false, true, false, true, true, false));
}

// The start grant travels down CreateProcess (explicitly 1 or 0, so a child
// never inherits its parent's value), and the active application yields to
// the whole tree it launched (Steam -> launcher -> game).
TEST_F(CocoaBridgeTest, ForegroundGrantCoversTheLaunchedProcessTree) {
    EXPECT_EQ(CocoaBridge::foreground_grant_env_entry(true),
              "VITREO_FOREGROUND_GRANT=1");
    EXPECT_EQ(CocoaBridge::foreground_grant_env_entry(false),
              "VITREO_FOREGROUND_GRANT=0");
    // steam(100) -> launcher(200) -> game(300); unrelated(400) under 1.
    const auto parent_of = [](i32 pid) -> i32 {
        switch (pid) {
        case 300: return 200;
        case 200: return 100;
        case 100: return 1;
        case 400: return 1;
        case 500: return 500;  // a corrupt self-parent entry
        default: return 0;
        }
    };
    EXPECT_TRUE(CocoaBridge::pid_descends_from(300, 200, parent_of));
    EXPECT_TRUE(CocoaBridge::pid_descends_from(200, 200, parent_of));
    EXPECT_TRUE(CocoaBridge::pid_descends_from(300, 100, parent_of));
    EXPECT_FALSE(CocoaBridge::pid_descends_from(400, 200, parent_of));
    EXPECT_FALSE(CocoaBridge::pid_descends_from(100, 200, parent_of));
    EXPECT_FALSE(CocoaBridge::pid_descends_from(500, 200, parent_of));
    EXPECT_FALSE(CocoaBridge::pid_descends_from(0, 200, parent_of));
}

// A guest ClipCursor may only hold the host cursor while the app is active and
// something is on screen. Minimising must free the cursor even though the
// guest still believes its clip is installed (Learn: the cursor is a shared
// resource; Windows drops the clip when the owner stops being foreground).
TEST_F(CocoaBridgeTest, GuestClipAppliesOnlyWhileActiveAndOnScreen) {
    EXPECT_TRUE(CocoaBridge::guest_clip_should_apply(true, true, true));
    // Minimised / ordered out: nothing on screen can own the cursor.
    EXPECT_FALSE(CocoaBridge::guest_clip_should_apply(true, true, false));
    // Cmd-Tabbed away.
    EXPECT_FALSE(CocoaBridge::guest_clip_should_apply(true, false, true));
    EXPECT_FALSE(CocoaBridge::guest_clip_should_apply(true, false, false));
    // No guest clip installed at all.
    EXPECT_FALSE(CocoaBridge::guest_clip_should_apply(false, true, true));
}
