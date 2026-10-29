/// @file test_cocoa_bridge.cpp
/// @brief Tests for Cocoa bridge (WINDOW-003): wheel delta, style mapping, coordinate and presentation-fit mapping

#include "cocoa_bridge_test_fixture.hpp"

namespace {
struct DefaultChildProcessRole {
    DefaultChildProcessRole() {
        // Regular policy turns this gtest binary into a Dock app. macOS then
        // keeps reopening it after a kill. Stay a child unless a test
        // explicitly opts into another role.
        if (std::getenv("VITREO_MACOS_PROCESS_ROLE") == nullptr) {
            ::setenv("VITREO_MACOS_PROCESS_ROLE", "child", 0);
        }
    }
} g_default_child_process_role;
}  // namespace

TEST(WheelDeltaTest, FractionalLinesAndPixelsReachTheGuestWithoutANotchDelay) {
    WheelDeltaAccumulator wheel;
    // Winuser.h WHEEL_DELTA is 120; WM_MOUSEWHEEL explicitly permits
    // divisions of it. These inputs previously produced no message.
    EXPECT_EQ(wheel.translate(1, 0.25, false), WHEEL_DELTA / 4);
    EXPECT_EQ(wheel.translate(1, -0.25, false), -WHEEL_DELTA / 4);
    EXPECT_EQ(wheel.translate(1, 1, false), WHEEL_DELTA);
    EXPECT_EQ(wheel.translate(1, 1, true), WHEEL_DELTA / 12);
    EXPECT_EQ(wheel.translate(1, -1, true), -WHEEL_DELTA / 12);
}

TEST(WheelDeltaTest, SubUnitMotionAccumulatesWithoutCrossingWindowsOrAxes) {
    WheelDeltaAccumulator vertical, horizontal;
    EXPECT_EQ(vertical.translate(1, 0.0625, true), 0);
    EXPECT_EQ(vertical.translate(1, 0.0625, true), 1);
    EXPECT_EQ(horizontal.translate(1, 0.0625, true), 0);
    EXPECT_EQ(vertical.translate(2, 0.0625, true), 0);
    EXPECT_EQ(vertical.translate(2, -0.0625, true), 0);
    EXPECT_EQ(vertical.translate(2, -0.0625, true), 0);
    EXPECT_EQ(vertical.translate(2, -0.0625, true), -1);
}

TEST(WheelDeltaTest, DeviceChangesAndExtremeDeltasRemainDefined) {
    WheelDeltaAccumulator wheel;
    EXPECT_EQ(wheel.translate(1, 0.0625, true), 0);
    EXPECT_EQ(wheel.translate(1, 0, false), 0);
    EXPECT_EQ(wheel.translate(1, std::numeric_limits<double>::infinity(), false), 0);
    EXPECT_EQ(wheel.translate(1, std::numeric_limits<double>::quiet_NaN(), false), 0);
    EXPECT_EQ(wheel.translate(1, std::numeric_limits<double>::max(), false),
              std::numeric_limits<i32>::max());
    EXPECT_EQ(wheel.translate(1, -std::numeric_limits<double>::max(), false),
              std::numeric_limits<i32>::min());
}

// ============================================================================
// Style Mapping Tests
// ============================================================================

TEST_F(CocoaBridgeTest, MapStyleOverlappedWindow) {
    u64 mask = CocoaBridge::map_style_to_cocoa(WS::OVERLAPPEDWINDOW);
    EXPECT_TRUE(mask & NSWindowStyleMask::Titled);
    EXPECT_TRUE(mask & NSWindowStyleMask::Closable);
    EXPECT_TRUE(mask & NSWindowStyleMask::Miniaturizable);
    EXPECT_TRUE(mask & NSWindowStyleMask::Resizable);
}

TEST_F(CocoaBridgeTest, MapStylePopup) {
    u64 mask = CocoaBridge::map_style_to_cocoa(WS::POPUP);
    EXPECT_EQ(mask, NSWindowStyleMask::Borderless);
}

TEST_F(CocoaBridgeTest, MapStyleCaptionOnly) {
    u64 mask = CocoaBridge::map_style_to_cocoa(WS::CAPTION);
    EXPECT_TRUE(mask & NSWindowStyleMask::Titled);
    EXPECT_FALSE(mask & NSWindowStyleMask::Closable);
    EXPECT_FALSE(mask & NSWindowStyleMask::Miniaturizable);
    EXPECT_FALSE(mask & NSWindowStyleMask::Resizable);
}

TEST_F(CocoaBridgeTest, MapStyleCaptionAndSysmenu) {
    u64 mask = CocoaBridge::map_style_to_cocoa(WS::CAPTION | WS::SYSMENU);
    EXPECT_TRUE(mask & NSWindowStyleMask::Titled);
    EXPECT_TRUE(mask & NSWindowStyleMask::Closable);
    EXPECT_FALSE(mask & NSWindowStyleMask::Miniaturizable);
    EXPECT_FALSE(mask & NSWindowStyleMask::Resizable);
}

TEST_F(CocoaBridgeTest, MapStyleZero) {
    u64 mask = CocoaBridge::map_style_to_cocoa(0);
    EXPECT_EQ(mask, NSWindowStyleMask::Borderless);
}

// ============================================================================
// Coordinate Translation Tests
// ============================================================================

TEST_F(CocoaBridgeTest, WindowsToCocoaRect) {
    // Window at (100, 200) size 300x300 on 1080-high screen
    // Cocoa Y = 1080 - 200 - 300 = 580
    Rect win{100, 200, 400, 500};
    Rect cocoa = CocoaBridge::windows_to_cocoa_rect(win, 1080);
    EXPECT_EQ(cocoa.left, 100);
    EXPECT_EQ(cocoa.right, 400);
    EXPECT_EQ(cocoa.top, 580);
    EXPECT_EQ(cocoa.bottom, 880);
}

TEST_F(CocoaBridgeTest, WindowsToCocoaRectTopLeft) {
    // Window at (0, 0) size 800x600 on 1080-high screen
    // Cocoa Y = 1080 - 0 - 600 = 480
    Rect win{0, 0, 800, 600};
    Rect cocoa = CocoaBridge::windows_to_cocoa_rect(win, 1080);
    EXPECT_EQ(cocoa.left, 0);
    EXPECT_EQ(cocoa.right, 800);
    EXPECT_EQ(cocoa.top, 480);
    EXPECT_EQ(cocoa.bottom, 1080);
}

TEST_F(CocoaBridgeTest, CocoaToWindowsRoundTrip) {
    Rect original{150, 300, 550, 700};
    Rect cocoa = CocoaBridge::windows_to_cocoa_rect(original, 1080);
    Rect back  = CocoaBridge::cocoa_to_windows_rect(cocoa, 1080);
    EXPECT_EQ(back.left, original.left);
    EXPECT_EQ(back.top, original.top);
    EXPECT_EQ(back.right, original.right);
    EXPECT_EQ(back.bottom, original.bottom);
}

TEST_F(CocoaBridgeTest, GuestClipMapsThroughFullscreenViewport) {
    // StarCraft-style exclusive mode: ClipCursor(0,0,640,480) must cover the
    // aspect-fit presentation on a 1920×1080 host view — not a 640×480 host
    // box (the pre-fullscreen lock bug).
    unsetenv("VITREO_INTEGER_SCALE");
    const auto fitted =
        vitreo::graphics::aspect_fit_rect(640, 480, 1920, 1080);
    double x_min = 0, y_min = 0, x_max = 0, y_max = 0;
    ASSERT_TRUE(CocoaBridge::guest_clip_to_viewport_bounds(
        0, 0, 640, 480, fitted.x, fitted.y, fitted.width, fitted.height, 640,
        480, x_min, y_min, x_max, y_max));
    EXPECT_NEAR(x_min, fitted.x, 0.01);
    EXPECT_NEAR(y_min, fitted.y, 0.01);
    EXPECT_NEAR(x_max, fitted.x + fitted.width, 0.01);
    EXPECT_NEAR(y_max, fitted.y + fitted.height, 0.01);
    // Must not collapse to guest resolution in host pixels.
    EXPECT_GT(x_max - x_min, 1000.0);
    EXPECT_GT(y_max - y_min, 700.0);
}

TEST_F(CocoaBridgeTest, GuestClipRejectsEmptyRect) {
    double x_min = 0, y_min = 0, x_max = 0, y_max = 0;
    EXPECT_FALSE(CocoaBridge::guest_clip_to_viewport_bounds(
        10, 10, 10, 20, 0, 0, 100, 100, 640, 480, x_min, y_min, x_max,
        y_max));
}

TEST_F(CocoaBridgeTest, PresentationPointMapsThroughLetterboxedViewport) {
    Point mapped{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        960, 540, 240, 0, 1440, 1080, 640, 480, mapped));
    EXPECT_EQ(mapped.x, 320);
    EXPECT_EQ(mapped.y, 240);
}

TEST_F(CocoaBridgeTest, PresentationPointRemovesVerticalLetterboxInset) {
    Point mapped{};
    // A 348x600 child surface fitted into a 348x644 presentation view has a
    // 22-pixel bar above it. A visual click at child-view y=163 addresses
    // drawable y=141, not raw view-local y=163.
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        134, 163, 0, 22, 348, 600, 348, 600, mapped));
    EXPECT_EQ(mapped.x, 134);
    EXPECT_EQ(mapped.y, 141);
}

TEST_F(CocoaBridgeTest, PresentationPointMapsViewportEdges) {
    Point mapped{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        240, 0, 240, 0, 1440, 1080, 640, 480, mapped));
    EXPECT_EQ(mapped.x, 0);
    EXPECT_EQ(mapped.y, 0);

    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        1679, 1079, 240, 0, 1440, 1080, 640, 480, mapped));
    EXPECT_EQ(mapped.x, 639);
    EXPECT_EQ(mapped.y, 479);
}

TEST_F(CocoaBridgeTest, PresentationPointRejectsLetterboxBars) {
    Point mapped{};
    EXPECT_FALSE(CocoaBridge::map_presentation_point(
        100, 540, 240, 0, 1440, 1080, 640, 480, mapped));
    EXPECT_FALSE(CocoaBridge::map_presentation_point(
        1800, 540, 240, 0, 1440, 1080, 640, 480, mapped));
}

TEST_F(CocoaBridgeTest, PresentationPointClampsLetterboxToGuestEdges) {
    Point mapped{};
    // Left letterbox bar → guest x = 0
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        100, 540, 240, 0, 1440, 1080, 640, 480, mapped, true));
    EXPECT_EQ(mapped.x, 0);
    EXPECT_EQ(mapped.y, 240);

    // Right letterbox bar → guest x = 639
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        1800, 540, 240, 0, 1440, 1080, 640, 480, mapped, true));
    EXPECT_EQ(mapped.x, 639);
    EXPECT_EQ(mapped.y, 240);

    // Below the viewport → guest y = 479 (bottom edge scroll zone)
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        960, 1200, 240, 0, 1440, 1080, 640, 480, mapped, true));
    EXPECT_EQ(mapped.x, 320);
    EXPECT_EQ(mapped.y, 479);
}

// DXGI exclusive full-screen at a 1920x1080 mode on a 2560x1440 display
// (the RV There Yet case): the mode fills the Space (same 16:9 aspect,
// scale 4/3), so guest client == screen coordinates span 0..1919 x 0..1079;
// SetCursorPos inverts the same fit. A 1280x1024 mode is pillarboxed
// (fit width 1440*1280/1024 = 1800, x inset 380) and the bars clamp.
TEST_F(CocoaBridgeTest, ExclusiveModeSpaceMapsGuestModeCoordinates) {
    Point mapped{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        1280, 720, 0, 0, 2560, 1440, 1920, 1080, mapped, true));
    EXPECT_EQ(mapped.x, 960);
    EXPECT_EQ(mapped.y, 540);
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        2559, 1439, 0, 0, 2560, 1440, 1920, 1080, mapped, true));
    EXPECT_EQ(mapped.x, 1919);
    EXPECT_EQ(mapped.y, 1079);
    double px = 0;
    double py = 0;
    ASSERT_TRUE(CocoaBridge::unmap_presentation_point(
        960, 540, 0, 0, 2560, 1440, 1920, 1080, px, py));
    EXPECT_NEAR(px, 1280.0, 1.0);
    EXPECT_NEAR(py, 720.0, 1.0);

    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        100, 720, 380, 0, 1800, 1440, 1280, 1024, mapped, true));
    EXPECT_EQ(mapped.x, 0);
    EXPECT_EQ(mapped.y, 512);
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        1280, 720, 380, 0, 1800, 1440, 1280, 1024, mapped, true));
    EXPECT_EQ(mapped.x, 640);
    EXPECT_EQ(mapped.y, 512);
}

// The host Space follows the desired state through AppKit transitions:
// nothing is toggled while one runs (the style mask is not final and AppKit
// ignores the toggle); a Space is formed only for a window on screen; an
// unwanted one is left. The RV There Yet startup race was a request issued
// while the borderless-fullscreen Space was still exiting.
// The alternating "layer_frame=1920x1080 / 2560x1440" lines of a Space
// transition are one 1920x1080 back buffer fitted into the windowed view
// and into the Space's view: each is the aspect fit of that view, and a
// relative position maps to the same guest pixel in both (the fit is
// stable; only the view changes).
TEST_F(CocoaBridgeTest, AspectFitOfOneBufferIsStableAcrossTheSpaceResize) {
    unsetenv("VITREO_INTEGER_SCALE");
    const auto windowed = vitreo::graphics::aspect_fit_rect(1920, 1080, 1920, 1080);
    EXPECT_DOUBLE_EQ(windowed.x, 0.0);
    EXPECT_DOUBLE_EQ(windowed.y, 0.0);
    EXPECT_DOUBLE_EQ(windowed.width, 1920.0);
    EXPECT_DOUBLE_EQ(windowed.height, 1080.0);
    const auto space = vitreo::graphics::aspect_fit_rect(1920, 1080, 2560, 1440);
    EXPECT_DOUBLE_EQ(space.x, 0.0);
    EXPECT_DOUBLE_EQ(space.y, 0.0);
    EXPECT_DOUBLE_EQ(space.width, 2560.0);
    EXPECT_DOUBLE_EQ(space.height, 1440.0);
    // A 16:10 display letterboxes the same buffer instead of stretching it.
    const auto tall = vitreo::graphics::aspect_fit_rect(1920, 1080, 2560, 1600);
    EXPECT_DOUBLE_EQ(tall.width, 2560.0);
    EXPECT_DOUBLE_EQ(tall.height, 1440.0);
    EXPECT_DOUBLE_EQ(tall.y, 80.0);
    Point a{};
    Point b{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        480, 270, windowed.x, windowed.y, windowed.width, windowed.height,
        1920, 1080, a, true));
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        640, 360, space.x, space.y, space.width, space.height, 1920, 1080, b,
        true));
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.y, b.y);
    EXPECT_EQ(a.x, 480);
}

TEST_F(CocoaBridgeTest, CursorPosMapsPresentationAfterPixelModeReset) {
    // GetCursorPos must use the presenter fit when the guest backbuffer
    // (a listed pixel mode) does not match the Cocoa view (points).
    EXPECT_TRUE(CocoaBridge::native_cursor_pos_maps_presentation(false, true));
    EXPECT_FALSE(CocoaBridge::native_cursor_pos_maps_presentation(false, false));
    EXPECT_TRUE(CocoaBridge::native_cursor_pos_maps_presentation(true, false));
    EXPECT_TRUE(CocoaBridge::native_cursor_pos_maps_presentation(true, true));

    // 1920×1080 into an 1800×1169 point view: scale = 1800/1920.
    const double fit_w = 1800.0;
    const double fit_h = 1080.0 * (1800.0 / 1920.0);
    const double fit_y = (1169.0 - fit_h) / 2.0;
    Point mapped{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        900.0, fit_y + fit_h / 2.0, 0.0, fit_y, fit_w, fit_h, 1920, 1080,
        mapped));
    EXPECT_EQ(mapped.x, 960);
    EXPECT_EQ(mapped.y, 540);
}

TEST_F(CocoaBridgeTest, RetargetAlwaysMapsPresentationDrawable) {
    // Events enter as view points in every mode; retarget is the one map.
    EXPECT_TRUE(CocoaBridge::retarget_applies_presentation_drawable_map(false));
    EXPECT_TRUE(CocoaBridge::retarget_applies_presentation_drawable_map(true));

    // Applying the fit a second time to an already-mapped guest sample
    // compresses coordinates — the Metal exclusive-mode bug.
    Point once{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        960, 540, 240, 0, 1440, 1080, 640, 480, once));
    EXPECT_EQ(once.x, 320);
    EXPECT_EQ(once.y, 240);
    Point twice{};
    ASSERT_TRUE(CocoaBridge::map_presentation_point(
        once.x, once.y, 240, 0, 1440, 1080, 640, 480, twice));
    EXPECT_NE(twice.x, once.x);
    EXPECT_NE(twice.y, once.y);
}
