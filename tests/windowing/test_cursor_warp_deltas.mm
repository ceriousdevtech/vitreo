/// @file test_cursor_warp_deltas.mm
/// @brief Vitreo's own cursor warps are not device motion.
///
/// Windows: DirectInput's relative mouse axes and RAWMOUSE lLastX/lLastY are
/// device motion; SetCursorPos and the ClipCursor confinement move the cursor
/// without any. macOS: an NSEvent's deltaX/deltaY are the cursor movement since
/// the previous mouse event (kCGMouseEventDeltaX), and CGWarpMouseCursorPosition
/// moves the cursor without an event, so the next event carries the warp.
/// An autolocking relative-mouse guest warps to the top-left and back to the
/// centre on every mode change; the next mouse event's delta reached
/// DirectInput as a down-right throw that pinned the guest cursor in the corner.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <windowing/cocoa/cursor_warp_deltas.hpp>

@interface VtrDeltaMouseEvent : NSEvent
@property(nonatomic) CGFloat selectedDeltaX;
@property(nonatomic) CGFloat selectedDeltaY;
@property(nonatomic) NSTimeInterval selectedTimestamp;
@end
@implementation VtrDeltaMouseEvent
- (CGFloat)deltaX { return self.selectedDeltaX; }
- (CGFloat)deltaY { return self.selectedDeltaY; }
- (NSTimeInterval)timestamp { return self.selectedTimestamp; }
- (NSEventType)type { return NSEventTypeMouseMoved; }
@end

namespace {

using vitreo::windowing::CursorWarpDeltas;

VtrDeltaMouseEvent* moved(NSTimeInterval time, CGFloat dx, CGFloat dy) {
    VtrDeltaMouseEvent* event = [[VtrDeltaMouseEvent alloc] init];
    event.selectedTimestamp = time;
    event.selectedDeltaX = dx;
    event.selectedDeltaY = dy;
    return event;
}

} // namespace

TEST(CursorWarpDeltasTest, WarpBeforeAnEventIsNotDeviceMotion) {
    CursorWarpDeltas warps;
    warps.note_warp(10.0, 960.0, 720.0);
    double dx = 963.0, dy = 718.0;  // the warp plus (3,-2) of hand movement
    warps.remove_warps(10.5, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 3.0);
    EXPECT_DOUBLE_EQ(dy, -2.0);
    EXPECT_TRUE(warps.empty());

    // The warp is spent: the next event's delta is all device motion.
    dx = 5.0;
    dy = 1.0;
    warps.remove_warps(11.0, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 5.0);
    EXPECT_DOUBLE_EQ(dy, 1.0);
}

TEST(CursorWarpDeltasTest, AnEventFromBeforeTheWarpKeepsItsDelta) {
    CursorWarpDeltas warps;
    warps.note_warp(20.0, -960.0, -720.0);
    // Generated before the warp, delivered after it.
    double dx = 2.0, dy = 2.0;
    warps.remove_warps(19.9, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 2.0);
    EXPECT_DOUBLE_EQ(dy, 2.0);
    EXPECT_FALSE(warps.empty());

    dx = -958.0;
    dy = -719.0;
    warps.remove_warps(20.1, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 2.0);
    EXPECT_DOUBLE_EQ(dy, 1.0);
}

TEST(CursorWarpDeltasTest, ConsecutiveWarpsBeforeOneEventAllCome) {
    // SDL's un-grab (to the top-left) then grab (to the centre) with no
    // movement between: the next event carries the net displacement.
    CursorWarpDeltas warps;
    warps.note_warp(30.0, -1378.5, -398.5);  // (1700,400) -> (321.5,1.5)
    warps.note_warp(30.1, 960.0, 720.0);     // (321.5,1.5) -> (1281.5,721.5)
    double dx = -414.5, dy = 319.5;          // (1281.5+4, 721.5-2) - (1700,400)
    warps.remove_warps(30.2, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 4.0);
    EXPECT_DOUBLE_EQ(dy, -2.0);
}

TEST(CursorWarpDeltasTest, AWarpAfterTheEventWaitsForTheNext) {
    CursorWarpDeltas warps;
    warps.note_warp(40.0, 100.0, 0.0);
    warps.note_warp(41.0, 0.0, 50.0);
    double dx = 101.0, dy = 0.0;
    warps.remove_warps(40.5, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 1.0);
    EXPECT_DOUBLE_EQ(dy, 0.0);
    dx = 0.0;
    dy = 49.0;
    warps.remove_warps(41.5, dx, dy);
    EXPECT_DOUBLE_EQ(dx, 0.0);
    EXPECT_DOUBLE_EQ(dy, -1.0);
}

TEST(CursorWarpDeltasTest, BridgeReportsDeviceMotionOfAHostMouseEvent) {
    using namespace vitreo::windowing;
    @autoreleasepool {
        CocoaBridge bridge;
        void* (*raw)(id) = [](id e) { return (__bridge void*)e; };
        bridge.note_host_cursor_warp(50.0, 960.0, 720.0);

        double dx = 0, dy = 0;
        bridge.native_mouse_device_delta(raw(moved(49.0, 3.0, 3.0)), dx, dy);
        EXPECT_DOUBLE_EQ(dx, 3.0) << "an event from before the warp";
        EXPECT_DOUBLE_EQ(dy, 3.0);

        bridge.native_mouse_device_delta(raw(moved(50.2, 961.0, 722.0)), dx, dy);
        EXPECT_DOUBLE_EQ(dx, 1.0) << "the warp is not device motion";
        EXPECT_DOUBLE_EQ(dy, 2.0);

        bridge.native_mouse_device_delta(raw(moved(50.4, 6.0, -4.0)), dx, dy);
        EXPECT_DOUBLE_EQ(dx, 6.0);
        EXPECT_DOUBLE_EQ(dy, -4.0);
    }
}
