/// @file test_def_window_proc.cpp
/// @brief Tests for DefWindowProcManager (WIN32-054)

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/user32/windows/def_window_proc.hpp>
#include <windowing/model/window_types.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <winapi/user32/messages/message_send.hpp>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

class DefWindowProcManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        WindowData data;
        data.title  = L"Hello World";
        data.parent = HWND_DESKTOP;
        test_hwnd = hwnd_mgr.create(data);
    }

    HwndManager  hwnd_mgr;
    MessageQueue queue;
    GuestMemory  memory;
    HWND         test_hwnd = 0;

    DefWindowProcManager make_dwp() {
        return DefWindowProcManager(hwnd_mgr, queue, memory);
    }
};

// ---------------------------------------------------------------------------
// WM_CLOSE
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, WheelPropagatesSynchronouslyThroughParents) {
    // Learn WM_MOUSEWHEEL and WM_MOUSEHWHEEL require propagation until a
    // parent handles the message, preserving signed delta and screen point.
    CocoaBridge bridge;
    MessageSendManager sender(hwnd_mgr, queue, bridge);
    auto dwp = make_dwp();
    dwp.set_message_send_manager(&sender);
    WindowData parent_data;
    parent_data.style = WS::CHILD;
    parent_data.parent = test_hwnd;
    parent_data.wndproc = 0x401000;
    const HWND parent = hwnd_mgr.create(parent_data);
    WindowData child_data;
    child_data.style = WS::CHILD;
    child_data.parent = parent;
    child_data.wndproc = 0x402000;
    const HWND child = hwnd_mgr.create(child_data);
    hwnd_mgr.find(test_hwnd)->wndproc = 0x403000;
    for (const bool is64 : {false, true}) {
        dwp.set_is_64bit(is64);
        for (const u32 message : {WM::MOUSEWHEEL, WM::MOUSEHWHEEL}) {
            for (const i16 delta : {i16{-120}, i16{30}, i16{120}}) {
                const u64 wp = (u64{static_cast<u16>(delta)} << 16) | 0x7fu;
                const u64 lp = (u64{static_cast<u16>(-40)} << 16) |
                    static_cast<u16>(-300);
                unsigned deliveries = 0;
                sender.set_wndproc_callback([&](HWND target, u32 msg, u64 w,
                                                u64 l, u64) -> u64 {
                    ++deliveries;
                    EXPECT_EQ(msg, message);
                    EXPECT_EQ(w, wp);
                    EXPECT_EQ(l, lp);
                    if (target == parent) return dwp.def_window_proc(target, msg, w, l);
                    EXPECT_EQ(target, test_hwnd);
                    return 73; // Oracle: a nonzero parent result is discarded.
                });
                EXPECT_EQ(dwp.def_window_proc(child, message, wp, lp), 0u);
                EXPECT_EQ(deliveries, 2u);
                EXPECT_TRUE(queue.empty());
                deliveries = 0;
                EXPECT_EQ(dwp.def_window_proc(test_hwnd, message, wp, lp), 0u);
                EXPECT_EQ(deliveries, 0u);
                sender.set_wndproc_callback([&](HWND target, u32, u64, u64, u64) -> u64 {
                    ++deliveries;
                    EXPECT_EQ(target, parent);
                    return 73;
                });
                EXPECT_EQ(dwp.def_window_proc(child, message, wp, lp), 0u);
                EXPECT_EQ(deliveries, 1u); // Stop at the first handler.
                deliveries = 0;
                hwnd_mgr.find(child)->style = WS::POPUP;
                EXPECT_EQ(dwp.def_window_proc(child, message, wp, lp), 0u);
                EXPECT_EQ(deliveries, 0u); // An owner is not a parent.
                hwnd_mgr.find(child)->style = WS::CHILD;
            }
        }
    }
}

// Learn WM_MOUSEACTIVATE: DefWindowProc passes a child's message to its
// parent first and returns a nonzero parent answer; otherwise (a top-level
// window, or a parent answering 0) the result is MA_ACTIVATE.
TEST_F(DefWindowProcManagerTest, MouseActivateAsksTheParentThenActivates) {
    CocoaBridge bridge;
    MessageSendManager sender(hwnd_mgr, queue, bridge);
    auto dwp = make_dwp();
    dwp.set_message_send_manager(&sender);
    WindowData child_data;
    child_data.style = WS::CHILD;
    child_data.parent = test_hwnd;
    child_data.wndproc = 0x402000;
    const HWND child = hwnd_mgr.create(child_data);
    hwnd_mgr.find(test_hwnd)->wndproc = 0x403000;
    constexpr u64 lp = (u64{WM::LBUTTONDOWN} << 16) | 1;  // HTCLIENT
    u64 parent_answer = 3;  // MA_NOACTIVATE
    unsigned asked = 0;
    sender.set_wndproc_callback([&](HWND target, u32 msg, u64 w, u64 l, u64) -> u64 {
        ++asked;
        EXPECT_EQ(target, test_hwnd);
        EXPECT_EQ(msg, WM::MOUSEACTIVATE);
        EXPECT_EQ(w, test_hwnd);
        EXPECT_EQ(l, lp);
        return parent_answer;
    });
    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::MOUSEACTIVATE, test_hwnd, lp), 1u);
    EXPECT_EQ(asked, 0u);
    EXPECT_EQ(dwp.def_window_proc(child, WM::MOUSEACTIVATE, test_hwnd, lp), 3u);
    EXPECT_EQ(asked, 1u);
    parent_answer = 0;
    EXPECT_EQ(dwp.def_window_proc(child, WM::MOUSEACTIVATE, test_hwnd, lp), 1u);
}

TEST_F(DefWindowProcManagerTest, WindowPosChangedGeneratesClientMoveAndSize) {
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    auto* window = hwnd_mgr.find(test_hwnd);
    window->rect = {-20, 30, 180, 170};
    window->client_offset = {3, 4};
    window->client_rect = {0, 0, 194, 130};
    for (const bool is64 : {false, true}) {
        auto dwp = make_dwp();
        dwp.set_is_64bit(is64);
        const GuestAddr flags = 0x10000 + (is64 ? 32 : 24);
        ASSERT_TRUE(memory.write_u32(flags, 3).is_ok()); // Public NOSIZE/NOMOVE do not suppress client notifications.
        dwp.def_window_proc(test_hwnd, WM::WINDOWPOSCHANGED, 0, 0x10000);
        MSG msg{};
        ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
        EXPECT_EQ(msg.message, WM::MOVE);
        EXPECT_EQ(static_cast<i16>(msg.lParam), -17);
        EXPECT_EQ(static_cast<i16>(msg.lParam >> 16), 34);
        ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
        EXPECT_EQ(msg.message, WM::SIZE);
        EXPECT_EQ(msg.wParam, 0u);
        EXPECT_EQ(msg.lParam, (130u << 16) | 194u);
        ASSERT_TRUE(memory.write_u32(flags, 0x1803).is_ok()); // No client move or size
        dwp.def_window_proc(test_hwnd, WM::WINDOWPOSCHANGED, 0, 0x10000);
        EXPECT_TRUE(queue.empty());
    }
}

TEST_F(DefWindowProcManagerTest, CloseTriggersDestroy) {
    auto dwp = make_dwp();
    dwp.def_window_proc(test_hwnd, WM::CLOSE, 0, 0);
    EXPECT_TRUE(dwp.was_destroy_triggered());
}

TEST_F(DefWindowProcManagerTest, ClosePostsDestroyToQueue) {
    auto dwp = make_dwp();
    dwp.def_window_proc(test_hwnd, WM::CLOSE, 0, 0);

    MSG msg{};
    ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.hwnd, test_hwnd);
    EXPECT_EQ(msg.message, WM::DESTROY);
}

// ---------------------------------------------------------------------------
// WM_GETTEXT / WM_GETTEXTLENGTH
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, GetTextReturnsTitleLength) {
    // WM_GETTEXT copies into lParam's buffer and returns the characters
    // copied (Learn; measured, s1win_misc_probe "-- defwindowproc text").
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::GETTEXT, 256, 0x10000);
    EXPECT_EQ(result, 11u); // "Hello World" == 11 chars
    EXPECT_EQ(memory.read_u16(0x10000).value_or(0), u16{'H'});
}

TEST_F(DefWindowProcManagerTest, GetTextLengthReturnsCorrectLength) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::GETTEXTLENGTH, 0, 0);
    EXPECT_EQ(result, 11u);
}

// ---------------------------------------------------------------------------
// WM_SETTEXT
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, SetTextReturnsTrue) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::SETTEXT, 0, 0x1000);
    EXPECT_EQ(result, 1u);
}

// Learn (WM_SETTEXT): "DefWindowProc sets and displays the window text".
// Both the stored text and the host display see the new string; a NULL
// lParam clears both.
TEST_F(DefWindowProcManagerTest, SetTextDisplaysNewTextAndNullClears) {
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    const char16_t text[] = u"Setup";
    for (u32 i = 0; i < 6; ++i) {
        ASSERT_TRUE(memory.write_u16(0x1000 + i * 2, static_cast<u16>(text[i])).is_ok());
    }
    std::vector<std::pair<windowing::HWND, std::wstring>> shown;
    auto dwp = make_dwp();
    dwp.set_display_text([&](windowing::HWND h, const std::wstring& t) {
        shown.emplace_back(h, t);
    });

    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::SETTEXT, 0, 0x1000), 1u);
    ASSERT_EQ(shown.size(), 1u);
    EXPECT_EQ(shown[0].first, test_hwnd);
    EXPECT_EQ(shown[0].second, L"Setup");
    EXPECT_EQ(hwnd_mgr.find(test_hwnd)->title, L"Setup");

    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::SETTEXT, 0, 0), 1u);
    ASSERT_EQ(shown.size(), 2u);
    EXPECT_EQ(shown[1].second, L"");
    EXPECT_EQ(hwnd_mgr.find(test_hwnd)->title, L"");
}

// ---------------------------------------------------------------------------
// WM_NCHITTEST
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, NcHitTestReturnsClient) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::NCHITTEST, 0, 0);
    EXPECT_EQ(result, HT::CLIENT);
}

TEST_F(DefWindowProcManagerTest, SetCursorInClientAppliesClassCursor) {
    u32 applied = 0;
    auto dwp = make_dwp();
    dwp.set_apply_cursor([&](u32 hcursor) { applied = hcursor; });
    // LOWORD(lParam)=HTCLIENT, HIWORD=WM_MOUSEMOVE
    const u64 lparam = (static_cast<u64>(0x0200) << 16) | HT::CLIENT;
    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::SETCURSOR, test_hwnd, lparam),
              1u);
    // No class cursor is registered on this HWND — do not force IDC_ARROW.
    EXPECT_EQ(applied, 0u);
}

TEST_F(DefWindowProcManagerTest, NcCalcSizeDefaultAppliesStandardCaptionFrame) {
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    WindowData* win = hwnd_mgr.find(test_hwnd);
    ASSERT_NE(win, nullptr);
    win->style = WS::CAPTION | WS::THICKFRAME;

    ASSERT_TRUE(memory.write_u32(0x1000, 100).is_ok());
    ASSERT_TRUE(memory.write_u32(0x1004, 100).is_ok());
    ASSERT_TRUE(memory.write_u32(0x1008, 500).is_ok());
    ASSERT_TRUE(memory.write_u32(0x100c, 400).is_ok());

    auto dwp = make_dwp();
    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::NCCALCSIZE, 1, 0x1000), 0u);
    EXPECT_EQ(memory.read_u32(0x1000).value_or(0), 108u);
    EXPECT_EQ(memory.read_u32(0x1004).value_or(0), 138u);
    EXPECT_EQ(memory.read_u32(0x1008).value_or(0), 492u);
    EXPECT_EQ(memory.read_u32(0x100c).value_or(0), 392u);
}

TEST_F(DefWindowProcManagerTest, NcCalcSizeReservesStandardScrollbars) {
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    hwnd_mgr.find(test_hwnd)->style = WS::VSCROLL | WS::HSCROLL;
    (void)memory.write_u32(0x1000, 0); (void)memory.write_u32(0x1004, 0);
    (void)memory.write_u32(0x1008, 400); (void)memory.write_u32(0x100c, 300);
    auto dwp = make_dwp();
    dwp.def_window_proc(test_hwnd, WM::NCCALCSIZE, 1, 0x1000);
    // SM_CXVSCROLL / SM_CYHSCROLL are 17 (measured,
    // tools/oracle_probe/s1win_display_probe.expected sm_2 / sm_3).
    EXPECT_EQ(memory.read_u32(0x1008).value(), 383u);
    EXPECT_EQ(memory.read_u32(0x100c).value(), 283u);
}

// ---------------------------------------------------------------------------
// WM_ERASEBKGND
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, EraseBkgndReturnsOne) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::ERASEBKGND, 0, 0);
    EXPECT_EQ(result, 1u);
}

// ---------------------------------------------------------------------------
// WM_QUERYOPEN
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, QueryOpenReturnsOne) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::QUERYOPEN, 0, 0);
    EXPECT_EQ(result, 1u);
}

// ---------------------------------------------------------------------------
// WM_SYSCOMMAND
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, SysCommandClosePostsWmClose) {
    auto dwp = make_dwp();
    dwp.def_window_proc(test_hwnd, WM::SYSCOMMAND, SC::CLOSE, 0);

    MSG msg{};
    ASSERT_TRUE(queue.peek(msg, NULL_HWND, 0, 0, PM::REMOVE));
    EXPECT_EQ(msg.hwnd, test_hwnd);
    EXPECT_EQ(msg.message, WM::CLOSE);
}

TEST_F(DefWindowProcManagerTest, SysCommandRestoreShowsWindow) {
    auto dwp = make_dwp();
    HWND shown = NULL_HWND;
    int cmd = -1;
    dwp.set_syscommand_show([&](HWND hwnd, int cmd_show) {
        shown = hwnd;
        cmd = cmd_show;
    });
    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::SYSCOMMAND, SC::RESTORE, 0), 0u);
    EXPECT_EQ(shown, test_hwnd);
    EXPECT_EQ(cmd, SW::RESTORE);
}

// ---------------------------------------------------------------------------
// Unknown / unhandled messages
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, UnknownMessageReturnsZero) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, 0xBEEF, 0, 0);
    EXPECT_EQ(result, 0u);
}

TEST_F(DefWindowProcManagerTest, DestroyMessageReturnsZeroDefault) {
    auto dwp = make_dwp();
    u64 result = dwp.def_window_proc(test_hwnd, WM::DESTROY, 0, 0);
    EXPECT_EQ(result, 0u);
}

// ---------------------------------------------------------------------------
// Export registration
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, MakeExportsIncludesWindowAndDialogDefaults) {
    DefWindowProcManager dwp = make_dwp();
    DllExports exports = make_def_window_proc_exports(dwp);
    ASSERT_EQ(exports.entries.size(), 4u);
    EXPECT_EQ(exports.entries[0].name, "DefWindowProcW");
    EXPECT_EQ(exports.entries[1].name, "DefWindowProcA");
    EXPECT_EQ(exports.entries[2].name, "DefDlgProcW");
    EXPECT_EQ(exports.entries[3].name, "DefDlgProcA");
}

// ---------------------------------------------------------------------------
// Interactive resize routing (WM_NCLBUTTONDOWN on a sizing border, SC_SIZE)
//
// A guest that paints its own frame -- every CEF/Electron app, and the only
// kind whose wndproc reports HT border codes at all -- resizes by sending
// DefWindowProc one of these. Before HT::LEFT..BOTTOMRIGHT existed, every one
// of them fell through the HTCAPTION test and returned 0, which is the whole
// of "the window cannot be resized". These pin the routing, not the gesture.
// ---------------------------------------------------------------------------

TEST_F(DefWindowProcManagerTest, NcLButtonDownOnEachSizingBorderStartsAResize) {
    for (u64 ht = HT::LEFT; ht <= HT::BOTTOMRIGHT; ++ht) {
        auto dwp = make_dwp();
        u32 got_edge = 0;
        int resize_calls = 0, drag_calls = 0;
        dwp.set_begin_window_resize(
            [&](HWND, u32 edge) { ++resize_calls; got_edge = edge; return true; });
        dwp.set_begin_window_drag([&](HWND) { ++drag_calls; return true; });

        dwp.def_window_proc(test_hwnd, WM::NCLBUTTONDOWN, ht, 0);

        EXPECT_EQ(resize_calls, 1) << "HT " << ht << " did not start a resize";
        EXPECT_EQ(got_edge, static_cast<u32>(ht));
        EXPECT_EQ(drag_calls, 0) << "HT " << ht << " must not be a MOVE";
    }
}

TEST_F(DefWindowProcManagerTest, NcLButtonDownOnCaptionStillDragsNotResizes) {
    auto dwp = make_dwp();
    int resize_calls = 0, drag_calls = 0;
    dwp.set_begin_window_resize([&](HWND, u32) { ++resize_calls; return true; });
    dwp.set_begin_window_drag([&](HWND) { ++drag_calls; return true; });

    dwp.def_window_proc(test_hwnd, WM::NCLBUTTONDOWN, HT::CAPTION, 0);
    EXPECT_EQ(drag_calls, 1);
    EXPECT_EQ(resize_calls, 0);

    // HTCLIENT is neither: a plain client click must start no gesture at all.
    dwp.def_window_proc(test_hwnd, WM::NCLBUTTONDOWN, HT::CLIENT, 0);
    EXPECT_EQ(drag_calls, 1);
    EXPECT_EQ(resize_calls, 0);
}

TEST_F(DefWindowProcManagerTest, SysCommandSizeMapsWmszEdgeToHitTestCode) {
    // WMSZ_* (1..8) and HT::LEFT..BOTTOMRIGHT (10..17) share an order, so the
    // mapping is a constant offset. Pin every one -- an off-by-one here sizes
    // the wrong edge, which looks like the window "jumping" on grab.
    const struct { u64 wmsz; u64 expect; } kCases[] = {
        {1, HT::LEFT},   {2, HT::RIGHT},      {3, HT::TOP},
        {4, HT::TOPLEFT},{5, HT::TOPRIGHT},   {6, HT::BOTTOM},
        {7, HT::BOTTOMLEFT}, {8, HT::BOTTOMRIGHT},
    };
    for (const auto& c : kCases) {
        auto dwp = make_dwp();
        u32 got_edge = 0;
        int calls = 0;
        dwp.set_begin_window_resize(
            [&](HWND, u32 e) { ++calls; got_edge = e; return true; });
        dwp.def_window_proc(test_hwnd, WM::SYSCOMMAND, SC::SIZE | c.wmsz, 0);
        EXPECT_EQ(calls, 1) << "WMSZ " << c.wmsz;
        EXPECT_EQ(got_edge, static_cast<u32>(c.expect)) << "WMSZ " << c.wmsz;
    }

    // A bare SC_SIZE is the keyboard "Size" menu item with no edge chosen.
    // There is no keyboard sizing mode, so it becomes the bottom-right grip
    // rather than doing nothing.
    auto dwp = make_dwp();
    u32 got_edge = 0;
    dwp.set_begin_window_resize([&](HWND, u32 e) { got_edge = e; return true; });
    dwp.def_window_proc(test_hwnd, WM::SYSCOMMAND, SC::SIZE, 0);
    EXPECT_EQ(got_edge, static_cast<u32>(HT::BOTTOMRIGHT));
}

TEST_F(DefWindowProcManagerTest, ResizeRoutingIsSafeWithNoHostHookInstalled) {
    // The hook is optional (headless hosts, tests). Every sizing path must
    // still return 0 rather than calling through a null std::function.
    auto dwp = make_dwp();
    for (u64 ht = HT::LEFT; ht <= HT::BOTTOMRIGHT; ++ht) {
        EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::NCLBUTTONDOWN, ht, 0), 0u);
    }
    EXPECT_EQ(dwp.def_window_proc(test_hwnd, WM::SYSCOMMAND, SC::SIZE | 8, 0), 0u);
}

// GuestOwnedFrame (.docs/windowing-native-presentation.md). The Electron
// rects are VS Code's main window under Vitreo: WS_CAPTION | WS_THICKFRAME,
// the client takes the caption and keeps 8 px left/right/bottom borders.
TEST(NcCalcSizeGuestFrameTest, CaptionConsumedByClientIsGuestOwned) {
    const u32 overlapped = WS::CAPTION | WS::THICKFRAME | WS::SYSMENU |
                           WS::MINIMIZEBOX | WS::MAXIMIZEBOX;
    const Rect outer{59, 246, 1259, 1046};
    // Whole window is client.
    EXPECT_TRUE(nccalcsize_guest_owns_frame(outer, outer, overlapped));
    EXPECT_TRUE(nccalcsize_guest_owns_frame(outer, outer, 0));
    // Electron restored: no top inset, invisible resize borders elsewhere.
    EXPECT_TRUE(nccalcsize_guest_owns_frame(
        outer, Rect{67, 246, 1251, 1038}, overlapped));
    // Electron maximized: the top inset is only the off-screen border.
    EXPECT_TRUE(nccalcsize_guest_owns_frame(
        Rect{-8, -8, 1928, 1088}, Rect{0, 0, 1920, 1080}, overlapped));
    // Stock frame: border plus caption band stays native chrome.
    EXPECT_FALSE(nccalcsize_guest_owns_frame(
        outer, Rect{67, 284, 1251, 1038}, overlapped));
    // A dialog frame keeps its 4 px border rule.
    EXPECT_FALSE(nccalcsize_guest_owns_frame(
        outer, Rect{63, 250 + 22, 1255, 1042}, WS::CAPTION));
    // Without WS_CAPTION, only client == window counts.
    EXPECT_FALSE(nccalcsize_guest_owns_frame(
        outer, Rect{67, 246, 1251, 1038}, WS::THICKFRAME | WS::POPUP));
}
