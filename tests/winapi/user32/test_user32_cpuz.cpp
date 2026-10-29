/// @file test_user32_cpuz.cpp
/// @brief Unit tests for user32_ext stubs exercised by CPU-Z (32-bit stdcall)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/dialogs/dialog.hpp>
#include <winapi/user32/messages/message_loop.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <windowing/model/window_types.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <windowing/messages/message_queue.hpp>
#include <winapi/user32/classes/window_class.hpp>

#include <array>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture — 32-bit stdcall calling convention
// =============================================================================

class User32CpuzTest : public ::testing::Test {
protected:
    void SetUp() override {
        ThreadManager::set_current_thread_id(1);
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
    }

    /// Push a 32-bit stdcall argument at the given index (arg0 -> rsp+4, etc.)
    void push_arg(usize index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4), value);
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    capsule::Vfs vfs;
    windowing::HwndManager hwnd_mgr;
    WindowClassManager class_mgr;
    windowing::CocoaBridge bridge;
    windowing::MessageQueue queue;
    User32ExtManager mgr{
        memory, vfs, hwnd_mgr, class_mgr, bridge, queue,
        [] { return std::string{"Test Apple GPU"}; }};
};

// =============================================================================
// GetDoubleClickTime — 0 args, returns 500
// =============================================================================

TEST_F(User32CpuzTest, GetDoubleClickTime_Returns500) {
    mgr.impl_GetDoubleClickTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 500u);
}

TEST_F(User32CpuzTest, EnumDisplayDevicesAReportsHostMetalAdapter) {
    constexpr GuestAddr display_device = DATA_BASE + 0x200;
    ASSERT_TRUE(memory.write_u32(display_device, 424).is_ok());
    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(display_device));
    push_arg(3, 0);

    mgr.impl_EnumDisplayDevicesA(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    std::array<char, 128> description{};
    ASSERT_TRUE(memory.read_bytes(
        display_device + 36,
        reinterpret_cast<Byte*>(description.data()),
        description.size()).is_ok());
    EXPECT_EQ(std::string_view(description.data()), "Test Apple GPU");
    EXPECT_EQ(memory.read_u32(display_device + 164).value(), 0x5u);
    std::array<char, 128> instance_id{};
    ASSERT_TRUE(memory.read_bytes(
        display_device + 168,
        reinterpret_cast<Byte*>(instance_id.data()),
        instance_id.size()).is_ok());
    EXPECT_EQ(
        std::string_view(instance_id.data()), "ROOT\\VITREO_METAL\\0000");
    std::array<char, 128> device_key{};
    ASSERT_TRUE(memory.read_bytes(
        display_device + 296,
        reinterpret_cast<Byte*>(device_key.data()),
        device_key.size()).is_ok());
    EXPECT_EQ(
        std::string_view(device_key.data()),
        "\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\"
        "ROOT\\VITREO_METAL\\0000");
}

TEST_F(User32CpuzTest, TabGetCurFocusReturnsNativeSelection) {
    windowing::WindowData parent{};
    parent.handle = 20;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(20, parent).is_ok());

    windowing::WindowData tabs{};
    tabs.handle = 21;
    tabs.parent = 20;
    tabs.rect = {3, 3, 397, 347};
    tabs.client_rect = tabs.rect;
    bridge.add_child_view(
        20, 21, tabs, 0, L"SysTabControl32", L"");
    ASSERT_EQ(bridge.tab_insert_item(21, 0, L"CPU"), 0);
    ASSERT_EQ(bridge.tab_insert_item(21, 1, L"Mainboard"), 1);
    ASSERT_EQ(bridge.tab_set_cur_sel(21, 1), 0);

    MessageSendManager message_send{hwnd_mgr, queue, bridge};
    push_arg(0, 21);
    push_arg(1, 0x132F); // TCM_GETCURFOCUS
    push_arg(2, 0);
    push_arg(3, 0);

    impl_SendMessageW(marshaler, message_send);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

TEST_F(User32CpuzTest, TabSetImageListAndRowCountContracts) {
    // Learn: TCM_SETIMAGELIST returns the previous HIMAGELIST; TCM_GETROWCOUNT
    // reports rows (1 for single-line tabs with items).
    windowing::WindowData parent{};
    parent.handle = 40;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(40, parent).is_ok());

    windowing::WindowData tabs{};
    tabs.handle = 41;
    tabs.parent = 40;
    tabs.rect = {3, 3, 397, 347};
    tabs.client_rect = tabs.rect;
    bridge.add_child_view(40, 41, tabs, 0, L"SysTabControl32", L"");

    MessageSendManager message_send{hwnd_mgr, queue, bridge};

    push_arg(0, 41);
    push_arg(1, 0x1303); // TCM_SETIMAGELIST
    push_arg(2, 0);
    push_arg(3, 0xABCD0001ull);
    impl_SendMessageW(marshaler, message_send);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    push_arg(0, 41);
    push_arg(1, 0x1302); // TCM_GETIMAGELIST
    push_arg(2, 0);
    push_arg(3, 0);
    impl_SendMessageW(marshaler, message_send);
    EXPECT_EQ(cpu.gpr.rax, 0xABCD0001ull);

    push_arg(0, 41);
    push_arg(1, 0x132C); // TCM_GETROWCOUNT
    push_arg(2, 0);
    push_arg(3, 0);
    impl_SendMessageW(marshaler, message_send);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_EQ(bridge.tab_insert_item(41, 0, L"Doc1"), 0);
    push_arg(0, 41);
    push_arg(1, 0x132C);
    push_arg(2, 0);
    push_arg(3, 0);
    impl_SendMessageW(marshaler, message_send);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    push_arg(0, 41);
    push_arg(1, 0x1334); // TCM_SETEXTENDEDSTYLE
    push_arg(2, 0);
    push_arg(3, 0x1);
    impl_SendMessageW(marshaler, message_send);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    constexpr GuestAddr tcitem = DATA_BASE + 0x800;
    constexpr GuestAddr text_buf = DATA_BASE + 0x880;
    ASSERT_TRUE(memory.write_u32(tcitem + 0, 0x0001).is_ok()); // TCIF_TEXT
    ASSERT_TRUE(memory.write_u32(tcitem + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(tcitem + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(tcitem + 12,
                                 static_cast<u32>(text_buf)).is_ok());
    ASSERT_TRUE(memory.write_u32(tcitem + 16, 0).is_ok());
    // "new"
    ASSERT_TRUE(memory.write_u16(text_buf + 0, u'n').is_ok());
    ASSERT_TRUE(memory.write_u16(text_buf + 2, u'e').is_ok());
    ASSERT_TRUE(memory.write_u16(text_buf + 4, u'w').is_ok());
    ASSERT_TRUE(memory.write_u16(text_buf + 6, 0).is_ok());
    push_arg(0, 41);
    push_arg(1, 0x133D); // TCM_SETITEMW
    push_arg(2, 0);
    push_arg(3, tcitem);
    impl_SendMessageW(marshaler, message_send);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    std::wstring renamed;
    ASSERT_TRUE(bridge.tab_get_item_text(41, 0, renamed));
    EXPECT_EQ(renamed, L"new");
}

TEST_F(User32CpuzTest, TabGetItemWFillsTcitemText) {
    // Learn: TCM_GETITEMW returns TRUE and fills TCITEM.pszText when TCIF_TEXT.
    windowing::WindowData parent{};
    parent.handle = 30;
    parent.rect = {0, 0, 400, 360};
    parent.client_rect = parent.rect;
    ASSERT_TRUE(bridge.create_window(30, parent).is_ok());

    windowing::WindowData tabs{};
    tabs.handle = 31;
    tabs.parent = 30;
    tabs.rect = {3, 3, 397, 347};
    tabs.client_rect = tabs.rect;
    bridge.add_child_view(30, 31, tabs, 0, L"SysTabControl32", L"");
    ASSERT_EQ(bridge.tab_insert_item(31, 0, L"CPU"), 0);
    ASSERT_EQ(bridge.tab_insert_item(31, 1, L"Mainboard"), 1);

    constexpr GuestAddr tcitem = DATA_BASE + 0x700;
    constexpr GuestAddr text_buf = DATA_BASE + 0x780;
    ASSERT_TRUE(memory.write_u32(tcitem + 0, 0x0001).is_ok()); // TCIF_TEXT
    ASSERT_TRUE(memory.write_u32(tcitem + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(tcitem + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(tcitem + 12,
                                 static_cast<u32>(text_buf)).is_ok());
    ASSERT_TRUE(memory.write_u32(tcitem + 16, 64).is_ok()); // cchTextMax
    ASSERT_TRUE(memory.write_u16(text_buf, 0xFFFF).is_ok());

    MessageSendManager message_send{hwnd_mgr, queue, bridge};
    push_arg(0, 31);
    push_arg(1, 0x133C); // TCM_GETITEMW
    push_arg(2, 1);      // index
    push_arg(3, tcitem);

    impl_SendMessageW(marshaler, message_send);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u16(text_buf + 0).value(), static_cast<u16>(L'M'));
    EXPECT_EQ(memory.read_u16(text_buf + 2).value(), static_cast<u16>(L'a'));
    EXPECT_EQ(memory.read_u16(text_buf + 18).value(), 0u); // null after "Mainboard"
}

TEST_F(User32CpuzTest, DialogBoxIndirectParamAUsesInMemoryTemplateAndReturnsEndDialogResult) {
    constexpr GuestAddr dialog_template = 0x10000;
    constexpr u64 dialog_proc = 0x401234;
    constexpr u32 init_param = 0xA5A5;

    // Minimal classic DLGTEMPLATE with no controls and the title "RCT3".
    ASSERT_TRUE(memory.write_u32(
        dialog_template + 0, 0x80C80000u).is_ok()); // popup/caption/sysmenu
    ASSERT_TRUE(memory.write_u32(dialog_template + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 8, 0).is_ok());  // cdit
    ASSERT_TRUE(memory.write_u16(dialog_template + 10, 0).is_ok()); // x
    ASSERT_TRUE(memory.write_u16(dialog_template + 12, 0).is_ok()); // y
    ASSERT_TRUE(memory.write_u16(dialog_template + 14, 120).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 16, 80).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 18, 0).is_ok()); // menu
    ASSERT_TRUE(memory.write_u16(dialog_template + 20, 0).is_ok()); // class
    ASSERT_TRUE(memory.write_u16(dialog_template + 22, u'R').is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 24, u'C').is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 26, u'T').is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 28, u'3').is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 30, 0).is_ok());

    DialogManager dialog_mgr{hwnd_mgr, class_mgr, bridge, queue, memory};
    mgr.set_dialog_manager(&dialog_mgr);
    bool saw_init_dialog = false;
    windowing::HWND dialog_hwnd = 0;
    std::vector<u32> destruction_messages;
    dialog_mgr.set_dlgproc_callback(
        [&](windowing::HWND hwnd, u32 message, u64, u64 lparam, u64 proc) {
            if (message == DialogManager::WM_INITDIALOG) {
                saw_init_dialog = true;
                dialog_hwnd = hwnd;
                EXPECT_EQ(lparam, init_param);
                EXPECT_EQ(proc, dialog_proc);
                EXPECT_FALSE(bridge.native_window_is_visible(hwnd));
                const windowing::WindowData* win = hwnd_mgr.find(hwnd);
                EXPECT_NE(win, nullptr);
                if (win) {
                    EXPECT_FALSE(win->visible);
                }
                EXPECT_TRUE(dialog_mgr.end_dialog(hwnd, 73));
                EXPECT_TRUE(hwnd_mgr.is_valid(hwnd)); // deferred until callback returns
            }
            if (message == windowing::WM::DESTROY || message == windowing::WM::NCDESTROY) {
                EXPECT_TRUE(hwnd_mgr.is_valid(hwnd));
                destruction_messages.push_back(message);
            }
            return 1u;
        });

    push_arg(0, 0);
    push_arg(1, static_cast<u32>(dialog_template));
    push_arg(2, 0);
    push_arg(3, static_cast<u32>(dialog_proc));
    push_arg(4, init_param);

    auto exports = create_user32_ext_thunks(mgr);
    const ExportEntry* entry = nullptr;
    for (const auto& candidate : exports.entries) {
        if (candidate.name == "DialogBoxIndirectParamA") {
            entry = &candidate;
            break;
        }
    }
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 20u);
    entry->handler(marshaler);

    EXPECT_FALSE(hwnd_mgr.is_valid(dialog_hwnd));
    EXPECT_EQ(destruction_messages, (std::vector<u32>{windowing::WM::DESTROY, windowing::WM::NCDESTROY}));
    EXPECT_TRUE(saw_init_dialog);
    EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 73);
}

TEST_F(User32CpuzTest, CreateDialogSendsInitDialogBeforeFirstShow) {
    // Learn CreateDialogParam: WM_INITDIALOG is sent before the dialog is
    // displayed. WS_VISIBLE then shows a fully created window (children +
    // INITDIALOG SetWindowPos already applied).
    constexpr GuestAddr dialog_template = 0x10000;
    constexpr u64 dialog_proc = 0x401234;

    ASSERT_TRUE(memory.write_u32(
        dialog_template + 0, 0x90C80000u).is_ok()); // popup/visible/caption/sysmenu
    ASSERT_TRUE(memory.write_u32(dialog_template + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 10, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 14, 120).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 16, 80).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 18, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 22, 0).is_ok());

    DialogManager dialog_mgr{hwnd_mgr, class_mgr, bridge, queue, memory};
    bool saw_init_dialog = false;
    windowing::HWND created = windowing::NULL_HWND;
    dialog_mgr.set_dlgproc_callback(
        [&](windowing::HWND hwnd, u32 message, u64, u64, u64) {
            if (message == DialogManager::WM_INITDIALOG) {
                saw_init_dialog = true;
                created = hwnd;
                EXPECT_FALSE(bridge.native_window_is_visible(hwnd));
                const windowing::WindowData* win = hwnd_mgr.find(hwnd);
                EXPECT_NE(win, nullptr);
                if (win) {
                    EXPECT_FALSE(win->visible);
                }
            }
            return 1u;
        });

    // An in-memory DLGTEMPLATE is CreateDialogIndirectParam's input
    // (Learn: CreateDialogParam's lpTemplateName is a resource name).
    windowing::HWND hwnd = dialog_mgr.create_dialog(
        0, dialog_template, windowing::NULL_HWND, dialog_proc, 0,
        /*post_init=*/false, /*require_template=*/true);
    EXPECT_NE(hwnd, windowing::NULL_HWND);
    EXPECT_TRUE(saw_init_dialog);
    EXPECT_EQ(hwnd, created);
    EXPECT_TRUE(bridge.native_window_is_visible(hwnd));
    const windowing::WindowData* win = hwnd_mgr.find(hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_TRUE(win->visible);
    EXPECT_TRUE((win->style & windowing::WS::VISIBLE) != 0);
    bridge.destroy_window(hwnd);
}

TEST_F(User32CpuzTest, CreateDialogWithoutVisibleStaysHidden) {
    constexpr GuestAddr dialog_template = 0x10000;
    constexpr u64 dialog_proc = 0x401234;

    ASSERT_TRUE(memory.write_u32(
        dialog_template + 0, 0x80C80000u).is_ok()); // popup/caption/sysmenu, no WS_VISIBLE
    ASSERT_TRUE(memory.write_u32(dialog_template + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 10, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 14, 120).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 16, 80).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 18, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u16(dialog_template + 22, 0).is_ok());

    DialogManager dialog_mgr{hwnd_mgr, class_mgr, bridge, queue, memory};
    dialog_mgr.set_dlgproc_callback(
        [](windowing::HWND, u32, u64, u64, u64) { return 1u; });

    // An in-memory DLGTEMPLATE is CreateDialogIndirectParam's input
    // (Learn: CreateDialogParam's lpTemplateName is a resource name).
    windowing::HWND hwnd = dialog_mgr.create_dialog(
        0, dialog_template, windowing::NULL_HWND, dialog_proc, 0,
        /*post_init=*/false, /*require_template=*/true);
    EXPECT_NE(hwnd, windowing::NULL_HWND);
    EXPECT_FALSE(bridge.native_window_is_visible(hwnd));
    const windowing::WindowData* win = hwnd_mgr.find(hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_FALSE(win->visible);
    bridge.destroy_window(hwnd);
}

TEST_F(User32CpuzTest, DialogBoxIndirectParamWRejectsUnreadableTemplate) {
    DialogManager dialog_mgr{hwnd_mgr, class_mgr, bridge, queue, memory};
    mgr.set_dialog_manager(&dialog_mgr);
    dialog_mgr.set_dlgproc_callback(
        [](windowing::HWND, u32, u64, u64, u64) { return 1u; });

    push_arg(0, 0);
    push_arg(1, 0x70000000);
    push_arg(2, 0);
    push_arg(3, 0x401234);
    push_arg(4, 0);
    set_last_error(error_codes::ERROR_SUCCESS);

    mgr.impl_DialogBoxIndirectParamW(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_DATA);
}

// =============================================================================
// GetAsyncKeyState — 1 arg: vkey
// =============================================================================

TEST_F(User32CpuzTest, GetAsyncKeyState_ReturnsZero) {
    push_arg(0, 0x41); // VK_A
    mgr.impl_GetAsyncKeyState(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(User32CpuzTest, GetAsyncKeyStateConsumesMousePress) {
    bridge.note_native_mouse_button_transition(0x01, true, windowing::NULL_HWND); // VK_LBUTTON
    push_arg(0, 0x01);
    mgr.impl_GetAsyncKeyState(marshaler);
    EXPECT_EQ(static_cast<u16>(cpu.gpr.rax) & 0x0001u, 0x0001u);

    mgr.impl_GetAsyncKeyState(marshaler);
    EXPECT_EQ(static_cast<u16>(cpu.gpr.rax) & 0x0001u, 0u);
}

TEST_F(User32CpuzTest, KeyboardPollingTracksNativePressAndRelease) {
    bridge.note_native_key_transition(0x54, true); // VK_T
    push_arg(0, 0x54);
    mgr.impl_GetAsyncKeyState(marshaler);
    EXPECT_EQ(static_cast<u16>(cpu.gpr.rax), 0x8001u);

    mgr.impl_GetAsyncKeyState(marshaler);
    EXPECT_EQ(static_cast<u16>(cpu.gpr.rax), 0x8000u);

    bridge.note_native_key_transition(0x54, false);
    mgr.impl_GetAsyncKeyState(marshaler);
    EXPECT_EQ(static_cast<u16>(cpu.gpr.rax), 0u);
}

TEST_F(User32CpuzTest, GetKeyboardStateReportsNativeKeysDown) {
    bridge.note_native_key_transition(0x5A, true); // VK_Z
    push_arg(0, DATA_BASE);
    mgr.impl_GetKeyboardState(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u8(DATA_BASE + 0x5A).value(), 0x80u);
    EXPECT_EQ(memory.read_u8(DATA_BASE + 0x41).value(), 0u);
}

TEST_F(User32CpuzTest, SetKeyboardStateRoundTripsTheCallingThreadTable) {
    constexpr GuestAddr input = DATA_BASE + 0x200;
    constexpr GuestAddr output = DATA_BASE + 0x400;
    std::array<u8, 256> state{};
    state[0x41] = 0x80; // VK_A is down.
    state[0x14] = 0x01; // VK_CAPITAL is toggled.
    ASSERT_TRUE(memory.write_bytes(
        input, reinterpret_cast<const Byte*>(state.data()),
        state.size()).is_ok());

    push_arg(0, input);
    mgr.impl_SetKeyboardState(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    push_arg(0, output);
    mgr.impl_GetKeyboardState(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u8(output + 0x41).value(), 0x80u);
    EXPECT_EQ(memory.read_u8(output + 0x14).value(), 0x01u);
    EXPECT_EQ(static_cast<u16>(mgr.thread_key_state(0x41)), 0xFF80u);
    EXPECT_EQ(static_cast<u16>(mgr.thread_key_state(0x14)), 0x0001u);
}

// GetKeyState is the key's state byte sign-extended to a SHORT: a key held
// down reads 0xFF80, toggled-and-down 0xFF81
// (tools/oracle_probe/s2msg_input_probe.expected getkeystate_A_down).
TEST_F(User32CpuzTest, SetKeyboardStateDoesNotAlterAnotherThread) {
    constexpr GuestAddr input = DATA_BASE + 0x200;
    std::array<u8, 256> state{};
    state[0x42] = 0x80; // VK_B is down only for thread 11.
    ASSERT_TRUE(memory.write_bytes(
        input, reinterpret_cast<const Byte*>(state.data()),
        state.size()).is_ok());

    ThreadManager::set_current_thread_id(11);
    push_arg(0, input);
    mgr.impl_SetKeyboardState(marshaler);
    ASSERT_EQ(static_cast<u16>(mgr.thread_key_state(0x42)), 0xFF80u);

    ThreadManager::set_current_thread_id(12);
    EXPECT_EQ(static_cast<u16>(mgr.thread_key_state(0x42)), 0u);
    ThreadManager::set_current_thread_id(1);
}

TEST_F(User32CpuzTest, SetKeyboardStateRejectsAnUnreadableArray) {
    set_last_error(0);
    push_arg(0, 0x70000000);
    mgr.impl_SetKeyboardState(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
}

TEST_F(User32CpuzTest, RetrievedMessagesAdvanceTheThreadKeyboardTable) {
    std::array<u8, 256> state{};
    constexpr GuestAddr input = DATA_BASE + 0x200;
    ASSERT_TRUE(memory.write_bytes(
        input, reinterpret_cast<const Byte*>(state.data()),
        state.size()).is_ok());
    push_arg(0, input);
    mgr.impl_SetKeyboardState(marshaler);

    windowing::MSG message{};
    message.message = windowing::WM::KEYDOWN;
    message.wParam = 0x43; // VK_C
    mgr.note_retrieved_keyboard_message(message);
    EXPECT_EQ(static_cast<u16>(mgr.thread_key_state(0x43)), 0xFF80u);

    message.message = windowing::WM::KEYUP;
    mgr.note_retrieved_keyboard_message(message);
    EXPECT_EQ(static_cast<u16>(mgr.thread_key_state(0x43)), 0u);

    message.message = windowing::WM::KEYDOWN;
    message.wParam = 0x14; // VK_CAPITAL
    mgr.note_retrieved_keyboard_message(message);
    EXPECT_EQ(static_cast<u16>(mgr.thread_key_state(0x14)), 0xFF81u);
}

// =============================================================================
// GetFocus — 0 args, returns 0 (no focus set)
// =============================================================================

TEST_F(User32CpuzTest, GetFocus_ReturnsZeroWhenNoFocus) {
    mgr.impl_GetFocus(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// SetRectEmpty — 1 arg: rect ptr, zeros 16 bytes
// =============================================================================

TEST_F(User32CpuzTest, SetRectEmpty_ZerosRectFields) {
    constexpr GuestAddr rect_addr = DATA_BASE + 0x100;
    // Pre-fill with non-zero data
    (void)memory.write_u32(rect_addr + 0, 10);
    (void)memory.write_u32(rect_addr + 4, 20);
    (void)memory.write_u32(rect_addr + 8, 30);
    (void)memory.write_u32(rect_addr + 12, 40);

    push_arg(0, rect_addr);
    mgr.impl_SetRectEmpty(marshaler);

    EXPECT_EQ(memory.read_u32(rect_addr + 0).value(), 0u);
    EXPECT_EQ(memory.read_u32(rect_addr + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(rect_addr + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(rect_addr + 12).value(), 0u);
}

// =============================================================================
// EqualRect — 2 args: rect1, rect2 ptrs; returns TRUE for identical
// =============================================================================

TEST_F(User32CpuzTest, EqualRect_ReturnsTrueForIdentical) {
    constexpr GuestAddr r1 = DATA_BASE + 0x200;
    constexpr GuestAddr r2 = DATA_BASE + 0x220;
    // Both rects: {1, 2, 3, 4}
    for (GuestAddr base : {r1, r2}) {
        (void)memory.write_u32(base + 0, 1);
        (void)memory.write_u32(base + 4, 2);
        (void)memory.write_u32(base + 8, 3);
        (void)memory.write_u32(base + 12, 4);
    }

    push_arg(0, r1);
    push_arg(1, r2);
    mgr.impl_EqualRect(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u); // TRUE
}

// =============================================================================
// AdjustWindowRectEx — 4 args, returns TRUE
// =============================================================================

TEST_F(User32CpuzTest, AdjustWindowRectEx_ReturnsTrue) {
    constexpr GuestAddr rect_addr = DATA_BASE + 0x300;
    (void)memory.write_u32(rect_addr + 0, 100); // left
    (void)memory.write_u32(rect_addr + 4, 100); // top
    (void)memory.write_u32(rect_addr + 8, 300); // right
    (void)memory.write_u32(rect_addr + 12, 300); // bottom

    push_arg(0, rect_addr);     // lpRect
    push_arg(1, 0x00CF0000);   // dwStyle = WS_OVERLAPPEDWINDOW
    push_arg(2, 0);            // bMenu = FALSE
    push_arg(3, 0);            // dwExStyle = 0
    mgr.impl_AdjustWindowRectEx(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// =============================================================================
// CharUpperBuffW — 2 args: buf ptr, count; uppercases chars
// =============================================================================

TEST_F(User32CpuzTest, CharUpperBuffW_UppercasesChars) {
    constexpr GuestAddr buf = DATA_BASE + 0x400;
    // Write "hello" as UTF-16LE
    (void)memory.write_u16(buf + 0, u'h');
    (void)memory.write_u16(buf + 2, u'e');
    (void)memory.write_u16(buf + 4, u'l');
    (void)memory.write_u16(buf + 6, u'l');
    (void)memory.write_u16(buf + 8, u'o');

    push_arg(0, buf);
    push_arg(1, 5);
    mgr.impl_CharUpperBuffW(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 5u);
    EXPECT_EQ(memory.read_u16(buf + 0).value(), static_cast<u16>('H'));
    EXPECT_EQ(memory.read_u16(buf + 2).value(), static_cast<u16>('E'));
    EXPECT_EQ(memory.read_u16(buf + 4).value(), static_cast<u16>('L'));
    EXPECT_EQ(memory.read_u16(buf + 6).value(), static_cast<u16>('L'));
    EXPECT_EQ(memory.read_u16(buf + 8).value(), static_cast<u16>('O'));
}

// =============================================================================
// IsCharLowerW — 1 arg: wchar; TRUE for 'a', FALSE for 'A'
// =============================================================================

TEST_F(User32CpuzTest, IsCharLowerW_TrueForLowercase) {
    push_arg(0, static_cast<u32>('a'));
    mgr.impl_IsCharLowerW(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(User32CpuzTest, IsCharLowerW_FalseForUppercase) {
    push_arg(0, static_cast<u32>('A'));
    mgr.impl_IsCharLowerW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// RegisterClipboardFormatW — 1 arg: format name ptr; returns non-zero
// =============================================================================

TEST_F(User32CpuzTest, RegisterClipboardFormatW_ReturnsNonZero) {
    constexpr GuestAddr str_addr = DATA_BASE + 0x500;
    // Write "MyFormat\0" as UTF-16LE
    const char16_t name[] = u"MyFormat";
    for (usize i = 0; i <= 8; ++i) {
        (void)memory.write_u16(str_addr + i * 2, static_cast<u16>(name[i]));
    }

    push_arg(0, str_addr);
    mgr.impl_RegisterClipboardFormatW(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(User32CpuzTest, RegisterClipboardFormatA_ReturnsNonZero) {
    // ANSI twin of RegisterClipboardFormatW — SciLexer WM_CREATE path.
    constexpr GuestAddr str_addr = DATA_BASE + 0x580;
    constexpr char name[] = "MyFormatA";
    ASSERT_TRUE(memory.write_bytes(
        str_addr, reinterpret_cast<const Byte*>(name), sizeof(name)).is_ok());
    push_arg(0, str_addr);
    mgr.impl_RegisterClipboardFormatA(marshaler);
    const u32 fmt = static_cast<u32>(cpu.gpr.rax);
    EXPECT_NE(fmt, 0u);
    EXPECT_GE(fmt, 0xC000u);
}

// =============================================================================
// GetKeyboardLayout — 1 arg: threadId; returns 0x04090409
// =============================================================================

TEST_F(User32CpuzTest, GetKeyboardLayout_ReturnsUSLayout) {
    push_arg(0, 0); // current thread
    mgr.impl_GetKeyboardLayout(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x04090409u);
}

// =============================================================================
// MapVirtualKeyW — 2 args; returns 0 for unknown
// =============================================================================

TEST_F(User32CpuzTest, MapVirtualKeyW_UnknownReturnsZero) {
    push_arg(0, 0xFF); // unknown VK
    push_arg(1, 0);    // MAPVK_VK_TO_VSC
    mgr.impl_MapVirtualKeyW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// CreateMenu — 0 args, returns non-zero handle
// =============================================================================

TEST_F(User32CpuzTest, CreateMenu_ReturnsNonZeroHandle) {
    mgr.impl_CreateMenu(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(User32CpuzTest, AppendMenuAExportAppendsAnsiString) {
    mgr.impl_CreatePopupMenu(marshaler);
    const u32 menu = static_cast<u32>(cpu.gpr.rax);
    constexpr GuestAddr label = DATA_BASE + 0x600;
    constexpr char text[] = "&Install";
    ASSERT_TRUE(memory.write_bytes(
        label, reinterpret_cast<const Byte*>(text), sizeof(text)).is_ok());

    push_arg(0, menu);
    push_arg(1, 0); // MF_STRING
    push_arg(2, 101);
    push_arg(3, static_cast<u32>(label));

    auto exports = create_user32_ext_thunks(mgr);
    auto append_menu = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) { return entry.name == "AppendMenuA"; });
    ASSERT_NE(append_menu, exports.entries.end());
    append_menu->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// =============================================================================
// DestroyMenu — 1 arg: hmenu; returns TRUE
// =============================================================================

TEST_F(User32CpuzTest, DestroyMenu_ReturnsTrue) {
    mgr.impl_CreatePopupMenu(marshaler);
    const u32 menu = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(menu, 0u);
    push_arg(0, menu);
    mgr.impl_DestroyMenu(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    // Not a menu (or destroyed already): FALSE, ERROR_INVALID_MENU_HANDLE
    // (measured, s1win_misc_probe destroymenu_again).
    push_arg(0, 0xDE100000);
    mgr.impl_DestroyMenu(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1401u);
}

// =============================================================================
// GetMenuItemCount — 1 arg: hmenu. Windows returns -1 on failure, and an
// unknown handle is a failure. Returning 0 instead would tell the caller it
// holds a valid but empty menu, which is a different fact.
// =============================================================================

TEST_F(User32CpuzTest, GetMenuItemCount_UnknownHandleReturnsMinusOne) {
    push_arg(0, 0xDE100000);
    mgr.impl_GetMenuItemCount(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
}

// =============================================================================
// SetWindowsHookExW — 4 args; returns non-zero hook handle
// =============================================================================

// A thread hook needs no module. A GLOBAL (dwThreadId 0) keyboard hook with
// hMod NULL fails with ERROR_HOOK_NEEDS_HMOD on Windows 11
// (tools/oracle_probe/s2msg_queue_probe.expected hook_global_no_hmod,
// hook_id 2 ... global_self err=594).
TEST_F(User32CpuzTest, SetWindowsHookExW_ReturnsNonZero) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    push_arg(0, 2);       // WH_KEYBOARD
    push_arg(1, 0x40100); // lpfn (callback addr)
    push_arg(2, 0);       // hMod
    push_arg(3, tid == 0 ? 1u : tid);  // this thread
    mgr.impl_SetWindowsHookExW(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u);

    set_last_error(0);
    push_arg(0, 2);
    push_arg(1, 0x40100);
    push_arg(2, 0);
    push_arg(3, 0);       // global without a DLL
    mgr.impl_SetWindowsHookExW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1428u);  // ERROR_HOOK_NEEDS_HMOD
}

// =============================================================================
// UnhookWindowsHookEx — 1 arg; returns TRUE
// =============================================================================

TEST_F(User32CpuzTest, PeekMessageRemoveInvokesWhGetMessageHook) {
    constexpr i32 WH_GETMESSAGE = 3;
    constexpr GuestAddr kHookProc = 0x40100;
    constexpr GuestAddr kMsgPtr = DATA_BASE + 0x40;
    constexpr u32 kWmUser = 0x0401;

    windowing::WindowData data{};
    data.title = L"Hook";
    data.wndproc = 0x402000;
    data.parent = windowing::HWND_DESKTOP;
    const windowing::HWND hwnd = hwnd_mgr.create(data);

    windowing::MSG posted{};
    posted.hwnd = hwnd;
    posted.message = kWmUser;
    posted.wParam = 7;
    posted.lParam = 9;
    ASSERT_TRUE(queue.post(posted));

    GuestAddr called_proc = 0;
    std::vector<u64> called_args;
    mgr.set_guest_call(
        [&](GuestAddr proc, const u64* args, usize count, bool) -> u64 {
            called_proc = proc;
            called_args.assign(args, args + count);
            return 0;
        });

    // A global hook names the DLL holding its procedure; hMod NULL fails
    // (s2msg_queue_probe hook_global_no_hmod: ERROR_HOOK_NEEDS_HMOD).
    push_arg(0, WH_GETMESSAGE);
    push_arg(1, static_cast<u32>(kHookProc));
    push_arg(2, 0x10000000u);  // a DLL's HMODULE
    push_arg(3, 0);
    mgr.impl_SetWindowsHookExW(marshaler);
    ASSERT_NE(static_cast<u32>(cpu.gpr.rax), 0u);

    MessageLoopManager mlm(hwnd_mgr, queue, bridge);
    mlm.set_user32_ext_manager(&mgr);

    push_arg(0, static_cast<u32>(kMsgPtr));
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, 0);
    push_arg(4, windowing::PM::REMOVE);
    impl_PeekMessageW(marshaler, mlm);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    EXPECT_EQ(called_proc, kHookProc);
    ASSERT_EQ(called_args.size(), 3u);
    EXPECT_EQ(called_args[0], 0u); // HC_ACTION
    EXPECT_EQ(called_args[1], 1u); // PM_REMOVE
    EXPECT_EQ(called_args[2], kMsgPtr);
    EXPECT_EQ(memory.read_u32(kMsgPtr).value_or(0), hwnd);
    EXPECT_EQ(memory.read_u32(kMsgPtr + 4).value_or(0), kWmUser);
}

TEST_F(User32CpuzTest, PeekMessageEmptyDoesNotInvokeWhGetMessageHook) {
    constexpr i32 WH_GETMESSAGE = 3;
    constexpr GuestAddr kHookProc = 0x40100;
    constexpr GuestAddr kMsgPtr = DATA_BASE + 0x40;

    bool called = false;
    mgr.set_guest_call(
        [&](GuestAddr, const u64*, usize, bool) -> u64 {
            called = true;
            return 0;
        });

    // A global hook names the DLL holding its procedure; hMod NULL fails
    // (s2msg_queue_probe hook_global_no_hmod: ERROR_HOOK_NEEDS_HMOD).
    push_arg(0, WH_GETMESSAGE);
    push_arg(1, static_cast<u32>(kHookProc));
    push_arg(2, 0x10000000u);  // a DLL's HMODULE
    push_arg(3, 0);
    mgr.impl_SetWindowsHookExW(marshaler);
    ASSERT_NE(static_cast<u32>(cpu.gpr.rax), 0u);

    MessageLoopManager mlm(hwnd_mgr, queue, bridge);
    mlm.set_user32_ext_manager(&mgr);

    push_arg(0, static_cast<u32>(kMsgPtr));
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, 0);
    push_arg(4, windowing::PM::REMOVE);
    impl_PeekMessageW(marshaler, mlm);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_FALSE(called);
}

TEST_F(User32CpuzTest, UnhookWindowsHookEx_ReturnsTrue) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    push_arg(0, 2);       // WH_KEYBOARD
    push_arg(1, 0x40100); // lpfn
    push_arg(2, 0);       // hMod
    push_arg(3, tid == 0 ? 1u : tid);  // this thread
    mgr.impl_SetWindowsHookExW(marshaler);
    const u32 hook = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(hook, 0u);

    push_arg(0, hook);
    mgr.impl_UnhookWindowsHookEx(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// =============================================================================
// CallNextHookEx — 4 args; returns 0
// =============================================================================

TEST_F(User32CpuzTest, CallNextHookEx_ReturnsZero) {
    push_arg(0, 0xFEED0001); // hhk
    push_arg(1, 0);          // nCode
    push_arg(2, 0);          // wParam
    push_arg(3, 0);          // lParam
    mgr.impl_CallNextHookEx(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// BeginDeferWindowPos — 1 arg; returns non-zero
// =============================================================================

TEST_F(User32CpuzTest, BeginDeferWindowPos_ReturnsNonZero) {
    push_arg(0, 4); // nNumWindows
    mgr.impl_BeginDeferWindowPos(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// EndDeferWindowPos — 1 arg; returns TRUE
// =============================================================================

TEST_F(User32CpuzTest, EndDeferWindowPos_ReturnsTrue) {
    push_arg(0, 1); // nNumWindows
    mgr.impl_BeginDeferWindowPos(marshaler);
    const u32 hdwp = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(hdwp, 0u);
    push_arg(0, hdwp);
    mgr.impl_EndDeferWindowPos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// Measured (tools/oracle_probe/s1win_msgseq2_probe.c end_invalid_hdwp): a
// handle BeginDeferWindowPos did not return is ERROR_INVALID_DWP_HANDLE.
TEST_F(User32CpuzTest, EndDeferWindowPos_InvalidHandleFails) {
    push_arg(0, 0xDF000001); // hdwp
    mgr.impl_EndDeferWindowPos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1405u);
}

// =============================================================================
// GetMessageTime — 0 args; returns non-zero
// =============================================================================

TEST_F(User32CpuzTest, GetMessageTime_ReturnsLastRetrievedMessageTime) {
    vitreo::windowing::MSG retrieved{};
    retrieved.message = vitreo::windowing::WM::TIMER;
    retrieved.time = 0x12345678u;
    mgr.note_retrieved_queue_message(retrieved);
    mgr.impl_GetMessageTime(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x12345678u);
}

// =============================================================================
// GetScrollPos — 2 args; returns 0
// =============================================================================

TEST_F(User32CpuzTest, GetScrollPos_ReturnsZero) {
    push_arg(0, 0x1234); // hWnd
    push_arg(1, 0);      // nBar = SB_HORZ
    mgr.impl_GetScrollPos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// SetScrollPos — 4 args; returns 0 (previous position)
// =============================================================================

TEST_F(User32CpuzTest, SetScrollPos_ReturnsZero) {
    push_arg(0, 0x1234); // hWnd
    push_arg(1, 0);      // nBar
    push_arg(2, 50);     // nPos
    push_arg(3, 1);      // bRedraw
    mgr.impl_SetScrollPos(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// CopyIcon — 1 arg; returns same handle
// =============================================================================

TEST_F(User32CpuzTest, CopyIcon_RejectsAHandleThatIsNoIcon) {
    // s3gdi_icon_probe (cur probe): CopyIcon(bogus) = NULL, error 1402.
    push_arg(0, 0x1C0001);
    mgr.impl_CopyIcon(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// DestroyIcon — 1 arg; returns TRUE
// =============================================================================

TEST_F(User32CpuzTest, DestroyIcon_RejectsAHandleThatIsNoIcon) {
    // DestroyIcon(bogus) = FALSE with the last error untouched.
    push_arg(0, 0x1C0001);
    set_last_error(57005);
    mgr.impl_DestroyIcon(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 57005u);
}

// =============================================================================
// IsClipboardFormatAvailable — 1 arg; returns FALSE
// =============================================================================

// CF_BITMAP is never on a clipboard nobody filled (CF_TEXT would depend on
// the host pasteboard's text, which other applications own).
TEST_F(User32CpuzTest, IsClipboardFormatAvailable_ReturnsFalse) {
    push_arg(0, 2); // CF_BITMAP
    mgr.impl_IsClipboardFormatAvailable(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// GetLastActivePopup — 1 arg; returns input hwnd
// =============================================================================

// A window without owned popups is its own last active popup; an invalid
// window is NULL / ERROR_INVALID_WINDOW_HANDLE
// (tools/oracle_probe/s2msg_input_probe.expected lastactivepopup_*).
TEST_F(User32CpuzTest, GetLastActivePopup_ReturnsInputHwnd) {
    windowing::WindowData data{};
    data.parent = windowing::HWND_DESKTOP;
    const windowing::HWND hwnd = hwnd_mgr.create(data);
    push_arg(0, hwnd);
    mgr.impl_GetLastActivePopup(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), hwnd);
    set_last_error(0);
    push_arg(0, 0xABCD);
    mgr.impl_GetLastActivePopup(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

// =============================================================================
// SystemParametersInfoA — 4 args; returns TRUE
// =============================================================================

TEST_F(User32CpuzTest, SystemParametersInfoA_ReturnsTrue) {
    push_arg(0, 0x30);       // SPI_GETWORKAREA
    push_arg(1, 0);          // uiParam
    push_arg(2, DATA_BASE + 0x600); // pvParam
    push_arg(3, 0);          // fWinIni
    mgr.impl_SystemParametersInfoA(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

TEST_F(User32CpuzTest, SystemParametersInfoWReturnsMouseWheelRouting) {
    constexpr GuestAddr routing = DATA_BASE + 0x600;
    ASSERT_TRUE(memory.write_u32(routing, 0xDEADBEEFu).is_ok());
    push_arg(0, 0x201C); // SPI_GETMOUSEWHEELROUTING
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(routing));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(routing).value(), 2u);

    push_arg(2, 0);
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(User32CpuzTest, SystemParametersInfoWReturnsFontSmoothingState) {
    constexpr GuestAddr enabled = DATA_BASE + 0x600;
    constexpr GuestAddr type = DATA_BASE + 0x604;
    ASSERT_TRUE(memory.write_u32(enabled, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(type, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x004A); // SPI_GETFONTSMOOTHING
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(enabled));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(enabled).value(), 1u);

    push_arg(0, 0x200A); // SPI_GETFONTSMOOTHINGTYPE
    push_arg(2, static_cast<u32>(type));
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(type).value(), 1u);
}

TEST_F(User32CpuzTest, SystemParametersInfoWReturnsUiAccessibilityState) {
    constexpr GuestAddr keyboard_cues = DATA_BASE + 0x600;
    constexpr GuestAddr client_animation = DATA_BASE + 0x604;
    ASSERT_TRUE(memory.write_u32(keyboard_cues, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(client_animation, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x100A); // SPI_GETKEYBOARDCUES
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(keyboard_cues));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(keyboard_cues).value(), 0u);

    push_arg(0, 0x1042); // SPI_GETCLIENTAREAANIMATION
    push_arg(2, static_cast<u32>(client_animation));
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(client_animation).value(), 1u);
}

TEST_F(User32CpuzTest, SystemParametersInfoWReportsNoGuestScreenSaverRunning) {
    constexpr GuestAddr running = DATA_BASE + 0x600;
    ASSERT_TRUE(memory.write_u32(running, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x0072); // SPI_GETSCREENSAVERRUNNING
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(running));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(running).value(), 0u);

    // A NULL pvParam is ERROR_NOACCESS (measured for SPI_GETWORKAREA,
    // tools/oracle_probe/s1win_display_probe.expected spi_workarea_null).
    set_last_error(error_codes::ERROR_SUCCESS);
    push_arg(2, 0);
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
}

TEST_F(User32CpuzTest, SystemParametersInfoWReturnsDisabledMouseCursorEffects) {
    constexpr GuestAddr trails = DATA_BASE + 0x600;
    constexpr GuestAddr shadow = DATA_BASE + 0x604;
    constexpr GuestAddr sonar = DATA_BASE + 0x608;
    ASSERT_TRUE(memory.write_u32(trails, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(shadow, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(sonar, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x005E); // SPI_GETMOUSETRAILS
    push_arg(1, 0);
    push_arg(2, static_cast<u32>(trails));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(trails).value(), 0u);

    push_arg(0, 0x101A); // SPI_GETCURSORSHADOW
    push_arg(2, static_cast<u32>(shadow));
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(shadow).value(), 0u);

    push_arg(0, 0x101C); // SPI_GETMOUSESONAR
    push_arg(2, static_cast<u32>(sonar));
    mgr.impl_SystemParametersInfoW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(sonar).value(), 0u);
}

TEST_F(User32CpuzTest, DisplayConfigReturnsStandardSdrWhiteLevel) {
    if (bridge.get_all_monitor_info().empty()) {
        GTEST_SKIP() << "AppKit has no NSScreen in this test process";
    }
    constexpr GuestAddr request = DATA_BASE + 0x680;
    ASSERT_TRUE(memory.write_u32(request + 0, 11).is_ok());
    ASSERT_TRUE(memory.write_u32(request + 4, 24).is_ok());
    // The adapter LUID QueryDisplayConfig reports; an unknown one is
    // ERROR_GEN_FAILURE (measured, s1win_display_probe dcgdi_bad_adapter).
    ASSERT_TRUE(memory.write_u32(request + 8, 0x5654524Du).is_ok());
    ASSERT_TRUE(memory.write_u32(request + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(request + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(request + 20, 0xDEADBEEFu).is_ok());

    push_arg(0, static_cast<u32>(request));
    mgr.impl_DisplayConfigGetDeviceInfo(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(request + 20).value(), 1000u);
}

TEST_F(User32CpuzTest, SystemParametersInfoWReturnsDisabledHighContrast) {
    constexpr GuestAddr high_contrast = DATA_BASE + 0x600;
    ASSERT_TRUE(memory.write_u32(high_contrast, 12).is_ok());
    ASSERT_TRUE(memory.write_u32(high_contrast + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(high_contrast + 8, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x0042); // SPI_GETHIGHCONTRAST
    push_arg(1, 12);
    push_arg(2, static_cast<u32>(high_contrast));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(high_contrast).value(), 12u);
    // Available but off: HCF_HIGHCONTRASTON clear (measured flags 0x7E,
    // s1win_display_probe spi_highcontrast).
    EXPECT_EQ(memory.read_u32(high_contrast + 4).value(), 0x7Eu);
    EXPECT_EQ(memory.read_u32(high_contrast + 8).value(), 0u);
}

TEST_F(User32CpuzTest, SystemParametersInfoWRejectsUnsupportedAction) {
    set_last_error(error_codes::ERROR_SUCCESS);
    push_arg(0, 0xDEAD);
    push_arg(1, 0);
    push_arg(2, DATA_BASE + 0x600);
    push_arg(3, 0);

    mgr.impl_SystemParametersInfoW(marshaler);

    // 32-bit: an unknown action is ERROR_CALL_NOT_IMPLEMENTED (measured,
    // s1win_display_probe32 spi_unknown_action; x64 reports 87).
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
}

TEST_F(User32CpuzTest, SystemParametersInfoWFillsNonClientMetricsFonts) {
    constexpr GuestAddr ncm = DATA_BASE + 0x600;
    constexpr u32 kSize = 504; // Vista+ NONCLIENTMETRICSW
    ASSERT_TRUE(memory.write_u32(ncm, kSize).is_ok());
    // Poison message-font lfHeight so a failed fill is obvious.
    ASSERT_TRUE(memory.write_u32(ncm + 408, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x0029); // SPI_GETNONCLIENTMETRICS
    push_arg(1, kSize);
    push_arg(2, static_cast<u32>(ncm));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoW(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(ncm).value(), kSize);
    // iCaptionHeight is SM_CYCAPTION - 1 (Windows 22 / 23, measured); with
    // Vitreo's native-chrome caption of 30 it is 29.
    EXPECT_EQ(memory.read_u32(ncm + 20).value(), 29u);
    // lfCaptionFont.lfHeight @ 24, lfMessageFont.lfHeight @ 408
    EXPECT_EQ(static_cast<i32>(memory.read_u32(ncm + 24).value()), -12);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(ncm + 408).value()), -12);
    EXPECT_EQ(memory.read_u32(ncm + 500).value(), 4u); // iPaddedBorderWidth
}

TEST_F(User32CpuzTest, SystemParametersInfoAFillsNonClientMetricsFonts) {
    constexpr GuestAddr ncm = DATA_BASE + 0x600;
    constexpr u32 kSize = 344; // Vista+ NONCLIENTMETRICSA
    ASSERT_TRUE(memory.write_u32(ncm, kSize).is_ok());
    ASSERT_TRUE(memory.write_u32(ncm + 280, 0xDEADBEEFu).is_ok());

    push_arg(0, 0x0029); // SPI_GETNONCLIENTMETRICS
    push_arg(1, kSize);
    push_arg(2, static_cast<u32>(ncm));
    push_arg(3, 0);
    mgr.impl_SystemParametersInfoA(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(ncm).value(), kSize);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(ncm + 24).value()), -12);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(ncm + 280).value()), -12);
}

// GetWindow (user32_windows_probe.expected gw_* lines, Windows 11):
// GW_ENABLEDPOPUP is the visible+enabled owned window, NULL (never hWnd)
// when there is none; a bad HWND is ERROR_INVALID_WINDOW_HANDLE and a bad
// uCmd ERROR_INVALID_GW_COMMAND; a NULL relationship leaves the error alone.
TEST_F(User32CpuzTest, GetWindow_EnabledPopupAndErrors) {
    windowing::WindowData main_data;
    main_data.parent = windowing::HWND_DESKTOP;
    main_data.enabled = true;
    const auto main_wnd = hwnd_mgr.create(main_data);
    windowing::WindowData popup_data;
    popup_data.parent = windowing::HWND_DESKTOP;
    popup_data.owner = main_wnd;
    popup_data.style = 0x80000000u;  // WS_POPUP
    popup_data.enabled = true;
    popup_data.visible = false;
    const auto popup = hwnd_mgr.create(popup_data);
    auto call = [&](u32 hwnd, u32 cmd) {
        push_arg(0, hwnd);
        push_arg(1, cmd);
        set_last_error(0xDEAD);
        mgr.impl_GetWindow(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(call(main_wnd, 6), 0u);  // popup hidden
    EXPECT_EQ(get_last_error(), 0xDEADu);
    hwnd_mgr.find(popup)->visible = true;
    EXPECT_EQ(call(main_wnd, 6), popup);
    hwnd_mgr.find(popup)->enabled = false;
    EXPECT_EQ(call(main_wnd, 6), 0u);
    EXPECT_EQ(call(popup, 6), 0u);
    EXPECT_EQ(call(popup, 4), main_wnd);  // GW_OWNER
    EXPECT_EQ(call(main_wnd, 5), 0u);     // GW_CHILD, none
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call(0x7FFF0, 5), 0u);
    EXPECT_EQ(get_last_error(), 1400u);
    EXPECT_EQ(call(main_wnd, 7), 0u);
    EXPECT_EQ(get_last_error(), 1443u);
}
