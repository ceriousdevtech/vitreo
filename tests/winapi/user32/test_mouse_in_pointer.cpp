/// @file test_mouse_in_pointer.cpp
/// @brief EnableMouseInPointer / WM_POINTER* conversion / GetPointerInfo.
///
/// Every expected value was measured on Windows (Windows, x86 and x64) with
/// tools/oracle_probe/mouse_in_pointer_probe.c; see its .expected files.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/messages/message_loop.hpp>
#include <winapi/user32/messages/mouse_in_pointer.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/thread/thread.hpp>

#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;
namespace mip = vitreo::winapi::mouse_in_pointer;

namespace {

constexpr u32 kTid = 7;
constexpr HWND kHwnd = 0x1234;

MSG hw_mouse(u32 message, u64 w_param, i32 x, i32 y, u32 time = 1000) {
    MSG msg{};
    msg.hwnd = kHwnd;
    msg.message = message;
    msg.wParam = w_param;
    msg.lParam = 0x00050005;  // client coordinates; replaced by screen
    msg.pt = {x, y};
    msg.time = time;
    msg.hardware_input = true;
    return msg;
}

u32 read32(const mip::PointerInfoImage& image, usize off) {
    u32 v = 0;
    std::memcpy(&v, image.bytes.data() + off, 4);
    return v;
}

u64 read64(const mip::PointerInfoImage& image, usize off) {
    u64 v = 0;
    std::memcpy(&v, image.bytes.data() + off, 8);
    return v;
}

class MouseInPointerTest : public ::testing::Test {
protected:
    void SetUp() override { mip::reset_for_testing(); }
    void TearDown() override { mip::reset_for_testing(); }

    mip::PointerInfo convert_ok(MSG& msg, u64 qpc = 5555) {
        EXPECT_TRUE(mip::convert(msg, kTid, qpc));
        mip::PointerInfo info{};
        EXPECT_EQ(mip::pointer_info(kTid, 1, info), 0u);
        return info;
    }
};

}  // namespace

TEST_F(MouseInPointerTest, EnableStateMachineMatchesWindows) {
    EXPECT_FALSE(mip::enabled());
    auto r = mip::enable(5);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_FALSE(mip::enabled());

    EXPECT_TRUE(mip::enable(1).ok);
    EXPECT_TRUE(mip::enabled());
    EXPECT_TRUE(mip::enable(1).ok);
    r = mip::enable(0);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, error_codes::ERROR_ACCESS_DENIED);
    EXPECT_TRUE(mip::enabled());
}

TEST_F(MouseInPointerTest, FirstDisableLocksDisabled) {
    EXPECT_TRUE(mip::enable(0).ok);
    EXPECT_TRUE(mip::enable(0).ok);
    const auto r = mip::enable(1);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, error_codes::ERROR_ACCESS_DENIED);
    EXPECT_FALSE(mip::enabled());
}

TEST_F(MouseInPointerTest, QueriesBeforeAnyPointerMessage) {
    mip::PointerInfo info{};
    EXPECT_EQ(mip::pointer_info(kTid, 1, info), error_codes::ERROR_INVALID_PARAMETER);
    u32 type = 0;
    EXPECT_EQ(mip::pointer_type(1, type), 0u);
    EXPECT_EQ(type, mip::PT_MOUSE);
}

TEST_F(MouseInPointerTest, DisabledLeavesMouseMessagesAlone) {
    MSG msg = hw_mouse(WM::MOUSEMOVE, 0, 309, 313);
    EXPECT_FALSE(mip::convert(msg, kTid, 1));
    EXPECT_EQ(msg.message, WM::MOUSEMOVE);
}

TEST_F(MouseInPointerTest, PostedMouseMessagesAreNotHardwareInput) {
    ASSERT_TRUE(mip::enable(1).ok);
    MSG msg = hw_mouse(WM::MOUSEMOVE, 0, 309, 313);
    msg.hardware_input = false;
    EXPECT_FALSE(mip::convert(msg, kTid, 1));
    EXPECT_EQ(msg.message, WM::MOUSEMOVE);
}

TEST_F(MouseInPointerTest, MeasuredMessageStream) {
    ASSERT_TRUE(mip::enable(1).ok);

    MSG move = hw_mouse(WM::MOUSEMOVE, 0, 309, 313, 4242);
    auto info = convert_ok(move, 9876);
    EXPECT_EQ(move.message, mip::WM_POINTERUPDATE);
    EXPECT_EQ(move.wParam, 0x20020001u);
    EXPECT_EQ(move.lParam, (313u << 16) | 309u);
    EXPECT_EQ(info.pointer_type, mip::PT_MOUSE);
    EXPECT_EQ(info.pointer_id, 1u);
    EXPECT_EQ(info.pointer_flags, 0x22002u);
    EXPECT_NE(info.source_device, 0u);
    EXPECT_EQ(info.hwnd_target, kHwnd);
    EXPECT_EQ(info.pixel_x, 309);
    EXPECT_EQ(info.pixel_y, 313);
    EXPECT_EQ(info.himetric_x, 8176);
    EXPECT_EQ(info.himetric_y, 8281);
    EXPECT_EQ(info.time, 4242u);
    EXPECT_EQ(info.history_count, 1u);
    EXPECT_EQ(info.key_states, 0u);
    EXPECT_EQ(info.performance_count, 9876u);
    EXPECT_EQ(info.button_change_type, 0u);
    const u32 first_frame = info.frame_id;

    MSG ldown = hw_mouse(WM::LBUTTONDOWN, 0x1, 305, 313);
    info = convert_ok(ldown);
    EXPECT_EQ(ldown.message, mip::WM_POINTERDOWN);
    EXPECT_EQ(ldown.wParam, 0x20160001u);
    EXPECT_EQ(info.pointer_flags, 0x12016u);
    EXPECT_EQ(info.key_states, 0x1u);
    EXPECT_EQ(info.button_change_type, 1u);
    EXPECT_EQ(info.himetric_x, 8070);
    EXPECT_EQ(info.frame_id, first_frame + 1);

    MSG drag = hw_mouse(WM::MOUSEMOVE, 0x1, 306, 313);
    info = convert_ok(drag);
    EXPECT_EQ(drag.message, mip::WM_POINTERUPDATE);
    EXPECT_EQ(drag.wParam, 0x20160001u);
    EXPECT_EQ(info.pointer_flags, 0x22016u);

    MSG lup = hw_mouse(WM::LBUTTONUP, 0x0, 306, 313);
    info = convert_ok(lup);
    EXPECT_EQ(lup.message, mip::WM_POINTERUP);
    EXPECT_EQ(lup.wParam, 0x20020001u);
    EXPECT_EQ(info.pointer_flags, 0x42002u);
    EXPECT_EQ(info.button_change_type, 2u);

    MSG rdown = hw_mouse(WM::RBUTTONDOWN, 0x2, 306, 313);
    info = convert_ok(rdown);
    EXPECT_EQ(rdown.message, mip::WM_POINTERDOWN);
    EXPECT_EQ(rdown.wParam, 0x20260001u);
    EXPECT_EQ(info.pointer_flags, 0x12026u);
    EXPECT_EQ(info.key_states, 0x2u);
    EXPECT_EQ(info.button_change_type, 3u);

    MSG rup = hw_mouse(WM::RBUTTONUP, 0x0, 306, 313);
    info = convert_ok(rup);
    EXPECT_EQ(rup.message, mip::WM_POINTERUP);
    EXPECT_EQ(info.button_change_type, 4u);

    MSG wheel = hw_mouse(WM::MOUSEWHEEL, 0x00780000, 306, 313);
    info = convert_ok(wheel);
    EXPECT_EQ(wheel.message, mip::WM_POINTERWHEEL);
    EXPECT_EQ(wheel.wParam, 0x00780001u);
    EXPECT_EQ(info.pointer_flags, 0x82002u);
    EXPECT_EQ(info.input_data, 120);

    MSG hwheel = hw_mouse(WM::MOUSEHWHEEL, 0xff880000, 306, 313);
    info = convert_ok(hwheel);
    EXPECT_EQ(hwheel.message, mip::WM_POINTERHWHEEL);
    EXPECT_EQ(hwheel.wParam, 0xff880001u);
    EXPECT_EQ(info.pointer_flags, 0x102002u);
    EXPECT_EQ(info.input_data, -120);

    EXPECT_EQ(mip::pointer_info(kTid, 0x7777, info), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(mip::pointer_info(kTid + 1, 1, info), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(MouseInPointerTest, ChordedButtonsAreUpdatesAndChangeTypesFollowTheEnum) {
    ASSERT_TRUE(mip::enable(1).ok);
    MSG ldown = hw_mouse(WM::LBUTTONDOWN, 0x1, 10, 10);
    convert_ok(ldown);
    MSG mdown = hw_mouse(WM::MBUTTONDOWN, 0x11, 10, 10);
    auto info = convert_ok(mdown);
    EXPECT_EQ(mdown.message, mip::WM_POINTERUPDATE);
    EXPECT_EQ(info.pointer_flags, 0x22056u);
    EXPECT_EQ(info.button_change_type, 5u);
    MSG x2down = hw_mouse(0x020B, (2ull << 16) | 0x51, 10, 10);
    info = convert_ok(x2down);
    EXPECT_EQ(info.button_change_type, 9u);
    EXPECT_EQ(info.key_states, 0x51u);
    EXPECT_EQ(info.pointer_flags & 0x1F0u, 0x150u);
    MSG lup = hw_mouse(WM::LBUTTONUP, 0x50, 10, 10);
    info = convert_ok(lup);
    EXPECT_EQ(lup.message, mip::WM_POINTERUPDATE);
    EXPECT_EQ(info.button_change_type, 2u);
}

TEST_F(MouseInPointerTest, DefWindowProcPromotionAndPrimaryDrop) {
    ASSERT_TRUE(mip::enable(1).ok);

    MSG move = hw_mouse(WM::MOUSEMOVE, 0, 20, 30);
    const MSG move_legacy = move;
    convert_ok(move);
    auto legacy = mip::promote(kTid, kHwnd, move.message, move.wParam, move.lParam);
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->message, WM::MOUSEMOVE);
    EXPECT_EQ(legacy->wParam, move_legacy.wParam);
    EXPECT_EQ(legacy->lParam, move_legacy.lParam);
    EXPECT_TRUE(legacy->pointer_promoted);
    // Only once, and a promoted message is never converted back.
    EXPECT_FALSE(mip::promote(kTid, kHwnd, move.message, move.wParam, move.lParam));
    MSG again = *legacy;
    EXPECT_FALSE(mip::convert(again, kTid, 1));

    MSG ldown = hw_mouse(WM::LBUTTONDOWN, 0x1, 20, 30);
    convert_ok(ldown);
    legacy = mip::promote(kTid, kHwnd, ldown.message, ldown.wParam, ldown.lParam);
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->message, WM::LBUTTONDOWN);
    EXPECT_EQ(legacy->wParam, 0x1u);

    MSG drag = hw_mouse(WM::MOUSEMOVE, 0x1, 21, 30);
    auto info = convert_ok(drag);
    EXPECT_EQ(drag.wParam, 0x00160001u);
    EXPECT_EQ(info.pointer_flags, 0x20016u);

    MSG lup = hw_mouse(WM::LBUTTONUP, 0x0, 21, 30);
    info = convert_ok(lup);
    EXPECT_EQ(lup.wParam, 0x00020001u);
    EXPECT_EQ(info.pointer_flags, 0x40002u);

    MSG ldown2 = hw_mouse(WM::LBUTTONDOWN, 0x1, 21, 30);
    info = convert_ok(ldown2);
    EXPECT_EQ(ldown2.wParam, 0x20160001u);
    EXPECT_EQ(info.pointer_flags, 0x12016u);

    // A pointer message that does not match the thread's record is not promoted.
    EXPECT_FALSE(mip::promote(kTid, kHwnd, mip::WM_POINTERUP, 0x20020001, 0));
}

TEST_F(MouseInPointerTest, PointerInfoLayoutMatchesMeasuredOffsets) {
    mip::PointerInfo info{};
    info.pointer_type = 4;
    info.pointer_id = 1;
    info.frame_id = 0x11;
    info.pointer_flags = 0x22002;
    info.source_device = 0x1234567890ull;
    info.hwnd_target = 0xABCD;
    info.pixel_x = 309;
    info.pixel_y = 313;
    info.himetric_x = 8176;
    info.himetric_y = 8281;
    info.time = 0x5151;
    info.history_count = 1;
    info.input_data = -120;
    info.key_states = 3;
    info.performance_count = 0x1122334455667788ull;
    info.button_change_type = 4;

    const auto x64 = mip::pointer_info_image(info, true);
    EXPECT_EQ(x64.size, 96u);
    EXPECT_EQ(read32(x64, 0), 4u);
    EXPECT_EQ(read32(x64, 4), 1u);
    EXPECT_EQ(read32(x64, 8), 0x11u);
    EXPECT_EQ(read32(x64, 12), 0x22002u);
    EXPECT_EQ(read64(x64, 16), 0x1234567890ull);
    EXPECT_EQ(read64(x64, 24), 0xABCDu);
    EXPECT_EQ(read32(x64, 32), 309u);
    EXPECT_EQ(read32(x64, 36), 313u);
    EXPECT_EQ(read32(x64, 40), 8176u);
    EXPECT_EQ(read32(x64, 44), 8281u);
    EXPECT_EQ(read32(x64, 48), 309u);
    EXPECT_EQ(read32(x64, 56), 8176u);
    EXPECT_EQ(read32(x64, 64), 0x5151u);
    EXPECT_EQ(read32(x64, 68), 1u);
    EXPECT_EQ(read32(x64, 72), static_cast<u32>(-120));
    EXPECT_EQ(read32(x64, 76), 3u);
    EXPECT_EQ(read64(x64, 80), 0x1122334455667788ull);
    EXPECT_EQ(read32(x64, 88), 4u);

    const auto x86 = mip::pointer_info_image(info, false);
    EXPECT_EQ(x86.size, 88u);
    EXPECT_EQ(read32(x86, 12), 0x22002u);
    EXPECT_EQ(read32(x86, 16), 0x34567890u);
    EXPECT_EQ(read32(x86, 20), 0xABCDu);
    EXPECT_EQ(read32(x86, 24), 309u);
    EXPECT_EQ(read32(x86, 32), 8176u);
    EXPECT_EQ(read32(x86, 40), 309u);
    EXPECT_EQ(read32(x86, 48), 8176u);
    EXPECT_EQ(read32(x86, 56), 0x5151u);
    EXPECT_EQ(read32(x86, 60), 1u);
    EXPECT_EQ(read32(x86, 64), static_cast<u32>(-120));
    EXPECT_EQ(read32(x86, 68), 3u);
    EXPECT_EQ(read64(x86, 72), 0x1122334455667788ull);
    EXPECT_EQ(read32(x86, 80), 4u);
}

TEST_F(MouseInPointerTest, PeekMessageDeliversPointerForHardwareMouseAndPromotedFirst) {
    ASSERT_TRUE(mip::enable(1).ok);
    HwndManager hwnd_mgr;
    MessageQueue queue;
    CocoaBridge bridge;
    WindowData data;
    data.title = L"Test";
    data.wndproc = 0x401000;
    data.parent = HWND_DESKTOP;
    const HWND hwnd = hwnd_mgr.create(data);
    MessageLoopManager mlm(hwnd_mgr, queue, bridge);

    MSG down = hw_mouse(WM::LBUTTONDOWN, 0x1, 40, 50);
    down.hwnd = hwnd;
    queue.post_input(down);
    MSG up = hw_mouse(WM::LBUTTONUP, 0x0, 40, 50);
    up.hwnd = hwnd;
    queue.post_input(up);

    MSG out{};
    ASSERT_TRUE(mlm.peek_message(out, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(out.message, mip::WM_POINTERDOWN);
    EXPECT_EQ(out.wParam, 0x20160001u);

    const u32 tid = ThreadManager::get_current_thread_id_static();
    auto legacy = mip::promote(tid, hwnd, out.message, out.wParam, out.lParam);
    ASSERT_TRUE(legacy.has_value());
    queue.post_input_front(*legacy);

    ASSERT_TRUE(mlm.peek_message(out, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(out.message, WM::LBUTTONDOWN);
    EXPECT_EQ(out.wParam, 0x1u);

    ASSERT_TRUE(mlm.peek_message(out, 0, 0, 0, PM::REMOVE));
    EXPECT_EQ(out.message, mip::WM_POINTERUP);
    EXPECT_EQ(out.wParam, 0x00020001u);
}
