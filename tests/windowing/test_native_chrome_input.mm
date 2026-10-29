/// @file test_native_chrome_input.mm
/// @brief A press on a window's native macOS chrome is non-client input.
///
/// On Windows a press on the title bar is WM_NCLBUTTONDOWN: DefWindowProc's
/// move loop owns the gesture and the application gets no client button
/// messages for it. Vitreo used to post WM_LBUTTONDOWN at a negative client y,
/// so an autolocking guest captured and clipped the mouse and warped it to
/// its centre on every motion while AppKit dragged the window after the
/// warped cursor: the window jumped down-left on each drag.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/objc_helpers.h>

@interface VtrChromeMouseEvent : NSEvent
@property(nonatomic, assign) NSWindow* selectedWindow;
@property(nonatomic) NSPoint selectedPoint;
@property(nonatomic) NSEventType selectedType;
@end
@implementation VtrChromeMouseEvent
- (NSWindow*)window { return self.selectedWindow; }
- (NSPoint)locationInWindow { return self.selectedPoint; }
- (NSEventType)type { return self.selectedType; }
@end

namespace {

VtrChromeMouseEvent* mouse(NSWindow* window, NSEventType type, NSPoint point) {
    VtrChromeMouseEvent* event = [[VtrChromeMouseEvent alloc] init];
    event.selectedWindow = window;
    event.selectedPoint = point;
    event.selectedType = type;
    return event;
}

} // namespace

TEST(NativeChromeInputTest, TitleBarPressIsNonClientUntilReleased) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle = 0x4c100;
        top.title = L"Native chrome press regression";
        top.style = WS::OVERLAPPEDWINDOW;
        top.rect = {100, 100, 746, 616};
        top.client_rect = {0, 0, 640, 480};
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        NSWindow* window = nil;
        for (NSWindow* candidate in [NSApp windows])
            if ([[candidate title] isEqualToString:@"Native chrome press regression"]) window = candidate;
        ASSERT_NE(window, nil);
        ASSERT_NE([window styleMask] & NSWindowStyleMaskTitled, 0u);
        const NSRect layout = [window contentLayoutRect];
        const NSPoint title_bar = NSMakePoint(NSMidX(layout), NSMaxY(layout) + 6);
        const NSPoint content = NSMakePoint(NSMidX(layout), NSMidY(layout));
        ASSERT_LT(title_bar.y, NSHeight([window frame]));
        void* (*raw)(id) = [](id e) { return (__bridge void*)e; };

        // A press in the client area is the client's.
        EXPECT_FALSE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseDown, content)), top.handle));
        EXPECT_FALSE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseUp, content)), top.handle));

        // A press on the title bar, its drags and its release are not.
        EXPECT_TRUE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseDown, title_bar)), top.handle));
        EXPECT_TRUE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseDragged, content)), top.handle));
        EXPECT_TRUE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseUp, content)), top.handle));
        // After the release, input is the client's again.
        EXPECT_FALSE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseDragged, content)), top.handle));
        EXPECT_FALSE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseDown, content)), top.handle));

        // The right button is tracked on its own.
        EXPECT_TRUE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeRightMouseDown, title_bar)), top.handle));
        EXPECT_TRUE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeRightMouseUp, title_bar)), top.handle));
        EXPECT_FALSE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeRightMouseUp, content)), top.handle));

        // A window holding mouse capture gets its input, as on Windows.
        bridge.set_mouse_capture(top.handle);
        EXPECT_FALSE(bridge.native_chrome_mouse_event(raw(mouse(window, NSEventTypeLeftMouseDown, title_bar)), top.handle));
        bridge.set_mouse_capture(NULL_HWND);
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeChromeInputTest, ChromelessWindowHasNoNonClientArea) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData popup;
        popup.handle = 0x4c200;
        popup.title = L"Native chrome popup regression";
        popup.style = WS::POPUP;
        popup.rect = {0, 0, 640, 480};
        popup.client_rect = {0, 0, 640, 480};
        ASSERT_TRUE(bridge.create_window(popup.handle, popup).is_ok());
        NSWindow* window = nil;
        for (NSWindow* candidate in [NSApp windows])
            if ([[candidate title] isEqualToString:@"Native chrome popup regression"]) window = candidate;
        ASSERT_NE(window, nil);
        VtrChromeMouseEvent* top_edge = mouse(window, NSEventTypeLeftMouseDown,
                                              NSMakePoint(10, NSHeight([window frame]) - 1));
        EXPECT_FALSE(bridge.native_chrome_mouse_event((__bridge void*)top_edge, popup.handle));
        bridge.destroy_window(popup.handle);
    }
}

TEST(NativePresentationTest, SwapChainAboveExistingPixelsAndBelowControls) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle = 0x4c101;
        top.title = L"Surface ordering regression";
        top.style = WS::OVERLAPPEDWINDOW;
        top.rect = {100, 100, 420, 340};
        top.client_rect = {0, 0, 320, 240};
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        std::vector<u8> pixels(320 * 240 * 4, 0);
        bridge.update_window_pixels(top.handle, pixels.data(), 320, 240);
        NSWindow* window = nil;
        for (NSWindow* candidate in [NSApp windows])
            if ([[candidate title] isEqualToString:@"Surface ordering regression"]) window = candidate;
        ASSERT_NE(window, nil);
        NSView* host = window.contentView;
        NSView* cpu = nil;
        for (NSView* view in host.subviews)
            if ([NSStringFromClass(view.class) isEqualToString:@"VtrPixelView"]) cpu = view;
        ASSERT_NE(cpu, nil);
        NSView* control = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 20, 20)];
        [host addSubview:control];
        NSView* metal = (__bridge NSView*)bridge.native_presentation_view(top.handle);
        ASSERT_NE(metal, nil);
        auto check_order = [&] {
            EXPECT_LT([host.subviews indexOfObject:cpu], [host.subviews indexOfObject:metal]);
            EXPECT_LT([host.subviews indexOfObject:metal], [host.subviews indexOfObject:control]);
        };
        check_order();
        [window setContentSize:NSMakeSize(400, 300)];
        bridge.update_window_pixels(top.handle, pixels.data(), 320, 240);
        EXPECT_EQ(bridge.native_presentation_view(top.handle), (__bridge void*)metal);
        check_order();
        bridge.destroy_window(top.handle);
    }
}
