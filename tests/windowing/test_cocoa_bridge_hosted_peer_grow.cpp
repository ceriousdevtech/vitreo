/// @file test_cocoa_bridge_hosted_peer_grow.cpp
/// @brief Tests for CocoaBridge hosted-peer child geometry when growing to the present surface size (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, HostedPeerChildGrowsFromPresentSurfaceSize) {
    // SetParent IPC often hosts the peer at 1×1 before the first DXGI present
    // size is known. The present path must grow that NSView or Metal fits into
    // one pixel and the parent window stays white.
    WindowData parent;
    parent.handle = 98;
    parent.rect = {0, 0, 1249, 1294};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(98, parent).is_ok());

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(98, 99, Rect{0, 0, 1, 1}));
    i32 w = 0;
    i32 h = 0;
    ASSERT_TRUE(bridge.control_view_size(99, w, h));
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);

    i32 x = 0;
    i32 y = 0;
    ASSERT_TRUE(bridge.control_view_frame(99, x, y, w, h));
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);
    // VtrContentView is flipped: Win32 (0,0,1,1) keeps Cocoa origin at (0,0).
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);

    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(99, 1249, 1294));
    ASSERT_TRUE(bridge.control_view_frame(99, x, y, w, h));
    EXPECT_EQ(w, 1249);
    EXPECT_EQ(h, 1294);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
}

TEST_F(CocoaBridgeTest, HostedPeerChildPresentResizesPlaceholderToLaterSurface) {
    // ANGLE/DComp often presents a probe swapchain (256×256) then replaces
    // the visual content with the real surface (700×440). SetContent draws
    // the new bitmap at its own size; the first Present must not freeze the
    // placeholder NSView.
    WindowData parent;
    parent.handle = 220;
    parent.rect = {0, 0, 1249, 1294};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(220, parent).is_ok());

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(220, 221, Rect{0, 0, 1, 1}));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(221, 256, 256));
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;
    ASSERT_TRUE(bridge.control_view_frame(221, x, y, w, h));
    EXPECT_EQ(w, 256);
    EXPECT_EQ(h, 256);

    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(221, 1024, 768));
    ASSERT_TRUE(bridge.control_view_frame(221, x, y, w, h));
    EXPECT_EQ(w, 1024);
    EXPECT_EQ(h, 768);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);

    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(221, 700, 440));
    ASSERT_TRUE(bridge.control_view_frame(221, x, y, w, h));
    EXPECT_EQ(w, 700);
    EXPECT_EQ(h, 440);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
}

TEST_F(CocoaBridgeTest, HostedPeerChildPresentDoesNotGrowRealWindowSize) {
    // IDXGISwapChain::Present draws into the HWND client; it does not
    // SetWindowPos. A popup/dropdown already sized by CreateWindow must not
    // expand to the compositor surface and cover sibling content.
    WindowData parent;
    parent.handle = 210;
    parent.rect = {0, 0, 1249, 1294};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(210, parent).is_ok());

    ASSERT_TRUE(bridge.ensure_hosted_peer_child(
        210, 211, Rect{80, 60, 80 + 600, 60 + 120}));
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;
    ASSERT_TRUE(bridge.control_view_frame(211, x, y, w, h));
    EXPECT_EQ(x, 80);
    EXPECT_EQ(y, 60);
    EXPECT_EQ(w, 600);
    EXPECT_EQ(h, 120);

    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(211, 1249, 1294));
    ASSERT_TRUE(bridge.control_view_frame(211, x, y, w, h));
    EXPECT_EQ(x, 80);
    EXPECT_EQ(y, 60);
    EXPECT_EQ(w, 600);
    EXPECT_EQ(h, 120);
}

TEST_F(CocoaBridgeTest, HostedPeerChildGrowKeepsWin32TopLeftOnUnflippedParent) {
    // Plain NSView parents are Y-up. A 1×1 host at Win32 (0,0) sits at
    // cocoa y=parent_h-1; growing height must recompute Y or clipsToBounds
    // hides the DXGI surface under the parent.
    WindowData parent;
    parent.handle = 198;
    parent.rect = {0, 0, 400, 300};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(198, parent).is_ok());

    // Force an unflipped intermediate host so the grow path exercises Y-up math.
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(198, 199, Rect{0, 0, 400, 300}));
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(199, 200, Rect{0, 0, 1, 1}));

    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;
    ASSERT_TRUE(bridge.control_view_frame(200, x, y, w, h));
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);
    // Parent peer host is a plain NSView (not flipped).
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 299);

    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(200, 400, 300));
    ASSERT_TRUE(bridge.control_view_frame(200, x, y, w, h));
    EXPECT_EQ(w, 400);
    EXPECT_EQ(h, 300);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
}

TEST_F(CocoaBridgeTest, HostedPeerChildSameSizePresentKeepsTopLeftAfterParentResize) {
    WindowData parent;
    parent.handle = 240;
    parent.rect = {0, 0, 400, 300};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(240, parent).is_ok());
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(240, 241, Rect{0, 0, 400, 300}));
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(241, 242, Rect{0, 0, 1, 1}));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(242, 132, 165));

    // The parent resizes after the first present; the surface keeps its size.
    // Windows child coordinates remain relative to the parent's upper left.
    ASSERT_TRUE(bridge.set_control_frame(241, Rect{0, 0, 400, 200}));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(242, 132, 165));
    i32 x = 0, y = 0, w = 0, h = 0;
    ASSERT_TRUE(bridge.control_view_frame(242, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 35);
    EXPECT_EQ(w, 132);
    EXPECT_EQ(h, 165);
}

TEST_F(CocoaBridgeTest, HostedPeerChildRepositionDoesNotMakeRealSizePresentationOwned) {
    WindowData parent;
    parent.handle = 243;
    parent.rect = {0, 0, 400, 300};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(243, parent).is_ok());
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(243, 244, Rect{0, 0, 400, 300}));
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(244, 245, Rect{0, 0, 132, 165}));
    ASSERT_TRUE(bridge.set_control_frame(244, Rect{0, 0, 400, 200}));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(245, 132, 165));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(245, 512, 512));
    i32 x = 0, y = 0, w = 0, h = 0;
    ASSERT_TRUE(bridge.control_view_frame(245, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 35);
    EXPECT_EQ(w, 132);
    EXPECT_EQ(h, 165);
}

TEST_F(CocoaBridgeTest, HostedPeerChildGrowExpandsPlaceholderAncestor) {
    WindowData top;
    top.handle = 230;
    top.rect = {0, 0, 700, 440};
    top.client_rect = top.rect;
    ASSERT_TRUE(bridge.create_window(230, top).is_ok());

    // CEF creates this browser-owned compositor container at its initial
    // 1024x768 probe size, even though the visible window is 700x440.
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(
        230, 231, Rect{0, 0, 1024, 768}));
    ASSERT_TRUE(bridge.ensure_hosted_peer_child(
        231, 232, Rect{0, 0, 1, 1}));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(
        232, 1024, 768));

    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;
    ASSERT_TRUE(bridge.control_view_frame(231, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 1024);
    EXPECT_EQ(h, 768);
    ASSERT_TRUE(bridge.control_view_frame(232, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 1024);
    EXPECT_EQ(h, 768);

    // A later SetContent surface replaces the probe and resizes the whole
    // presentation placeholder chain, not just the leaf.
    // In production the browser resizes its parent HWND first, leaving the
    // old 1024x768 child temporarily larger than the 700x440 parent.
    ASSERT_TRUE(bridge.set_control_frame(231, Rect{0, 0, 700, 440}));
    ASSERT_TRUE(bridge.grow_hosted_peer_child_to_present_size(
        232, 700, 440));
    ASSERT_TRUE(bridge.control_view_frame(231, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 700);
    EXPECT_EQ(h, 440);
    ASSERT_TRUE(bridge.control_view_frame(232, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 700);
    EXPECT_EQ(h, 440);
}
