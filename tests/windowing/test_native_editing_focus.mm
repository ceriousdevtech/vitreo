#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAShapeLayer.h>
#import <QuartzCore/CAMetalLayer.h>
#include <array>
#include <cwchar>
#include <string>
#include <gtest/gtest.h>
#include <windowing/cocoa/objc_helpers.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

@interface VtrRoutingWheelEvent : NSEvent
@property(nonatomic, assign) NSWindow* selectedWindow;
@property(nonatomic) NSPoint selectedPoint;
@end
@implementation VtrRoutingWheelEvent
- (NSWindow*)window { return self.selectedWindow; }
- (NSPoint)locationInWindow { return self.selectedPoint; }
- (BOOL)hasPreciseScrollingDeltas { return NO; }
- (CGFloat)scrollingDeltaY { return -1; }
- (CGFloat)scrollingDeltaX { return 1; }
- (NSEventModifierFlags)modifierFlags { return NSEventModifierFlagShift; }
@end

TEST(NativeWheelRoutingTest, HoveredChildAndPeerAncestorReceiveBothAxesWithoutChangingFocus) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        MessageQueue queue;
        WindowData top;
        top.handle = 0x4b100;
        top.title = L"Native wheel routing regression";
        top.style = WS::POPUP;
        top.rect = {-100, -80, 300, 220};
        top.client_rect = {0, 0, 400, 300};
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        WindowData child;
        child.handle = 0x4b101;
        child.parent = top.handle;
        child.rect = {20, 30, 380, 280};
        child.client_rect = {0, 0, 360, 250};
        child.visible = true;
        child.wndproc = 0x401000;
        bridge.add_child_view(top.handle, child.handle, child, 0, L"GuestWindow", L"");
        NSWindow* window = nil;
        for (NSWindow* candidate in [NSApp windows])
            if ([[candidate title] isEqualToString:@"Native wheel routing regression"])
                window = candidate;
        ASSERT_NE(window, nil);
        bridge.set_keyboard_focus_hwnd(0x4b102); // Separate focus sink, not under the pointer.
        VtrRoutingWheelEvent* event = [[VtrRoutingWheelEvent alloc] init];
        event.selectedWindow = window;
        event.selectedPoint = [window.contentView convertPoint:NSMakePoint(50, 50) toView:nil];
        // Oracle wheel_routing_probe: mode 2 targets the hovered child on
        // both axes and keeps the focused child unchanged. A peer host has
        // no local procedure: the dispatchable ancestor must receive input.
        for (bool peer : {false, true}) {
            SCOPED_TRACE(peer);
            if (peer) {
                ASSERT_TRUE(bridge.ensure_hosted_peer_child(child.handle, 0x4b103,
                                                          vitreo::windowing::Rect{0, 0, 360, 250}));
                ASSERT_TRUE(bridge.set_control_z_order(0x4b103, HWND_TOP));
            }
            bridge.translate_scroll_wheel((__bridge void*)event, queue);
            std::vector<MSG> wheels;
            MSG msg{};
            while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE))
                if (msg.message == WM::MOUSEWHEEL || msg.message == WM::MOUSEHWHEEL)
                    wheels.push_back(msg);
            ASSERT_EQ(wheels.size(), 2u);
            EXPECT_EQ(wheels[0].message, WM::MOUSEWHEEL);
            EXPECT_EQ(wheels[1].message, WM::MOUSEHWHEEL);
            for (const auto& wheel : wheels) {
                EXPECT_EQ(wheel.hwnd, child.handle);
                EXPECT_EQ(wheel.wParam & 0xffffu, mouse_keys::SHIFT);
                EXPECT_EQ(static_cast<i16>(wheel.lParam & 0xffffu), -50);
                EXPECT_EQ(static_cast<i16>((wheel.lParam >> 16) & 0xffffu), -30);
            }
            EXPECT_EQ(static_cast<i16>(wheels[0].wParam >> 16), -WHEEL_DELTA);
            EXPECT_EQ(static_cast<i16>(wheels[1].wParam >> 16), WHEEL_DELTA);
            EXPECT_EQ(bridge.keyboard_focus_hwnd(), 0x4b102u);
        }
        [event release];
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeWheelRoutingTest, UnrelatedBackgroundScaleCannotMoveChildScreenPoint) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle = 0x4b110;
        top.title = L"Wheel surface coordinate regression";
        top.style = WS::POPUP;
        top.rect = {-100, -80, 300, 220};
        top.client_rect = {0, 0, 400, 300};
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        WindowData child;
        child.handle = 0x4b111;
        child.parent = top.handle;
        child.rect = {20, 30, 380, 280};
        child.client_rect = {0, 0, 360, 250};
        child.visible = true;
        child.wndproc = 0x401000;
        bridge.add_child_view(top.handle, child.handle, child, 0, L"GuestWindow", L"");
        NSView* background = (__bridge NSView*)bridge.native_presentation_view(top.handle);
        ASSERT_NE(background, nil);
        NSWindow* window = background.window;
        background.hidden = NO;
        background.wantsLayer = YES;
        ASSERT_NE(background.layer, nil);
        [window.contentView addSubview:background positioned:NSWindowBelow relativeTo:nil];
        CAMetalLayer* layer = [CAMetalLayer layer];
        [background.layer addSublayer:layer];
        ASSERT_EQ(layer.superlayer, background.layer);
        VtrRoutingWheelEvent* event = [[VtrRoutingWheelEvent alloc] init];
        event.selectedWindow = window;
        // WM_MOUSEWHEEL specifies signed SCREEN coordinates. A stationary
        // pointer over the fixed child cannot move when a different, lower
        // surface changes its backbuffer size (Apple view hierarchy contract).
        for (int scale : {1, 2}) {
            SCOPED_TRACE(scale);
            layer.drawableSize = CGSizeMake(400 * scale, 300 * scale);
            for (NSPoint point : {NSMakePoint(50, 50), NSMakePoint(170, 140)}) {
                event.selectedPoint = [window.contentView convertPoint:point toView:nil];
                NSPoint inverse{};
                ASSERT_TRUE(map_guest_surface_point_to_native(window,
                    static_cast<i32>(point.x), static_cast<i32>(point.y), inverse));
                const NSPoint roundtrip = [window.contentView convertPoint:inverse fromView:nil];
                EXPECT_NEAR(roundtrip.x, point.x, 0.01);
                EXPECT_NEAR(roundtrip.y, point.y, 0.01);
                MessageQueue queue;
                bridge.translate_scroll_wheel((__bridge void*)event, queue);
                MSG msg{};
                unsigned count = 0;
                while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
                    if (msg.message != WM::MOUSEWHEEL && msg.message != WM::MOUSEHWHEEL) continue;
                    ++count;
                    EXPECT_EQ(msg.hwnd, child.handle);
                    EXPECT_EQ(static_cast<i16>(msg.lParam), static_cast<i32>(point.x) - 100);
                    EXPECT_EQ(static_cast<i16>(msg.lParam >> 16), static_cast<i32>(point.y) - 80);
                    EXPECT_EQ(msg.pt.x, static_cast<i32>(point.x) - 100);
                    EXPECT_EQ(msg.pt.y, static_cast<i32>(point.y) - 80);
                }
                EXPECT_EQ(count, 2u);
            }
        }
        [event release];
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeWheelRoutingTest, FlippedContentHitUsesSuperviewCoordinates) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle = 0x4b130;
        top.style = WS::POPUP;
        top.rect = {0, 0, 400, 300};
        top.client_rect = top.rect;
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        WindowData child;
        child.handle = 0x4b131;
        child.parent = top.handle;
        child.rect = {20, 30, 120, 110};
        child.client_rect = {0, 0, 100, 80};
        child.visible = true;
        child.wndproc = 0x401000;
        bridge.add_child_view(top.handle, child.handle, child, 0, L"GuestWindow", L"");
        // Apple hitTest's input is in the receiver's superview coordinates.
        // A top-left child must not be tested at the vertically reflected y.
        EXPECT_EQ(bridge.resolve_client_mouse_target(top.handle, 50, 50), child.handle);
        EXPECT_EQ(bridge.resolve_client_mouse_target(top.handle, 50, 250), top.handle);
        MessageQueue queue;
        bridge.post_client_mouse_message(queue, top.handle, WM::MOUSEMOVE, 0, (50u << 16) | 50u);
        MSG msg{};
        ASSERT_TRUE(queue.peek(msg, NULL_HWND, WM::MOUSEMOVE, WM::MOUSEMOVE, PM::REMOVE));
        EXPECT_EQ(msg.hwnd, child.handle);
        EXPECT_EQ(static_cast<i16>(msg.lParam), 30);
        EXPECT_EQ(static_cast<i16>(msg.lParam >> 16), 20);
        EXPECT_EQ(msg.pt.x, 50);
        EXPECT_EQ(msg.pt.y, 50);
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeSurfacePointTest, NestedSurfaceOriginScaleOcclusionAndInverseAgree) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle = 0x4b120;
        top.style = WS::POPUP;
        top.rect = {0, 0, 400, 300};
        top.client_rect = top.rect;
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        NSView* background = (__bridge NSView*)bridge.native_presentation_view(top.handle);
        NSWindow* window = background.window;
        background.wantsLayer = YES;
        background.hidden = NO;
        CAMetalLayer* back = [CAMetalLayer layer];
        back.drawableSize = CGSizeMake(800, 600);
        [background.layer addSublayer:back];
        VtrMetalView* container = [[VtrMetalView alloc] initWithFrame:NSMakeRect(20, 30, 200, 180)];
        [window.contentView addSubview:container positioned:NSWindowAbove relativeTo:nil];
        VtrMetalView* surface = [[VtrMetalView alloc] initWithFrame:NSMakeRect(-30, 20, 100, 80)];
        surface.wantsLayer = YES;
        CAMetalLayer* front = [CAMetalLayer layer];
        front.drawableSize = CGSizeMake(200, 160);
        [surface.layer addSublayer:front];
        [container addSubview:surface positioned:NSWindowAbove relativeTo:nil];
        ASSERT_EQ([window.contentView hitTest:[window.contentView convertPoint:NSMakePoint(40, 90) toView:window.contentView.superview]], surface);
        const auto check = [&](double x, double y, i32 expected_x, i32 expected_y, bool clamp = false) {
            i32 gx = 0, gy = 0;
            const NSPoint p = [window.contentView convertPoint:NSMakePoint(x, y) toView:nil];
            EXPECT_TRUE(map_native_point_to_guest_surface(window, p, clamp, gx, gy));
            EXPECT_EQ(gx, expected_x);
            EXPECT_EQ(gy, expected_y);
        };
        // Child origin=(-10,50), local=(50,40), scaled to=(100,80).
        check(40, 90, 90, 130);
        NSPoint inverse{};
        ASSERT_TRUE(map_guest_surface_point_to_native(window, 90, 130, inverse));
        const NSPoint native = [window.contentView convertPoint:inverse fromView:nil];
        EXPECT_NEAR(native.x, 40.25, 0.01);
        EXPECT_NEAR(native.y, 90.25, 0.01);
        i32 gx = 0, gy = 0;
        ASSERT_TRUE(map_native_point_to_guest_surface(window, inverse, false, gx, gy));
        EXPECT_EQ(gx, 90);
        EXPECT_EQ(gy, 130);
        VtrPixelView* nearer = [[VtrPixelView alloc] initWithFrame:NSMakeRect(0, 0, 100, 80)];
        nearer.scalesFramebuffer = YES;
        nearer.framebufferWidth = 50;
        nearer.framebufferHeight = 40;
        [surface addSubview:nearer];
        check(40, 90, 15, 70); // Nearest Pixel wins over its Metal ancestor.
        [nearer removeFromSuperview];
        [nearer release];
        check(350, 250, 700, 500); // Top-level surface still scales.
        check(-10, -20, -10, -20); // Captured/off-client fallback.
        check(-10, -20, 0, 0, true); // Exclusive edge clamp.
        surface.hidden = YES;
        check(40, 90, 40, 90); // Unscaled foreground container shields background.
        container.hidden = YES;
        check(40, 90, 80, 180); // Hidden branch cannot own the point.
        back.drawableSize = CGSizeMake(800, 400); // 400x200 fit, y inset50.
        check(100, 100, 200, 100);
        check(100, 20, 200, 0, true);
        const NSPoint bar = [window.contentView convertPoint:NSMakePoint(100, 20) toView:nil];
        EXPECT_FALSE(map_native_point_to_guest_surface(window, bar, false, gx, gy));
        [surface release];
        [container release];
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeSurfacePointTest, LetterboxedFullscreenCornersMapIntoTheFit) {
    // StarCraft: a 640x480 DirectDraw mode aspect-fit into a 1800x1130
    // display-covering window (fit 1507x1130, x inset 146.5). ClipCursor
    // (0,0)-(640,480) maps its corner pixels through the presentation; a
    // corner that silently fell back to 1:1 desktop coordinates confined the
    // host cursor to a 640x480-point box in the top-left of the display.
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle = 0x4b140;
        top.style = WS::POPUP;
        top.rect = {0, 0, 1800, 1130};
        top.client_rect = top.rect;
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        NSView* background = (__bridge NSView*)bridge.native_presentation_view(top.handle);
        NSWindow* window = background.window;
        background.wantsLayer = YES;
        background.hidden = NO;
        CAMetalLayer* layer = [CAMetalLayer layer];
        [background.layer addSublayer:layer];
        // Before the first present no surface owns the point: a covering
        // window must fail (the clip then holds the whole display) instead
        // of mapping guest 639,479 to desktop point 639,479.
        layer.drawableSize = CGSizeMake(0, 0);
        NSPoint unready{};
        EXPECT_FALSE(map_guest_surface_point_to_native(window, 639, 479, unready, true));
        EXPECT_TRUE(map_guest_surface_point_to_native(window, 639, 479, unready, false));
        layer.drawableSize = CGSizeMake(640, 480);
        const double scale = 1130.0 / 480.0;
        const double inset = (1800.0 - 640.0 * scale) / 2.0;
        for (const auto& corner : {std::array<i32, 2>{0, 0}, std::array<i32, 2>{639, 0},
                                   std::array<i32, 2>{0, 479}, std::array<i32, 2>{639, 479}}) {
            SCOPED_TRACE(corner[0]);
            SCOPED_TRACE(corner[1]);
            NSPoint native{};
            ASSERT_TRUE(map_guest_surface_point_to_native(window, corner[0], corner[1], native, true));
            const NSPoint client = [window.contentView convertPoint:native fromView:nil];
            const NSPoint win32 = vtr_win32_point_in_view(window.contentView, client);
            EXPECT_NEAR(win32.x, inset + (corner[0] + 0.5) * scale, 0.01);
            EXPECT_NEAR(win32.y, (corner[1] + 0.5) * scale, 0.01);
        }
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeMessageWaitTest, NestedHitTestPumpsCannotReverseButtonTransitions) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        MessageQueue queue;
        WindowData data;
        data.handle = 0x4a123;
        data.title = L"Native input ordering regression";
        data.rect = {100, 100, 500, 400};
        data.style = WS::POPUP;
        ASSERT_TRUE(bridge.create_window(data.handle, data).is_ok());
        bridge.set_client_drawn_frame(data.handle, true);
        bridge.set_visible(data.handle, true);
        NSWindow* window = nil;
        for (NSWindow* candidate in [NSApp windows]) {
            if ([[candidate title] isEqualToString:@"Native input ordering regression"])
                window = candidate;
        }
        ASSERT_NE(window, nil);
        bridge.pump_events(queue);
        for (bool outer_wait : {false, true}) {
            for (bool inner_wait : {false, true}) {
                SCOPED_TRACE(outer_wait);
                SCOPED_TRACE(inner_wait);
                MSG discarded{};
                while (queue.peek(discarded, NULL_HWND, 0, 0, PM::REMOVE)) {}
                unsigned hit_calls = 0;
                bridge.set_native_hit_test_callback([&](HWND, i32, i32) {
                    ++hit_calls;
                    if (inner_wait) bridge.wait_events(queue, 0);
                    else bridge.pump_events(queue);
                    return static_cast<i32>(1) /* HTCLIENT, generated from winuser.h */;
                });
                const double now = [[NSProcessInfo processInfo] systemUptime];
                for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
                    NSEvent* event = [NSEvent mouseEventWithType:type
                        location:NSMakePoint(20, 20) modifierFlags:0 timestamp:now
                        windowNumber:[window windowNumber] context:nil
                        eventNumber:0 clickCount:1 pressure:1];
                    [NSApp postEvent:event atStart:NO];
                }
                if (outer_wait) bridge.wait_events(queue, 0);
                else bridge.pump_events(queue);
                std::vector<u32> buttons;
                MSG message{};
                while (queue.peek(message, NULL_HWND, 0, 0, PM::REMOVE)) {
                    if (message.hwnd == data.handle &&
                        (message.message == WM::LBUTTONDOWN || message.message == WM::LBUTTONUP))
                        buttons.push_back(message.message);
                }
                EXPECT_EQ(hit_calls, 1u);
                EXPECT_EQ(buttons, (std::vector<u32>{WM::LBUTTONDOWN, WM::LBUTTONUP}));
            }
        }
        bridge.set_native_hit_test_callback({});
        bridge.destroy_window(data.handle);
    }
}

static void check_native_sent_message_wait(bool event_pump) {
    using namespace vitreo::windowing;
    std::atomic<bool> resize{false}, shutdown{false}, sent_message{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool guest_resumed = false;
    bool resumed_inside_native_callback = false;
    std::thread::id reply_thread;
    // ShowWindow and event pumping can both send synchronous notifications
    // back from AppKit. The waiting guest must answer on its own host thread.
    auto native_callback = [&] {
        sent_message = true;
        std::unique_lock lock(mutex);
        resumed_inside_native_callback = cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return guest_resumed; });
        sent_message = false;
    };
    std::thread guest([&] {
        CocoaBridge::NativeWaitScope wait_scope([&] {
            if (!sent_message.exchange(false)) return;
            std::lock_guard lock(mutex);
            reply_thread = std::this_thread::get_id();
            guest_resumed = true;
            cv.notify_one();
        });
        if (event_pump) {
            vtr_run_on_main_sync_bailout(^{ native_callback(); }, resize, &shutdown);
        } else {
            vtr_run_on_main_sync(^{ native_callback(); });
        }
        finished = true;
    });
    const auto guest_thread = guest.get_id();
    while (!finished) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    guest.join();
    EXPECT_TRUE(resumed_inside_native_callback);
    EXPECT_EQ(reply_thread, guest_thread);
}

TEST(NativeMessageWaitTest, SynchronousNativeCallServicesGuestReply) {
    check_native_sent_message_wait(false);
}

TEST(NativeMessageWaitTest, EventPumpServicesGuestReply) {
    check_native_sent_message_wait(true);
}

// A main thread that is itself waiting -- inside a cross-thread SendMessage to
// a guest thread, pumping events until the reply comes -- may not reach the
// run loop that would run a worker's vtr_run_on_main_sync block. If that
// worker is the thread it waits for, neither proceeds (Satisfactory: the game
// thread in SetCursor, the main thread nested in WM_ACTIVATE sends). The
// main thread's waits drain the queue themselves; here the main thread only
// ever drains and never runs its run loop, and the worker's requests must
// still complete, on the main thread, in order.
TEST(NativeMessageWaitTest, MainThreadWaitRunsWorkerRequestsWithoutTheRunLoop) {
    using namespace vitreo::windowing;
    ASSERT_TRUE([NSThread isMainThread]);
    // Shared state on the heap: if the requests never complete, the worker is
    // left behind still blocked, and must not point into this frame.
    struct State {
        std::atomic<bool> finished{false};
        std::atomic<int> on_main{0};
        std::mutex order_mutex;
        std::vector<int> order;
    };
    auto state = std::make_shared<State>();
    std::thread worker([state] {
        for (int i = 0; i < 3; ++i) {
            vtr_run_on_main_sync(^{
                if ([NSThread isMainThread]) ++state->on_main;
                std::lock_guard lock(state->order_mutex);
                state->order.push_back(i);
            });
        }
        state->finished = true;
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!state->finished && std::chrono::steady_clock::now() < deadline) {
        vtr_drain_main_work();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!state->finished) {
        worker.detach();
        FAIL() << "the worker's main-thread requests waited for the run loop";
    }
    worker.join();
    EXPECT_EQ(state->on_main.load(), 3);
    EXPECT_EQ(state->order, (std::vector<int>{0, 1, 2}));
}

@interface NSView (VitreoEditingLookupTest)
- (NSTextField*)vtr_findEditableTextField;
@end

TEST(NativeEditingFocusTest, HiddenDialogFieldCannotStealClipboardShortcut) {
    @autoreleasepool {
        NSView* content = [[NSClassFromString(@"VtrContentView") alloc]
            initWithFrame:NSMakeRect(0, 0, 300, 200)];
        ASSERT_NE(content, nil);
        NSView* hidden_dialog = [[NSView alloc] initWithFrame:content.bounds];
        NSTextField* hidden_field = [[NSTextField alloc]
            initWithFrame:NSMakeRect(0, 0, 100, 20)];
        [hidden_field setEditable:YES];
        [hidden_dialog addSubview:hidden_field];
        [hidden_dialog setHidden:YES];
        [content addSubview:hidden_dialog];
        EXPECT_EQ([content vtr_findEditableTextField], nil);
        [hidden_dialog setHidden:NO];
        EXPECT_EQ([content vtr_findEditableTextField], hidden_field);
        [hidden_field release];
        [hidden_dialog release];
        [content release];
    }
}

#include <windowing/cocoa/cocoa_bridge.hpp>
#include <windowing/messages/message_queue.hpp>

TEST(NativeMenuTest, PreservesNestedMenusSeparatorsAndExplicitShortcuts) {
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    MessageQueue queue;
    bridge.set_message_queue(queue);
    WindowData window;
    window.handle = 20;
    window.rect = {0, 0, 400, 300}; window.client_rect = window.rect;
    ASSERT_TRUE(bridge.create_window(20, window).is_ok());
    Win32MenuItem copy; copy.id = 101; copy.text = L"&Copy\tCtrl+C";
    Win32MenuItem paste; paste.id = 102; paste.text = L"&Paste\tCtrl+V";
    Win32MenuItem separator; separator.is_separator = true;
    Win32MenuItem leaf; leaf.id = 103; leaf.text = L"&Nested action\tCtrl+Shift+N";
    Win32MenuItem group; group.is_popup = true; group.text = L"Group"; group.children = {leaf};
    Win32MenuItem edit; edit.is_popup = true; edit.text = L"&Edit";
    edit.children = {copy, paste, separator, group};
    bridge.install_menu_bar(20, Win32MenuBar{{edit}});
    NSMenu* menu = NSApp.mainMenu;
    NSMenu* nativeEdit = nil;
    NSInteger edits = 0;
    for (NSMenuItem* item in menu.itemArray) if ([item.title isEqualToString:@"Edit"]) { ++edits; nativeEdit = item.submenu; }
    EXPECT_EQ(edits, 1);
    ASSERT_NE(nativeEdit, nil);
    ASSERT_GE(nativeEdit.numberOfItems, 4); // AppKit may append writing tools.
    EXPECT_TRUE([nativeEdit itemAtIndex:2].isSeparatorItem);
    NSMenuItem* nativeCopy = [nativeEdit itemAtIndex:0];
    EXPECT_TRUE([nativeCopy.keyEquivalent isEqualToString:@"c"]);
    EXPECT_EQ(nativeCopy.keyEquivalentModifierMask, NSEventModifierFlagCommand);
    NSMenu* nested = [nativeEdit itemAtIndex:3].submenu;
    ASSERT_NE(nested, nil); ASSERT_EQ(nested.numberOfItems, 1);
    NSMenuItem* action = [nested itemAtIndex:0];
    EXPECT_TRUE([action.title isEqualToString:@"Nested action"]);
    EXPECT_EQ(action.keyEquivalentModifierMask, NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [nested performActionForItemAtIndex:0];
    MSG message{};
    ASSERT_TRUE(queue.peek(message, 0, WM::COMMAND, WM::COMMAND, PM::REMOVE));
    EXPECT_EQ(message.hwnd, 20u); EXPECT_EQ(message.wParam, 103u);
    // A menu need not display its accelerator table as text.
    edit.children[0].text = L"&Copy";
    edit.children[1].text = L"&Paste";
    bridge.install_menu_bar(20, Win32MenuBar{{edit}});
    edits = 0;
    for (NSMenuItem* item in NSApp.mainMenu.itemArray)
        if ([item.title isEqualToString:@"Edit"]) ++edits;
    EXPECT_EQ(edits, 1);

}

TEST(NativeScrollbarTest, RendersRangeAndVisibilityWithoutScrollingGuestContent) {
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    WindowData window; window.title = L"Scrollbar contract";
    window.rect = {0, 0, 400, 300}; window.client_rect = window.rect;
    ASSERT_TRUE(bridge.create_window(30, window).is_ok());
    ScrollBarState state;
    state.minimum = 0; state.maximum = 99; state.page = 25; state.position = 50;
    state.visible = true;
    ASSERT_TRUE(bridge.update_window_scrollbar(30, 1, state, true));
    NSScroller* scroller = nil;
    for (NSWindow* native in NSApp.windows) if ([native.title isEqualToString:@"Scrollbar contract"])
        for (NSView* view in native.contentView.subviews)
            if ([view isKindOfClass:NSScroller.class]) scroller = (NSScroller*)view;
    ASSERT_NE(scroller, nil);
    EXPECT_FALSE(scroller.hidden);
    EXPECT_NEAR(scroller.knobProportion, 0.25, 0.001);
    EXPECT_NEAR(scroller.doubleValue, 2.0 / 3, 0.001);
    state.visible = false;
    ASSERT_TRUE(bridge.update_window_scrollbar(30, 1, state, true));
    EXPECT_TRUE(scroller.hidden);
    bridge.destroy_window(30);
}

TEST(NativeToolbarTest, MappedButtonFaceIsTransparentAndImagePixelsRemainOpaque) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    WindowData window; window.title = L"Toolbar alpha contract";
    window.rect = {0, 0, 300, 100}; window.client_rect = window.rect;
    ASSERT_TRUE(bridge.create_window(40, window).is_ok());
    WindowData toolbar; toolbar.handle = 41; toolbar.parent = 40;
    toolbar.rect = {0, 0, 300, 24}; toolbar.client_rect = toolbar.rect;
    bridge.add_child_view(40, 41, toolbar, 0, L"ToolbarWindow32", L"");
    // A 2x1 24-bit BMP: mapped COLOR_BTNFACE followed by an opaque red pixel.
    std::vector<u8> bmp(62, 0);
    auto word = [&](usize at, u16 v) { bmp[at] = v; bmp[at + 1] = v >> 8; };
    auto dword = [&](usize at, u32 v) { word(at, v); word(at + 2, v >> 16); };
    bmp[0] = 'B'; bmp[1] = 'M'; dword(2, 62); dword(10, 54); dword(14, 40);
    dword(18, 2); dword(22, 1); word(26, 1); word(28, 24); dword(34, 8);
    bmp[54] = bmp[55] = bmp[56] = 240; bmp[59] = 255;
    const u32 bitmap = bridge.register_bitmap_bmp(bmp);
    ASSERT_NE(bitmap, 0u); ASSERT_EQ(bridge.toolbar_add_bitmap(41, bitmap, 1), 0);
    CocoaBridge::ToolbarButton button; button.image = 0; button.command = 1; button.state = 4;
    ASSERT_TRUE(bridge.toolbar_insert_buttons(41, 0, {button}));
    NSButton* nativeButton = nil;
    for (NSWindow* native in NSApp.windows) if ([native.title isEqualToString:@"Toolbar alpha contract"])
        for (NSView* container in native.contentView.subviews)
            for (NSView* child in container.subviews)
                if ([child isKindOfClass:NSButton.class] && child.tag == 1) nativeButton = (NSButton*)child;
    ASSERT_NE(nativeButton, nil);
    NSImageRep* representation = nativeButton.image.representations.firstObject;
    ASSERT_TRUE([representation isKindOfClass:NSBitmapImageRep.class]);
    NSBitmapImageRep* pixels = (NSBitmapImageRep*)representation;
    ASSERT_EQ(pixels.pixelsWide, 2); ASSERT_EQ(pixels.pixelsHigh, 1);
    // Inspect the stored pixel values before host display-profile conversion.
    EXPECT_EQ(pixels.bitmapData[3], 0);
    EXPECT_EQ(pixels.bitmapData[4], 255);
    EXPECT_EQ(pixels.bitmapData[5], 0);
    EXPECT_EQ(pixels.bitmapData[6], 0);
    EXPECT_EQ(pixels.bitmapData[7], 255);
    bridge.destroy_window(40);
}

// SetWindowRgn: Learn says nothing outside the region is displayed. A shaped
// window (Unreal's splash cuts its logo out with a region built from the
// image's opaque pixels) used to keep only the region's bounding box, so the
// cut-away pixels showed as a black rectangle.
TEST(NativeWindowRegionTest, EveryRectangleOfTheRegionShapesTheWindow) {
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    WindowData window; window.title = L"Region contract";
    window.style = 0x80000000u;  // WS_POPUP
    window.rect = {0, 0, 100, 50}; window.client_rect = window.rect;
    ASSERT_TRUE(bridge.create_window(50, window).is_ok());
    NSWindow* native = nil;
    for (NSWindow* w in NSApp.windows)
        if ([w.title isEqualToString:@"Region contract"]) native = w;
    ASSERT_NE(native, nil);
    const BOOL was_opaque = native.isOpaque;

    // Two bars with a gap between them: (0,0)-(40,50) and (60,0)-(100,50).
    const std::vector<std::array<vitreo::i32, 4>> parts = {{0, 0, 40, 50}, {60, 0, 100, 50}};
    bridge.set_window_rgn(50, true, 0, 0, 100, 50, false, parts);
    EXPECT_FALSE(native.isOpaque);
    EXPECT_EQ(native.backgroundColor.alphaComponent, 0.0);
    NSView* content = native.contentView;
    ASSERT_TRUE([content.layer.mask isKindOfClass:[CAShapeLayer class]]);
    CGPathRef path = ((CAShapeLayer*)content.layer.mask).path;
    const CGFloat sx = content.bounds.size.width / 100.0;
    const CGFloat h = content.bounds.size.height;
    const CGFloat sy = h / 50.0;
    // Cocoa y grows upward; sample the middle row.
    const auto inside = [&](double x) {
        return CGPathContainsPoint(path, nullptr, CGPointMake(x * sx, h - 25 * sy), false);
    };
    EXPECT_TRUE(inside(20));   // first bar
    EXPECT_FALSE(inside(50));  // the gap is not shown
    EXPECT_TRUE(inside(80));   // second bar

    // A region covering the whole window hides nothing: back to opaque.
    bridge.set_window_rgn(50, true, 0, 0, 100, 50, false, {});
    EXPECT_EQ(native.isOpaque, was_opaque);

    // Removing the region restores the window as it was.
    bridge.set_window_rgn(50, true, 0, 0, 100, 50, false, parts);
    bridge.set_window_rgn(50, false, 0, 0, 0, 0, false, {});
    EXPECT_EQ(native.isOpaque, was_opaque);
    EXPECT_EQ(content.layer.mask, nil);
    bridge.destroy_window(50);
}

// A shown window that has not painted is its class brush's erase colour on
// Windows, and opaque black for a NULL brush
// (tools/oracle_probe/unpainted_window_probe). AppKit's light plate showed
// instead: Steam's SDL popup menus (NULL brush) flashed white.
TEST(NativeWindowPlateTest, UnpaintedWindowShowsItsEraseColourOrBlack) {
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    const auto plate_of = [](const wchar_t* title) -> NSColor* {
        NSString* t = [NSString stringWithCharacters:(const unichar*)std::u16string(
                                                         title, title + wcslen(title)).data()
                                              length:wcslen(title)];
        for (NSWindow* w in NSApp.windows)
            if ([w.title isEqualToString:t])
                return [w.backgroundColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        return nil;
    };
    WindowData null_brush; null_brush.title = L"Plate null brush";
    null_brush.style = 0x86000000u;  // WS_POPUP|WS_CLIPSIBLINGS|WS_CLIPCHILDREN (SDL_app)
    null_brush.ex_style = 0x08000080u;
    null_brush.rect = {0, 0, 120, 80}; null_brush.client_rect = null_brush.rect;
    ASSERT_TRUE(bridge.create_window(60, null_brush).is_ok());
    NSColor* black = plate_of(L"Plate null brush");
    ASSERT_NE(black, nil);
    EXPECT_EQ(black.redComponent, 0.0);
    EXPECT_EQ(black.greenComponent, 0.0);
    EXPECT_EQ(black.blueComponent, 0.0);
    EXPECT_EQ(black.alphaComponent, 1.0);

    WindowData green_brush = null_brush; green_brush.title = L"Plate green brush";
    green_brush.erase_color = 0x0000FF00u;  // COLORREF green
    ASSERT_TRUE(bridge.create_window(61, green_brush).is_ok());
    NSColor* green = plate_of(L"Plate green brush");
    ASSERT_NE(green, nil);
    EXPECT_NEAR(green.greenComponent, 1.0, 0.001);
    EXPECT_NEAR(green.redComponent, 0.0, 0.001);

    bridge.destroy_window(60);
    bridge.destroy_window(61);
}

// A window the platform moves itself (AppKit runs the caption drag that
// DefWindowProc hands it for WM_NCLBUTTONDOWN(HTCAPTION)) must report its new
// Win32 client origin to USER32. Without this, GetWindowRect/ClientToScreen
// kept the pre-drag position and every popup the guest placed from them
// (Steam's menus) opened where the window used to be.
TEST(NativeWindowMoveTest, PlatformMoveReportsNewClientOriginButGuestMoveDoesNot) {
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    WindowData window; window.title = L"Native move contract";
    window.style = 0x80000000u;  // WS_POPUP
    window.rect = {100, 120, 300, 220}; window.client_rect = {0, 0, 200, 100};
    ASSERT_TRUE(bridge.create_window(70, window).is_ok());
    bridge.set_frame(70, vitreo::windowing::Rect{100, 120, 300, 220});
    std::vector<std::array<vitreo::i32, 3>> moves;
    bridge.set_native_move_callback([&](HWND hwnd, vitreo::i32 x, vitreo::i32 y) {
        moves.push_back({static_cast<vitreo::i32>(hwnd), x, y});
    });
    NSWindow* native = nil;
    for (NSWindow* w in NSApp.windows)
        if ([w.title isEqualToString:@"Native move contract"]) native = w;
    ASSERT_NE(native, nil);

    // The guest's own SetWindowPos goes through set_frame: no echo back.
    bridge.set_frame(70, vitreo::windowing::Rect{110, 130, 310, 230});
    EXPECT_TRUE(moves.empty());

    // A platform move: 40 right and 30 down in Win32 terms (Cocoa y grows up).
    NSPoint origin = native.frame.origin;
    [native setFrameOrigin:NSMakePoint(origin.x + 40, origin.y - 30)];
    ASSERT_EQ(moves.size(), 1u);
    EXPECT_EQ(moves[0][0], 70);
    EXPECT_EQ(moves[0][1], 150);
    EXPECT_EQ(moves[0][2], 160);

    // The same position again is not a move.
    [native setFrameOrigin:NSMakePoint(origin.x + 40, origin.y - 30)];
    EXPECT_EQ(moves.size(), 1u);
    bridge.destroy_window(70);
}

TEST(NativeWindowMoveTest, NativeResizeDelegateBalancesGestureBoundaries) {
    using namespace vitreo::windowing;
    CocoaBridge bridge;
    WindowData window;
    window.title = L"Native resize lifecycle";
    window.style = WS::POPUP | WS::THICKFRAME;
    window.rect = {100, 120, 300, 220};
    window.client_rect = {0, 0, 200, 100};
    ASSERT_TRUE(bridge.create_window(72, window).is_ok());
    NSWindow* native = nil;
    for (NSWindow* w in NSApp.windows)
        if ([w.title isEqualToString:@"Native resize lifecycle"]) native = w;
    ASSERT_NE(native, nil);
    std::vector<bool> phases;
    bridge.set_native_move_size_callback([&](HWND hwnd, bool entering) {
        EXPECT_EQ(hwnd, 72u);
        phases.push_back(entering);
    });
    id<NSWindowDelegate> delegate = native.delegate;
    NSNotification* note = [NSNotification notificationWithName:@"lifecycle-test" object:native];
    [delegate windowWillStartLiveResize:note];
    [delegate windowWillStartLiveResize:note];
    [delegate windowDidEndLiveResize:note];
    [delegate windowDidEndLiveResize:note];
    EXPECT_EQ(phases, (std::vector<bool>{true, false}));
    bridge.set_frame(72, vitreo::windowing::Rect{110, 130, 310, 230});
    EXPECT_EQ(phases.size(), 2u); // Programmatic geometry is not a modal gesture.
    bridge.destroy_window(72);
}

// A press-and-drag in a guest window's client area is client input: the
// guest's WM_NCHITTEST decides, only HTCAPTION moves the window (Learn
// WM_NCHITTEST, DefWindowProc WM_NCLBUTTONDOWN), and AppKit's
// drag-by-background rule never applies to guest pixels (NSView answers
// mouseDownCanMoveWindow YES for a non-opaque view by default).
TEST(NativeWindowDragTest, ClientAreaPressNeverMovesTheWindow) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        MessageQueue queue;
        WindowData top;
        top.handle = 0x4b200;
        top.title = L"Native drag regression";
        top.style = WS::POPUP;
        top.rect = {100, 100, 740, 580};
        top.client_rect = {0, 0, 640, 480};
        ASSERT_TRUE(bridge.create_window(top.handle, top).is_ok());
        bridge.set_client_drawn_frame(top.handle, true);
        NSWindow* window = nil;
        for (NSWindow* candidate in [NSApp windows])
            if ([[candidate title] isEqualToString:@"Native drag regression"])
                window = candidate;
        ASSERT_NE(window, nil);
        NSView* metal = (__bridge NSView*)bridge.native_presentation_view(top.handle);
        ASSERT_NE(metal, nil);
        EXPECT_FALSE([metal mouseDownCanMoveWindow]);
        EXPECT_FALSE([[window contentView] mouseDownCanMoveWindow]);
        EXPECT_FALSE([window isMovableByWindowBackground]);

        const NSRect before = [window frame];
        i32 answer = 1;
        int hit_tests = 0;
        bridge.set_native_hit_test_callback(
            [&](HWND, i32, i32) { ++hit_tests; return answer; });
        // HTCLIENT, HTNOWHERE, HTTRANSPARENT, HTERROR, HTBOTTOMRIGHT: no
        // move; the press goes to the guest as client mouse input.
        for (const i32 hit : {1, 0, -1, -2, 17}) {
            answer = hit;
            EXPECT_FALSE(bridge.perform_guest_frame_mouse_down(
                nullptr, top.handle, 320, 240)) << hit;
            EXPECT_TRUE(NSEqualRects([window frame], before)) << hit;
        }
        EXPECT_EQ(hit_tests, 5) << "the guest's WM_NCHITTEST decides";
        bridge.post_client_mouse_message(queue, top.handle, WM::LBUTTONDOWN,
                                         0x0001, (240u << 16) | 320u);
        MSG msg{};
        bool saw_down = false;
        while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE))
            saw_down = saw_down || (msg.message == WM::LBUTTONDOWN &&
                                    msg.hwnd == top.handle);
        EXPECT_TRUE(saw_down);
        bridge.destroy_window(top.handle);
    }
}

TEST(NativeMessageWaitTest, ControlTextReadCompletesDuringMainThreadCooperativeWait) {
    using namespace vitreo;
    using namespace vitreo::windowing;
    ASSERT_TRUE([NSThread isMainThread]);
    @autoreleasepool {
        CocoaBridge bridge;
        WindowData top;
        top.handle=0x4b200; top.title=L"Cooperative text read";
        top.style=WS::POPUP; top.rect={0,0,300,120}; top.client_rect={0,0,300,120};
        ASSERT_TRUE(bridge.create_window(top.handle,top).is_ok());
        WindowData child;
        child.handle=0x4b201; child.parent=top.handle;
        child.rect={0,0,200,40}; child.client_rect={0,0,200,40};
        bridge.add_child_view(top.handle,child.handle,child,0x0081,L"Edit",L"snapshot");
        NSWindow* native_window=nil;
        for(NSWindow* window in [NSApp windows])
            if([[window title] isEqualToString:@"Cooperative text read"]) native_window=window;
        ASSERT_NE(native_window,nil);
        NSTextField* field=nil;
        std::vector<NSView*> pending{native_window.contentView};
        while(!pending.empty()) {
            NSView* view=pending.back(); pending.pop_back();
            if([view isKindOfClass:[NSTextField class]]) { field=(NSTextField*)view; break; }
            for(NSView* subview in [view subviews]) pending.push_back(subview);
        }
        ASSERT_NE(field,nil);
        for(bool editing : {false,true}) {
            if(editing) {
                [field selectText:nil];
                NSTextView* editor=(NSTextView*)[native_window firstResponder];
                ASSERT_TRUE([editor isKindOfClass:[NSTextView class]]);
                ASSERT_EQ([editor delegate],(id<NSTextViewDelegate>)field);
                [editor setString:@"live editing"];
            }
            std::atomic<bool> finished{false};
            std::wstring result;
            std::thread worker([&] {
                @autoreleasepool { result=bridge.get_control_text(child.handle); }
                finished=true;
            });
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);
            // A synchronous guest-message wait only services broker requests. It
            // cannot enter the AppKit run loop to service raw dispatch_sync.
            while(!finished && std::chrono::steady_clock::now()<deadline) {
                vtr_drain_main_work();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool cooperative=finished;
            // Always release the baseline's raw-dispatch worker before teardown.
            while(!finished) CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,true);
            worker.join();
            EXPECT_TRUE(cooperative);
            EXPECT_EQ(result,editing ? L"live editing" : L"snapshot");
        }
        bridge.destroy_window(top.handle);
    }
}
