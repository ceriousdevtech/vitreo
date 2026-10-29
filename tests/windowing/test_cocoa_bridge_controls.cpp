/// @file test_cocoa_bridge_controls.cpp
/// @brief Tests for CocoaBridge common-control state: tab, combo, status bar and toolbar (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, TabControlStoresItemsSelectionAndItemRect) {
    WindowData parent;
    parent.handle = 20;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = {0, 0, 400, 360};
    ASSERT_TRUE(bridge.create_window(20, parent).is_ok());

    WindowData tabs;
    tabs.handle = 21;
    tabs.parent = 20;
    tabs.rect = {3, 3, 397, 347};
    tabs.client_rect = tabs.rect;
    tabs.user_data = 77;
    bridge.add_child_view(
        20, 21, tabs, 0, L"SysTabControl32", L"");

    EXPECT_EQ(bridge.tab_insert_item(21, 0, L"CPU"), 0);
    EXPECT_EQ(bridge.tab_insert_item(21, 1, L"Mainboard"), 1);
    EXPECT_EQ(bridge.tab_get_item_count(21), 2);
    EXPECT_EQ(bridge.tab_get_cur_sel(21), 0);
    EXPECT_EQ(bridge.tab_set_cur_sel(21, 1), 0);
    EXPECT_EQ(bridge.tab_get_cur_sel(21), 1);

    Rect item_rect{};
    EXPECT_TRUE(bridge.tab_get_item_rect(21, 0, item_rect));
    EXPECT_GT(item_rect.right, item_rect.left);
    EXPECT_GT(item_rect.bottom, item_rect.top);

    WindowData page;
    page.handle = 22;
    page.parent = 21;
    page.rect = {0, 0, 394, 320};
    page.client_rect = page.rect;
    page.visible = true;
    bridge.add_container_view(21, 22, page);
    bridge.set_visible(22, true);
}

TEST_F(CocoaBridgeTest, TabDeletionKeepsLabelsAndDataAligned) {
    WindowData parent;
    parent.handle = 20;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(20, parent).is_ok());
    WindowData tabs;
    tabs.handle = 21;
    tabs.parent = 20;
    tabs.rect = {0, 0, 400, 300};
    tabs.client_rect = tabs.rect;
    bridge.add_child_view(20, 21, tabs, 0, L"SysTabControl32", L"");
    ASSERT_EQ(bridge.tab_insert_item(21, 0, L"first", 101), 0);
    ASSERT_EQ(bridge.tab_insert_item(21, 1, L"second", 202), 1);
    (void)bridge.tab_set_cur_sel(21, 1);
    EXPECT_FALSE(bridge.tab_delete_item(21, -1));
    EXPECT_FALSE(bridge.tab_delete_item(21, 2));
    ASSERT_TRUE(bridge.tab_delete_item(21, 0));
    EXPECT_EQ(bridge.tab_get_item_count(21), 1);
    EXPECT_EQ(bridge.tab_get_cur_sel(21), 0);
    std::wstring label;
    u64 data = 0;
    ASSERT_TRUE(bridge.tab_get_item_text(21, 0, label));
    ASSERT_TRUE(bridge.tab_get_item_lparam(21, 0, data));
    EXPECT_EQ(label, L"second");
    EXPECT_EQ(data, 202u);
    ASSERT_TRUE(bridge.tab_delete_item(21, 0));
    EXPECT_EQ(bridge.tab_get_item_count(21), 0);
    EXPECT_EQ(bridge.tab_get_cur_sel(21), -1);
}

TEST_F(CocoaBridgeTest, NativeTabSelectionQueuesWindowsNotifications) {
    MessageQueue queue;
    bridge.set_message_queue(queue);

    WindowData parent;
    parent.handle = 23;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = {0, 0, 400, 360};
    ASSERT_TRUE(bridge.create_window(23, parent).is_ok());

    WindowData tabs;
    tabs.handle = 24;
    tabs.parent = 23;
    tabs.rect = {3, 3, 397, 347};
    tabs.client_rect = tabs.rect;
    tabs.user_data = 91;
    bridge.add_child_view(
        23, 24, tabs, 0, L"SysTabControl32", L"");

    bridge.enqueue_tab_notify(24, true);
    bridge.enqueue_tab_notify(24, false);

    MSG message{};
    ASSERT_TRUE(queue.get(message));
    EXPECT_EQ(message.hwnd, 23u);
    EXPECT_EQ(message.message, WM::NOTIFY);
    EXPECT_EQ(message.wParam, 91u);
    HWND control = 0;
    bool changing = false;
    EXPECT_TRUE(CocoaBridge::decode_tab_notify_lparam(
        message.lParam, &control, &changing));
    EXPECT_EQ(control, 24u);
    EXPECT_TRUE(changing);

    ASSERT_TRUE(queue.get(message));
    EXPECT_TRUE(CocoaBridge::decode_tab_notify_lparam(
        message.lParam, &control, &changing));
    EXPECT_EQ(control, 24u);
    EXPECT_FALSE(changing);
}

TEST_F(CocoaBridgeTest, TabContentAreaPassesHitsToSiblingPixelView) {
    // Notepad++ DocTab: SysTabControl32 covers the client; Scintilla is a
    // sibling in the display area. Content hits must reach Scintilla.
    WindowData parent;
    parent.handle = 110;
    parent.rect = {0, 0, 400, 300};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(110, parent).is_ok());

    WindowData editor;
    editor.handle = 111;
    editor.parent = 110;
    editor.rect = {2, 26, 398, 298};
    editor.client_rect = {0, 0, 396, 272};
    editor.visible = true;
    editor.wndproc = 0x401000;
    bridge.add_child_view(110, 111, editor, 0, L"Scintilla", L"");

    WindowData tabs;
    tabs.handle = 112;
    tabs.parent = 110;
    tabs.rect = {0, 0, 400, 300};
    tabs.client_rect = tabs.rect;
    tabs.visible = true;
    bridge.add_child_view(110, 112, tabs, 0, L"SysTabControl32", L"");
    ASSERT_EQ(bridge.tab_insert_item(112, 0, L"new 1"), 0);

    // Mid-editor (below the tab strip) → Scintilla sibling.
    EXPECT_EQ(bridge.resolve_client_mouse_target(110, 200, 150), 111u);
}

TEST_F(CocoaBridgeTest, EditableComboPreservesEditTextAndListData) {
    WindowData parent;
    parent.handle = 20;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(20, parent).is_ok());
    WindowData combo;
    combo.handle = 21;
    combo.parent = 20;
    combo.style = WS::CHILD | WS::VISIBLE | 2; // CBS_DROPDOWN
    combo.rect = {0, 0, 200, 100};
    combo.client_rect = combo.rect;
    bridge.add_child_view(20, 21, combo, 0x0085, L"", L"");
    EXPECT_EQ(bridge.combo_add_item(21, L"first"), 0);
    EXPECT_EQ(bridge.combo_add_item(21, L"second"), 1);
    EXPECT_TRUE(bridge.combo_set_item_data(21, 1, 0x123456789ull));
    EXPECT_EQ(bridge.combo_add_item(21, L"inserted", 0), 0);
    bool ok = false;
    EXPECT_EQ(bridge.combo_get_item_data(21, 2, &ok), 0x123456789ull);
    EXPECT_TRUE(ok);
    EXPECT_EQ(bridge.combo_set_cur_sel(21, 2), 2);
    EXPECT_EQ(bridge.get_control_text(21), L"second");
    EXPECT_TRUE(bridge.set_control_text(21, L"typed query"));
    EXPECT_EQ(bridge.get_control_text(21), L"typed query");
    EXPECT_EQ(bridge.combo_get_item_text(21, 2), L"second");
    EXPECT_TRUE(bridge.combo_reset_content(21));
    EXPECT_EQ(bridge.combo_get_count(21), 0);
    EXPECT_EQ(bridge.get_control_text(21), L"");
}

TEST_F(CocoaBridgeTest, StatusPartsRetainTextAcrossResizing) {
    WindowData parent;
    parent.handle = 20;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(20, parent).is_ok());
    WindowData status;
    status.handle = 21;
    status.parent = 20;
    status.style = WS::CHILD | WS::VISIBLE;
    status.rect = {0, 338, 400, 360};
    status.client_rect = {0, 0, 400, 22};
    bridge.add_child_view(20, 21, status, 0, L"msctls_statusbar32", L"");
    EXPECT_TRUE(bridge.status_set_parts(21, {200, -1}));
    EXPECT_TRUE(bridge.status_set_text(21, 0, L"Length: 42"));
    EXPECT_TRUE(bridge.status_set_text(21, 1, L"UTF-8"));
    EXPECT_FALSE(bridge.status_set_text(21, 2, L"invalid"));
    EXPECT_TRUE(bridge.status_set_parts(21, {300, -1}));
    EXPECT_EQ(bridge.status_get_text(21, 0), L"Length: 42");
    EXPECT_EQ(bridge.status_get_text(21, 1), L"UTF-8");
    EXPECT_EQ(bridge.status_get_parts(21), (std::vector<i32>{300, -1}));
}

TEST_F(CocoaBridgeTest, ToolbarKeepsCommandsStateAndSeparatorWidths) {
    WindowData parent;
    parent.handle = 20;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(20, parent).is_ok());
    WindowData toolbar;
    toolbar.handle = 21;
    toolbar.parent = 20;
    toolbar.style = WS::CHILD | WS::VISIBLE;
    toolbar.rect = {0, 0, 400, 24};
    toolbar.client_rect = toolbar.rect;
    bridge.add_child_view(20, 21, toolbar, 0, L"ToolbarWindow32", L"");
    bridge.toolbar_set_size(21, 24, 20);
    EXPECT_TRUE(bridge.toolbar_insert_buttons(21, -1,
        {{-1, 101, 4, 0, 0x123456789ull, L"New"}, {8, 0, 4, 1, 0, L""}, {-1, 102, 4, 0, 0, L"Open"}}));
    EXPECT_EQ(bridge.toolbar_width(21), 56);
    ASSERT_TRUE(bridge.toolbar_set_state(21, 102, 8, true));
    EXPECT_EQ(bridge.toolbar_width(21), 32);
    ASSERT_TRUE(bridge.toolbar_set_state(21, 101, 4, false));
    auto buttons = bridge.toolbar_buttons(21);
    ASSERT_EQ(buttons.size(), 3);
    EXPECT_EQ(buttons[0].command, 101u);
    EXPECT_EQ(buttons[0].data, 0x123456789ull);
    EXPECT_EQ(buttons[0].state & 4, 0);
    EXPECT_FALSE(bridge.toolbar_set_state(21, 999, 4, true));
}

// Win32 child positions are relative to the parent's client top-left, and
// resizing a window never moves its children. A guest-drawn page is an
// unflipped (Y-up) view, so its children's Cocoa origins are bottom-relative;
// growing the page must shift them or they slide down by the delta (Inno
// Setup's scaled wizard drew its MainPanel header ~74 px low).
TEST_F(CocoaBridgeTest, ChildKeepsWin32TopOffsetWhenUnflippedParentGrows) {
    WindowData parent;
    parent.handle = 30;
    parent.rect = {0, 0, 400, 400};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(30, parent).is_ok());

    WindowData page;
    page.handle = 31;
    page.parent = 30;
    page.rect = {0, 0, 300, 200};
    page.client_rect = {0, 0, 300, 200};
    page.visible = true;
    page.wndproc = 0x401000;
    bridge.add_child_view(30, 31, page, 0, L"TNewNotebookPage", L"");

    WindowData panel;
    panel.handle = 32;
    panel.parent = 31;
    panel.rect = {0, 10, 300, 60};
    panel.client_rect = {0, 0, 300, 50};
    panel.visible = true;
    panel.wndproc = 0x401000;
    bridge.add_child_view(31, 32, panel, 0, L"TPanel", L"");

    i32 x = 0, y = 0, w = 0, h = 0;
    ASSERT_TRUE(bridge.control_view_frame(32, x, y, w, h));
    // Unflipped page: origin.y = page_h - top - h.
    ASSERT_EQ(y, 200 - 10 - 50);

    ASSERT_TRUE(bridge.set_control_frame(31, Rect{0, 0, 360, 260}));
    ASSERT_TRUE(bridge.control_view_frame(32, x, y, w, h));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 260 - 10 - 50);  // still 10 px below the page's top edge
    EXPECT_EQ(w, 300);
    EXPECT_EQ(h, 50);
}

// BS_3STATE / BS_AUTO3STATE are checkboxes with BST_INDETERMINATE (Learn:
// Button Styles); VCL's TCheckBox is BS_3STATE. They used to fall through to a
// push button.
TEST_F(CocoaBridgeTest, ThreeStateButtonIsCheckboxWithIndeterminate) {
    WindowData parent;
    parent.handle = 40;
    parent.rect = {0, 0, 300, 100};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(40, parent).is_ok());
    for (const u32 type : {0x05u, 0x06u}) {
        WindowData box;
        box.handle = 41;
        box.parent = 40;
        box.style = WS::CHILD | WS::VISIBLE | type;
        box.rect = {0, 0, 200, 17};
        box.client_rect = box.rect;
        bridge.add_child_view(40, 41, box, 0x0080, L"Button", L"Don't create a folder");
        EXPECT_TRUE(bridge.set_button_state(41, 2));
        EXPECT_EQ(bridge.get_button_check(41), 2u);
        EXPECT_TRUE(bridge.set_button_state(41, 1));
        EXPECT_EQ(bridge.get_button_check(41), 1u);
        EXPECT_TRUE(bridge.set_button_state(41, 0));
        EXPECT_EQ(bridge.get_button_check(41), 0u);
        bridge.destroy_window(41);
    }
    // A two-state checkbox shows BST_INDETERMINATE as checked.
    WindowData plain;
    plain.handle = 42;
    plain.parent = 40;
    plain.style = WS::CHILD | WS::VISIBLE | 0x03u;
    plain.rect = {0, 20, 200, 37};
    plain.client_rect = plain.rect;
    bridge.add_child_view(40, 42, plain, 0x0080, L"Button", L"x");
    EXPECT_TRUE(bridge.set_button_state(42, 2));
    EXPECT_EQ(bridge.get_button_check(42), 1u);
}

// An owner-draw LISTBOX is painted entirely by its owner into a guest-pixel
// overlay above the native list view. Once USER32 claims its input
// (set_control_owns_input), a click there is the list's own WM_LBUTTONDOWN —
// not a hit on the native view underneath, which knows nothing of its items.
TEST_F(CocoaBridgeTest, InputOwningControlOverlayTakesMouse) {
    WindowData parent;
    parent.handle = 130;
    parent.rect = {0, 0, 300, 200};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(130, parent).is_ok());
    WindowData list;
    list.handle = 131;
    list.parent = 130;
    list.style = WS::CHILD | WS::VISIBLE | 0x0020 | 0x0040;  // LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS
    list.rect = {10, 10, 210, 110};
    list.client_rect = {0, 0, 200, 100};
    list.visible = true;
    list.wndproc = 0x401000;
    bridge.add_child_view(130, 131, list, 0x0083, L"ListBox", L"");
    std::vector<u8> pixels(200u * 100u * 4u, 0xFF);
    bridge.update_window_pixels(131, pixels.data(), 200, 100);
    const HWND before = bridge.resolve_client_mouse_target(130, 60, 40);
    EXPECT_NE(before, 131u);  // passes through to the native view
    bridge.set_control_owns_input(131);
    EXPECT_EQ(bridge.resolve_client_mouse_target(130, 60, 40), 131u);
    bridge.update_window_pixels(131, pixels.data(), 200, 100);  // a repaint keeps it
    EXPECT_EQ(bridge.resolve_client_mouse_target(130, 60, 40), 131u);
}
