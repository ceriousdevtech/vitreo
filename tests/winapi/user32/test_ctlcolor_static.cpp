/// @file test_ctlcolor_static.cpp
/// @brief A text STATIC asks its parent for its colours with
///        WM_CTLCOLORSTATIC each time it is drawn (MS Learn
///        WM_CTLCOLORSTATIC): wParam is a DC for the control, lParam the
///        control; the parent sets the text colour and background mode/colour
///        on that DC and returns the brush the control's background is filled
///        with. A zero result is the default handling.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/user32_internal.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/classes/window_class.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <windowing/messages/message_queue.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <optional>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

namespace {
constexpr u32 kWmPaint = 0x000F;
constexpr u32 kWmCtlColorStatic = 0x0138;
constexpr u16 kAtomStatic = 0x0082;
constexpr u32 kWsChild = 0x40000000u, kWsVisible = 0x10000000u;
constexpr u32 kSsLeft = 0x0, kSsIcon = 0x3;
constexpr i32 kTransparent = 1, kOpaque = 2;
}  // namespace

class CtlColorStaticTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;

    void SetUp() override {
        if (GetParam()) cpu_.init_long_mode();
        else cpu_.init_protected_mode();
        (void)memory_.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        gdi_.set_guest_memory(&memory_);
        mgr_ = std::make_unique<User32ExtManager>(memory_, vfs_, hwnd_mgr_, class_mgr_, bridge_, queue_);
        msgsend_ = std::make_unique<MessageSendManager>(hwnd_mgr_, queue_, bridge_);
        mgr_->set_message_send_manager(msgsend_.get());
        mgr_->set_painting_manager(&paint_);
        mgr_->set_gdi_manager(&gdi_);
        msgsend_->set_wndproc_callback([this](HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64) -> u64 {
            if (msg < 0x0132 || msg > 0x0138) return 0;  // WM_CTLCOLOR*
            seen_msg_ = msg;
            ++calls_;
            seen_parent_ = hwnd;
            seen_dc_ = wParam;
            seen_control_ = lParam;
            return answer_ ? answer_(static_cast<Handle>(wParam)) : 0;
        });

        WindowData parent{};
        parent.parent = HWND_DESKTOP;
        parent.wndproc = 0x00401000;
        parent.rect = {0, 0, 400, 300};
        parent.client_rect = {0, 0, 400, 300};
        parent.visible = true;
        parent_ = hwnd_mgr_.create(parent);
        ASSERT_TRUE(bridge_.create_window(parent_, *hwnd_mgr_.find(parent_)).is_ok());
        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    HWND make_static(u32 ss_style) {
        WindowData child{};
        child.parent = parent_;
        child.class_atom = kAtomStatic;
        child.class_name = L"Static";
        child.style = kWsChild | kWsVisible | ss_style;
        child.rect = {12, 30, 424, 48};
        child.client_rect = {0, 0, 412, 18};
        child.visible = true;
        child.enabled = true;
        const HWND hwnd = hwnd_mgr_.create(child);
        bridge_.add_child_view(parent_, hwnd, *hwnd_mgr_.find(hwnd), kAtomStatic, L"Static", L"c:\\file.exe");
        return hwnd;
    }

    void paint(HWND hwnd) {
        DllExports exports = create_user32_ext_thunks(*mgr_);
        ExportEntry entry{};
        for (const auto& e : exports.entries) {
            if (e.name == "CallWindowProcA") { entry = e; break; }
        }
        ASSERT_TRUE(static_cast<bool>(entry.handler));
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        if (GetParam()) {
            cpu_.gpr.rcx = kNativeControlWndProc;
            cpu_.gpr.rdx = static_cast<u64>(hwnd);
            cpu_.gpr.r8 = kWmPaint;
            cpu_.gpr.r9 = 0;
            (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, 0);
        } else {
            const u32 args[] = {static_cast<u32>(kNativeControlWndProc), hwnd, kWmPaint, 0, 0};
            for (u32 i = 0; i < 5; ++i) (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4, args[i]);
        }
        entry.handler(m);
    }

    GuestMemory memory_;
    capsule::Vfs vfs_;
    HwndManager hwnd_mgr_;
    WindowClassManager class_mgr_;
    CocoaBridge bridge_;
    MessageQueue queue_;
    HandleTable handles_;
    GdiManager gdi_{handles_};
    PaintingManager paint_;
    std::unique_ptr<User32ExtManager> mgr_;
    std::unique_ptr<MessageSendManager> msgsend_;
    cpu::CpuState cpu_;
    HWND parent_ = 0;
    std::function<u64(Handle)> answer_;
    int calls_ = 0;
    HWND seen_parent_ = 0;
    u64 seen_dc_ = 0, seen_control_ = 0;
    u32 seen_msg_ = 0;

    HWND make_control(u16 atom, const wchar_t* cls, u32 style) {
        WindowData child{};
        child.parent = parent_;
        child.class_atom = atom;
        child.class_name = cls;
        child.style = kWsChild | kWsVisible | style;
        child.rect = {200, 100, 340, 290};
        child.client_rect = {0, 0, 140, 190};
        child.visible = true;
        child.enabled = true;
        const HWND hwnd = hwnd_mgr_.create(child);
        bridge_.add_child_view(parent_, hwnd, *hwnd_mgr_.find(hwnd), atom, cls, L"");
        return hwnd;
    }
};

TEST_P(CtlColorStaticTest, ParentColoursReachTheLabel) {
    // A setup engine's progress page: white text on its black brush,
    // TRANSPARENT background mode.
    const Handle black = gdi_.create_solid_brush(0x000000);
    answer_ = [&](Handle dc) {
        (void)gdi_.set_text_color(dc, 0xFFFFFF);
        (void)gdi_.set_bk_mode(dc, kTransparent);
        return static_cast<u64>(black);
    };
    const HWND label = make_static(kSsLeft);
    paint(label);
    ASSERT_EQ(calls_, 1);
    EXPECT_EQ(seen_parent_, parent_);
    EXPECT_EQ(seen_control_, static_cast<u64>(label));
    EXPECT_NE(seen_dc_, 0u);
    u32 text = 0;
    std::optional<u32> fill;
    ASSERT_TRUE(bridge_.native_label_colors(label, &text, &fill));
    EXPECT_EQ(text, 0xFFFFFFu);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(*fill, 0x000000u);

    // SetWindowText keeps the colours the parent chose.
    ASSERT_TRUE(bridge_.set_control_text(label, L"c:\\...\\armada2.exe"));
    ASSERT_TRUE(bridge_.native_label_colors(label, &text, &fill));
    EXPECT_EQ(text, 0xFFFFFFu);
}

TEST_P(CtlColorStaticTest, HollowBrushPaintsNoBackground) {
    const Handle hollow = gdi_.get_stock_object(5);  // NULL_BRUSH
    answer_ = [&](Handle dc) {
        (void)gdi_.set_text_color(dc, 0x0000FF);
        (void)gdi_.set_bk_mode(dc, kOpaque);
        (void)gdi_.set_bk_color(dc, 0x00FF00);
        return static_cast<u64>(hollow);
    };
    const HWND label = make_static(kSsLeft);
    paint(label);
    ASSERT_EQ(calls_, 1);
    u32 text = 0;
    std::optional<u32> fill;
    ASSERT_TRUE(bridge_.native_label_colors(label, &text, &fill));
    EXPECT_EQ(text, 0x0000FFu);
    EXPECT_FALSE(fill.has_value());
}

TEST_P(CtlColorStaticTest, ZeroResultKeepsDefaultColours) {
    const HWND label = make_static(kSsLeft);
    u32 before = 0;
    std::optional<u32> fill_before;
    ASSERT_TRUE(bridge_.native_label_colors(label, &before, &fill_before));
    answer_ = [&](Handle dc) {
        (void)gdi_.set_text_color(dc, 0x123456);
        return u64{0};
    };
    paint(label);
    ASSERT_EQ(calls_, 1);
    u32 after = 0;
    std::optional<u32> fill_after;
    ASSERT_TRUE(bridge_.native_label_colors(label, &after, &fill_after));
    EXPECT_EQ(after, before);
    EXPECT_EQ(fill_after, fill_before);
}

TEST_P(CtlColorStaticTest, ImageStaticDoesNotAskForTextColours) {
    const HWND icon = make_static(kSsIcon);
    paint(icon);
    EXPECT_EQ(calls_, 0);
}

INSTANTIATE_TEST_SUITE_P(Abi, CtlColorStaticTest, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// Learn WM_CTLCOLORSTATIC / WM_CTLCOLOREDIT / WM_CTLCOLORLISTBOX: a read-only
// (or disabled) edit asks with WM_CTLCOLORSTATIC, an editable one with
// WM_CTLCOLOREDIT, a list box with WM_CTLCOLORLISTBOX; the brush and text
// colour reach the native text view / table.
TEST_P(CtlColorStaticTest, EditAndListBoxAskWithTheirOwnMessages) {
    const Handle black = gdi_.create_solid_brush(0x000000);
    answer_ = [&](Handle dc) {
        (void)gdi_.set_text_color(dc, 0x00FFFF);
        (void)gdi_.set_bk_mode(dc, kTransparent);
        return static_cast<u64>(black);
    };
    constexpr u16 kAtomEdit = 0x0081, kAtomListBox = 0x0083;
    constexpr u32 kEsMultiline = 0x0004, kEsAutoVScroll = 0x0040, kEsReadOnly = 0x0800;
    const struct { u16 atom; const wchar_t* cls; u32 style; u32 message; } kRows[] = {
        {kAtomEdit, L"Edit", kEsMultiline | kEsAutoVScroll | kEsReadOnly, 0x0138},  // read-only
        {kAtomEdit, L"Edit", kEsMultiline | kEsAutoVScroll, 0x0133},               // editable
        {kAtomListBox, L"ListBox", 0x00A00001u, 0x0134},                           // LBS_NOTIFY|WS_VSCROLL|WS_BORDER
    };
    for (const auto& row : kRows) {
        calls_ = 0;
        const HWND control = make_control(row.atom, row.cls, row.style);
        paint(control);
        ASSERT_EQ(calls_, 1) << std::hex << row.message;
        EXPECT_EQ(seen_msg_, row.message);
        EXPECT_EQ(seen_control_, static_cast<u64>(control));
        u32 text = 0;
        std::optional<u32> fill;
        ASSERT_TRUE(bridge_.native_label_colors(control, &text, &fill)) << std::hex << row.message;
        EXPECT_EQ(text, 0x00FFFFu);
        ASSERT_TRUE(fill.has_value());
        EXPECT_EQ(*fill, 0x000000u);
    }
    // A disabled edit asks with WM_CTLCOLORSTATIC too.
    calls_ = 0;
    const HWND disabled = make_control(kAtomEdit, L"Edit", kEsMultiline);
    hwnd_mgr_.find(disabled)->enabled = false;
    paint(disabled);
    ASSERT_EQ(calls_, 1);
    EXPECT_EQ(seen_msg_, 0x0138u);
}

// Learn Edit Control Styles: an ES_READONLY edit refuses typing (single- and
// multiline). The colours its parent chose survive a WM_SETTEXT and apply
// to text inserted after it.
TEST_P(CtlColorStaticTest, ReadOnlyEditRefusesTypingAndKeepsColoursAcrossText) {
    const Handle black = gdi_.create_solid_brush(0x000000);
    answer_ = [&](Handle dc) {
        (void)gdi_.set_text_color(dc, 0x00BFFF);
        (void)gdi_.set_bk_mode(dc, kTransparent);
        return static_cast<u64>(black);
    };
    constexpr u16 kAtomEdit = 0x0081;
    constexpr u32 kEsMultiline = 0x0004, kEsReadOnly = 0x0800;
    const HWND description = make_control(kAtomEdit, L"Edit", kEsMultiline | kEsReadOnly);
    const HWND single = make_control(kAtomEdit, L"Edit", kEsReadOnly);
    const HWND editable = make_control(kAtomEdit, L"Edit", kEsMultiline);
    EXPECT_TRUE(bridge_.edit_is_read_only(description));
    EXPECT_TRUE(bridge_.edit_is_read_only(single));
    EXPECT_FALSE(bridge_.edit_is_read_only(editable));
    for (const HWND edit : {description, editable}) {
        paint(edit);
        ASSERT_TRUE(bridge_.set_control_text(edit, L"Warzone: eight players"));
        u32 text = 0;
        std::optional<u32> fill;
        ASSERT_TRUE(bridge_.native_label_colors(edit, &text, &fill));
        EXPECT_EQ(text, 0x00BFFFu);
        ASSERT_TRUE(fill.has_value());
        EXPECT_EQ(*fill, 0x000000u);
        // Inserted text takes the parent's text colour.
        EXPECT_EQ(bridge_.edit_typing_text_color(edit), 0x00BFFFu);
    }
}

// Learn Edit Control Styles, ES_NOHIDESEL: without it an edit control's
// selection is hidden while it does not have the focus.
TEST_P(CtlColorStaticTest, UnfocusedEditHidesItsSelectionUnlessNoHideSel) {
    constexpr u16 kAtomEdit = 0x0081;
    constexpr u32 kEsMultiline = 0x0004, kEsNoHideSel = 0x0100, kEsReadOnly = 0x0800;
    const HWND hides = make_control(kAtomEdit, L"Edit", kEsMultiline | kEsReadOnly);
    const HWND keeps = make_control(kAtomEdit, L"Edit", kEsMultiline | kEsNoHideSel);
    ASSERT_TRUE(bridge_.set_control_text(hides, L"Game Speed 6\r\n"));
    ASSERT_TRUE(bridge_.set_control_text(keeps, L"Game Speed 6\r\n"));
    EXPECT_FALSE(bridge_.edit_shows_unfocused_selection(hides));
    EXPECT_TRUE(bridge_.edit_shows_unfocused_selection(keeps));
}
