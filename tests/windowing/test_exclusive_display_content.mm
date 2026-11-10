// A top-level window beside an exclusive display mode is placed through the
// aspect fit with its content view's bounds at the guest client size. The
// guest pixel surface must stay on the client origin across refits: Armada
// II's menu dialog, fitted at 800x600 and then refitted at 640x480, showed
// only its top-left corner near the screen's bottom right.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/cocoa_bridge.hpp>

using namespace vitreo::windowing;

namespace {

NSView* make_content_with_surface(NSView** surface) {
    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 600)];
    NSView* pixels = [[NSView alloc] initWithFrame:content.bounds];
    pixels.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:pixels];
    *surface = pixels;
    return content;
}

// Native frame (scaled) then guest bounds, as place_in_exclusive_display does.
void place(NSView* content, NSSize frame, NSSize guest_client) {
    [content setFrameSize:frame];
    CocoaBridge::set_content_bounds((__bridge void*)content, guest_client.width,
                                    guest_client.height);
}

}  // namespace

TEST(ExclusiveDisplayContent, SurfaceStaysOnClientOriginAcrossRefits) {
    @autoreleasepool {
        NSView* surface = nil;
        NSView* content = make_content_with_surface(&surface);
        // An 800x600 dialog in a 640x480 mode on a 2560x1440 display (3x)
        // overhangs the fit; the game then resizes it to its 640x480 owner.
        place(content, NSMakeSize(2400, 1800), NSMakeSize(800, 600));
        place(content, NSMakeSize(1920, 1440), NSMakeSize(640, 480));
        EXPECT_TRUE(NSEqualRects(content.bounds, NSMakeRect(0, 0, 640, 480)));
        EXPECT_TRUE(NSEqualRects(surface.frame, NSMakeRect(0, 0, 640, 480)))
            << NSStringFromRect(surface.frame).UTF8String;
        // The client's top-left guest pixel is the native frame's top-left.
        const NSPoint top_left = [content convertPoint:NSMakePoint(0, 480) toView:nil];
        EXPECT_DOUBLE_EQ(top_left.x, 0.0);
        EXPECT_DOUBLE_EQ(top_left.y, 1440.0);
        // Back to 1:1 when the exclusive mode ends.
        place(content, NSMakeSize(800, 600), NSMakeSize(800, 600));
        EXPECT_TRUE(NSEqualRects(surface.frame, NSMakeRect(0, 0, 800, 600)));
    }
}

TEST(ExclusiveDisplayContent, ScaledBoundsAloneMoveTheSurface) {
    // Discriminator: plain setBoundsSize leaves AppKit's autoresizing to move
    // the full-cover surface, which is the defect the helper exists for.
    @autoreleasepool {
        NSView* surface = nil;
        NSView* content = make_content_with_surface(&surface);
        [content setFrameSize:NSMakeSize(2400, 1800)];
        [content setBoundsSize:NSMakeSize(800, 600)];
        [content setFrameSize:NSMakeSize(1920, 1440)];
        [content setBoundsSize:NSMakeSize(640, 480)];
        EXPECT_FALSE(NSEqualRects(surface.frame, NSMakeRect(0, 0, 640, 480)))
            << NSStringFromRect(surface.frame).UTF8String;
    }
}
