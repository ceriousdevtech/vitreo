// SPDX-License-Identifier: LGPL-2.1-only
/// MessageBox uType decoding (winuser.h values; Learn nf-winuser-messageboxw).
#include <gtest/gtest.h>
#include <windowing/model/message_box_layout.hpp>

using namespace vitreo;
namespace mb = vitreo::windowing::message_box;

TEST(MessageBoxLayout, CancelTryContinueHasItsThreeButtons) {
    const auto l = mb::layout(0x6);  // MB_CANCELTRYCONTINUE
    ASSERT_EQ(l.count, 3u);
    EXPECT_EQ(l.buttons[0].id, mb::IDCANCEL);
    EXPECT_EQ(l.buttons[1].id, 10);  // IDTRYAGAIN
    EXPECT_EQ(l.buttons[2].id, 11);  // IDCONTINUE
    EXPECT_TRUE(l.has_cancel);
    EXPECT_EQ(mb::default_result(0x6), mb::IDCANCEL);
}

TEST(MessageBoxLayout, DefButtonSelectsTheDefault) {
    EXPECT_EQ(mb::default_result(0x4 | 0x100), mb::IDNO);          // YESNO|DEFBUTTON2
    EXPECT_EQ(mb::default_result(0x2), mb::IDABORT);               // ABORTRETRYIGNORE
    EXPECT_EQ(mb::default_result(0x2 | 0x200), mb::IDIGNORE);      // |DEFBUTTON3
    EXPECT_EQ(mb::default_result(0x0 | 0x300), mb::IDOK);          // out of range -> first
    EXPECT_EQ(mb::default_result(0x5 | 0x100), mb::IDCANCEL);      // RETRYCANCEL|DEFBUTTON2
}

TEST(MessageBoxLayout, HelpButtonAndIcons) {
    const auto l = mb::layout(0x1 | 0x4000 | 0x200);  // OKCANCEL|HELP|DEFBUTTON3
    ASSERT_EQ(l.count, 3u);
    EXPECT_EQ(l.buttons[2].id, mb::IDHELP_BUTTON);
    EXPECT_EQ(l.default_index, 2u);
    EXPECT_EQ(mb::default_result(0x1 | 0x4000 | 0x200), mb::IDOK);
    EXPECT_EQ(mb::layout(0x10).icon, mb::Icon::Error);
    EXPECT_EQ(mb::layout(0x20).icon, mb::Icon::Question);
    EXPECT_EQ(mb::layout(0x30).icon, mb::Icon::Warning);
    EXPECT_EQ(mb::layout(0x40).icon, mb::Icon::Information);
}
