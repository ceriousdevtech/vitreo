// WM_SETFONT with a guest face gives the native control the font GDI measured
// with (vtr_gdi_matching_font). Regression: that font was autoreleased twice
// (CFBridgingRelease already autoreleases without ARC); the next pool drain
// freed it under the NSTextView and the installer crashed in -setFont:.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/objc_helpers.h>
#include <windowing/cocoa/cocoa_bridge.hpp>

using namespace vitreo;
using namespace vitreo::windowing;

namespace {

TEST(ControlFontFaceTest, GuestFaceFontSurvivesPoolDrains) {
    CocoaBridge bridge;
    WindowData parent;
    parent.handle = 60;
    parent.rect = {0, 0, 400, 200};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(60, parent).is_ok());
    WindowData edit;
    edit.handle = 61;
    edit.parent = 60;
    edit.style = WS::CHILD | WS::VISIBLE | 0x0004;  // ES_MULTILINE
    edit.rect = {0, 0, 300, 100};
    edit.client_rect = edit.rect;
    bridge.add_child_view(60, 61, edit, 0x0081, L"Edit", L"text");
    WindowData label;
    label.handle = 62;
    label.parent = 60;
    label.style = WS::CHILD | WS::VISIBLE;
    label.rect = {0, 110, 300, 130};
    label.client_rect = label.rect;
    bridge.add_child_view(60, 62, label, 0x0082, L"Static", L"label");

    for (int round = 0; round < 3; ++round) {
        @autoreleasepool {
            EXPECT_TRUE(bridge.set_control_font_face(61, u"Tahoma", 8.25f, 400, false));
            EXPECT_TRUE(bridge.set_control_font_face(62, u"Tahoma", 8.25f, 700, false));
        }
    }
    @autoreleasepool {
        NSFont* matching = vtr_gdi_matching_font(u"Tahoma", 8.25f, 400, false);
        ASSERT_NE(matching, nil);
        EXPECT_GT([matching pointSize], 0.0);
    }
    bridge.destroy_window(62);
    bridge.destroy_window(61);
}

}  // namespace
