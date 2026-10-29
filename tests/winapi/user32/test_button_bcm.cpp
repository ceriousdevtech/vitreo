// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_button_bcm.cpp
/// @brief Button class BCM_* (comctl32 v6) and BM_SETIMAGE / BM_GETIMAGE
/// through SendMessageW, in both bitnesses.
///
/// Expected values come from tools/oracle_probe/button_bcm_probe.c
/// (button_bcm_probe{64,32}.expected and button_bcm_probe64.nov6.expected).
/// BCM_GETIDEALSIZE is checked as the probe's rule over the text box the
/// same GDI measures, since host fonts are not Windows' fonts: push buttons
/// add 7 x 9 (with the default 1-pixel margins), default buttons 2 more,
/// split buttons 32 more in width, check boxes and radio buttons
/// 13 + 5 + width("0") / 2 + margins in width.

#include <gtest/gtest.h>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <vitreo/common.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/user32/messages/message_send.hpp>

#include <initializer_list>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

namespace vitreo::winapi {
void impl_SendMessageW(Marshaler& m, MessageSendManager& msm);
}

namespace {

constexpr u32 BCM_GETIDEALSIZE = 0x1601, BCM_SETIMAGELIST = 0x1602, BCM_GETIMAGELIST = 0x1603,
              BCM_SETTEXTMARGIN = 0x1604, BCM_GETTEXTMARGIN = 0x1605,
              BCM_SETDROPDOWNSTATE = 0x1606, BCM_SETSPLITINFO = 0x1607,
              BCM_GETSPLITINFO = 0x1608, BCM_SETNOTE = 0x1609, BCM_GETNOTE = 0x160A,
              BCM_GETNOTELENGTH = 0x160B, BCM_SETSHIELD = 0x160C;
constexpr u32 BM_GETIMAGE = 0xF6, BM_SETIMAGE = 0xF7;
constexpr u32 WS_CHILD_VISIBLE = 0x50000000;

class ButtonBcmTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr DATA = 0x10000, STACK = 0x80000;
    static constexpr GuestAddr OUT = DATA, BUF = DATA + 0x100, TEXT = DATA + 0x400;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x4000, MemoryProtection::ReadWrite).is_ok());
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK + 0x2000;
        marshaler.attach_cpu(cpu);
        gdi.set_guest_memory(&memory);
        msm.set_gdi_manager(&gdi);
        msm.set_common_controls_v6(true);
        msm.set_image_list_size_query([](u64 h, i32& cx, i32& cy) {
            if (h != 0x5000) return false;
            cx = cy = 16;
            return true;
        });
    }

    bool x64() const { return GetParam(); }

    HWND button(u32 style, const std::wstring& text, i32 w = 77, i32 h = 23) {
        WindowData d;
        d.class_name = L"Button";
        d.title = text;
        d.style = WS_CHILD_VISIBLE | style;
        d.parent = HWND_DESKTOP;
        d.rect = {5, 5, 5 + w, 5 + h};
        return hwnd_mgr.create(d);
    }

    u64 send(HWND hwnd, u32 msg, u64 wp, u64 lp) {
        const u64 args[4] = {hwnd, msg, wp, lp};
        for (usize i = 0; i < 4; ++i) {
            if (x64()) {
                u64* gp[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
                *gp[i] = args[i];
            } else {
                EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
            }
        }
        impl_SendMessageW(marshaler, msm);
        return x64() ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFF);
    }

    void write_ints(GuestAddr at, std::initializer_list<i32> v) {
        usize i = 0;
        for (i32 x : v) ASSERT_TRUE(memory.write_u32(at + 4 * i++, static_cast<u32>(x)).is_ok());
    }
    i32 read_int(GuestAddr at) { return static_cast<i32>(memory.read_u32(at).value()); }

    /// GDI's DT_CALCRECT box of @p text in @p font, and width("0").
    void measure(Handle font, const std::u16string& text, i32& w, i32& h, i32& zero) {
        const Handle dc = gdi.create_compatible_dc(0);
        const Handle old = gdi.select_object(dc, font);
        i32 l = 0, t = 0, r = 0, b = 0;
        (void)gdi.draw_text_ex(dc, text, l, t, r, b, 0x400 /* DT_CALCRECT */, nullptr);
        w = r - l;
        h = b - t;
        i32 zh = 0;
        (void)gdi.text_extent(dc, u"0", zero, zh);
        (void)gdi.select_object(dc, old);
        (void)gdi.delete_dc(dc);
    }

    void ideal(HWND hwnd, i32 in_cx, i32& cx, i32& cy, u64& r) {
        write_ints(OUT, {in_cx, 0});
        r = send(hwnd, BCM_GETIDEALSIZE, 0, OUT);
        cx = read_int(OUT);
        cy = read_int(OUT + 4);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HandleTable handles;
    GdiManager gdi{handles};
    HwndManager hwnd_mgr;
    MessageQueue queue;
    CocoaBridge bridge;
    MessageSendManager msm{hwnd_mgr, queue, bridge};
};

TEST_P(ButtonBcmTest, IdealSizeIsTheTextBoxPlusEachTypesFixedParts) {
    const Handle font = gdi.create_font_w(-12, 400, false, u"Helvetica");
    const std::u16string text = u"Install SumatraPDF";
    i32 tw = 0, th = 0, zero = 0;
    measure(font, text, tw, th, zero);
    ASSERT_GT(tw, 0);

    struct Row { u32 style; i32 dx; i32 dy; };
    const Row rows[] = {
        {0x0, 7, 9},                    // BS_PUSHBUTTON
        {0x1, 9, 11},                   // BS_DEFPUSHBUTTON
        {0x8, 7, 9},                    // BS_USERBUTTON
        {0xC, 39, 9},                   // BS_SPLITBUTTON
        {0xD, 41, 11},                  // BS_DEFSPLITBUTTON
        {0x3, 20 + zero / 2, 9},        // BS_AUTOCHECKBOX
        {0x9, 20 + zero / 2, 9},        // BS_AUTORADIOBUTTON
        {0x2000, 7, 9},                 // BS_PUSHBUTTON | BS_MULTILINE, no width
    };
    for (const Row& row : rows) {
        const HWND b = button(row.style, std::wstring(text.begin(), text.end()));
        msm.window_data(b)->hfont = font;
        i32 cx = 0, cy = 0;
        u64 r = 0;
        ideal(b, 0, cx, cy, r);
        EXPECT_EQ(r, 1u) << std::hex << row.style;
        EXPECT_EQ(cx, tw + row.dx) << std::hex << row.style;
        EXPECT_EQ(cy, th + row.dy) << std::hex << row.style;
        // A width is honoured only with BS_MULTILINE (probe "push/.../incx40").
        if (row.style == 0x0) {
            ideal(b, 40, cx, cy, r);
            EXPECT_EQ(cx, tw + 7);
        }
    }
}

TEST_P(ButtonBcmTest, OtherTypesAndEmptyTextReportTheWindowRectangle) {
    // BS_3STATE, BS_AUTO3STATE, BS_GROUPBOX, BS_PUSHBOX, BS_OWNERDRAW
    // (probe "3state/auto3state/group/pushbox/owner"), and empty text.
    for (u32 style : {0x5u, 0x6u, 0x7u, 0xAu, 0xBu}) {
        const HWND b = button(style, L"Install", 77, 23);
        i32 cx = 0, cy = 0;
        u64 r = 0;
        ideal(b, 0, cx, cy, r);
        EXPECT_EQ(r, 1u);
        EXPECT_EQ(cx, 77) << style;
        EXPECT_EQ(cy, 23) << style;
    }
    const HWND empty = button(0x0, L"", 120, 40);
    i32 cx = 0, cy = 0;
    u64 r = 0;
    ideal(empty, 0, cx, cy, r);
    EXPECT_EQ(cx, 120);
    EXPECT_EQ(cy, 40);
    EXPECT_EQ(send(empty, BCM_GETIDEALSIZE, 0, 0), 0u);  // NULL SIZE
}

TEST_P(ButtonBcmTest, TextMarginAndImageListShapeTheIdealSize) {
    const Handle font = gdi.create_font_w(-12, 400, false, u"Helvetica");
    i32 tw = 0, th = 0, zero = 0;
    measure(font, u"Install SumatraPDF", tw, th, zero);
    const HWND b = button(0x0, L"Install SumatraPDF");
    msm.window_data(b)->hfont = font;

    // Default margin 1,1,1,1 (probe "margin default").
    write_ints(BUF, {-7, -7, -7, -7});
    EXPECT_EQ(send(b, BCM_GETTEXTMARGIN, 0, BUF), 1u);
    EXPECT_EQ(read_int(BUF), 1);
    EXPECT_EQ(read_int(BUF + 12), 1);
    // 3,4,5,6 adds (3+5)-2 and (4+6)-2 (probe "margin set ... ideal=112,32").
    write_ints(BUF, {3, 4, 5, 6});
    EXPECT_EQ(send(b, BCM_SETTEXTMARGIN, 0, BUF), 1u);
    i32 cx = 0, cy = 0;
    u64 r = 0;
    ideal(b, 0, cx, cy, r);
    EXPECT_EQ(cx, tw + 7 + 6);
    EXPECT_EQ(cy, th + 9 + 8);
    EXPECT_EQ(send(b, BCM_SETTEXTMARGIN, 0, 0), 0u);
    EXPECT_EQ(send(b, BCM_GETTEXTMARGIN, 0, 0), 0u);
    write_ints(BUF, {1, 1, 1, 1});
    (void)send(b, BCM_SETTEXTMARGIN, 0, BUF);

    // BUTTON_IMAGELIST: x86 {himl@0, margin@4, align@20}, x64 {himl@0,
    // margin@8, align@24}; a 16x16 list with margins 1,2,3,4 adds 20 beside
    // the text, or 22 above it (probe "imagelist align=0/2/4").
    const u64 m_off = x64() ? 8 : 4, a_off = x64() ? 24 : 20;
    auto set_list = [&](u32 align) {
        ASSERT_TRUE(memory.write_u64(BUF, 0x5000).is_ok());
        write_ints(BUF + m_off, {1, 2, 3, 4});
        ASSERT_TRUE(memory.write_u32(BUF + a_off, align).is_ok());
        EXPECT_EQ(send(b, BCM_SETIMAGELIST, 0, BUF), 1u);
    };
    set_list(0);
    ideal(b, 0, cx, cy, r);
    EXPECT_EQ(cx, tw + 7 + 20);
    EXPECT_EQ(cy, std::max(th + 9, 22));
    set_list(2);
    ideal(b, 0, cx, cy, r);
    EXPECT_EQ(cx, tw + 7);
    EXPECT_EQ(cy, th + 9 + 22);
    set_list(4);
    ideal(b, 0, cx, cy, r);
    EXPECT_EQ(cx, 20);
    EXPECT_EQ(cy, 22);
    ASSERT_TRUE(memory.write_u64(OUT + 0x40, 0).is_ok());
    EXPECT_EQ(send(b, BCM_GETIMAGELIST, 0, OUT + 0x40), 1u);
    EXPECT_EQ(x64() ? memory.read_u64(OUT + 0x40).value() : memory.read_u32(OUT + 0x40).value(), 0x5000u);
    EXPECT_EQ(read_int(OUT + 0x40 + a_off), 4);
    // A bad alignment is stored (probe "imagelist badalign r=1").
    set_list(5);
}

TEST_P(ButtonBcmTest, NotesAreCommandLinkOnly) {
    const HWND push = button(0x0, L"Push");
    const HWND link = button(0xE, L"Link", 200, 50);
    write_ints(TEXT, {});
    const std::u16string note = u"A note";
    for (usize i = 0; i <= note.size(); ++i)
        ASSERT_TRUE(memory.write_u16(TEXT + i * 2, i < note.size() ? note[i] : 0).is_ok());

    set_last_error(0);
    EXPECT_EQ(send(push, BCM_SETNOTE, 0, TEXT), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_SUPPORTED);
    EXPECT_EQ(send(push, BCM_GETNOTELENGTH, 0, 0), 0u);

    EXPECT_EQ(send(link, BCM_SETNOTE, 0, TEXT), 1u);
    EXPECT_EQ(send(link, BCM_GETNOTELENGTH, 0, 0), 6u);
    write_ints(OUT, {64});
    EXPECT_EQ(send(link, BCM_GETNOTE, OUT, BUF), 1u);
    EXPECT_EQ(memory.read_u16(BUF).value(), u'A');
    EXPECT_EQ(memory.read_u16(BUF + 12).value(), 0u);
    // Short buffer: what fits plus NUL, the needed size, 122 (probe "small").
    write_ints(OUT, {3});
    set_last_error(0);
    EXPECT_EQ(send(link, BCM_GETNOTE, OUT, BUF), 0u);
    EXPECT_EQ(read_int(OUT), 7);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u16(BUF + 4).value(), 0u);
}

TEST_P(ButtonBcmTest, SplitInfoDropDownShieldAndImages) {
    const HWND b = button(0xC, L"Split");
    // BUTTON_SPLITINFO x86 {mask@0, glyph@4, style@8, size@12};
    // x64 {mask@0, glyph@8, style@16, size@20}. Defaults BCSS_STRETCH, 15x0.
    const u64 st = x64() ? 16 : 8, sz = x64() ? 20 : 12;
    write_ints(BUF, {0xC});
    EXPECT_EQ(send(b, BCM_GETSPLITINFO, 0, BUF), 1u);
    EXPECT_EQ(read_int(BUF + st), 2);
    EXPECT_EQ(read_int(BUF + sz), 15);
    EXPECT_EQ(read_int(BUF + sz + 4), 0);
    write_ints(BUF, {0xC});
    write_ints(BUF + st, {5});
    write_ints(BUF + sz, {20, 10});
    EXPECT_EQ(send(b, BCM_SETSPLITINFO, 0, BUF), 1u);
    write_ints(BUF, {0xC, 0, 0, 0, 0, 0, 0, 0});
    (void)send(b, BCM_GETSPLITINFO, 0, BUF);
    EXPECT_EQ(read_int(BUF + st), 5);
    EXPECT_EQ(read_int(BUF + sz), 20);
    EXPECT_EQ(send(b, BCM_SETDROPDOWNSTATE, 1, 0), 0u);
    EXPECT_EQ(send(b, BCM_SETSHIELD, 0, 1), 0u);
    // BM_SETIMAGE returns the previous handle of that type.
    EXPECT_EQ(send(b, BM_SETIMAGE, 0, 0x1234), 0u);
    EXPECT_EQ(send(b, BM_SETIMAGE, 0, 0x5678), 0x1234u);
    EXPECT_EQ(send(b, BM_GETIMAGE, 0, 0), 0x5678u);
    EXPECT_EQ(send(b, BM_GETIMAGE, 1, 0), 0u);
}

TEST_P(ButtonBcmTest, WithoutCommonControls6EveryBcmMessageAnswersZero) {
    msm.set_common_controls_v6(false);
    const HWND b = button(0x0, L"Install");
    write_ints(OUT, {0, 0});
    write_ints(BUF, {-7, -7, -7, -7});
    for (u32 msg = BCM_GETIDEALSIZE; msg <= BCM_SETSHIELD; ++msg) {
        EXPECT_EQ(send(b, msg, 0, msg == BCM_GETIDEALSIZE ? OUT : BUF), 0u) << std::hex << msg;
    }
    EXPECT_EQ(read_int(OUT), 0);   // nothing written (probe nov6 "ideal ... cx=0 cy=0")
    EXPECT_EQ(read_int(BUF), -7);  // nothing written (probe nov6 "margin default -7")
}

INSTANTIATE_TEST_SUITE_P(Bitness, ButtonBcmTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& i) {
                             return i.param ? std::string("x64") : std::string("x86");
                         });

}  // namespace
