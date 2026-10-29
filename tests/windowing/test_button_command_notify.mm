// A native push button's click posts WM_COMMAND to its parent with
// wParam = MAKEWPARAM(LOWORD(id), BN_CLICKED) and lParam = the button HWND
// (Learn: BN_CLICKED; WM_COMMAND). The ID is truncated to 16 bits.
//
// Regression: the handler posted the full 32-bit control ID. Delphi VCL sets
// every control's GWL_ID to its own HWND (0x10050), so bit 16 landed in the
// notification code as BN_DOUBLECLICKED and Inno Setup's Next never clicked.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/objc_helpers.h>
#include <windowing/messages/message_queue.hpp>

using namespace vitreo;
using namespace vitreo::windowing;

namespace {

MSG click_with_id(u32 control_id) {
    MessageQueue queue;
    VtrButtonHandler* handler = [[VtrButtonHandler alloc] init];
    handler.parentHwnd = 0x10038;
    handler.controlHwnd = 0x10050;
    handler.controlId = control_id;
    handler.queue = &queue;
    [handler buttonClicked:nil];
    [handler release];
    MSG msg{};
    EXPECT_TRUE(queue.get(msg));
    return msg;
}

TEST(CocoaButtonCommandTest, IdAboveSixteenBitsIsTruncatedAndCodeIsClicked) {
    const MSG msg = click_with_id(0x10050);
    EXPECT_EQ(msg.hwnd, 0x10038u);
    EXPECT_EQ(msg.message, WM::COMMAND);
    EXPECT_EQ(msg.wParam & 0xFFFFu, 0x0050u);      // LOWORD = LOWORD(id)
    EXPECT_EQ((msg.wParam >> 16) & 0xFFFFu, 0u);   // HIWORD = BN_CLICKED
    EXPECT_EQ(msg.wParam, 0x0050u);
    EXPECT_EQ(msg.lParam, 0x10050u);               // button HWND
}

TEST(CocoaButtonCommandTest, DialogIdPassesThrough) {
    const MSG msg = click_with_id(1);  // IDOK
    EXPECT_EQ(msg.wParam, 1u);
    EXPECT_EQ(msg.lParam, 0x10050u);
}

}  // namespace
