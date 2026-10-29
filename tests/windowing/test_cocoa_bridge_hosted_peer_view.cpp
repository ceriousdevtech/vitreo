/// @file test_cocoa_bridge_hosted_peer_view.cpp
/// @brief Tests for CocoaBridge hosted-peer and foreign presentation view hosting, z-order and mouse targeting (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, ForeignPresentHostsMetalOnNegativelyPositionedChild) {
    // Learn: WS_CHILD is clipped to the parent client. CEF DComp children are
    // often placed with a negative Y under custom title chrome; the Metal
    // surface must stay parented to that child so it cannot cover chrome.
    WindowData parent;
    parent.handle = 93;
    parent.rect = {0, 0, 362, 719};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(93, parent).is_ok());

    WindowData compositor;
    compositor.handle = 94;
    compositor.parent = 93;
    compositor.rect = {0, -166, 348, 480};
    compositor.client_rect = {0, 0, 348, 646};
    compositor.visible = true;
    compositor.wndproc = 0x1234;
    bridge.add_child_view(
        93, 94, compositor, 0, L"Chrome_WidgetWin_1", L"");

    ASSERT_NE(bridge.native_foreign_presentation_view(94), nullptr);
    EXPECT_TRUE(bridge.foreign_presentation_is_control_local(94));
}

TEST_F(CocoaBridgeTest, HostedPeerChildGoesOnTopLikeSetParent) {
    // Cross-process SetParent cannot move the GPU process's NSView, so the
    // browser hosts a peer child under the CEF parent. SetParent puts the
    // moved window at the TOP of its new parent's child z-order
    // (tools/oracle_probe/setparent_zorder_probe.expected). This test used to
    // pin the host BELOW local siblings, which let Chrome's Legacy window
    // bury every popup's GPU content under its unpainted plate.
    WindowData parent;
    parent.handle = 95;
    parent.rect = {0, 0, 348, 646};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(95, parent).is_ok());

    WindowData sibling;
    sibling.handle = 96;
    sibling.parent = 95;
    sibling.rect = {0, 0, 348, 646};
    sibling.client_rect = sibling.rect;
    sibling.visible = true;
    sibling.wndproc = 0x1234;
    bridge.add_child_view(95, 96, sibling, 0, L"Chrome_WidgetWin_0", L"");

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(
        95, 97, Rect{0, 0, 348, 646}));
    EXPECT_TRUE(bridge.has_control_view(97));
    ASSERT_NE(bridge.native_foreign_presentation_view(97), nullptr);
    EXPECT_TRUE(bridge.foreign_presentation_is_control_local(97));
    EXPECT_TRUE(bridge.control_view_is_above(97, 96));
}

TEST_F(CocoaBridgeTest, TransparentChildComposesBeneathItsSiblings) {
    // tools/oracle_probe/child_surface_order_probe.cpp (Windows 11, 48 cases).
    // A WS_EX_TRANSPARENT child never covers a sibling, even at HWND_TOP and
    // even when it erases or fills its whole area. That holds for GDI, layered
    // and DirectComposition siblings. A non-transparent sibling at HWND_TOP
    // does cover. Chrome raises its WS_EX_TRANSPARENT "Legacy Window" with
    // SetWindowPos(HWND_TOP, SWP_NOREDRAW) on every resize.
    WindowData parent;
    parent.handle = 230;
    parent.rect = {0, 0, 200, 120};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(230, parent).is_ok());

    WindowData legacy;
    legacy.handle = 231;
    legacy.parent = 230;
    legacy.rect = {0, 0, 200, 120};
    legacy.client_rect = legacy.rect;
    legacy.visible = true;
    legacy.wndproc = 0x1234;
    legacy.ex_style = 0x00000020u;  // WS_EX_TRANSPARENT
    bridge.add_child_view(230, 231, legacy, 0, L"Chrome_RenderWidgetHostHWND", L"");

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(230, 232, Rect{0, 0, 200, 120}));
    EXPECT_TRUE(bridge.control_view_is_above(232, 231));

    // Raising the transparent child does not put it over the surface child.
    ASSERT_TRUE(bridge.set_control_z_order(231, HWND_TOP));
    EXPECT_TRUE(bridge.control_view_is_above(232, 231));

    // A non-transparent sibling raised to the top does cover it...
    WindowData opaque = legacy;
    opaque.handle = 233;
    opaque.ex_style = 0;
    bridge.add_child_view(230, 233, opaque, 0, L"OpaqueChild", L"");
    ASSERT_TRUE(bridge.set_control_z_order(233, HWND_TOP));
    EXPECT_TRUE(bridge.control_view_is_above(233, 232));
    // ...and the transparent child stays beneath both.
    ASSERT_TRUE(bridge.set_control_z_order(231, HWND_TOP));
    EXPECT_TRUE(bridge.control_view_is_above(233, 231));
    EXPECT_TRUE(bridge.control_view_is_above(232, 231));
}

TEST_F(CocoaBridgeTest, HostedPeerChildRaisesAboveSiblingAfterPresent) {
    // SetParent puts the peer host on top (setparent_zorder_probe.expected),
    // and a later raise of the compositor child keeps it there.
    WindowData parent;
    parent.handle = 100;
    parent.rect = {0, 0, 348, 646};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(100, parent).is_ok());

    WindowData legacy;
    legacy.handle = 101;
    legacy.parent = 100;
    legacy.rect = {0, 0, 348, 646};
    legacy.client_rect = legacy.rect;
    legacy.visible = true;
    legacy.wndproc = 0x1234;
    bridge.add_child_view(100, 101, legacy, 0, L"Chrome_WidgetWin_0", L"");

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(100, 102, Rect{0, 0, 348, 646}));
    EXPECT_TRUE(bridge.control_view_is_above(102, 101));
    ASSERT_TRUE(bridge.set_control_z_order(101, HWND_TOP));
    EXPECT_TRUE(bridge.control_view_is_above(101, 102));
    ASSERT_TRUE(bridge.set_control_z_order(102, HWND_TOP));
    EXPECT_TRUE(bridge.control_view_is_above(102, 101));
}

TEST_F(CocoaBridgeTest, ShowSurvivesPresentationViewRecreation) {
    // set_presentation_view_hidden caches what it last pushed, so the peer
    // present path does not take a synchronous main-thread round trip per
    // child per frame just to re-assert "visible".
    //
    // A presentation view is CREATED HIDDEN, and views are detached and
    // recreated for the same HWND during a run. A cache keyed only on the HWND
    // therefore describes the OLD view after a recreation, and the next show
    // looks redundant and is skipped -- leaving the new view hidden forever.
    // That is a window that never appears, which is what dropdowns and popups
    // failing to show looks like.
    WindowData parent;
    parent.handle = 130;
    parent.rect = {0, 0, 800, 600};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(130, parent).is_ok());

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(130, 131, Rect{0, 0, 400, 300}));
    ASSERT_NE(bridge.native_foreign_presentation_view(131), nullptr);

    // First show: the cache is empty, so this does the work.
    EXPECT_TRUE(bridge.set_presentation_view_hidden(131, false));
    EXPECT_FALSE(bridge.presentation_view_is_hidden(131));

    // Recreate the view for the same HWND, exactly as a detach/reattach does.
    // The replacement starts hidden.
    bridge.destroy_window(131);
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(130, 131, Rect{0, 0, 400, 300}));
    ASSERT_NE(bridge.native_foreign_presentation_view(131), nullptr);

    // Second show must reach the NEW view. Against the stale cache this
    // returned false and left the window invisible.
    (void)bridge.set_presentation_view_hidden(131, false);
    EXPECT_FALSE(bridge.presentation_view_is_hidden(131))
        << "the recreated presentation view is still hidden: the cached "
           "hidden-state outlived the view it described";
}

TEST_F(CocoaBridgeTest, HostedPeerPresentDoesNotStealMouseTarget) {
    // Peer compositor HWNDs have no local wndproc. Retargeting clicks there
    // makes MessageLoop drop them — Chrome UI looks painted but dead.
    WindowData parent;
    parent.handle = 103;
    parent.rect = {0, 0, 800, 600};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(103, parent).is_ok());

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(103, 104, Rect{0, 0, 800, 600}));
    void* peer_view = bridge.native_foreign_presentation_view(104);
    ASSERT_NE(peer_view, nullptr);
    (void)bridge.set_presentation_view_hidden(104, false);
    ASSERT_TRUE(bridge.set_control_z_order(104, HWND_TOP));
    EXPECT_TRUE(bridge.is_hosted_peer_child(104));

    HWND containing = 0;
    EXPECT_EQ(bridge.resolve_client_mouse_target(103, 100, 100, &containing),
              103u);
    // The peer has no wndproc in this process, so mouse input stays locally
    // dispatchable while WM_SETCURSOR.wParam still identifies the HWND under
    // the pointer, as required by the Windows contract.
    EXPECT_EQ(containing, 104u);
}

TEST_F(CocoaBridgeTest, HostedPeerPresentTargetsNearestLocalAncestor) {
    WindowData top;
    top.handle = 104;
    top.rect = {0, 0, 800, 600};
    top.client_rect = top.rect;
    ASSERT_TRUE(bridge.create_window(104, top).is_ok());

    WindowData browser;
    browser.handle = 105;
    browser.parent = 104;
    browser.rect = {20, 30, 780, 580};
    browser.client_rect = {0, 0, 760, 550};
    browser.visible = true;
    browser.wndproc = 0x1234;
    bridge.add_child_view(
        104, 105, browser, 0, L"Chrome_WidgetWin_0", L"");

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(
        105, 106, Rect{0, 0, 760, 550}));
    void* peer_view = bridge.native_foreign_presentation_view(106);
    ASSERT_NE(peer_view, nullptr);
    ASSERT_TRUE(bridge.set_control_z_order(106, HWND_TOP));

    HWND containing = 0;
    EXPECT_EQ(bridge.resolve_client_mouse_target(104, 100, 100, &containing),
              105u);
    // The transport presentation view is hidden after an out-of-process
    // compositor handoff. Its visible hosted-peer container still identifies
    // the Windows HWND containing the pointer for WM_SETCURSOR.
    EXPECT_EQ(containing, 106u);
}
