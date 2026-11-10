// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_def_dlg_proc.cpp
/// @brief The predefined dialog window procedure around a DLGPROC.
///
/// Expected values: tools/oracle_probe/defdlgproc{32,64}.expected (Windows 11,
/// x86 and x64 identical), probe defdlgproc_probe.c; rules from Learn DLGPROC
/// and "Dialog Box Default Message Processing".

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/user32/windows/def_window_proc.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/painting/sys_colors.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <windowing/model/window_types.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <functional>
#include <vector>
#include <winapi/user32/classes/window_class.hpp>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

namespace {
constexpr GuestAddr kDlgProc = 0x00500000;
constexpr u32 kWmApp1 = 0x8001, kWmGetTextLength = 0x000E;
constexpr u32 kWmCharToItem = 0x002F, kWmCompareItem = 0x0039, kWmVKeyToItem = 0x002E,
              kWmQueryDragIcon = 0x0037, kWmInitDialog = 0x0110;
constexpr u32 kWmCtlColorMsgBox = 0x0132, kWmCtlColorEdit = 0x0133, kWmCtlColorListBox = 0x0134,
              kWmCtlColorBtn = 0x0135, kWmCtlColorDlg = 0x0136, kWmCtlColorStatic = 0x0138;
constexpr u32 kWmEraseBkgnd = 0x0014, kWmClose = 0x0010, kWmCommand = 0x0111;
constexpr u32 kDmGetDefId = 0x0400, kDmSetDefId = 0x0401;
constexpr u32 kBsPushButton = 0, kBsDefPushButton = 1;
constexpr u32 k3DFace = 15, kWindow = 5;
}  // namespace

class DefDlgProcTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        dwp_.set_message_send_manager(&sender_);
        dwp_.set_gdi_manager(&gdi_);
        dwp_.set_is_64bit(GetParam());
        // The DLGPROC under test: returns `ret_` for `msg_`, optionally after
        // SetWindowLongPtr(DWLP_MSGRESULT, 0x1234); answers WM_CTLCOLORDLG
        // with `dlg_brush_` when set.
        dwp_.set_guest_call([this](GuestAddr proc, const u64* args, usize, bool) -> u64 {
            EXPECT_EQ(proc, kDlgProc);
            const auto msg = static_cast<u32>(args[1]);
            if (msg == kWmCtlColorDlg) {
                ++ctlcolordlg_calls_;
                if (dlg_brush_) return dlg_brush_;
            }
            if (msg == msg_) {
                if (set_result_) hwnd_mgr_.find(static_cast<HWND>(args[0]))->msg_result = 0x1234;
                return ret_;
            }
            return 0;
        });
        // SendMessage to the dialog runs the predefined dialog procedure.
        sender_.set_wndproc_callback([this](HWND target, u32 msg, u64 w, u64 l, u64) -> u64 {
            return dwp_.dialog_procedure(target, msg, w, l, kDlgProc);
        });

        WindowData d;
        d.parent = HWND_DESKTOP;
        d.is_dialog = true;
        d.wndproc = kDlgProc;
        d.dlgproc = kDlgProc;
        d.title = L"probe";
        d.rect = {0, 0, 16, 16};
        d.client_rect = {0, 0, 16, 16};
        dialog_ = hwnd_mgr_.create(d);
        ok_ = make_button(1, kBsDefPushButton, true);
        cancel_ = make_button(2, kBsPushButton, true);
    }

    HWND make_button(u32 id, u32 type, bool enabled) {
        WindowData b;
        b.parent = dialog_;
        b.class_atom = 0x0080;
        b.class_name = L"Button";
        b.style = WS::CHILD | WS::VISIBLE | type;
        b.ctrl_id = id;
        b.enabled = enabled;
        return hwnd_mgr_.create(b);
    }

    u64 send(u32 msg, u64 w, u64 l, u64 ret, bool set_result = false) {
        msg_ = msg;
        ret_ = ret;
        set_result_ = set_result;
        const u64 r = dwp_.dialog_procedure(dialog_, msg, w, l, kDlgProc);
        msg_ = 0;
        return r;
    }

    /// The DLGPROC's INT_PTR as the guest returns it.
    u64 int_ptr(i64 v) const {
        return GetParam() ? static_cast<u64>(v) : static_cast<u64>(static_cast<u32>(v));
    }
    u64 lresult(i64 v) const { return static_cast<u64>(v); }

    HwndManager hwnd_mgr_;
    MessageQueue queue_;
    GuestMemory memory_;
    CocoaBridge bridge_;
    MessageSendManager sender_{hwnd_mgr_, queue_, bridge_};
    HandleTable handles_;
    GdiManager gdi_{handles_};
    DefWindowProcManager dwp_{hwnd_mgr_, queue_, memory_};
    HWND dialog_ = 0, ok_ = 0, cancel_ = 0;
    u32 msg_ = 0;
    u64 ret_ = 0;
    bool set_result_ = false;
    u64 dlg_brush_ = 0;
    int ctlcolordlg_calls_ = 0;
};

TEST_P(DefDlgProcTest, MessageResultRules) {
    // "user FALSE -> 0", "user msgresult_after_false=0"
    hwnd_mgr_.find(dialog_)->msg_result = 0x77;
    EXPECT_EQ(send(kWmApp1, 0, 0, 0), 0u);
    EXPECT_EQ(hwnd_mgr_.find(dialog_)->msg_result, 0u);
    // "user TRUE stale -> 0": DWLP_MSGRESULT is cleared before the DLGPROC.
    hwnd_mgr_.find(dialog_)->msg_result = 0x77;
    EXPECT_EQ(send(kWmApp1, 0, 0, 1), 0u);
    // "user TRUE set -> 1234", "user 5 set -> 1234"
    EXPECT_EQ(send(kWmApp1, 0, 0, 1, true), 0x1234u);
    EXPECT_EQ(send(kWmApp1, 0, 0, 5, true), 0x1234u);
    // "gettextlength FALSE -> 5" (DefWindowProc), "gettextlength TRUE stale -> 0"
    EXPECT_EQ(send(kWmGetTextLength, 0, 0, 0), 5u);
    hwnd_mgr_.find(dialog_)->msg_result = 0x77;
    EXPECT_EQ(send(kWmGetTextLength, 0, 0, 1), 0u);
}

TEST_P(DefDlgProcTest, DirectResultMessages) {
    // "<msg> FALSE -> 0  ret5(msgresult set) -> 5  ret-2 -> -2"
    for (const u32 msg : {kWmCharToItem, kWmCompareItem, kWmVKeyToItem, kWmQueryDragIcon, kWmInitDialog}) {
        EXPECT_EQ(send(msg, 0, 0, 0), 0u) << msg;
        EXPECT_EQ(send(msg, 0, 0, 5, true), 5u) << msg;
        EXPECT_EQ(send(msg, 0, 0, int_ptr(-2)), lresult(-2)) << msg;
    }
    // "ctlcolor static ret_brush -> is_red=1"
    const Handle red = gdi_.create_solid_brush(0x0000FF);
    const Handle dc = gdi_.create_compatible_dc(0);
    EXPECT_EQ(send(kWmCtlColorStatic, dc, 0, red), static_cast<u64>(red));
}

TEST_P(DefDlgProcTest, ControlColourDefaults) {
    const Handle dc = gdi_.create_compatible_dc(0);
    struct Row { u32 msg; u32 index; };
    // "ctlcolor msgbox/btn/dlg/static FALSE -> COLOR_3DFACE text=000000 bk=f0f0f0 mode=2",
    // "ctlcolor edit/listbox FALSE -> COLOR_WINDOW text=000000 bk=ffffff mode=2"
    for (const Row row : {Row{kWmCtlColorMsgBox, k3DFace}, Row{kWmCtlColorEdit, kWindow},
                          Row{kWmCtlColorListBox, kWindow}, Row{kWmCtlColorBtn, k3DFace},
                          Row{kWmCtlColorDlg, k3DFace}, Row{kWmCtlColorStatic, k3DFace}}) {
        (void)gdi_.set_text_color(dc, 0x123456);
        (void)gdi_.set_bk_color(dc, 0x654321);
        (void)gdi_.set_bk_mode(dc, 1);
        EXPECT_EQ(send(row.msg, dc, 0, 0), sys_colors::kSysBrushBase | row.index) << row.msg;
        EXPECT_EQ(gdi_.get_text_color(dc), sys_colors::get(8)) << row.msg;
        EXPECT_EQ(gdi_.get_bk_color(dc), sys_colors::get(row.index)) << row.msg;
        EXPECT_EQ(gdi_.get_bk_mode(dc), 2) << row.msg;
    }
    // The measured colours themselves (Windows 11 defaults).
    EXPECT_EQ(sys_colors::get(k3DFace), 0xF0F0F0u);
    EXPECT_EQ(sys_colors::get(kWindow), 0xFFFFFFu);
}

TEST_P(DefDlgProcTest, EraseBackgroundUsesTheDialogBrush) {
    const Handle dc = gdi_.create_compatible_dc(0);
    // A bitmap compatible with a memory DC is monochrome (Learn
    // CreateCompatibleBitmap); take the screen's format.
    const Handle screen = gdi_.get_dc(0);
    const Handle bmp = gdi_.create_compatible_bitmap(screen, 16, 16);
    (void)gdi_.select_object(dc, bmp);
    // "erasebkgnd FALSE default -> 1 pixel00=f0f0f0 pixel1515=f0f0f0 ctlcolordlg_calls=1"
    (void)gdi_.fill_rect_gdi(dc, 0, 0, 16, 16, 0x000000);
    EXPECT_EQ(send(kWmEraseBkgnd, dc, 0, 0), 1u);
    EXPECT_EQ(ctlcolordlg_calls_, 1);
    EXPECT_EQ(gdi_.get_pixel(dc, 0, 0), 0xF0F0F0u);
    EXPECT_EQ(gdi_.get_pixel(dc, 15, 15), 0xF0F0F0u);
    // "erasebkgnd TRUE -> 0 pixel00=000000 ... ctlcolordlg_calls=0"
    ctlcolordlg_calls_ = 0;
    (void)gdi_.fill_rect_gdi(dc, 0, 0, 16, 16, 0x000000);
    EXPECT_EQ(send(kWmEraseBkgnd, dc, 0, 1), 0u);
    EXPECT_EQ(ctlcolordlg_calls_, 0);
    EXPECT_EQ(gdi_.get_pixel(dc, 0, 0), 0x000000u);
    // "erasebkgnd FALSE red -> 1 pixel00=0000ff pixel1515=0000ff ctlcolordlg_calls=1"
    dlg_brush_ = gdi_.create_solid_brush(0x0000FF);
    EXPECT_EQ(send(kWmEraseBkgnd, dc, 0, 0), 1u);
    EXPECT_EQ(ctlcolordlg_calls_, 1);
    EXPECT_EQ(gdi_.get_pixel(dc, 0, 0), 0x0000FFu);
    EXPECT_EQ(gdi_.get_pixel(dc, 15, 15), 0x0000FFu);
}

TEST_P(DefDlgProcTest, CloseClicksCancel) {
    // "close enabled cancel -> 0 during_send=0 after_pump=1 wp=2 lp_is_cancel=1"
    EXPECT_EQ(send(kWmClose, 0, 0, 0), 0u);
    MSG posted{};
    ASSERT_TRUE(queue_.peek(posted, dialog_, 0, 0, PM::REMOVE));
    EXPECT_EQ(posted.message, kWmCommand);
    EXPECT_EQ(posted.wParam, 2u);
    EXPECT_EQ(posted.lParam, static_cast<u64>(cancel_));
    EXPECT_TRUE(hwnd_mgr_.is_valid(dialog_));
    // "close disabled cancel -> 0 ... after_pump=0"
    hwnd_mgr_.find(cancel_)->enabled = false;
    EXPECT_EQ(send(kWmClose, 0, 0, 0), 0u);
    EXPECT_FALSE(queue_.peek(posted, dialog_, 0, 0, PM::REMOVE));
    // "close no cancel -> 0 ... after_pump=1 wp=2 lp_is_cancel=0 lp_null=1"
    (void)hwnd_mgr_.destroy(cancel_);
    EXPECT_EQ(send(kWmClose, 0, 0, 0), 0u);
    ASSERT_TRUE(queue_.peek(posted, dialog_, 0, 0, PM::REMOVE));
    EXPECT_EQ(posted.wParam, 2u);
    EXPECT_EQ(posted.lParam, 0u);
}

TEST_P(DefDlgProcTest, DefaultPushButtonId) {
    // "getdefid FALSE -> 534b0001"
    EXPECT_EQ(send(kDmGetDefId, 0, 0, 0), 0x534B0001u);
    // "setdefid(2) FALSE -> 1", "getdefid after set -> 534b0002 ok_style=0 cancel_style=1"
    EXPECT_EQ(send(kDmSetDefId, 2, 0, 0), 1u);
    EXPECT_EQ(send(kDmGetDefId, 0, 0, 0), 0x534B0002u);
    EXPECT_EQ(hwnd_mgr_.find(ok_)->style & 0xF, kBsPushButton);
    EXPECT_EQ(hwnd_mgr_.find(cancel_)->style & 0xF, kBsDefPushButton);
    // "setdefid(999 missing) -> 1 then getdefid -> 534b0002"
    EXPECT_EQ(send(kDmSetDefId, 999, 0, 0), 1u);
    EXPECT_EQ(send(kDmGetDefId, 0, 0, 0), 0x534B0002u);
}

INSTANTIATE_TEST_SUITE_P(Abi, DefDlgProcTest, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// Learn DefDlgProc / "Dialog Box Default Message Processing": a message the
// DLGPROC returns FALSE for gets DefWindowProc's processing, so WM_SETCURSOR
// over the client area sets the dialog class's cursor (Learn WM_SETCURSOR).
// A DLGPROC that handles it (TRUE) keeps whatever cursor it chose.
TEST_P(DefDlgProcTest, SetCursorFalseSetsTheClassCursor) {
    WindowClassManager classes;
    WindowClassData cls;
    cls.class_name = L"VitreoDlgCursorProbe";
    cls.hcursor = 0x10011;
    const ATOM atom = classes.register_class(cls);
    ASSERT_NE(atom, 0);
    hwnd_mgr_.find(dialog_)->class_atom = atom;
    dwp_.set_wclass_manager(&classes);
    std::vector<u32> applied;
    dwp_.set_apply_cursor([&](u32 cursor) { applied.push_back(cursor); });
    constexpr u32 kWmSetCursor = 0x0020;
    const u64 client = 1u | (0x0200u << 16);  // HTCLIENT, WM_MOUSEMOVE
    EXPECT_EQ(send(kWmSetCursor, dialog_, client, 0), 1u);
    EXPECT_EQ(applied, (std::vector<u32>{0x10011}));
    applied.clear();
    // Handled by the DLGPROC: the result is DWLP_MSGRESULT ("user TRUE stale
    // -> 0"), and no default processing runs.
    EXPECT_EQ(send(kWmSetCursor, dialog_, client, 1), 0u);
    EXPECT_TRUE(applied.empty());

    // The predefined dialog class's cursor is the system arrow
    // (dialog_class_cursor_probe{32,64}: class_cursor_is_arrow=1).
    const WindowClassData* dialog_class = classes.find_by_name(L"#32770");
    ASSERT_NE(dialog_class, nullptr);
    hwnd_mgr_.find(dialog_)->class_atom = dialog_class->atom;
    classes.set_system_cursor_provider([] { return u64{0x10003}; });
    applied.clear();
    EXPECT_EQ(send(kWmSetCursor, dialog_, client, 0), 1u);
    EXPECT_EQ(applied, (std::vector<u32>{0x10003}));
    dwp_.set_apply_cursor({});
    dwp_.set_wclass_manager(nullptr);
}
