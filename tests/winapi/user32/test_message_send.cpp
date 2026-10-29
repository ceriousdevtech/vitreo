/// @file test_message_send.cpp
/// @brief Tests for MessageSendManager (WIN32-055)

#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <vitreo/common.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <winapi/user32/messages/sdk_edit_readonly.hpp>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

#include "edit_readonly_oracle.inc"

class MessageSendManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        WindowData data;
        data.title = L"Test";
        data.wndproc = 0x401000;
        data.parent = HWND_DESKTOP;
        test_hwnd = hwnd_mgr.create(data);

        WindowData data2;
        data2.title = L"Test2";
        data2.wndproc = 0x402000;
        data2.parent = HWND_DESKTOP;
        test_hwnd2 = hwnd_mgr.create(data2);
    }

    HwndManager  hwnd_mgr;
    MessageQueue queue;
    CocoaBridge  bridge;
    HWND test_hwnd  = 0;
    HWND test_hwnd2 = 0;

    MessageSendManager make_msm() {
        return MessageSendManager(hwnd_mgr, queue, bridge);
    }
};

TEST_F(MessageSendManagerTest, ReadOnlyMessageIsSharedByEditAndRichEdit) {
    auto msm = make_msm();
    GuestMemory memory;
    cpu::CpuState state;
    const wchar_t* classes[] = {L"Edit", L"RICHEDIT50W", L"RichEdit20W", L"RichEdit20A", L"RichEdit"};
    for (bool wide : {false, true}) {
        if (wide) state.init_long_mode(); else state.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(state);
        HWND controls[5]{};
        for (usize i = 0; i < 5; ++i) {
            WindowData data{}; data.class_name = classes[i]; data.title = L"original";
            controls[i] = hwnd_mgr.create(data);
        }
        for (const auto& expected : read_only_oracle) {
            if (expected.wide != wide) continue;
            const HWND hwnd = controls[expected.class_index];
            SCOPED_TRACE(::testing::Message() << "wide=" << wide << " class=" << expected.class_index
                         << " input=" << expected.input);
            set_last_error(0x6abedef1);
            state.gpr.rax = 0;
            EXPECT_TRUE(msm.dispatch_marshaled_control(m, hwnd, sdk_edit_readonly::EM_SETREADONLY,
                                                       expected.input, 0x1234, false, true));
            EXPECT_EQ(state.gpr.rax, expected.result);
            const auto* data = hwnd_mgr.find(hwnd);
            ASSERT_NE(data, nullptr);
            EXPECT_EQ((data->style & sdk_edit_readonly::ES_READONLY) != 0, expected.read_only);
            EXPECT_EQ(data->title == L"original", expected.same_text);
            EXPECT_EQ(get_last_error(), expected.last_error);
        }
        for (HWND hwnd : controls) hwnd_mgr.destroy(hwnd);
    }
}

// ---------------------------------------------------------------------------
// PostQuitMessage
// ---------------------------------------------------------------------------

TEST_F(MessageSendManagerTest, PostQuitMessageSetsQuitPosted) {
    auto msm = make_msm();
    msm.post_quit_message(0);
    EXPECT_TRUE(queue.quit_posted());
}

TEST_F(MessageSendManagerTest, PostQuitMessagePreservesExitCode) {
    auto msm = make_msm();
    msm.post_quit_message(42);
    EXPECT_TRUE(queue.quit_posted());
    EXPECT_EQ(queue.quit_exit_code(), 42);
}

// ---------------------------------------------------------------------------
// PostMessageW
// ---------------------------------------------------------------------------

TEST_F(MessageSendManagerTest, PostMessageAddsToQueue) {
    auto msm = make_msm();
    EXPECT_TRUE(msm.post_message(test_hwnd, WM::PAINT, 0, 0));
    EXPECT_FALSE(queue.empty());

    MSG msg{};
    queue.get(msg);
    EXPECT_EQ(msg.hwnd, test_hwnd);
    EXPECT_EQ(msg.message, WM::PAINT);
}

TEST_F(MessageSendManagerTest, PostMessageNullHwndCreatesThreadMessage) {
    auto msm = make_msm();
    EXPECT_TRUE(msm.post_message(NULL_HWND, WM::TIMER, 1, 2));

    MSG msg{};
    queue.get(msg);
    EXPECT_EQ(msg.hwnd, NULL_HWND);
    EXPECT_EQ(msg.message, WM::TIMER);
}

TEST_F(MessageSendManagerTest, PostMessageInvalidHwndFails) {
    auto msm = make_msm();
    EXPECT_FALSE(msm.post_message(0xDEAD, WM::PAINT, 0, 0));
}

TEST_F(MessageSendManagerTest, PostMessagePreservesWParamAndLParam) {
    auto msm = make_msm();
    constexpr u64 w = 0x1234567890ABCDEFull;
    constexpr u64 l = 0xFEDCBA0987654321ull;
    EXPECT_TRUE(msm.post_message(test_hwnd, WM::COMMAND, w, l));

    MSG msg{};
    queue.get(msg);
    EXPECT_EQ(msg.wParam, w);
    EXPECT_EQ(msg.lParam, l);
}

TEST_F(MessageSendManagerTest, PostMessageBroadcastPostsToAllTopLevel) {
    auto msm = make_msm();
    EXPECT_TRUE(msm.post_message(HWND_BROADCAST, WM::CLOSE, 0, 0));

    // Should have posted to both test_hwnd and test_hwnd2
    EXPECT_EQ(queue.size(), 2u);

    MSG msg1{};
    queue.get(msg1);
    MSG msg2{};
    queue.get(msg2);

    // Both should be WM_CLOSE and addressed to the two test windows
    EXPECT_EQ(msg1.message, WM::CLOSE);
    EXPECT_EQ(msg2.message, WM::CLOSE);
    // The two messages must target different windows
    EXPECT_NE(msg1.hwnd, msg2.hwnd);
}

// ---------------------------------------------------------------------------
// SendMessageW
// ---------------------------------------------------------------------------

TEST_F(MessageSendManagerTest, SendMessageWithCallbackCallsIt) {
    auto msm = make_msm();

    bool called = false;
    HWND captured_hwnd = 0;
    u32  captured_msg  = 0;
    u64  captured_wp   = 0;
    u64  captured_lp   = 0;
    u64  captured_proc = 0;

    msm.set_wndproc_callback(
        [&](HWND h, u32 m, u64 wp, u64 lp, u64 proc) -> u64 {
            called = true;
            captured_hwnd = h;
            captured_msg  = m;
            captured_wp   = wp;
            captured_lp   = lp;
            captured_proc = proc;
            return 99;
        }
    );

    u64 result = msm.send_message(test_hwnd, WM::PAINT, 7, 8);

    EXPECT_TRUE(called);
    EXPECT_EQ(captured_hwnd, test_hwnd);
    EXPECT_EQ(captured_msg, WM::PAINT);
    EXPECT_EQ(captured_wp, 7u);
    EXPECT_EQ(captured_lp, 8u);
    EXPECT_EQ(captured_proc, 0x401000u);
    EXPECT_EQ(result, 99u);
}

TEST_F(MessageSendManagerTest, SendMessageReturnsCallbackResult) {
    auto msm = make_msm();
    msm.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 0xBEEF; }
    );
    EXPECT_EQ(msm.send_message(test_hwnd, WM::CLOSE, 0, 0), 0xBEEFu);
}

TEST_F(MessageSendManagerTest, SendMessageWithoutCallbackReturnsZero) {
    auto msm = make_msm();
    EXPECT_EQ(msm.send_message(test_hwnd, WM::CLOSE, 0, 0), 0u);
}

TEST_F(MessageSendManagerTest, ProgressMessagesTrackAndReturnControlState) {
    WindowData progress{};
    progress.parent = test_hwnd;
    progress.class_name = L"msctls_progress32";
    progress.wndproc = 0;
    const HWND progress_hwnd = hwnd_mgr.create(progress);
    auto msm = make_msm();

    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_SETRANGE32, 20, 80),
              100u << 16);
    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_SETPOS, 50, 0), 20u);
    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_DELTAPOS, 40, 0), 50u);
    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_GETPOS, 0, 0), 80u);
    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_SETSTEP, 15, 0), 10u);
    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_STEPIT, 0, 0), 80u);
    EXPECT_EQ(msm.send_message(progress_hwnd, PBM_GETPOS, 0, 0), 20u);
}

TEST_F(MessageSendManagerTest, ProgressMessagesDoNotStealRebarIds) {
    // Learn: PBM_* and RB_* share WM_USER+N values. A ReBarWindow32 must get
    // RB_INSERTBANDW / RB_GETBARHEIGHT, not PBM_SETMARQUEE / PBM_SETSTEP.
    WindowData rebar{};
    rebar.parent = test_hwnd;
    rebar.class_name = L"ReBarWindow32";
    rebar.wndproc = 0;
    const HWND rebar_hwnd = hwnd_mgr.create(rebar);
    auto msm = make_msm();

    EXPECT_EQ(msm.send_message(rebar_hwnd, RB_INSERTBANDW, static_cast<u64>(-1),
                               0x1000),
              1u);
    EXPECT_EQ(msm.send_message(rebar_hwnd, RB_GETBANDCOUNT, 0, 0), 1u);
    EXPECT_EQ(msm.send_message(rebar_hwnd, RB_GETROWCOUNT, 0, 0), 1u);
    EXPECT_EQ(msm.send_message(rebar_hwnd, RB_GETBARHEIGHT, 0, 0), 30u);
    EXPECT_EQ(msm.send_message(rebar_hwnd, RB_SETBARINFO, 0, 0x1000), 1u);
}

TEST_F(MessageSendManagerTest, ToolbarButtonSizeSurvivesForRebarLayout) {
    WindowData toolbar{};
    toolbar.parent = test_hwnd;
    toolbar.class_name = L"ToolbarWindow32";
    toolbar.wndproc = 0;
    const HWND tb = hwnd_mgr.create(toolbar);
    auto msm = make_msm();

    EXPECT_EQ(msm.send_message(tb, TB_SETBUTTONSIZE, 0, (22u << 16) | 24u), 1u);
    EXPECT_EQ(msm.send_message(tb, TB_GETBUTTONSIZE, 0, 0),
              (22u << 16) | 24u);
    EXPECT_EQ(msm.send_message(tb, TB_ADDBUTTONSW, 3, 0x1000), 1u);
    EXPECT_EQ(msm.send_message(tb, TB_SETBUTTONSIZE, 0, (15u << 16) | 16u), 1u);
    EXPECT_EQ(msm.send_message(tb, TB_GETBUTTONSIZE, 0, 0), (21u << 16) | 23u);
    // Auto-size padding must not silently resize fixed-size buttons.
    msm.send_message(tb, 0x457, 0, (20u << 16) | 20u);
    EXPECT_EQ(msm.send_message(tb, TB_GETBUTTONSIZE, 0, 0), (21u << 16) | 23u);

}

TEST_F(MessageSendManagerTest, EditLimitTextSetAndGet) {
    WindowData edit{};
    edit.parent = test_hwnd;
    edit.class_name = L"Edit";
    edit.wndproc = 0;
    const HWND edit_hwnd = hwnd_mgr.create(edit);
    auto msm = make_msm();

    EXPECT_EQ(msm.send_message(edit_hwnd, EM_GETLIMITTEXT, 0, 0), 32767u);
    EXPECT_EQ(msm.send_message(edit_hwnd, EM_SETLIMITTEXT, 24, 0), 0u);
    EXPECT_EQ(msm.send_message(edit_hwnd, EM_GETLIMITTEXT, 0, 0), 24u);
    EXPECT_EQ(msm.send_message(edit_hwnd, EM_LIMITTEXT, 0, 0), 0u);
    EXPECT_EQ(msm.send_message(edit_hwnd, EM_GETLIMITTEXT, 0, 0), 0x7FFFFFFEu);

    WindowData multi{};
    multi.parent = test_hwnd;
    multi.class_name = L"Edit";
    multi.style = 0x0004; // ES_MULTILINE
    multi.wndproc = 0;
    const HWND multi_hwnd = hwnd_mgr.create(multi);
    EXPECT_EQ(msm.send_message(multi_hwnd, EM_SETLIMITTEXT, 0, 0), 0u);
    EXPECT_EQ(msm.send_message(multi_hwnd, EM_GETLIMITTEXT, 0, 0), 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// SendMessageTimeoutW
// ---------------------------------------------------------------------------

TEST_F(MessageSendManagerTest, SendMessageTimeoutDelegatesToSendMessage) {
    auto msm = make_msm();
    msm.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 0xCAFE; }
    );
    MessageQueue::SendWaitStatus status{};
    u64 result = msm.send_message_timeout(test_hwnd, WM::PAINT, 0, 0, 0, 5000, status);
    EXPECT_EQ(status, MessageQueue::SendWaitStatus::Completed);
    EXPECT_EQ(result, 0xCAFEu);
}

// ---------------------------------------------------------------------------
// make_message_send_exports
// ---------------------------------------------------------------------------

TEST_F(MessageSendManagerTest, MakeExportsReturnsFourEntries) {
    auto msm = make_msm();
    DllExports exports = make_message_send_exports(msm);
    EXPECT_EQ(exports.entries.size(), 15u);
    EXPECT_EQ(exports.entries[0].name, "PostQuitMessage");
    EXPECT_EQ(exports.entries[1].name, "PostMessageW");
    EXPECT_EQ(exports.entries[2].name, "PostMessageA");
    EXPECT_EQ(exports.entries[3].name, "PostThreadMessageW");
    EXPECT_EQ(exports.entries[4].name, "PostThreadMessageA");
    EXPECT_EQ(exports.entries[5].name, "SendMessageW");
    EXPECT_EQ(exports.entries[6].name, "SendMessageA");
    EXPECT_EQ(exports.entries[7].name, "SendMessageTimeoutW");
    EXPECT_EQ(exports.entries[8].name, "SendMessageTimeoutA");
    EXPECT_EQ(exports.entries[9].name, "SendNotifyMessageW");
    EXPECT_EQ(exports.entries[10].name, "SendNotifyMessageA");
    EXPECT_EQ(exports.entries[11].name, "InSendMessage");
    EXPECT_EQ(exports.entries[12].name, "InSendMessageEx");
    EXPECT_EQ(exports.entries[13].name, "ReplyMessage");
    EXPECT_EQ(exports.entries[14].name, "GetInputState");
}

// Both PostThreadMessage entry points must exist and must pop their four
// arguments. An unregistered A form falls through to Vitreo's generic
// missing-export stub, which returns 0xFFFFFFFF *and pops nothing* — the
// caller then returns through a stack slot 16 bytes off. That is how
// InstallShield's setup engine reached rip=0x12 during shutdown.
TEST_F(MessageSendManagerTest, BothPostThreadMessageFormsPopFourArguments) {
    auto msm = make_msm();
    DllExports exports = make_message_send_exports(msm);

    const ExportEntry* ansi = nullptr;
    const ExportEntry* wide = nullptr;
    for (const auto& entry : exports.entries) {
        if (entry.name == "PostThreadMessageA") ansi = &entry;
        if (entry.name == "PostThreadMessageW") wide = &entry;
    }

    ASSERT_NE(ansi, nullptr);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(ansi->stdcall_bytes, 16u);
    EXPECT_EQ(wide->stdcall_bytes, 16u);
    EXPECT_NE(ansi->handler, nullptr);
    EXPECT_NE(wide->handler, nullptr);
}

// ---------------------------------------------------------------------------
// SendMessage to invalid HWND
// ---------------------------------------------------------------------------

TEST_F(MessageSendManagerTest, SendMessageInvalidHwndReturnsZero) {
    auto msm = make_msm();
    msm.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 123; }
    );
    EXPECT_EQ(msm.send_message(0xDEAD, WM::PAINT, 0, 0), 0u);
}

TEST_F(MessageSendManagerTest, ToolbarMarshalsGuestButtonsAndRebarHeight) {
    auto* parent = hwnd_mgr.find(test_hwnd);
    parent->rect = {0, 0, 400, 300};
    parent->client_rect = parent->rect;
    ASSERT_TRUE(bridge.create_window(test_hwnd, *parent).is_ok());
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x20000, MemoryProtection::ReadWrite).is_ok());
    for (bool wide : {false, true}) {
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory);
        m.attach_cpu(cpu);
        auto msm = make_msm();
        WindowData data;
        data.parent = test_hwnd;
        data.class_name = L"ToolbarWindow32";
        data.style = WS::CHILD;
        data.rect = {0, 0, 400, 24}; data.client_rect = data.rect;
        const HWND toolbar = hwnd_mgr.create(data);
        bridge.add_child_view(test_hwnd, toolbar, data, 0, data.class_name, L"");
        ASSERT_TRUE(m.write_u32(0x1000, 0xffffffffu).is_ok());
        ASSERT_TRUE(m.write_u32(0x1004, 123).is_ok());
        ASSERT_TRUE(m.write_u8(0x1008, 4).is_ok());
        ASSERT_TRUE(m.write_u8(0x1009, 0).is_ok());
        if (wide) {
            ASSERT_TRUE(m.write_u64(0x1010, 0x123456789ull).is_ok());
            ASSERT_TRUE(m.write_u64(0x1018, 0x20000).is_ok());
        } else {
            ASSERT_TRUE(m.write_u32(0x100c, 0x12345678).is_ok());
            ASSERT_TRUE(m.write_u32(0x1010, 0x20000).is_ok());
        }
        ASSERT_TRUE(m.write_wide_string(0x20000, "New").is_ok());
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, toolbar, 0x444, 1, 0x1000, false));
        EXPECT_EQ(cpu.gpr.rax, 1u);
        const auto buttons = bridge.toolbar_buttons(toolbar);
        ASSERT_EQ(buttons.size(), 1);
        EXPECT_EQ(buttons[0].command, 123u);
        EXPECT_EQ(buttons[0].data, wide ? 0x123456789ull : 0x12345678ull);
        EXPECT_EQ(buttons[0].text, L"New");
        data.class_name = L"ReBarWindow32";
        const HWND rebar = hwnd_mgr.create(data);
        bridge.add_child_view(test_hwnd, rebar, data, 0, data.class_name, L"");
        ASSERT_TRUE(m.write_u32(0x1000, wide ? 128 : 100).is_ok());
        ASSERT_TRUE(m.write_u32(0x1004, 0x30).is_ok());
        if (wide) ASSERT_TRUE(m.write_u64(0x1028, toolbar).is_ok());
        else ASSERT_TRUE(m.write_u32(0x1020, toolbar).is_ok());
        ASSERT_TRUE(m.write_u32(wide ? 0x1034 : 0x1028, 20).is_ok());
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, rebar, 0x40a, 0xffffffffu, 0x1000, false));
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(hwnd_mgr.find(rebar)->client_rect.bottom, 24);
        EXPECT_EQ(msm.send_message(rebar, RB_GETBARHEIGHT, 0, 0), 24u);
        i32 requested_width = 400, requested_height = 0;
        msm.adjust_common_control_size(rebar, requested_width, requested_height);
        EXPECT_EQ(requested_height, 24);
        hwnd_mgr.find(rebar)->style |= 4u; // CCS_NORESIZE
        requested_height = 7;
        msm.adjust_common_control_size(rebar, requested_width, requested_height);
        EXPECT_EQ(requested_height, 7);
        hwnd_mgr.find(rebar)->style &= ~4u;
        ASSERT_TRUE(msm.dispatch_rebar_control(m, rebar, 0x410, 0, 0));
        EXPECT_EQ(cpu.gpr.rax, 0u);
        ASSERT_TRUE(msm.dispatch_rebar_control(m, rebar, 0x410, 42, 0));
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xffffffffu);
        ASSERT_TRUE(m.write_u32(wide ? 0x1034 : 0x1028, 0).is_ok());
        ASSERT_TRUE(msm.dispatch_rebar_control(m, rebar, 0x41c, 0, 0x1000));
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(m.read_u32(wide ? 0x1034 : 0x1028).value(), 20u);
    }
}

TEST_F(MessageSendManagerTest, OwnerDrawListPreservesItemDataAndMarshalsDrawCallbacks) {
    auto* parent = hwnd_mgr.find(test_hwnd);
    parent->rect = {0, 0, 400, 300}; parent->client_rect = parent->rect;
    ASSERT_TRUE(bridge.create_window(test_hwnd, *parent).is_ok());
    GuestMemory memory;
    for (bool wide : {false, true}) {
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        HandleTable handles;
        GdiManager gdi(handles); gdi.set_guest_memory(&memory);
        gdi.set_hwnd_manager(&hwnd_mgr); gdi.set_cocoa_bridge(&bridge);
        auto msm = make_msm(); msm.set_gdi_manager(&gdi);
        WindowData data; data.parent = test_hwnd; data.class_name = L"ListBox";
        data.style = WS::CHILD | 0x10; data.ctrl_id = 7;
        data.rect = {0, 0, 120, 30}; data.client_rect = data.rect;
        const HWND list = hwnd_mgr.create(data);
        bridge.add_child_view(test_hwnd, list, data, 7, data.class_name, L"");
        const u64 item = wide ? 0x1234567800000001ull : 1;
        int measured = 0, drawn = 0, deleted = 0, colored = 0;
        msm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64 wp, u64 lp, u64) -> u64 {
            EXPECT_EQ(hwnd, test_hwnd);
            if (msg == 0x134) { // Learn WM_CTLCOLORLISTBOX: HDC, HWND, not control ID.
                ++colored;
                EXPECT_EQ(gdi.window_from_dc(wp), list);
                EXPECT_EQ(lp, list);
                return 0; // Leave the default background brush in effect.
            }
            EXPECT_EQ(wp, 7u);
            if (msg == 0x2c) {
                ++measured; EXPECT_EQ(memory.read_u32(lp).value(), 2u);
                const u64 value = wide ? memory.read_u64(lp + 24).value() : memory.read_u32(lp + 20).value();
                EXPECT_EQ(value, item); (void)memory.write_u32(lp + 16, 15);
            } else if (msg == 0x2b) {
                ++drawn;
                const u64 value = wide ? memory.read_u64(lp + 56).value() : memory.read_u32(lp + 44).value();
                EXPECT_EQ(value, item);
                EXPECT_EQ(memory.read_u32(lp + 8).value(), 0u);
                const auto dc = wide ? memory.read_u64(lp + 32).value() : memory.read_u32(lp + 24).value();
                EXPECT_EQ(gdi.window_from_dc(dc), list);
                EXPECT_EQ(memory.read_u32(lp + (wide ? 52 : 40)).value(), 15u);
            } else if (msg == 0x2d) {
                ++deleted;
                EXPECT_EQ(memory.read_u32(lp).value(), 2u);
                EXPECT_EQ(memory.read_u32(lp + 8).value(), 0u);
                EXPECT_EQ(wide ? memory.read_u64(lp + 16).value() : memory.read_u32(lp + 12).value(), list);
                EXPECT_EQ(wide ? memory.read_u64(lp + 24).value() : memory.read_u32(lp + 16).value(), item);
            }
            return 1;
        });
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x180, 0, item, false));
        EXPECT_EQ(cpu.gpr.rax, 0u); EXPECT_EQ(measured, 1); EXPECT_GT(drawn, 0);
        EXPECT_GT(colored, 0);
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x199, 0, 0, false));
        EXPECT_EQ(cpu.gpr.rax, item);
        const int before = drawn;
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0xb, 0, 0, false));
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x186, 0, 0, false));
        EXPECT_EQ(drawn, before);
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0xb, 1, 0, false));
        EXPECT_GT(drawn, before);
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x182, 0, 0, false));
        EXPECT_EQ(cpu.gpr.rax, 0u); EXPECT_EQ(deleted, 1);
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x180, 0, item, false));
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x184, 0, 0, false));
        EXPECT_EQ(deleted, 2);
        bridge.destroy_window(list); hwnd_mgr.destroy(list);
    }
}

// A cross-thread SendMessage is dispatched by the receiver's retrieval loop.
TEST_F(MessageSendManagerTest, CrossThreadSendRunsOnWindowOwner) {
    auto msm = make_msm();
    hwnd_mgr.find(test_hwnd)->owner_thread_id = 57;
    std::atomic<bool> done{false};
    std::atomic<u32> callback_tid{0};
    msm.set_wndproc_callback([&](HWND, u32, u64, u64, u64) -> u64 {
        callback_tid = ThreadManager::get_current_thread_id_static();
        return 0x123456789abcdef0ULL;
    });
    std::thread receiver([&] {
        ThreadManager::set_current_thread_id(57);
        while (!done) {
            MSG msg{};
            queue.peek_for_thread(msg, 57, hwnd_mgr);
            std::this_thread::yield();
        }
    });
    EXPECT_EQ(msm.send_message(test_hwnd, 0x0409u, 0, 0),
              0x123456789abcdef0ULL);
    done = true;
    receiver.join();
    EXPECT_EQ(callback_tid.load(), 57u);
}

TEST_F(MessageSendManagerTest, NativeHitTestRunsOnPrimaryGuestThread) {
    auto msm = make_msm();
    hwnd_mgr.find(test_hwnd)->owner_thread_id = 1;
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    const auto host_thread = std::this_thread::get_id();
    ThreadManager::set_current_thread_id(0); // AppKit's identity in run.cpp.
    std::atomic<bool> done{false};
    std::atomic<bool> ran_on_host{false};
    std::atomic<u32> callback_tid{0};
    msm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64, u64 lp, u64) -> u64 {
        ran_on_host = std::this_thread::get_id() == host_thread;
        callback_tid = ThreadManager::get_current_thread_id_static();
        EXPECT_EQ(hwnd, test_hwnd);
        EXPECT_EQ(msg, WM::NCHITTEST);
        EXPECT_EQ(lp, 0x00200010u);
        return 2; // HTCAPTION: preserve the actual guest hit-test result.
    });
    std::thread receiver([&] {
        ThreadManager::set_current_thread_id(1);
        while (!done) {
            MSG msg{};
            queue.peek_for_thread(msg, 1, hwnd_mgr);
            std::this_thread::yield();
        }
    });
    EXPECT_EQ(msm.send_message(test_hwnd, WM::NCHITTEST, 0, 0x00200010), 2u);
    done = true;
    receiver.join();
    ThreadManager::set_current_thread_id(previous_tid);
    EXPECT_EQ(callback_tid.load(), 1u);
    EXPECT_FALSE(ran_on_host.load());
}

TEST_F(MessageSendManagerTest, CrossThreadSendServicesReciprocalSentMessagesOnly) {
    auto msm = make_msm();
    const u32 sender = ThreadManager::get_current_thread_id_static();
    hwnd_mgr.find(test_hwnd)->owner_thread_id = sender;
    hwnd_mgr.find(test_hwnd2)->owner_thread_id = 57;
    MSG posted{};
    posted.hwnd = test_hwnd;
    posted.message = 0x0408;
    queue.post(posted);
    std::atomic<bool> done{false};
    std::atomic<u32> outer_tid{0}, inner_tid{0};
    msm.set_wndproc_callback([&](HWND hwnd, u32, u64, u64, u64) -> u64 {
        if (hwnd == test_hwnd2) {
            outer_tid = ThreadManager::get_current_thread_id_static();
            return msm.send_message(test_hwnd, 0x0409, 0, 0) + 1;
        }
        inner_tid = ThreadManager::get_current_thread_id_static();
        return 41;
    });
    std::thread receiver([&] {
        ThreadManager::set_current_thread_id(57);
        while (!done) {
            MSG msg{};
            // Sent messages are delivered even when the posted-message filter
            // excludes the sent message number and HWND.
            queue.peek_for_thread(msg, 57, hwnd_mgr, test_hwnd, 1, 2);
            std::this_thread::yield();
        }
    });
    EXPECT_EQ(msm.send_message(test_hwnd2, 0x0409, 0, 0), 42u);
    done = true;
    receiver.join();
    EXPECT_EQ(outer_tid.load(), 57u);
    EXPECT_EQ(inner_tid.load(), sender);
    EXPECT_EQ(queue.size(), 1u);
}

// SendMessageTimeoutW (Learn, nf-winuser-sendmessagetimeoutw): nonzero on
// success with the LRESULT in *lpdwResult (DWORD_PTR: 8 bytes on x64, 4 on
// x86); 0 + ERROR_TIMEOUT on time-out; 0 + ERROR_INVALID_WINDOW_HANDLE for a
// bad window. Failures store 0 in *lpdwResult (user32_windows_probe.expected).
namespace {
void setup_smto_call(cpu::CpuState& cpu, Marshaler& m, bool wide, u64 hwnd,
                     u32 msg, u32 flags, u32 timeout, GuestAddr result_ptr) {
    if (wide) {
        cpu.init_long_mode();
        cpu.gpr.rsp = 0x8000;
        cpu.gpr.rcx = hwnd; cpu.gpr.rdx = msg; cpu.gpr.r8 = 0; cpu.gpr.r9 = 0;
        ASSERT_TRUE(m.write_u64(0x8028, 0xAAAAAAAA00000000ull | flags).is_ok());
        ASSERT_TRUE(m.write_u64(0x8030, 0xBBBBBBBB00000000ull | timeout).is_ok());
        ASSERT_TRUE(m.write_u64(0x8038, result_ptr).is_ok());
    } else {
        cpu.init_protected_mode();
        cpu.set_sp(0x8000);
        const u32 args[7] = {static_cast<u32>(hwnd), msg, 0, 0, flags, timeout,
                             static_cast<u32>(result_ptr)};
        for (u32 i = 0; i < 7; ++i) ASSERT_TRUE(m.write_u32(0x8004 + 4 * i, args[i]).is_ok());
    }
}
} // namespace

TEST_F(MessageSendManagerTest, SendMessageTimeoutWritesLresultAndReturnsTrue) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    hwnd_mgr.find(test_hwnd)->owner_thread_id = ThreadManager::get_current_thread_id_static();
    for (bool wide : {false, true}) {
        cpu::CpuState cpu;
        Marshaler m(memory); m.attach_cpu(cpu);
        auto msm = make_msm();
        msm.set_wndproc_callback([](HWND, u32, u64, u64, u64) -> u64 { return 0; });
        ASSERT_TRUE(m.write_u64(0x2000, 0xCCCCCCCCCCCCCCCCull).is_ok());
        setup_smto_call(cpu, m, wide, test_hwnd, 0x0000u, 0, 1000, 0x2000);
        impl_SendMessageTimeoutW(m, msm);
        // WM_NULL answering 0 is SUCCESS, not "hung".
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(memory.read_u64(0x2000).value(),
                  wide ? 0u : 0xCCCCCCCC00000000ull);
        msm.set_wndproc_callback([](HWND, u32, u64, u64, u64) -> u64 { return 0xFFFFFFFF12345678ull; });
        setup_smto_call(cpu, m, wide, test_hwnd, 0x0400, 0, 1000, 0x2000);
        impl_SendMessageTimeoutW(m, msm);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(memory.read_u64(0x2000).value(),
                  wide ? 0xFFFFFFFF12345678ull : 0xCCCCCCCC12345678ull);
    }
}

TEST_F(MessageSendManagerTest, SendMessageTimeoutInvalidWindowFails) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    for (u64 bad : {u64{0}, u64{0xDEAD}}) {
        cpu::CpuState cpu;
        Marshaler m(memory); m.attach_cpu(cpu);
        auto msm = make_msm();
        ASSERT_TRUE(m.write_u64(0x2000, 0x5555ull).is_ok());
        setup_smto_call(cpu, m, true, bad, 0x10, 0, 10, 0x2000);
        set_last_error(0xDEAD);
        impl_SendMessageTimeoutW(m, msm);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), 1400u);  // ERROR_INVALID_WINDOW_HANDLE
        EXPECT_EQ(memory.read_u64(0x2000).value(), 0u);
    }
}

// SendMessage(NULL) must fail — never rewrite to an arbitrary top-level
// (08c M8; user32_windows_probe smto_null_hwnd).
TEST_F(MessageSendManagerTest, SendMessageNullHwndFails) {
    auto msm = make_msm();
    set_last_error(0xDEAD);
    EXPECT_EQ(msm.send_message(NULL_HWND, /*WM_NULL*/0, 1, 2), 0u);
    EXPECT_EQ(get_last_error(), 1400u);
}

TEST_F(MessageSendManagerTest, SendMessageTimeoutCrossThreadTimesOut) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    hwnd_mgr.find(test_hwnd2)->owner_thread_id = 0x7777;  // never pumps
    cpu::CpuState cpu;
    Marshaler m(memory); m.attach_cpu(cpu);
    auto msm = make_msm();
    ASSERT_TRUE(msm.is_cross_thread_wndproc(test_hwnd2))
        << "owner=" << hwnd_mgr.find(test_hwnd2)->owner_thread_id
        << " self=" << ThreadManager::get_current_thread_id_static()
        << " hwnd=" << test_hwnd2;
    ASSERT_TRUE(m.write_u64(0x2000, 0x5555ull).is_ok());
    setup_smto_call(cpu, m, true, test_hwnd2, 0x0400, /*SMTO_BLOCK*/1, 40, 0x2000);
    ASSERT_EQ(static_cast<HWND>(m.arg(0)), test_hwnd2);
    ASSERT_EQ(m.arg32(4), 1u);
    ASSERT_EQ(m.arg32(5), 40u);
    set_last_error(0xDEAD);
    const auto t0 = std::chrono::steady_clock::now();
    impl_SendMessageTimeoutW(m, msm);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 1460u);  // ERROR_TIMEOUT
    EXPECT_EQ(memory.read_u64(0x2000).value(), 0u);
    // Timed-out send stays queued for a later receiver pump — do not
    // dispatch_sent_messages(owner) on the sender thread: that would run the
    // callback where guest tid != owner and re-enter SendMessage forever.
}

// HWND_BROADCAST (Learn): deliver to every top-level window; success is
// nonzero even when individual windows return 0 (user32_windows_probe
// smto_broadcast).
TEST_F(MessageSendManagerTest, SendMessageTimeoutBroadcastHitsAllTopLevel) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    hwnd_mgr.find(test_hwnd)->owner_thread_id =
        ThreadManager::get_current_thread_id_static();
    hwnd_mgr.find(test_hwnd2)->owner_thread_id =
        ThreadManager::get_current_thread_id_static();
    std::vector<HWND> hit;
    auto msm = make_msm();
    msm.set_wndproc_callback([&](HWND h, u32 msg, u64, u64, u64) -> u64 {
        if (msg == 0x0000u) hit.push_back(h);  // WM_NULL
        return 0;
    });
    cpu::CpuState cpu;
    Marshaler m(memory); m.attach_cpu(cpu);
    set_last_error(0xDEAD);
    setup_smto_call(cpu, m, true, 0xFFFFull, 0x0000u, /*SMTO_ABORTIFHUNG*/2, 200, 0);
    impl_SendMessageTimeoutW(m, msm);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves last-error
    ASSERT_EQ(hit.size(), 2u);
    EXPECT_NE(hit[0], hit[1]);
}

TEST_F(MessageSendManagerTest, IndependentControlTablesSurviveConcurrentMessages) {
    // Constants generated from the installed SDK commctrl.h/winuser.h.
    constexpr u32 kTTM_SETMAXTIPWIDTH = 0x418;
    constexpr u32 kTTM_GETMAXTIPWIDTH = 0x419;
    constexpr unsigned workers = 8;
    constexpr unsigned controls = 128;
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide ? "x64" : "x86");
        auto msm = make_msm();
        std::barrier start(workers);
        std::atomic<unsigned> failures{0};
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                GuestMemory memory;
                cpu::CpuState cpu;
                if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                Marshaler m(memory);
                m.attach_cpu(cpu);
                const auto create = [&](const wchar_t* name) {
                    WindowData data;
                    data.class_name = name;
                    data.parent = test_hwnd;
                    if (data.class_name == L"Button") data.style = 0x2; // SDK BS_CHECKBOX
                    return hwnd_mgr.create(data);
                };
                const auto check = [&](bool success) {
                    if (!success) ++failures;
                };
                start.arrive_and_wait();
                for (unsigned i = 0; i < controls; ++i) {
                    const HWND progress = create(L"msctls_progress32");
                    const HWND button = create(L"Button");
                    const HWND toolbar = create(L"ToolbarWindow32");
                    const HWND tooltip = create(L"tooltips_class32");
                    const HWND rebar = create(L"ReBarWindow32");
                    const u32 position = (worker * controls + i) % 100;
                    check(msm.send_message(progress, PBM_SETPOS, position, 0) == 0);
                    check(msm.send_message(progress, PBM_GETPOS, 0, 0) == position);
                    check(msm.send_message(button, BM_SETCHECK, BST_CHECKED, 0) == 0);
                    check(msm.send_message(button, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    const u32 width_limit = worker * controls + i + 1;
                    const u32 size = (30u << 16) | (32u + worker);
                    check(msm.send_message(toolbar, TB_SETBUTTONSIZE, 0, size) == 1);
                    check(msm.send_message(toolbar, TB_GETBUTTONSIZE, 0, 0) == size);
                    check(msm.dispatch_tooltip_control(m, tooltip, kTTM_SETMAXTIPWIDTH, 0, width_limit));
                    check(cpu.gpr.rax == (wide ? ~0ull : 0xffffffffu));
                    check(msm.dispatch_tooltip_control(m, tooltip, kTTM_GETMAXTIPWIDTH, 0, 0));
                    check(cpu.gpr.rax == width_limit);
                    check(msm.send_message(rebar, RB_GETBANDCOUNT, 0, 0) == 0);
                    i32 width = 80, height = 60;
                    msm.adjust_common_control_size(rebar, width, height);
                    check(msm.dispatch_tooltip_control(m, tooltip, kTTM_SETMAXTIPWIDTH, 0, ~0ull));
                    check(cpu.gpr.rax == width_limit);
                    check(msm.dispatch_tooltip_control(m, tooltip, kTTM_GETMAXTIPWIDTH, 0, 0));
                    check(cpu.gpr.rax == (wide ? ~0ull : 0xffffffffu));
                }
            });
        }
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(failures.load(), 0u);
    }
}

// LB_FINDSTRING / LB_FINDSTRINGEXACT / LB_SELECTSTRING and WM_GETDLGCODE,
// replaying tools/oracle_probe/listbox_find_probe{32,64}.expected (identical).
// VCL's list box incremental search sends LB_FINDSTRING(-1, "\t") on Tab; a
// 0 there reset Inno's check list to its first item on every Tab.
TEST_F(MessageSendManagerTest, OwnerDrawListFindStringMatchesWindows) {
    auto* parent = hwnd_mgr.find(test_hwnd);
    parent->rect = {0, 0, 400, 300}; parent->client_rect = parent->rect;
    ASSERT_TRUE(bridge.create_window(test_hwnd, *parent).is_ok());
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x5000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    for (bool wide : {false, true}) {
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        HandleTable handles;
        GdiManager gdi(handles); gdi.set_guest_memory(&memory);
        gdi.set_hwnd_manager(&hwnd_mgr); gdi.set_cocoa_bridge(&bridge);
        auto msm = make_msm(); msm.set_gdi_manager(&gdi);
        msm.set_wndproc_callback([&](HWND, u32 msg, u64, u64 lp, u64) -> u64 {
            if (msg == 0x2c) (void)memory.write_u32(lp + 16, 16);
            return 1;
        });
        auto make = [&](u32 style) {
            WindowData data; data.parent = test_hwnd; data.class_name = L"ListBox";
            data.style = WS::CHILD | style; data.ctrl_id = 7;
            data.rect = {0, 0, 200, 200}; data.client_rect = data.rect;
            const HWND h = hwnd_mgr.create(data);
            bridge.add_child_view(test_hwnd, h, data, 7, data.class_name, L"");
            return h;
        };
        auto send = [&](HWND h, u32 msg, u64 wp, u64 lp) {
            EXPECT_TRUE(msm.dispatch_marshaled_control(m, h, msg, wp, lp, false)) << msg;
            return static_cast<i32>(cpu.gpr.rax);
        };
        auto str = [&](const char* text) {
            EXPECT_TRUE(m.write_wide_string(0x5000, text).is_ok());
            return u64{0x5000};
        };
        const u64 minus1 = wide ? ~0ull : 0xFFFFFFFFull;
        const u64 minus2 = wide ? ~1ull : 0xFFFFFFFEull;
        const HWND l = make(0x20 | 0x40 | 0x01);
        EXPECT_EQ(send(l, 0x87, 0, 0), 0x81);  // dlgcode
        for (const char* t : {"Apple", "banana", "Apricot", "ap"}) send(l, 0x180, 0, str(t));
        send(l, 0x186, 1, 0);
        EXPECT_EQ(send(l, 0x18f, minus1, str("ap")), 0);
        EXPECT_EQ(send(l, 0x18f, 0, str("ap")), 2);
        EXPECT_EQ(send(l, 0x18f, 2, str("ap")), 3);
        EXPECT_EQ(send(l, 0x18f, 3, str("ap")), 0);
        EXPECT_EQ(send(l, 0x18f, 4, str("ap")), 0);
        EXPECT_EQ(send(l, 0x18f, 100, str("ap")), 0);
        EXPECT_EQ(send(l, 0x18f, minus2, str("ap")), 0);
        EXPECT_EQ(send(l, 0x18f, minus1, str("BAN")), 1);
        EXPECT_EQ(send(l, 0x18f, minus1, str("\t")), -1);
        EXPECT_EQ(send(l, 0x18f, minus1, str("")), -1);
        EXPECT_EQ(send(l, 0x18f, 1, str("")), -1);
        EXPECT_EQ(send(l, 0x1a2, minus1, str("AP")), 3);
        EXPECT_EQ(send(l, 0x1a2, minus1, str("appl")), -1);
        EXPECT_EQ(send(l, 0x1a2, 0, str("apple")), 0);
        EXPECT_EQ(send(l, 0x188, 0, 0), 1);
        EXPECT_EQ(send(l, 0x18c, minus1, str("zz")), -1);
        EXPECT_EQ(send(l, 0x188, 0, 0), 1);
        EXPECT_EQ(send(l, 0x18c, minus1, str("apr")), 2);
        EXPECT_EQ(send(l, 0x188, 0, 0), 2);
        EXPECT_EQ(send(l, 0x18c, minus1, str("\t")), -1);
        EXPECT_EQ(send(l, 0x188, 0, 0), 2);
        bridge.destroy_window(l); hwnd_mgr.destroy(l);

        const HWND d = make(0x20);
        EXPECT_EQ(send(d, 0x87, 0, 0), 0x81);
        for (u64 v : {0x1111ull, 0x2222ull, 0x1111ull}) send(d, 0x180, 0, v);
        EXPECT_EQ(send(d, 0x18f, minus1, 0x1111), 0);
        EXPECT_EQ(send(d, 0x18f, 0, 0x1111), 2);
        EXPECT_EQ(send(d, 0x18f, minus1, 0x3333), -1);
        EXPECT_EQ(send(d, 0x1a2, minus1, 0x2222), 1);
        EXPECT_EQ(send(d, 0x188, 0, 0), -1);
        EXPECT_EQ(send(d, 0x18c, minus1, 0x2222), 1);
        EXPECT_EQ(send(d, 0x188, 0, 0), 1);
        bridge.destroy_window(d); hwnd_mgr.destroy(d);

        const HWND e = make(0x20 | 0x40);
        EXPECT_EQ(send(e, 0x18f, minus1, str("a")), -1);
        EXPECT_EQ(send(e, 0x18c, minus1, str("a")), -1);
        EXPECT_EQ(send(e, 0x188, 0, 0), -1);
        bridge.destroy_window(e); hwnd_mgr.destroy(e);
    }
}

// DRAWITEMSTRUCT.itemState (Learn): ODS_FOCUS marks the caret item while the
// list has the keyboard focus (item 0 before any selection), ODS_DISABLED
// every item of a disabled list; WM_SETFOCUS / WM_KILLFOCUS repaint and send
// LBN_SETFOCUS / LBN_KILLFOCUS whatever LBS_NOTIFY says.
TEST_F(MessageSendManagerTest, OwnerDrawListItemStateTracksFocusAndDisabled) {
    auto* parent = hwnd_mgr.find(test_hwnd);
    parent->rect = {0, 0, 400, 300}; parent->client_rect = parent->rect;
    ASSERT_TRUE(bridge.create_window(test_hwnd, *parent).is_ok());
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x5000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    for (bool wide : {false, true}) {
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        HandleTable handles;
        GdiManager gdi(handles); gdi.set_guest_memory(&memory);
        gdi.set_hwnd_manager(&hwnd_mgr); gdi.set_cocoa_bridge(&bridge);
        auto msm = make_msm(); msm.set_gdi_manager(&gdi);
        WindowData data; data.parent = test_hwnd; data.class_name = L"ListBox";
        data.style = WS::CHILD | 0x10 | 0x40;  // OWNERDRAWFIXED | HASSTRINGS, no LBS_NOTIFY
        data.ctrl_id = 9;
        data.rect = {0, 0, 120, 60}; data.client_rect = data.rect;
        const HWND list = hwnd_mgr.create(data);
        bridge.add_child_view(test_hwnd, list, data, 7, data.class_name, L"");
        std::vector<std::pair<u32, u32>> drawn;  // itemID, itemState
        std::vector<u64> commands;
        msm.set_wndproc_callback([&](HWND, u32 msg, u64 wp, u64 lp, u64) -> u64 {
            if (msg == 0x2c) {
                (void)memory.write_u32(lp + 16, 16);
            } else if (msg == 0x2b) {
                drawn.emplace_back(memory.read_u32(lp + 8).value_or(99),
                                   memory.read_u32(lp + 16).value_or(99));
            } else if (msg == 0x111) {
                commands.push_back(wp);
            }
            return 1;
        });
        for (u32 i = 0; i < 2; ++i) {
            ASSERT_TRUE(m.write_wide_string(0x5000, "row").is_ok());
            ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x180, 0, 0x5000, false));
        }
        auto states = [&]() {
            std::vector<u32> out;
            for (const auto& [id, st] : drawn) out.push_back(st);
            return out;
        };
        bridge.set_keyboard_focus_hwnd(list);
        drawn.clear(); commands.clear();
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x7, 0, 0, false));  // WM_SETFOCUS
        EXPECT_EQ(states(), (std::vector<u32>{0x10, 0x0}));  // caret = item 0
        ASSERT_EQ(commands.size(), 1u);
        EXPECT_EQ(commands[0], (4ull << 16) | 9u);           // LBN_SETFOCUS
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x186, 1, 0, false));  // LB_SETCURSEL
        drawn.clear();
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0xf, 0, 0, false));
        EXPECT_EQ(states(), (std::vector<u32>{0x0, 0x11}));  // selected + focus
        bridge.set_keyboard_focus_hwnd(NULL_HWND);
        drawn.clear(); commands.clear();
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x8, 0, 0, false));  // WM_KILLFOCUS
        EXPECT_EQ(states(), (std::vector<u32>{0x0, 0x1}));
        ASSERT_EQ(commands.size(), 1u);
        EXPECT_EQ(commands[0], (5ull << 16) | 9u);           // LBN_KILLFOCUS
        hwnd_mgr.find(list)->style |= WS::DISABLED;
        drawn.clear();
        ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0xf, 0, 0, false));
        EXPECT_EQ(states(), (std::vector<u32>{0x4, 0x5}));
        bridge.destroy_window(list); hwnd_mgr.destroy(list);
    }
}

// LBS_OWNERDRAWVARIABLE (Learn): the owner gets WM_MEASUREITEM for every item
// (itemID = its index) and the rows stack at those heights; LB_GETITEMRECT,
// LB_GETITEMHEIGHT, LB_ITEMFROMPOINT and click / arrow-key selection follow
// them. A VCL TCheckListBox-style list (Inno Setup's task list) draws its
// checkboxes in these rows.
TEST_F(MessageSendManagerTest, OwnerDrawVariableListMeasuresEachItemAndSelects) {
    auto* parent = hwnd_mgr.find(test_hwnd);
    parent->rect = {0, 0, 400, 300}; parent->client_rect = parent->rect;
    ASSERT_TRUE(bridge.create_window(test_hwnd, *parent).is_ok());
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x5000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    for (bool wide : {false, true}) {
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        HandleTable handles;
        GdiManager gdi(handles); gdi.set_guest_memory(&memory);
        gdi.set_hwnd_manager(&hwnd_mgr); gdi.set_cocoa_bridge(&bridge);
        auto msm = make_msm(); msm.set_gdi_manager(&gdi);
        WindowData data; data.parent = test_hwnd; data.class_name = L"ListBox";
        data.style = WS::CHILD | 0x20 | 0x40 | 0x01;  // VARIABLE | HASSTRINGS | NOTIFY
        data.ctrl_id = 9;
        data.rect = {0, 0, 120, 50}; data.client_rect = data.rect;
        const HWND list = hwnd_mgr.create(data);
        bridge.add_child_view(test_hwnd, list, data, 7, data.class_name, L"");
        const u32 heights[] = {10, 20, 30};
        std::vector<u32> measured_ids;
        // Oracle (listbox_measure_order_probe): the new item is already in the
        // list when the parent gets WM_MEASUREITEM.
        std::vector<u32> counts_seen, lengths_seen;
        std::vector<u64> commands;
        msm.set_wndproc_callback([&](HWND, u32 msg, u64 wp, u64 lp, u64) -> u64 {
            if (msg == 0x2c) {  // WM_MEASUREITEM
                const u32 id = memory.read_u32(lp + 8).value_or(99);
                measured_ids.push_back(id);
                EXPECT_TRUE(msm.dispatch_marshaled_control(m, list, 0x18b, 0, 0, false));
                counts_seen.push_back(static_cast<u32>(cpu.gpr.rax));   // LB_GETCOUNT
                EXPECT_TRUE(msm.dispatch_marshaled_control(m, list, 0x18a, id, 0, false));
                lengths_seen.push_back(static_cast<u32>(cpu.gpr.rax));  // LB_GETTEXTLEN
                (void)memory.write_u32(lp + 16, heights[std::min<u32>(id, 2)]);
            } else if (msg == 0x111) {
                commands.push_back(wp);
            }
            return 1;
        });
        const char* texts[] = {"a", "bb", "ccc"};
        for (u32 i = 0; i < 3; ++i) {
            ASSERT_TRUE(m.write_wide_string(0x5000, texts[i]).is_ok());
            ASSERT_TRUE(msm.dispatch_marshaled_control(m, list, 0x180, 0, 0x5000, false));
            EXPECT_EQ(cpu.gpr.rax & 0xFFFFFFFF, i);
        }
        EXPECT_EQ(measured_ids, (std::vector<u32>{0, 1, 2}));
        EXPECT_EQ(counts_seen, (std::vector<u32>{1, 2, 3}));
        EXPECT_EQ(lengths_seen, (std::vector<u32>{1, 2, 3}));
        auto send = [&](u32 msg, u64 wp, u64 lp) {
            EXPECT_TRUE(msm.dispatch_marshaled_control(m, list, msg, wp, lp, false)) << msg;
            return static_cast<u32>(cpu.gpr.rax);
        };
        EXPECT_EQ(send(0x1a1, 1, 0), 20u);                       // LB_GETITEMHEIGHT
        EXPECT_EQ(send(0x198, 2, 0x5100), 0u);                    // LB_GETITEMRECT
        EXPECT_EQ(memory.read_u32(0x5104).value_or(0), 30u);
        EXPECT_EQ(memory.read_u32(0x510c).value_or(0), 60u);
        EXPECT_EQ(send(0x1a9, 0, (25u << 16) | 5u), 1u);         // inside item 1
        EXPECT_EQ(send(0x1a9, 0, (45u << 16) | 5u), 2u);         // inside item 2 (30..60)
        EXPECT_EQ(send(0x1a9, 0, (200u << 16) | 5u), 0x10002u);  // below: nearest, outside
        EXPECT_EQ(send(0x1a0, 0, 300), 0xFFFFFFFFu);              // > 255: LB_ERR
        EXPECT_EQ(send(0x1a0, 0, 12), 0u);
        EXPECT_EQ(send(0x1a1, 0, 0), 12u);
        commands.clear();
        send(0x201, 1, (40u << 16) | 5u);                         // click item 2 (32..62)
        EXPECT_EQ(send(0x188, 0, 0), 2u);                         // LB_GETCURSEL
        ASSERT_EQ(commands.size(), 1u);
        EXPECT_EQ(commands[0], (1ull << 16) | 9u);                // LBN_SELCHANGE
        send(0x100, 0x26, 0);                                     // VK_UP
        EXPECT_EQ(send(0x188, 0, 0), 1u);
        EXPECT_EQ(commands.size(), 2u);
        send(0x203, 1, (5u << 16) | 5u);                          // double-click item 0
        EXPECT_EQ(send(0x188, 0, 0), 0u);
        EXPECT_EQ(commands.back(), (2ull << 16) | 9u);            // LBN_DBLCLK
        bridge.destroy_window(list); hwnd_mgr.destroy(list);
    }
}

// EDITSTREAM is packed at four bytes by the Windows SDK, including on x64.
#include <winapi/user32/messages/sdk_richedit_stream.hpp>
TEST_F(MessageSendManagerTest, StreamInUsesPackedSdkCallbackAndDiscardsErrorBuffer) {
    for (bool wide : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 4096, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState state;
        if (wide) state.init_long_mode(); else state.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(state);
        auto msm = make_msm();
        const u64 cookie = wide ? 0x1234567887654321ull : 0x87654321ull;
        const u64 callback = wide ? 0x123456788000ull : 0x12345678ull;
        usize error_offset;
        if (wide) {
            sdk_richedit_stream::EDITSTREAM<u64> stream{cookie, 0xabcdef01, callback};
            ASSERT_TRUE(memory.write_bytes(0x3000, reinterpret_cast<const u8*>(&stream), sizeof(stream)).is_ok());
            error_offset = offsetof(decltype(stream), dwError);
        } else {
            sdk_richedit_stream::EDITSTREAM<u32> stream{static_cast<u32>(cookie), 0xabcdef01, static_cast<u32>(callback)};
            ASSERT_TRUE(memory.write_bytes(0x3000, reinterpret_cast<const u8*>(&stream), sizeof(stream)).is_ok());
            error_offset = offsetof(decltype(stream), dwError);
        }
        GuestAddr scratch = 0;
        int calls = 0;
        msm.set_guest_call([&](GuestAddr proc, const u64* args, usize count, bool is64)->u64 {
            EXPECT_EQ(proc, callback); EXPECT_EQ(args[0], cookie);
            EXPECT_EQ(count, 4u); EXPECT_EQ(is64, wide); ++calls;
            scratch = args[1];
            EXPECT_TRUE(memory.write_u32(args[3], 3).is_ok());
            const u8 data[] = {'b','a','d'};
            EXPECT_TRUE(memory.write_bytes(args[1], data, sizeof(data)).is_ok());
            return 0x1234;
        });
        EXPECT_TRUE(msm.dispatch_marshaled_control(m, test_hwnd, 0x449, 1, 0x3000, false, true));
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(memory.read_u32(0x3000 + error_offset).value(), 0x1234u);
        EXPECT_EQ(state.gpr.rax, 0u);
        EXPECT_NE(scratch, 0u);
        if (scratch) EXPECT_FALSE(memory.read_u8(scratch).is_ok());
    }
}

TEST_F(MessageSendManagerTest, StreamInNestedCallsOwnBuffersAndClearErrors) {
    for (bool wide : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 4096, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState state;
        if (wide) state.init_long_mode(); else state.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(state);
        auto msm = make_msm();
        const usize error_offset = wide ? offsetof(sdk_richedit_stream::EDITSTREAM<u64>, dwError)
                                        : offsetof(sdk_richedit_stream::EDITSTREAM<u32>, dwError);
        for (u32 index=0; index<2; ++index) {
            if(wide) {
                sdk_richedit_stream::EDITSTREAM<u64> stream{index, 0xffffffff, 0x10000};
                ASSERT_TRUE(memory.write_bytes(0x3000+index*64,reinterpret_cast<const u8*>(&stream),sizeof(stream)).is_ok());
            } else {
                sdk_richedit_stream::EDITSTREAM<u32> stream{index, 0xffffffff, 0x10000};
                ASSERT_TRUE(memory.write_bytes(0x3000+index*64,reinterpret_cast<const u8*>(&stream),sizeof(stream)).is_ok());
            }
        }
        GuestAddr outer=0, inner=0;
        msm.set_guest_call([&](GuestAddr, const u64* args, usize, bool)->u64 {
            if(args[0]==0) {
                outer=args[1];
                EXPECT_NE(outer, 0u);
                EXPECT_TRUE(memory.write_u8(outer, 0xa5).is_ok());
                EXPECT_TRUE(msm.dispatch_marshaled_control(m,test_hwnd,0x449,1,0x3040,false,true));
                EXPECT_EQ(memory.read_u8(outer).value(),0xa5);
                EXPECT_FALSE(memory.read_u8(inner).is_ok());
            } else { inner=args[1]; EXPECT_NE(inner,outer); }
            EXPECT_TRUE(memory.write_u32(args[3],0).is_ok());
            // The x64 callback returns DWORD; upper RAX is not part of it.
            return wide ? 0x1234567800000000ull : 0;
        });
        EXPECT_TRUE(msm.dispatch_marshaled_control(m,test_hwnd,0x449,1,0x3000,false,true));
        EXPECT_EQ(memory.read_u32(0x3000+error_offset).value(),0u);
        EXPECT_EQ(memory.read_u32(0x3040+error_offset).value(),0u);
        EXPECT_FALSE(memory.read_u8(outer).is_ok());
    }
}

#include <winapi/user32/windows/window.hpp>
TEST_F(MessageSendManagerTest, StreamInTextMatchesWindowsResultAndOutput) {
    WindowClassManager classes;
    WindowClassData cls{}; cls.class_name=L"StreamParent"; cls.wndproc=0x1000;
    classes.register_class(cls);
    WindowManager wm(hwnd_mgr,classes,bridge,queue);
    wm.set_wndproc_callback([](HWND,u32 msg,u64,u64,u64)->u64 { return msg==WM::NCCREATE ? 1 : 0; });
    const HWND parent=wm.create_window(0,L"StreamParent",L"",WS::OVERLAPPEDWINDOW,0,0,200,100,0,0,0,0);
    ASSERT_NE(parent,0u);
    const HWND edit=wm.create_window(0,L"Edit",L"",WS::CHILD|0x0004,0,0,160,60,parent,0,0,0);
    ASSERT_NE(edit,0u);
    for(bool wide : {false,true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000,4096,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState state;
        if(wide) state.init_long_mode(); else state.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(state);
        if(wide) {
            sdk_richedit_stream::EDITSTREAM<u64> stream{0x87654321,0xabcdef01,0x10000};
            ASSERT_TRUE(memory.write_bytes(0x3000,reinterpret_cast<const u8*>(&stream),sizeof(stream)).is_ok());
        } else {
            sdk_richedit_stream::EDITSTREAM<u32> stream{0x87654321,0xabcdef01,0x10000};
            ASSERT_TRUE(memory.write_bytes(0x3000,reinterpret_cast<const u8*>(&stream),sizeof(stream)).is_ok());
        }
        auto msm=make_msm(); int calls=0;
        msm.set_guest_call([&](GuestAddr,const u64* args,usize,bool)->u64 {
            if(calls++==0) {
                const u8 bytes[]={'a','b','c'};
                EXPECT_TRUE(memory.write_bytes(args[1],bytes,sizeof(bytes)).is_ok());
                EXPECT_TRUE(memory.write_u32(args[3],sizeof(bytes)).is_ok());
            } else EXPECT_TRUE(memory.write_u32(args[3],0).is_ok());
            return 0;
        });
        EXPECT_TRUE(msm.dispatch_marshaled_control(m,edit,0x449,1,0x3000,false,true));
        EXPECT_EQ(state.gpr.rax,3u); EXPECT_EQ(calls,2);
        const auto offset=wide ? offsetof(sdk_richedit_stream::EDITSTREAM<u64>,dwError)
                               : offsetof(sdk_richedit_stream::EDITSTREAM<u32>,dwError);
        EXPECT_EQ(memory.read_u32(0x3000+offset).value(),0u);
        EXPECT_EQ(bridge.get_control_text(edit),L"abc");
    }
    EXPECT_TRUE(wm.destroy_window(parent));
}
