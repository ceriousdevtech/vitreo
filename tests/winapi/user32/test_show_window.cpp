/// @file test_show_window.cpp
/// @brief Tests for ShowWindowManager (WIN32-052)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/windows/show_window.hpp>
#include <winapi/user32/windows/set_cursor_pos.hpp>
#include <winapi/user32/dialogs/dialog.hpp>
#include <winapi/user32/classes/window_class.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <windowing/messages/message_queue.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <ipc/hwnd_registry.hpp>

#include <filesystem>
#include <string>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

class FakeControlCursorHost final : public ControlHost, public CursorHost {
public:
    void set_mouse_capture(HWND) override {}
    void track_mouse_event(HWND, bool, bool, u32) override {}
    void cancel_mouse_tracking(HWND, bool, bool) override {}
    void show_tooltip(HWND, const std::wstring&, i32, i32) override {}
    void hide_tooltip(HWND) override {}
    void measure_tooltip(const std::wstring&, i32, i32& w, i32& h) const override {
        w = 0; h = 0;
    }
    void query_mouse_tracking(HWND, bool& leave, bool& hover,
                              u32& hover_ms) const override {
        leave = false; hover = false; hover_ms = 0;
    }
    bool set_control_text(HWND hwnd, const std::wstring& text) override {
        control_text_hwnd = hwnd;
        control_text = text;
        return control_exists;
    }
    std::wstring get_control_text(HWND) const override { return control_text; }
    void set_title(HWND hwnd, std::wstring_view title) override {
        title_hwnd = hwnd;
        window_title = title;
    }
    bool set_control_enabled(HWND hwnd, bool enabled) override {
        enabled_hwnd = hwnd;
        enabled_value = enabled;
        return true;
    }
    bool set_button_check(HWND, bool) override { return false; }
    void apply_auto_button_click(HWND) override {}
    void add_child_view(
        HWND, HWND, const WindowData&, u16, const std::wstring&,
        const std::wstring&) override {}
    bool set_control_image_data(HWND, const u8*, usize) override {
        return false;
    }
    bool set_control_font(HWND, float, u16, bool) override { return false; }
    void add_container_view(HWND, HWND, const WindowData&) override {}
    VoidResult create_window(HWND, const WindowData&) override {
        return VoidResult::ok();
    }
    void destroy_window(HWND) override {}
    void set_visible(HWND, bool) override {}
    MonitorInfo primary_monitor{};
    MonitorInfo get_primary_monitor_info() const override { return primary_monitor; }
    bool activate_window(HWND) override { return false; }
    void pump_events(MessageQueue&) override {}
    void wait_events(MessageQueue&, u32) override {}
    bool pop_tree_notify(TreeNotify&) override { return false; }
    i32 tab_get_item_count(HWND) const override { return -1; }
    void get_native_cursor_pos(i32& x, i32& y) const override {
        x = cursor_x;
        y = cursor_y;
    }
    bool set_native_cursor_pos(i32 x, i32 y) override {
        cursor_x = x;
        cursor_y = y;
        return cursor_move_succeeds;
    }
    void set_cursor_clip(bool enabled, i32 l, i32 t, i32 r,
                         i32 b) override {
        clip_enabled = enabled;
        clip = windowing::Rect{l, t, r, b};
    }
    bool get_cursor_clip(i32& l, i32& t, i32& r, i32& b) const override {
        if (!clip_enabled) return false;
        l = clip.left; t = clip.top; r = clip.right; b = clip.bottom;
        return true;
    }
    u32 last_input_tick_count() const noexcept override { return 0; }

    bool control_exists{false};
    bool cursor_move_succeeds{true};
    HWND control_text_hwnd{0};
    HWND title_hwnd{0};
    HWND enabled_hwnd{0};
    bool enabled_value{true};
    std::wstring control_text;
    std::wstring window_title;
    i32 cursor_x{0};
    i32 cursor_y{0};
    bool clip_enabled{false};
    windowing::Rect clip{};
};

class ShowWindowManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a test window (initially hidden, enabled)
        WindowData data;
        data.title   = L"Test";
        data.parent  = HWND_DESKTOP;
        data.visible = false;
        data.enabled = true;
        data.style   = 0;
        data.rect    = windowing::Rect{100, 100, 740, 580};
        data.client_rect = windowing::Rect{0, 0, 640, 480};
        test_hwnd = hwnd_mgr.create(data);
    }

    HwndManager  hwnd_mgr;
    CocoaBridge  bridge;
    MessageQueue queue;
    HWND         test_hwnd = NULL_HWND;

    ShowWindowManager make_sm() {
        return ShowWindowManager(hwnd_mgr, bridge, queue);
    }
};

// ---------------------------------------------------------------------------
// ShowWindow
// ---------------------------------------------------------------------------

TEST_F(ShowWindowManagerTest, ShowWindowMakesVisible) {
    auto sm = make_sm();
    sm.show_window(test_hwnd, SW::SHOW);

    const WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_TRUE(win->visible);
}

TEST_F(ShowWindowManagerTest, ShowWindowHideMakesInvisible) {
    auto sm = make_sm();
    // First show, then hide
    sm.show_window(test_hwnd, SW::SHOW);
    sm.show_window(test_hwnd, SW::HIDE);

    const WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_FALSE(win->visible);
}

TEST_F(ShowWindowManagerTest, ShowWindowReturnsPreviousVisibleState) {
    auto sm = make_sm();
    // Window starts hidden
    bool prev = sm.show_window(test_hwnd, SW::SHOW);
    EXPECT_FALSE(prev); // was hidden

    prev = sm.show_window(test_hwnd, SW::HIDE);
    EXPECT_TRUE(prev); // was visible
}

TEST_F(ShowWindowManagerTest, ShowWindowOnInvalidHwndReturnsFalse) {
    auto sm = make_sm();
    bool result = sm.show_window(0xDEAD, SW::SHOW);
    EXPECT_FALSE(result);
}

TEST_F(ShowWindowManagerTest, ShowWindowMaintainsIconicZoomedAndStyleState) {
    auto sm = make_sm();

    sm.show_window(test_hwnd, SW::SHOWMAXIMIZED);
    const WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_TRUE(win->visible);
    EXPECT_TRUE(win->maximized);
    EXPECT_FALSE(win->minimized);
    EXPECT_NE(win->style & WS::VISIBLE, 0u);
    EXPECT_NE(win->style & WS::MAXIMIZE, 0u);
    EXPECT_EQ(win->style & WS::MINIMIZE, 0u);
    EXPECT_EQ(win->normal_rect.left, 100);
    EXPECT_EQ(win->normal_rect.right, 740);

    sm.show_window(test_hwnd, SW::MINIMIZE);
    EXPECT_TRUE(win->minimized);
    EXPECT_FALSE(win->maximized);
    EXPECT_TRUE(win->restore_to_maximized);
    EXPECT_NE(win->style & WS::MINIMIZE, 0u);
    EXPECT_EQ(win->style & WS::MAXIMIZE, 0u);

    sm.show_window(test_hwnd, SW::RESTORE);
    EXPECT_FALSE(win->minimized);
    EXPECT_TRUE(win->maximized);

    sm.show_window(test_hwnd, SW::SHOWNORMAL);
    EXPECT_FALSE(win->minimized);
    EXPECT_FALSE(win->maximized);
    EXPECT_EQ(win->style & (WS::MINIMIZE | WS::MAXIMIZE), 0u);
    EXPECT_EQ(win->rect.left, 100);
    EXPECT_EQ(win->rect.top, 100);
    EXPECT_EQ(win->rect.right, 740);
    EXPECT_EQ(win->rect.bottom, 580);

    sm.show_window(test_hwnd, SW::HIDE);
    EXPECT_EQ(win->style & WS::VISIBLE, 0u);
}

TEST_F(ShowWindowManagerTest, WindowPlacementRetainsNormalGeometryWhileIconic) {
    auto sm = make_sm();
    const windowing::Rect normal{40, 50, 840, 650};

    ASSERT_TRUE(sm.set_window_placement(
        test_hwnd, 0x0002u, SW::SHOWMINIMIZED,
        windowing::Point{7, 8}, windowing::Point{9, 10}, normal));

    const WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_TRUE(win->minimized);
    EXPECT_TRUE(win->restore_to_maximized);
    EXPECT_EQ(win->minimized_position.x, 7);
    EXPECT_EQ(win->maximized_position.y, 10);
    EXPECT_EQ(win->normal_rect.left, 40);
    EXPECT_EQ(win->normal_rect.bottom, 650);

    sm.show_window(test_hwnd, SW::RESTORE);
    EXPECT_TRUE(win->maximized);
    EXPECT_FALSE(win->minimized);
    EXPECT_EQ(win->normal_rect.left, 40);
    EXPECT_EQ(win->normal_rect.bottom, 650);
}

TEST_F(ShowWindowManagerTest,
       SetWindowPlacementAppliesNormalRectToZeroSizedWindow) {
    // Learn: CreateWindow(..., 0,0,0,0) then SetWindowPlacement(SW_SHOWNORMAL)
    // must restamp the live frame — ShowWindow alone leaves a "normal" window
    // at its current (zero) size.
    WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    win->rect = windowing::Rect{0, 0, 0, 0};
    win->client_rect = windowing::Rect{0, 0, 0, 0};
    win->normal_rect = win->rect;
    win->normal_client_rect = win->client_rect;
    win->visible = false;

    auto sm = make_sm();
    const windowing::Rect normal{80, 60, 1104, 780};
    ASSERT_TRUE(sm.set_window_placement(
        test_hwnd, 0, SW::SHOWNORMAL,
        windowing::Point{-1, -1}, windowing::Point{-1, -1}, normal));

    EXPECT_TRUE(win->visible);
    EXPECT_EQ(win->rect.left, 80);
    EXPECT_EQ(win->rect.top, 60);
    EXPECT_EQ(win->rect.right, 1104);
    EXPECT_EQ(win->rect.bottom, 780);
    EXPECT_EQ(win->client_rect.width(), 1024);
    EXPECT_EQ(win->client_rect.height(), 720);
    EXPECT_EQ(win->normal_rect.width(), 1024);
    EXPECT_EQ(win->normal_rect.height(), 720);
}

TEST_F(ShowWindowManagerTest, ShowWindowPostsWmShowWindow) {
    auto sm = make_sm();
    queue.clear();
    sm.show_window(test_hwnd, SW::SHOW);

    // Should find WM_SHOWWINDOW in the queue
    MSG msg{};
    bool found = false;
    while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
        if (msg.message == WM::SHOWWINDOW && msg.hwnd == test_hwnd) {
            found = true;
            EXPECT_EQ(msg.wParam, 1u); // being shown
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ShowWindowManagerTest, ShowWindowShowNormalPostsMessages) {
    auto sm = make_sm();
    queue.clear();
    sm.show_window(test_hwnd, SW::SHOWNORMAL);

    // Expect WM_SHOWWINDOW, WM_SIZE, WM_MOVE
    bool has_show = false;
    bool has_size = false;
    bool has_move = false;
    MSG msg{};
    while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
        if (msg.message == WM::SHOWWINDOW) { has_show = true; }
        if (msg.message == WM::SIZE)       { has_size = true; }
        if (msg.message == WM::MOVE)       { has_move = true; }
    }
    EXPECT_TRUE(has_show);
    EXPECT_TRUE(has_size);
    EXPECT_TRUE(has_move);
}

TEST_F(ShowWindowManagerTest,
       ShowNormalKeepsCurrentGeometryForHiddenNormalWindow) {
    WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    // Creation geometry is deliberately stale; the application resized the
    // still-hidden normal window before its first ShowWindow call.
    win->normal_rect = windowing::Rect{100, 100, 740, 580};
    win->normal_client_rect = windowing::Rect{0, 0, 640, 480};
    win->rect = windowing::Rect{956, 419, 1318, 1112};
    win->client_rect = windowing::Rect{0, 0, 354, 655};

    auto sm = make_sm();
    queue.clear();
    sm.show_window(test_hwnd, SW::SHOWNORMAL);

    EXPECT_EQ(win->rect.left, 956);
    EXPECT_EQ(win->rect.top, 419);
    EXPECT_EQ(win->rect.width(), 362);
    EXPECT_EQ(win->rect.height(), 693);
    EXPECT_EQ(win->client_rect.width(), 354);
    EXPECT_EQ(win->client_rect.height(), 655);

    MSG msg{};
    bool found_size = false;
    while (queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
        if (msg.hwnd == test_hwnd && msg.message == WM::SIZE) {
            found_size = true;
            EXPECT_EQ(static_cast<i16>(msg.lParam & 0xFFFFu), 354);
            EXPECT_EQ(static_cast<i16>((msg.lParam >> 16) & 0xFFFFu), 655);
        }
    }
    EXPECT_TRUE(found_size);
}

TEST_F(ShowWindowManagerTest, ShowingParentInvalidatesVisibleDescendants) {
    WindowData child_data;
    child_data.parent = test_hwnd;
    child_data.visible = true;
    child_data.style = WS::CHILD | WS::VISIBLE;
    HWND child = hwnd_mgr.create(child_data);

    WindowData grandchild_data;
    grandchild_data.parent = child;
    grandchild_data.visible = true;
    grandchild_data.style = WS::CHILD | WS::VISIBLE;
    HWND grandchild = hwnd_mgr.create(grandchild_data);

    WindowData hidden_data;
    hidden_data.parent = test_hwnd;
    hidden_data.visible = false;
    hidden_data.style = WS::CHILD;
    HWND hidden = hwnd_mgr.create(hidden_data);

    PaintingManager painting(&hwnd_mgr);
    auto sm = make_sm();
    sm.set_painting_manager(&painting);
    sm.show_window(test_hwnd, SW::SHOW);

    EXPECT_TRUE(painting.needs_paint(test_hwnd));
    EXPECT_TRUE(painting.needs_paint(child));
    EXPECT_TRUE(painting.needs_paint(grandchild));
    EXPECT_FALSE(painting.needs_paint(hidden));
}

TEST_F(ShowWindowManagerTest, OwnerHideAndShowPropagatesToOwnedPopup) {
    auto sm = make_sm();

    // Owner top-level window
    WindowData owner_data;
    owner_data.title   = L"Owner";
    owner_data.parent  = HWND_DESKTOP;
    owner_data.visible = true;
    owner_data.enabled = true;
    owner_data.style   = WS::OVERLAPPEDWINDOW;
    owner_data.rect    = windowing::Rect{10, 10, 400, 300};
    HWND owner = hwnd_mgr.create(owner_data);

    // Owned top-level popup
    WindowData popup_data;
    popup_data.title   = L"Owned";
    popup_data.parent  = HWND_DESKTOP;
    popup_data.owner   = owner;
    popup_data.visible = true;
    popup_data.enabled = true;
    popup_data.style   = WS::POPUP;
    popup_data.rect    = windowing::Rect{30, 30, 260, 180};
    HWND owned = hwnd_mgr.create(popup_data);

    sm.show_window(owner, SW::HIDE);
    const WindowData* owned_after_hide = hwnd_mgr.find(owned);
    ASSERT_NE(owned_after_hide, nullptr);
    EXPECT_FALSE(owned_after_hide->visible);
    EXPECT_TRUE(owned_after_hide->hidden_by_owner);

    sm.show_window(owner, SW::SHOW);
    const WindowData* owned_after_show = hwnd_mgr.find(owned);
    ASSERT_NE(owned_after_show, nullptr);
    EXPECT_TRUE(owned_after_show->visible);
    EXPECT_FALSE(owned_after_show->hidden_by_owner);
}

// ---------------------------------------------------------------------------
// IsWindowVisible
// ---------------------------------------------------------------------------

TEST_F(ShowWindowManagerTest, IsWindowVisibleTrueForVisibleWindow) {
    auto sm = make_sm();
    sm.show_window(test_hwnd, SW::SHOW);
    EXPECT_TRUE(sm.is_window_visible(test_hwnd));
}

TEST_F(ShowWindowManagerTest, IsWindowVisibleFalseForHiddenWindow) {
    auto sm = make_sm();
    // Window starts hidden
    EXPECT_FALSE(sm.is_window_visible(test_hwnd));
}

TEST_F(ShowWindowManagerTest, IsWindowVisibleFalseIfParentHidden) {
    auto sm = make_sm();

    // Make the parent visible first
    sm.show_window(test_hwnd, SW::SHOW);

    // Create a visible child of test_hwnd
    WindowData child_data;
    child_data.title   = L"Child";
    child_data.parent  = test_hwnd;
    child_data.visible = true;
    child_data.enabled = true;
    child_data.style   = WS::CHILD | WS::VISIBLE;
    HWND child = hwnd_mgr.create(child_data);

    // Child visible while parent visible
    EXPECT_TRUE(sm.is_window_visible(child));

    // Hide parent → child should report invisible
    sm.show_window(test_hwnd, SW::HIDE);
    EXPECT_FALSE(sm.is_window_visible(child));
}

TEST_F(ShowWindowManagerTest, IsWindowVisibleFalseForInvalidHwnd) {
    auto sm = make_sm();
    EXPECT_FALSE(sm.is_window_visible(0xDEAD));
}

// SetWindowLong(GWL_STYLE) WS_VISIBLE bit is what IsWindowVisible reads
// (Learn Remarks; 08c M5).
TEST_F(ShowWindowManagerTest, IsWindowVisibleFollowsSetWindowLongStyleBit) {
    auto sm = make_sm();
    sm.show_window(test_hwnd, SW::SHOW);
    ASSERT_TRUE(sm.is_window_visible(test_hwnd));

    WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    win->style &= ~WS::VISIBLE;
    win->visible = true;  // stale bool must not win over the style bit
    EXPECT_FALSE(sm.is_window_visible(test_hwnd));

    win->style |= WS::VISIBLE;
    win->visible = false;
    EXPECT_TRUE(sm.is_window_visible(test_hwnd));
}

// Peer / foreign HWND: Learn requires IsWindowVisible to honour WS_VISIBLE
// published in the capsule hwnd_registry (08c H9).
TEST_F(ShowWindowManagerTest, IsWindowVisibleUsesRegistryForForeignHwnd) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-isvisible-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);
    // Use a handle outside HwndManager's local FIRST_HWND sequence so find()
    // takes the peer-registry path (both allocators start at 0x10000).
    constexpr u32 hwnd = 0x00ABCDEF;
    ipc::hwnd_registry_publish(hwnd, 40, 40, 1, "VitreoIsVis", "peer");
    ipc::hwnd_registry_set_visible(hwnd, true);
    auto sm = make_sm();
    EXPECT_TRUE(sm.is_window_visible(static_cast<HWND>(hwnd)));
    ipc::hwnd_registry_set_visible(hwnd, false);
    EXPECT_FALSE(sm.is_window_visible(static_cast<HWND>(hwnd)));
    ipc::hwnd_registry_unpublish(hwnd);
    ::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// UpdateWindow
// ---------------------------------------------------------------------------

TEST_F(ShowWindowManagerTest, UpdateWindowDoesNothingWithoutUpdateRegion) {
    auto sm = make_sm();
    sm.show_window(test_hwnd, SW::SHOW);
    queue.clear();

    bool result = sm.update_window(test_hwnd);
    EXPECT_TRUE(result);

    EXPECT_TRUE(queue.empty());
}

TEST_F(ShowWindowManagerTest, UpdateWindowNoPaintForHiddenWindow) {
    auto sm = make_sm();
    // Window is hidden by default
    queue.clear();

    bool result = sm.update_window(test_hwnd);
    EXPECT_TRUE(result);

    // Queue should be empty — no WM_PAINT posted
    EXPECT_TRUE(queue.empty());
}

TEST_F(ShowWindowManagerTest, UpdateWindowReturnsFalseForInvalidHwnd) {
    auto sm = make_sm();
    EXPECT_FALSE(sm.update_window(0xDEAD));
}

// ---------------------------------------------------------------------------
// EnableWindow
// ---------------------------------------------------------------------------

TEST_F(ShowWindowManagerTest, EnableWindowTogglesEnabledState) {
    auto sm = make_sm();
    // Window starts enabled
    sm.enable_window(test_hwnd, false);

    const WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_FALSE(win->enabled);

    sm.enable_window(test_hwnd, true);
    EXPECT_TRUE(win->enabled);
}

TEST_F(ShowWindowManagerTest, EnableWindowReturnsPreviousState) {
    auto sm = make_sm();
    // Window starts enabled
    bool prev = sm.enable_window(test_hwnd, false);
    EXPECT_TRUE(prev); // was enabled

    prev = sm.enable_window(test_hwnd, true);
    EXPECT_FALSE(prev); // was disabled
}

TEST_F(ShowWindowManagerTest, EnableWindowOnInvalidHwndReturnsFalse) {
    auto sm = make_sm();
    EXPECT_FALSE(sm.enable_window(0xDEAD, true));
}

TEST_F(ShowWindowManagerTest, EnableWindowThunkRejectsInvalidHostWindow) {
    auto sm = make_sm();
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = 0; // invalid HWND
    cpu.gpr.rdx = 1; // enable
    set_last_error(error_codes::ERROR_SUCCESS);

    impl_EnableWindow(marshaler, sm);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

TEST(PlatformHostTest, EnableWindowUsesControlHostInterface) {
    HwndManager hwnd_mgr;
    MessageQueue queue;
    FakeControlCursorHost host;
    WindowData data;
    data.enabled = true;
    const HWND hwnd = hwnd_mgr.create(data);
    ShowWindowManager manager(hwnd_mgr, host, queue);

    EXPECT_TRUE(manager.enable_window(hwnd, false));
    EXPECT_EQ(host.enabled_hwnd, hwnd);
    EXPECT_FALSE(host.enabled_value);
}

TEST(PlatformHostTest, SetCursorPosUsesCursorHostInterface) {
    FakeControlCursorHost host;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = static_cast<u32>(-45);
    cpu.gpr.rdx = 123;

    // A virtual screen with a monitor left of the primary: -45 is on it.
    impl_SetCursorPos(marshaler, host, windowing::Rect{-1920, 0, 2560, 1440});

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(host.cursor_x, -45);
    EXPECT_EQ(host.cursor_y, 123);
}

// Learn SetCursorPos: coordinates outside the ClipCursor rectangle are
// adjusted so the cursor stays inside it; unclipped, that rectangle is the
// virtual screen (GetClipCursor). RECT right/bottom are exclusive (Learn,
// RECT structure), so the far edge clamps to right-1 / bottom-1.
TEST(PlatformHostTest, SetCursorPosConfinesToVirtualScreenWhenUnclipped) {
    FakeControlCursorHost host;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const windowing::Rect screen{0, 0, 2560, 1440};

    // INT_MIN is what CVTTSS2SI yields for a NaN/Inf float (UE's
    // FSlateUser::SetPointerPosition truncates floats this way).
    cpu.gpr.rcx = 0x80000000u;
    cpu.gpr.rdx = 0x80000000u;
    impl_SetCursorPos(marshaler, host, screen);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(host.cursor_x, 0);
    EXPECT_EQ(host.cursor_y, 0);

    cpu.gpr.rcx = 5000;
    cpu.gpr.rdx = 1440;
    impl_SetCursorPos(marshaler, host, screen);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(host.cursor_x, 2559);
    EXPECT_EQ(host.cursor_y, 1439);

    // The upper dword of an int argument's register is undefined on x64.
    cpu.gpr.rcx = 0xDEADBEEF00000010ull;
    cpu.gpr.rdx = 0xDEADBEEF00000020ull;
    impl_SetCursorPos(marshaler, host, screen);
    EXPECT_EQ(host.cursor_x, 0x10);
    EXPECT_EQ(host.cursor_y, 0x20);
}

TEST(PlatformHostTest, SetCursorPosConfinesToClipCursorRect) {
    FakeControlCursorHost host;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const windowing::Rect screen{0, 0, 2560, 1440};
    host.set_cursor_clip(true, 100, 200, 300, 400);

    cpu.gpr.rcx = 50;
    cpu.gpr.rdx = 1000;
    impl_SetCursorPos(marshaler, host, screen);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(host.cursor_x, 100);
    EXPECT_EQ(host.cursor_y, 399);

    cpu.gpr.rcx = 150;
    cpu.gpr.rdx = 250;
    impl_SetCursorPos(marshaler, host, screen);
    EXPECT_EQ(host.cursor_x, 150);
    EXPECT_EQ(host.cursor_y, 250);

    cpu.gpr.rcx = 300;
    cpu.gpr.rdx = 199;
    impl_SetCursorPos(marshaler, host, screen);
    EXPECT_EQ(host.cursor_x, 299);
    EXPECT_EQ(host.cursor_y, 200);
}

TEST(PlatformHostTest, SetCursorPosNeverLeavesTheScreenEvenWhenClipDoes) {
    // The cursor cannot be placed off the virtual screen; a clip rectangle
    // reaching past it does not extend the reachable area.
    const windowing::Rect screen{0, 0, 1920, 1080};
    const windowing::Rect clip{-500, -500, 5000, 5000};
    const CursorPoint p = confine_cursor_point(4000, -300, screen, &clip);
    EXPECT_EQ(p.x, 1919);
    EXPECT_EQ(p.y, 0);
    const CursorPoint unclipped =
        confine_cursor_point(-1, 1080, screen, nullptr);
    EXPECT_EQ(unclipped.x, 0);
    EXPECT_EQ(unclipped.y, 1079);
}

TEST(PlatformHostTest, SetCursorPosReportsHostFailure) {
    FakeControlCursorHost host;
    host.cursor_move_succeeds = false;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = 10;
    cpu.gpr.rdx = 10;
    set_last_error(0);
    impl_SetCursorPos(marshaler, host, windowing::Rect{0, 0, 640, 480});
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

TEST(PlatformHostTest, SetWindowTextUsesControlHostInterface) {
    HwndManager hwnd_mgr;
    WindowClassManager class_mgr;
    MessageQueue queue;
    GuestMemory memory;
    FakeControlCursorHost host;
    WindowData data;
    data.title = L"old";
    const HWND hwnd = hwnd_mgr.create(data);
    DialogManager manager(hwnd_mgr, class_mgr, host, queue, memory);

    EXPECT_TRUE(manager.set_window_text(hwnd, L"new title"));
    EXPECT_EQ(host.title_hwnd, hwnd);
    EXPECT_EQ(host.window_title, L"new title");
    EXPECT_EQ(hwnd_mgr.find(hwnd)->title, L"new title");
}

TEST(PlatformHostTest, InternalGetWindowTextIsExportedAndReadsStoredUnicodeText) {
    constexpr GuestAddr buffer = 0x2000;
    HwndManager hwnd_mgr;
    WindowClassManager class_mgr;
    MessageQueue queue;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x4000,
                                  MemoryProtection::ReadWrite).is_ok());
    FakeControlCursorHost host;
    WindowData data;
    data.title = L"stored title";
    const HWND hwnd = hwnd_mgr.create(data);
    DialogManager manager(hwnd_mgr, class_mgr, host, queue, memory);

    const DllExports exports = create_dialog_thunks(manager);
    const auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "InternalGetWindowText";
        });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 12u);

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rcx = hwnd;
    cpu.gpr.rdx = buffer;
    cpu.gpr.r8 = 7; // room for six characters plus the terminator
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    it->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 6u);
    EXPECT_EQ(memory.read_u16(buffer + 0).value_or(0), u's');
    EXPECT_EQ(memory.read_u16(buffer + 10).value_or(0), u'd');
    EXPECT_EQ(memory.read_u16(buffer + 12).value_or(1), 0u);
}

TEST(PlatformHostTest, CenteredDialogUsesMonitorWorkAreaNotOwnerWindow) {
    // DS_CENTER: Microsoft Learn Dialog Box Styles. Negative work-area origins
    // are valid for monitors left of the primary and must not be clamped to 0.
    for (bool with_owner : {false, true}) {
        HwndManager hwnd_mgr;
        WindowClassManager class_mgr;
        MessageQueue queue;
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
        FakeControlCursorHost host;
        host.primary_monitor.work_left = -1200;
        host.primary_monitor.work_top = 40;
        host.primary_monitor.work_right = 0;
        host.primary_monitor.work_bottom = 840;
        WindowData owner_data{};
        owner_data.rect = {3000, 2000, 3400, 2300};
        owner_data.client_rect = {0, 0, 400, 300};
        const HWND owner = with_owner ? hwnd_mgr.create(owner_data) : NULL_HWND;
        // Classic DLGTEMPLATE, no font/controls/title: 100x50 DLU = 200x100 px.
        (void)memory.write_u32(0x10000, WS::POPUP | DS::CENTER);
        (void)memory.write_u16(0x10000 + 14, 100);
        (void)memory.write_u16(0x10000 + 16, 50);
        DialogManager manager(hwnd_mgr, class_mgr, host, queue, memory);
        const auto hwnd = manager.create_dialog(0, 0x10000, owner, 0, 0, false, true);
        const auto* dialog = hwnd_mgr.find(hwnd);
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->rect.left, -700);
        EXPECT_EQ(dialog->rect.top, 390);
        EXPECT_EQ(dialog->rect.width(), 200);
        EXPECT_EQ(dialog->rect.height(), 100);
    }
}
