/// @file test_cocoa_bridge_window_surface.cpp
/// @brief Tests for CocoaBridge host window opacity, hit-testing and guest pixel surfaces (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, GdiFramebufferPresentsTopDown) {
    // SoftBitmap row 0 is the visual top. A flipped layer-hosting VtrPixelView
    // inverted GDI flushes (installer wizard pages). Capture must keep the
    // red top row above the blue bottom row.
    WindowData data;
    data.handle = 270;
    data.style = WS::POPUP;
    data.rect = {80, 60, 80 + 32, 60 + 32};
    data.client_rect = {0, 0, 32, 32};
    data.visible = true;
    ASSERT_TRUE(bridge.create_window(270, data).is_ok());
    bridge.set_visible(270, true);

    constexpr i32 w = 32;
    constexpr i32 h = 32;
    std::vector<u8> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u,
                           0);
    auto put = [&](i32 x, i32 y, u8 r, u8 g, u8 b) {
        const size_t i =
            (static_cast<size_t>(y) * static_cast<size_t>(w) +
             static_cast<size_t>(x)) *
            4u;
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    };
    for (i32 x = 0; x < w; ++x) {
        put(x, 0, 255, 0, 0);
        put(x, h - 1, 0, 0, 255);
    }
    bridge.update_window_pixels(270, pixels.data(), w, h);

    std::vector<u8> captured;
    i32 cw = 0;
    i32 ch = 0;
    ASSERT_TRUE(bridge.capture_window_rgba(270, captured, cw, ch));
    ASSERT_GE(cw, 1);
    ASSERT_GE(ch, 1);
    ASSERT_EQ(captured.size(), static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4u);

    auto sample = [&](i32 x, i32 y) {
        const size_t i =
            (static_cast<size_t>(y) * static_cast<size_t>(cw) +
             static_cast<size_t>(x)) *
            4u;
        return std::array<u8, 4>{captured[i], captured[i + 1], captured[i + 2],
                                 captured[i + 3]};
    };
    const auto top = sample(cw / 2, 0);
    const auto bottom = sample(cw / 2, ch - 1);
    EXPECT_GT(top[0], top[2]) << "top row should stay red";
    EXPECT_GT(bottom[2], bottom[0]) << "bottom row should stay blue";
}

TEST_F(CocoaBridgeTest, CompositionHostClearOnlyForLayeredPopup) {
    constexpr u32 kWsExLayered = 0x00080000u;
    EXPECT_FALSE(CocoaBridge::composition_host_should_be_clear(
        WS::POPUP, 0, 480, 200, 1920, 1080));
    EXPECT_FALSE(CocoaBridge::composition_host_should_be_clear(
        WS::POPUP, 0, 1920, 1080, 1920, 1080));
    EXPECT_FALSE(CocoaBridge::composition_host_should_be_clear(
        WS::POPUP, 0, 1900, 1060, 1920, 1080));
    EXPECT_TRUE(CocoaBridge::composition_host_should_be_clear(
        WS::POPUP, kWsExLayered, 480, 200, 1920, 1080));
    EXPECT_FALSE(CocoaBridge::composition_host_should_be_clear(
        WS::OVERLAPPEDWINDOW, 0, 1920, 1080, 1920, 1080));
}

TEST_F(CocoaBridgeTest, CoveringPopupStaysOpaqueAndHittable) {
    const auto mon = bridge.get_primary_monitor_info();
    WindowData data;
    data.handle = 264;
    data.style = WS::POPUP;
    data.rect = {mon.left, mon.top, mon.right, mon.bottom};
    data.client_rect = {0, 0, mon.width, mon.height};
    ASSERT_TRUE(bridge.create_window(264, data).is_ok());
    EXPECT_TRUE(bridge.native_window_is_opaque(264));
    EXPECT_FALSE(bridge.native_window_ignores_mouse_events(264));
    EXPECT_TRUE(bridge.native_content_hits_point(264, mon.width / 2,
                                                mon.height / 2));
}

TEST_F(CocoaBridgeTest, SmallBorderlessPopupStaysOpaque) {
    WindowData data;
    data.handle = 251;
    data.style = WS::POPUP;
    data.rect = {80, 60, 80 + 480, 60 + 200};
    data.client_rect = {0, 0, 480, 200};
    ASSERT_TRUE(bridge.create_window(251, data).is_ok());
    EXPECT_TRUE(bridge.native_window_is_opaque(251));
}

TEST_F(CocoaBridgeTest, ClientDrawnFrameClearsOpaqueHost) {
    // Chromium #49154 is created with WS_CAPTION then WM_NCCALCSIZE takes the
    // full frame. AppKit's titled default is an opaque white plate; DComp
    // premultiplied Present composites onto that plate unless the host is
    // non-opaque and clear.
    WindowData data;
    data.handle = 250;
    data.style = WS::POPUP | WS::CAPTION | WS::SYSMENU | WS::CLIPCHILDREN;
    data.rect = {80, 60, 80 + 700, 60 + 330};
    data.client_rect = {0, 0, 700, 330};
    ASSERT_TRUE(bridge.create_window(250, data).is_ok());
    EXPECT_TRUE(bridge.native_window_is_opaque(250));
    bridge.set_client_drawn_frame(250, true);
    // Still AppKit-opaque so the window-server hit-tests the full frame.
    EXPECT_TRUE(bridge.native_window_is_opaque(250));
    EXPECT_FALSE(bridge.native_window_ignores_mouse_events(250));
}

TEST_F(CocoaBridgeTest, GdiFlushWithZeroAlphaStaysOpaqueForHitTesting) {
    // InstallShield clear-host plates used premultiplied CGImages. GDI
    // SoftBitmaps usually leave A=0, which punched a hole in the window-server
    // hit mask so clicks activated the HWND behind the guest.
    WindowData data{};
    data.handle = 263;
    data.style = WS::POPUP;
    data.ex_style = 0x00080000u; // WS_EX_LAYERED
    data.rect = {80, 60, 80 + 64, 60 + 64};
    data.client_rect = {0, 0, 64, 64};
    ASSERT_TRUE(bridge.create_window(263, data).is_ok());

    std::vector<u8> pixels(64u * 64u * 4u, 0);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 0x20;
        pixels[i + 1] = 0x40;
        pixels[i + 2] = 0x80;
        pixels[i + 3] = 0; // GDI-typical unused alpha
    }
    bridge.update_window_pixels(263, pixels.data(), 64, 64);
    // kCGImageAlphaNoneSkipLast — RGB is shown, A is ignored for hit-testing.
    EXPECT_EQ(bridge.native_pixel_contents_alpha_info(263), 5);
    EXPECT_FALSE(bridge.native_window_ignores_mouse_events(263));
    EXPECT_TRUE(bridge.native_content_hits_point(263, 32, 32));
}

TEST_F(CocoaBridgeTest, ClearHostStillReceivesMouseUnlessTransparent) {
    // Learn: only WS_EX_TRANSPARENT is click-through. A clear composition
    // host (WS_EX_LAYERED / covering popup) must still own its window rect
    // or clicks activate the HWND behind it.
    constexpr u32 kWsExLayered = 0x00080000u;
    constexpr u32 kWsExTransparent = 0x00000020u;

    WindowData layered{};
    layered.handle = 260;
    layered.style = WS::POPUP;
    layered.ex_style = kWsExLayered;
    layered.rect = {80, 60, 80 + 480, 60 + 200};
    layered.client_rect = {0, 0, 480, 200};
    ASSERT_TRUE(bridge.create_window(260, layered).is_ok());
    EXPECT_TRUE(bridge.native_window_is_opaque(260));
    EXPECT_FALSE(bridge.native_window_ignores_mouse_events(260));
    EXPECT_TRUE(bridge.native_content_hits_point(260, 240, 100));

    WindowData passthrough = layered;
    passthrough.handle = 261;
    passthrough.ex_style = kWsExLayered | kWsExTransparent;
    ASSERT_TRUE(bridge.create_window(261, passthrough).is_ok());
    EXPECT_TRUE(bridge.native_window_ignores_mouse_events(261));
}

TEST_F(CocoaBridgeTest, ClientDrawnDialogStaysOpaque) {
    // InstallShield Sd* pages are dialogs that also take the non-client via
    // WM_NCCALCSIZE. Clearing those hosts punches the desktop through native
    // controls and GDI splash bitmaps (preparing wizard at the bottom-right).
    WindowData data;
    data.handle = 252;
    data.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    data.is_dialog = true;
    data.rect = {80, 60, 80 + 480, 60 + 200};
    data.client_rect = {0, 0, 480, 200};
    ASSERT_TRUE(bridge.create_window(252, data).is_ok());
    EXPECT_TRUE(bridge.native_window_is_opaque(252));
    bridge.set_client_drawn_frame(252, true);
    EXPECT_TRUE(bridge.native_window_is_opaque(252));
}

TEST_F(CocoaBridgeTest, NativeChildCanPresentGuestPaintedPixels) {
    WindowData parent;
    parent.handle = 30;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = {0, 0, 320, 240};
    ASSERT_TRUE(bridge.create_window(30, parent).is_ok());

    WindowData child;
    child.handle = 31;
    child.parent = 30;
    child.rect = {10, 10, 110, 50};
    child.client_rect = {0, 0, 100, 40};
    child.visible = true;
    bridge.add_child_view(30, 31, child, 0x0082, L"", L"");

    EXPECT_FALSE(bridge.has_guest_pixel_surface(31));
    std::array<u8, 4 * 4 * 4> pixels{};
    pixels.fill(0xFF);
    bridge.update_window_pixels(31, pixels.data(), 4, 4);
    EXPECT_TRUE(bridge.has_guest_pixel_surface(31));
}

TEST_F(CocoaBridgeTest, LabeledNativeChildSkipsBlankWhitePixels) {
    WindowData parent;
    parent.handle = 32;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = {0, 0, 320, 240};
    ASSERT_TRUE(bridge.create_window(32, parent).is_ok());

    WindowData child;
    child.handle = 33;
    child.parent = 32;
    child.rect = {10, 10, 110, 50};
    child.client_rect = {0, 0, 100, 40};
    child.visible = true;
    bridge.add_child_view(32, 33, child, 0x0082, L"", L"C:");

    std::array<u8, 4 * 4 * 4> pixels{};
    pixels.fill(0xFF);
    bridge.update_window_pixels(33, pixels.data(), 4, 4);
    EXPECT_FALSE(bridge.has_guest_pixel_surface(33));
}

TEST_F(CocoaBridgeTest, DialogPixelsStayBehindNativeChildControls) {
    WindowData parent;
    parent.handle = 40;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(40, parent).is_ok());

    WindowData page;
    page.handle = 41;
    page.parent = 40;
    page.rect = {0, 0, 300, 200};
    page.client_rect = page.rect;
    page.visible = true;
    bridge.add_container_view(40, 41, page);

    WindowData label;
    label.handle = 42;
    label.parent = 41;
    label.rect = {10, 10, 110, 30};
    label.client_rect = {0, 0, 100, 20};
    label.visible = true;
    bridge.add_child_view(41, 42, label, 0x0082, L"", L"Visible label");

    std::vector<u8> pixels(300u * 200u * 4u, 0xFF);
    bridge.update_window_pixels(41, pixels.data(), 300, 200);

    EXPECT_TRUE(bridge.has_guest_pixel_surface(41));
    EXPECT_TRUE(bridge.guest_pixel_surface_is_behind_native_children(41));
}

TEST_F(CocoaBridgeTest, GuestOwnedFramePreservesSizingBorder) {
    WindowData data{};
    data.handle = 650;
    data.style = WS::OVERLAPPEDWINDOW;
    data.rect = {80, 60, 780, 390};
    data.client_rect = {0, 0, 700, 330};
    ASSERT_TRUE(bridge.create_window(650, data).is_ok());
    ASSERT_TRUE(bridge.native_window_is_resizable(650));
    bridge.set_client_drawn_frame(650, true);
    EXPECT_TRUE(bridge.native_window_is_resizable(650));
    bridge.set_client_drawn_frame(650, false);
    EXPECT_TRUE(bridge.native_window_is_resizable(650));

    data.handle = 651;
    data.style = WS::POPUP | WS::THICKFRAME;
    data.client_drawn_frame = true;
    ASSERT_TRUE(bridge.create_window(651, data).is_ok());
    EXPECT_TRUE(bridge.native_window_is_resizable(651));
    data.handle = 652;
    data.style = WS::POPUP;
    ASSERT_TRUE(bridge.create_window(652, data).is_ok());
    EXPECT_FALSE(bridge.native_window_is_resizable(652));
}

TEST_F(CocoaBridgeTest, ChildSurfaceCompositesUnpaintedPixelsTransparent) {
    // A WS_CHILD window shares its parent's pixels on Windows: an owner-draw
    // button (BS_OWNERDRAW) whose WM_DRAWITEM draws nothing is invisible.
    // Its surface keeps A=0 where unpainted (SoftBitmap::child_window), so the
    // overlay composites alpha; the parent's top-level surface stays opaque.
    WindowData parent;
    parent.handle = 50;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = {0, 0, 320, 240};
    ASSERT_TRUE(bridge.create_window(50, parent).is_ok());

    WindowData child;
    child.handle = 51;
    child.parent = 50;
    child.style = WS::CHILD | WS::VISIBLE | 0x0Bu;  // BS_OWNERDRAW
    child.rect = {10, 10, 110, 50};
    child.client_rect = {0, 0, 100, 40};
    child.visible = true;
    bridge.add_child_view(50, 51, child, 0x0080, L"", L"");

    std::array<u8, 4 * 4 * 4> pixels{};  // never painted: A=0
    bridge.update_window_pixels(51, pixels.data(), 4, 4);
    ASSERT_TRUE(bridge.has_guest_pixel_surface(51));
    EXPECT_EQ(bridge.native_pixel_contents_alpha_info(51),
              1);  // kCGImageAlphaPremultipliedLast
}

TEST_F(CocoaBridgeTest, IconStaticShowsTheIconImage) {
    // An SS_ICON static draws the icon STM_SETIMAGE / STM_SETICON gives it
    // (Learn: STM_SETIMAGE); a NULL icon clears it.
    WindowData parent;
    parent.handle = 60;
    parent.rect = {0, 0, 320, 240};
    parent.client_rect = {0, 0, 320, 240};
    ASSERT_TRUE(bridge.create_window(60, parent).is_ok());

    WindowData icon;
    icon.handle = 61;
    icon.parent = 60;
    icon.style = WS::CHILD | WS::VISIBLE | 0x03u;  // SS_ICON
    icon.rect = {10, 10, 42, 42};
    icon.client_rect = {0, 0, 32, 32};
    icon.visible = true;
    bridge.add_child_view(60, 61, icon, 0x0082, L"", L"");
    EXPECT_FALSE(bridge.native_control_image_size(61).has_value());

    std::vector<u8> rgba(32u * 32u * 4u, 0xFF);
    ASSERT_TRUE(bridge.set_control_image_rgba(61, 32, 32, rgba));
    const auto size = bridge.native_control_image_size(61);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, std::make_pair(32, 32));

    ASSERT_TRUE(bridge.set_control_image_rgba(61, 0, 0, {}));
    EXPECT_FALSE(bridge.native_control_image_size(61).has_value());
}
