// SPDX-License-Identifier: LGPL-2.1-only
// A WS_CHILD dialog is an ordinary child window: CreateDialogIndirectParam
// gives it the template's rectangle in its parent's client coordinates and
// creating it changes no sibling. Learn DLGTEMPLATE ("x, y: in dialog
// template units ... relative to the upper-left corner of the parent
// window's client area" for WS_CHILD) and MapDialogRect (x * baseX / 4,
// y * baseY / 8). Without DS_SETFONT the base units are 8 x 16 (measured,
// s1win_misc_probe mapdialogrect: 4,8 DLU -> 8,16).
#include <gtest/gtest.h>
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include <cstring>
#include <vector>
using namespace vitreo;
using namespace vitreo::winapi;

namespace {
constexpr u32 kWsChild = 0x40000000u, kWsVisible = 0x10000000u, kWsPopup = 0x80000000u;

/// DLGTEMPLATE with no menu, class, title or items.
std::vector<u8> child_template(u32 style, i16 x, i16 y, i16 cx, i16 cy) {
    std::vector<u8> t(18 + 6, 0);
    std::memcpy(t.data(), &style, 4);
    std::memcpy(t.data() + 10, &x, 2);
    std::memcpy(t.data() + 12, &y, 2);
    std::memcpy(t.data() + 14, &cx, 2);
    std::memcpy(t.data() + 16, &cy, 2);
    return t;
}
}  // namespace

class ChildDialogGeometry : public ::testing::Test {
protected:
    static constexpr GuestAddr kTemplates = 0x500000;
    GuestMemory memory;
    windowing::HwndManager windows;
    WindowClassManager classes;
    windowing::CocoaBridge bridge;
    windowing::MessageQueue queue;
    DialogManager dialogs{windows, classes, bridge, queue, memory};
    windowing::HWND parent = windowing::NULL_HWND;

    void SetUp() override {
        ThreadManager::set_current_thread_id(1);
        ASSERT_TRUE(memory.map_region(kTemplates, 0x1000, MemoryProtection::ReadWrite).is_ok());
        windowing::WindowData frame;
        frame.style = kWsPopup | kWsVisible;
        frame.rect = {0, 0, 1600, 1000};
        frame.client_rect = {0, 0, 1600, 1000};
        frame.visible = true;
        parent = windows.create(frame);
        ASSERT_NE(parent, windowing::NULL_HWND);
        ASSERT_TRUE(bridge.create_window(parent, *windows.find(parent)).is_ok());
    }

    windowing::HWND create(GuestAddr at, const std::vector<u8>& tmpl) {
        EXPECT_TRUE(memory.write_bytes(at, tmpl.data(), tmpl.size()).is_ok());
        return dialogs.create_dialog(0, at, parent, 0, 0, false, true, false);
    }
};

TEST_F(ChildDialogGeometry, ChildDialogKeepsItsTemplateRectangle) {
    // 218 x 69 DLU at (5, 10): the measured template of a setup engine's
    // progress page, which Vitreo stretched to its 2560 x 1440 parent.
    const auto page = create(kTemplates, child_template(kWsChild | kWsVisible, 5, 10, 218, 69));
    ASSERT_NE(page, windowing::NULL_HWND);
    const auto* data = windows.find(page);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.left, 10);           // 5 * 8 / 4
    EXPECT_EQ(data->rect.top, 20);            // 10 * 16 / 8
    EXPECT_EQ(data->rect.right, 10 + 436);    // 218 * 8 / 4
    EXPECT_EQ(data->rect.bottom, 20 + 138);   // 69 * 16 / 8
    EXPECT_EQ(data->client_rect.right - data->client_rect.left, 436);
    EXPECT_EQ(data->client_rect.bottom - data->client_rect.top, 138);
}

TEST_F(ChildDialogGeometry, CreatingAChildDialogHidesNoSibling) {
    const auto first = create(kTemplates, child_template(kWsChild | kWsVisible, 0, 0, 100, 50));
    const auto second = create(kTemplates + 0x100, child_template(kWsChild | kWsVisible, 0, 60, 100, 50));
    ASSERT_NE(first, windowing::NULL_HWND);
    ASSERT_NE(second, windowing::NULL_HWND);
    EXPECT_TRUE(windows.find(first)->visible);
    EXPECT_TRUE(windows.find(second)->visible);
    EXPECT_FALSE(bridge.control_view_is_hidden(first));
    EXPECT_FALSE(bridge.control_view_is_hidden(second));
    EXPECT_EQ(windows.find(second)->rect.top, 120);  // 60 * 16 / 8
}
