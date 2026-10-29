/// @file test_cocoa_bridge_input.cpp
/// @brief Tests for CocoaBridge client mouse target resolution and modifier-key translation (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, ClientMouseInputNegotiatesCursorFirst) {
    MessageQueue queue;
    constexpr HWND target = 0x1234;

    bridge.post_client_mouse_message(
        queue, target, WM::MOUSEMOVE, 0x5u, 0x00200010u);

    MSG cursor{};
    ASSERT_TRUE(queue.peek(cursor, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(cursor.hwnd, target);
    EXPECT_EQ(cursor.message, WM::SETCURSOR);
    EXPECT_EQ(cursor.wParam, target);
    EXPECT_EQ(cursor.lParam & 0xFFFFu, 1u); // HTCLIENT
    EXPECT_EQ(cursor.lParam >> 16, WM::MOUSEMOVE);

    MSG mouse{};
    ASSERT_TRUE(queue.peek(mouse, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(mouse.hwnd, target);
    EXPECT_EQ(mouse.message, WM::MOUSEMOVE);
    EXPECT_EQ(mouse.wParam, 0x5u);
    EXPECT_EQ(mouse.lParam, 0x00200010u);
    EXPECT_EQ(mouse.pt.x, 16);
    EXPECT_EQ(mouse.pt.y, 32);
}

TEST_F(CocoaBridgeTest, ClientMouseTargetsCompositionChild) {
    WindowData parent;
    parent.handle = 50;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(50, parent).is_ok());

    WindowData child;
    child.handle = 51;
    child.parent = 50;
    child.rect = {10, 20, 210, 220};
    child.client_rect = {0, 0, 200, 200};
    child.visible = true;
    bridge.add_container_view(50, 51, child);
    ASSERT_NE(bridge.native_presentation_view(51), nullptr);

    MessageQueue queue;
    bridge.post_client_mouse_message(
        queue, 50, WM::MOUSEMOVE, 0, 0x0046003Cu); // parent client (60,70)

    MSG cursor{};
    ASSERT_TRUE(queue.peek(cursor, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(cursor.hwnd, 51u);
    EXPECT_EQ(cursor.wParam, 51u);
    EXPECT_EQ(cursor.message, WM::SETCURSOR);

    MSG mouse{};
    ASSERT_TRUE(queue.peek(mouse, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(mouse.hwnd, 51u);
    EXPECT_EQ(mouse.message, WM::MOUSEMOVE);
    EXPECT_EQ(static_cast<i16>(mouse.lParam & 0xFFFFu), 50);
    EXPECT_EQ(static_cast<i16>((mouse.lParam >> 16) & 0xFFFFu), 50);
    EXPECT_EQ(mouse.pt.x, 60);
    EXPECT_EQ(mouse.pt.y, 70);
}

TEST_F(CocoaBridgeTest, ClientMouseTargetsForeignPixelChild) {
    WindowData parent;
    parent.handle = 60;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(60, parent).is_ok());

    WindowData child;
    child.handle = 61;
    child.parent = 60;
    child.rect = {10, 20, 210, 220};
    child.client_rect = {0, 0, 200, 200};
    child.visible = true;
    child.wndproc = 0x1234;
    bridge.add_child_view(60, 61, child, 0, L"Chrome_WidgetWin_0", L"");

    std::vector<u8> pixels(200u * 200u * 4u, 0x80u);
    bridge.update_window_pixels(
        61, pixels.data(), 200, 200, /*above_native_children=*/true);
    ASSERT_TRUE(bridge.has_guest_pixel_surface(61));

    MessageQueue queue;
    bridge.post_client_mouse_message(
        queue, 60, WM::MOUSEMOVE, 0, 0x0046003Cu); // parent client (60,70)

    MSG cursor{};
    ASSERT_TRUE(queue.peek(cursor, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(cursor.hwnd, 61u);
    EXPECT_EQ(cursor.wParam, 61u);

    MSG mouse{};
    ASSERT_TRUE(queue.peek(mouse, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(mouse.hwnd, 61u);
    EXPECT_EQ(static_cast<i16>(mouse.lParam & 0xFFFFu), 50);
    EXPECT_EQ(static_cast<i16>((mouse.lParam >> 16) & 0xFFFFu), 50);
}

// Learn, TrackMouseEvent/WM_MOUSELEAVE: the leave is posted when the cursor
// leaves the client area of the window passed to TrackMouseEvent. A guest
// arms tracking on the child that receives its WM_MOUSEMOVE; moves that stay
// inside that child must not post WM_MOUSELEAVE, and leaving it must.
TEST_F(CocoaBridgeTest, MouseLeaveTrackingFollowsTheRoutedChild) {
    WindowData parent;
    parent.handle = 64;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(64, parent).is_ok());

    WindowData child;
    child.handle = 65;
    child.parent = 64;
    child.rect = {10, 20, 210, 220};
    child.client_rect = {0, 0, 200, 200};
    child.visible = true;
    child.wndproc = 0x1234;
    bridge.add_child_view(64, 65, child, 0, L"Chrome_WidgetWin_0", L"");
    std::vector<u8> pixels(200u * 200u * 4u, 0x80u);
    bridge.update_window_pixels(
        65, pixels.data(), 200, 200, /*above_native_children=*/true);

    auto drain = [](MessageQueue& queue, HWND hwnd, u32 message) {
        u32 n = 0;
        MSG msg{};
        while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE))
            if (msg.hwnd == hwnd && msg.message == message) ++n;
        return n;
    };

    MessageQueue queue;
    bridge.track_mouse_event(65, /*leave=*/true, /*hover=*/false, 0);
    bridge.post_client_mouse_message(queue, 64, WM::MOUSEMOVE, 0, 0x0046003Cu);
    bridge.post_client_mouse_message(queue, 64, WM::MOUSEMOVE, 0, 0x00500040u);
    EXPECT_EQ(drain(queue, 65, WM::MOUSELEAVE), 0u);
    bool leave = false, hover = false;
    u32 hover_ms = 0;
    bridge.query_mouse_tracking(65, leave, hover, hover_ms);
    EXPECT_TRUE(leave);

    // Parent client (5,5) is outside the child: exactly one leave, and the
    // request is cancelled by its own delivery.
    bridge.post_client_mouse_message(queue, 64, WM::MOUSEMOVE, 0, 0x00050005u);
    bridge.post_client_mouse_message(queue, 64, WM::MOUSEMOVE, 0, 0x00060006u);
    EXPECT_EQ(drain(queue, 65, WM::MOUSELEAVE), 1u);
    bridge.query_mouse_tracking(65, leave, hover, hover_ms);
    EXPECT_FALSE(leave);
}

TEST_F(CocoaBridgeTest, ForeignPixelSurfaceOwnsInputAboveNativeDescendants) {
    WindowData parent;
    parent.handle = 70;
    parent.rect = {0, 0, 400, 760};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(70, parent).is_ok());

    WindowData surface_owner;
    surface_owner.handle = 71;
    surface_owner.parent = 70;
    surface_owner.rect = {6, 58, 354, 704};
    surface_owner.client_rect = {0, 0, 348, 646};
    surface_owner.visible = true;
    surface_owner.wndproc = 0x1234;
    bridge.add_child_view(
        70, 71, surface_owner, 0, L"ForeignSurfaceOwner", L"");

    WindowData native_descendant;
    native_descendant.handle = 72;
    native_descendant.parent = 71;
    native_descendant.rect = {0, 0, 348, 646};
    native_descendant.client_rect = native_descendant.rect;
    native_descendant.visible = true;
    native_descendant.wndproc = 0x5678;
    bridge.add_child_view(
        71, 72, native_descendant, 0, L"NativeDescendant", L"");

    std::vector<u8> pixels(348u * 646u * 4u, 0x80u);
    bridge.update_window_pixels(
        71, pixels.data(), 348, 646, /*above_native_children=*/true);

    MessageQueue queue;
    bridge.post_client_mouse_message(
        queue, 70, WM::MOUSEMOVE, 0,
        (200u << 16) | 100u); // top-level client (100,200)

    MSG cursor{};
    ASSERT_TRUE(queue.peek(cursor, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(cursor.hwnd, 71u);
    EXPECT_EQ(cursor.wParam, 71u);

    MSG mouse{};
    ASSERT_TRUE(queue.peek(mouse, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(mouse.hwnd, 71u);
    EXPECT_EQ(static_cast<i16>(mouse.lParam & 0xFFFFu), 94);
    EXPECT_EQ(static_cast<i16>((mouse.lParam >> 16) & 0xFFFFu), 142);
}

TEST_F(CocoaBridgeTest, ForeignPixelSurfaceDoesNotCoverParentChrome) {
    WindowData parent;
    parent.handle = 80;
    parent.rect = {0, 0, 362, 719};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(80, parent).is_ok());

    WindowData browser;
    browser.handle = 81;
    browser.parent = 80;
    browser.rect = {6, 32, 354, 678};
    browser.client_rect = {0, 0, 348, 646};
    browser.visible = true;
    browser.wndproc = 0x1234;
    bridge.add_child_view(
        80, 81, browser, 0, L"Chrome_WidgetWin_0", L"");

    std::vector<u8> pixels(348u * 646u * 4u, 0x80u);
    bridge.update_window_pixels(
        81, pixels.data(), 348, 646, /*above_native_children=*/true);

    MessageQueue queue;
    bridge.post_client_mouse_message(
        queue, 80, WM::MOUSEMOVE, 0,
        (12u << 16) | 340u); // Parent close button, above browser child.

    MSG cursor{};
    ASSERT_TRUE(queue.peek(cursor, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(cursor.hwnd, 80u);

    MSG mouse{};
    ASSERT_TRUE(queue.peek(mouse, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(mouse.hwnd, 80u);
}

TEST_F(CocoaBridgeTest, TopLevelOwnsPixelSurfaceDistinctFromBrowserChild) {
    WindowData parent;
    parent.handle = 90;
    parent.rect = {0, 0, 362, 719};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(90, parent).is_ok());

    WindowData browser;
    browser.handle = 91;
    browser.parent = 90;
    browser.rect = {6, 32, 354, 678};
    browser.client_rect = {0, 0, 348, 646};
    browser.visible = true;
    browser.wndproc = 0x1234;
    bridge.add_child_view(
        90, 91, browser, 0, L"Chrome_WidgetWin_0", L"");

    std::vector<u8> browser_pixels(348u * 646u * 4u, 0x80u);
    bridge.update_window_pixels(
        91, browser_pixels.data(), 348, 646,
        /*above_native_children=*/true);

    std::vector<u8> parent_pixels(362u * 719u * 4u, 0x20u);
    bridge.update_window_pixels(
        90, parent_pixels.data(), 362, 719);

    EXPECT_TRUE(bridge.has_guest_pixel_surface(91));
    EXPECT_TRUE(bridge.has_guest_pixel_surface(90));
}

TEST_F(CocoaBridgeTest, ForeignPresentDoesNotReplaceTopLevelGuestPixels) {
    WindowData parent;
    parent.handle = 92;
    parent.rect = {0, 0, 348, 646};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(92, parent).is_ok());

    std::vector<u8> pixels(348u * 646u * 4u, 0x22u);
    bridge.update_window_pixels(92, pixels.data(), 348, 646);
    ASSERT_TRUE(bridge.has_guest_pixel_surface(92));

    EXPECT_EQ(bridge.native_foreign_presentation_view(92), nullptr);
    EXPECT_TRUE(bridge.has_guest_pixel_surface(92));
}

// ============================================================================
// Modifier Key Transition Tests
// ============================================================================
//
// macOS delivers modifier press/release as NSEventTypeFlagsChanged rather than
// keyDown/keyUp. These cover the translation into Windows key messages, which
// Ctrl-based application hotkeys (StarCraft control groups) depend on.

namespace {
// NSEventModifierFlags values, spelled out so the test does not need AppKit.
constexpr u64 kModShift   = 1ull << 17;
constexpr u64 kModControl = 1ull << 18;
constexpr u64 kModOption  = 1ull << 19;

constexpr u32 kVkShift   = 0x10;
constexpr u32 kVkControl = 0x11;
constexpr u32 kVkMenu    = 0x12;
} // namespace

TEST_F(CocoaBridgeTest, FlagsChangedPostsControlKeyDownThenUp) {
    MessageQueue queue;
    constexpr HWND target = 0x1234;

    bridge.post_modifier_transitions(target, kModControl, queue);
    MSG msg{};
    ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.message, WM::KEYDOWN);
    EXPECT_EQ(msg.wParam, kVkControl);
    EXPECT_EQ(msg.hwnd, target);
    // The key-state APIs must agree with the message that was posted.
    EXPECT_EQ(bridge.get_native_key_state(kVkControl),
              static_cast<i16>(0x8000));

    // Releasing the modifier reports the matching key-up exactly once.
    bridge.post_modifier_transitions(target, 0, queue);
    ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.message, WM::KEYUP);
    EXPECT_EQ(msg.wParam, kVkControl);
}

TEST_F(CocoaBridgeTest, FlagsChangedIgnoresUnchangedModifiers) {
    MessageQueue queue;
    constexpr HWND target = 0x1234;

    bridge.post_modifier_transitions(target, kModShift, queue);
    MSG msg{};
    ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.wParam, kVkShift);

    // A second event with the same mask is not a transition: macOS re-sends
    // flagsChanged for every modifier change, so repeats must not enqueue.
    bridge.post_modifier_transitions(target, kModShift, queue);
    EXPECT_FALSE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
}

TEST_F(CocoaBridgeTest, FlagsChangedReportsEachModifierSeparately) {
    MessageQueue queue;
    constexpr HWND target = 0x1234;

    bridge.post_modifier_transitions(target, kModControl | kModOption, queue);
    MSG first{};
    MSG second{};
    ASSERT_TRUE(queue.peek(first, NULL_HWND, 0, 0, PM::REMOVE));
    ASSERT_TRUE(queue.peek(second, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(first.message, WM::KEYDOWN);
    EXPECT_EQ(second.message, WM::KEYDOWN);
    EXPECT_EQ(first.wParam, kVkControl);
    EXPECT_EQ(second.wParam, kVkMenu);
    EXPECT_EQ(bridge.get_native_key_state(kVkMenu), static_cast<i16>(0x8000));
}

// Learn (SetCapture): while a window holds the capture, mouse lParam is in
// THAT window's client coordinates. A list box captures on WM_LBUTTONDOWN, so
// its WM_LBUTTONUP must arrive in the list's own client space — the same item
// the down hit — even though the event enters as the top-level's client point.
// Before, a child capture kept the top-level point (off by the list's
// position) and Inno's check list never toggled on a click.
TEST_F(CocoaBridgeTest, ChildCaptureReceivesItsOwnClientCoordinates) {
    WindowData parent;
    parent.handle = 140;
    parent.rect = {0, 0, 400, 300};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(140, parent).is_ok());
    WindowData list;
    list.handle = 141;
    list.parent = 140;
    list.style = WS::CHILD | WS::VISIBLE | 0x0020 | 0x0040;
    list.rect = {40, 109, 240, 209};
    list.client_rect = {0, 0, 200, 100};
    list.visible = true;
    list.wndproc = 0x401000;
    bridge.add_child_view(140, 141, list, 0x0083, L"ListBox", L"");
    std::vector<u8> pixels(200u * 100u * 4u, 0xFF);
    bridge.update_window_pixels(141, pixels.data(), 200, 100);
    bridge.set_control_owns_input(141);

    auto post = [&](u32 message, i32 x, i32 y) {
        MessageQueue queue;
        bridge.post_client_mouse_message(
            queue, 140, message, 0,
            (static_cast<u64>(static_cast<u16>(y)) << 16) | static_cast<u16>(x));
        MSG msg{};
        while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
            if (msg.message == message) return msg;
        }
        ADD_FAILURE() << "no message 0x" << std::hex << message;
        return MSG{};
    };
    const MSG down = post(WM::LBUTTONDOWN, 49, 144);
    EXPECT_EQ(down.hwnd, 141u);
    EXPECT_EQ(down.lParam, (35u << 16) | 9u);

    bridge.set_mouse_capture(141);
    const MSG up = post(WM::LBUTTONUP, 49, 144);
    EXPECT_EQ(up.hwnd, 141u);
    EXPECT_EQ(up.lParam, (35u << 16) | 9u);
    // Released outside the list: still the list's, in its (negative) space.
    const MSG outside = post(WM::LBUTTONUP, 20, 100);
    EXPECT_EQ(outside.hwnd, 141u);
    EXPECT_EQ(static_cast<i16>(outside.lParam & 0xFFFF), -20);
    EXPECT_EQ(static_cast<i16>((outside.lParam >> 16) & 0xFFFF), -9);
    bridge.set_mouse_capture(NULL_HWND);
}
