/// @file test_user32_ext_window_pos.cpp
/// @brief SetWindowPos/MoveWindow contracts: WM_WINDOWPOSCHANGING/CHANGED, WM_GETMINMAXINFO,
///        WM_NCCALCSIZE, z-order, WM_SIZE/WM_MOVE notification suppression.

#include "user32_ext_thunk_harness.hpp"

// =============================================================================
// SetWindowPos contract tests
// =============================================================================

TEST_F(User32ExtThunkTest, MoveWindowConsumedPositionChangeSuppressesMoveAndSize) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::OVERLAPPEDWINDOW;
    wd.rect = windowing::Rect{10, 20, 210, 140};
    wd.client_rect = windowing::Rect{0, 0, 184, 92};
    wd.wndproc = 0x401000;
    wd.visible = true;
    const HWND hwnd = hwnd_mgr_.create(wd);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<u32> messages;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64, u64, u64) {
            EXPECT_EQ(target, hwnd);
            messages.push_back(message);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 30;
    cpu_.gpr.r8 = 40;
    cpu_.gpr.r9 = 320;
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, 240);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, 0); // bRepaint = FALSE
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("MoveWindow").handler(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    // WM_GETMINMAXINFO comes from DefWindowProc's WM_WINDOWPOSCHANGING
    // handling, so a procedure that consumes every message never sees it
    // (measured, tools/oracle_probe/s1win_msgseq2_probe.c
    // swp_tiny_without_dwp_changing).
    EXPECT_EQ(messages,
              (std::vector<u32>{
                  WM::WINDOWPOSCHANGING,
                  WM::NCCALCSIZE,
                  WM::WINDOWPOSCHANGED}));
}

TEST_F(User32ExtThunkTest, WindowPositionCallbackMayDestroyItsWindow) {
    for (const u32 destroy_on : {WM::WINDOWPOSCHANGING, WM::NCCALCSIZE,
                                 WM::WINDOWPOSCHANGED}) {
        SCOPED_TRACE(destroy_on);
        WindowData data{};
        data.parent = HWND_DESKTOP;
        data.style = WS::POPUP;
        data.rect = {10, 20, 110, 100};
        data.client_rect = {0, 0, 100, 80};
        data.wndproc = 0x401000;
        const HWND hwnd = hwnd_mgr_.create(data);
        HWND replacement = 0;
        MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
        message_send.set_wndproc_callback(
            [&](HWND, u32 message, u64, u64, u64) -> u64 {
                if (message == destroy_on) {
                    hwnd_mgr_.destroy(hwnd);
                    WindowData next{};
                    next.parent = HWND_DESKTOP;
                    next.style = WS::POPUP;
                    next.rect = {40, 50, 60, 70};
                    next.client_rect = {0, 0, 20, 20};
                    replacement = hwnd_mgr_.create(next);
                }
                return 0;
            });
        mgr_->set_message_send_manager(&message_send);
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = HWND_TOP;
        cpu_.gpr.r8 = 30;
        cpu_.gpr.r9 = 40;
        set_fastcall_arg4_6(200, 160, 0x14);
        Marshaler marshaler(memory_);
        marshaler.attach_cpu(cpu_);
        find_export("SetWindowPos").handler(marshaler);
        EXPECT_EQ(cpu_.gpr.rax, 1u); // Native Windows completes successfully.
        EXPECT_FALSE(hwnd_mgr_.is_valid(hwnd));
        ASSERT_NE(replacement, 0u);
        const auto* live = hwnd_mgr_.find(replacement);
        ASSERT_NE(live, nullptr);
        EXPECT_EQ(live->rect.left, 40);
        EXPECT_EQ(live->rect.top, 50);
        EXPECT_EQ(live->rect.width(), 20);
        EXPECT_EQ(live->client_rect.width(), 20);
        hwnd_mgr_.destroy(replacement);
        mgr_->set_message_send_manager(nullptr);
    }
}

TEST_F(User32ExtThunkTest, UnchangedWindowPositionDoesNotRepeatLayoutNotifications) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::POPUP;
    wd.rect = {10, 20, 210, 140};
    wd.client_rect = {0, 0, 200, 120};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);
    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<u32> messages;
    message_send.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64, u64) -> u64 {
            messages.push_back(message);
            return 0;
        });
    mgr_->set_message_send_manager(&message_send);
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 10;
    cpu_.gpr.r9 = 20;
    set_fastcall_arg4_6(200, 120, 0x14); // NOZORDER | NOACTIVATE
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("SetWindowPos").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(messages, (std::vector<u32>{WM::WINDOWPOSCHANGING}));
}

TEST_F(User32ExtThunkTest, SetWindowPosHonorsWindowPosChangingGeometry) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::POPUP;
    wd.rect = windowing::Rect{10, 20, 210, 140};
    wd.client_rect = windowing::Rect{0, 0, 200, 120};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64, u64 lparam, u64) -> u64 {
            EXPECT_EQ(target, hwnd);
            if (message == WM::WINDOWPOSCHANGING) {
                EXPECT_NE(lparam, 0u);
                if (lparam == 0) return 0u;
                EXPECT_TRUE(memory_.write_u32(lparam + 0x08, 50).is_ok());
                EXPECT_TRUE(memory_.write_u32(lparam + 0x0c, 60).is_ok());
                EXPECT_TRUE(memory_.write_u32(lparam + 0x10, 640).is_ok());
                EXPECT_TRUE(memory_.write_u32(lparam + 0x14, 480).is_ok());
            }
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 30;
    cpu_.gpr.r9 = 40;
    set_fastcall_arg4_6(320, 240, 0);
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("SetWindowPos").handler(marshaler);

    ASSERT_EQ(cpu_.gpr.rax, 1u);
    const WindowData* changed = hwnd_mgr_.find(hwnd);
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->rect.left, 50);
    EXPECT_EQ(changed->rect.top, 60);
    EXPECT_EQ(changed->rect.right, 690);
    EXPECT_EQ(changed->rect.bottom, 540);
    EXPECT_EQ(changed->client_rect.width(), 640);
    EXPECT_EQ(changed->client_rect.height(), 480);
}

TEST_F(User32ExtThunkTest, DefaultWindowProcEnforcesGetMinMaxInfoConstraint) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::OVERLAPPEDWINDOW;
    wd.rect = windowing::Rect{10, 20, 732, 651};
    wd.client_rect = windowing::Rect{0, 0, 706, 583};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    DefWindowProcManager dwp(hwnd_mgr_, queue_, memory_);
    dwp.set_message_send_manager(&message_send);
    dwp.set_windowpos_scratch(0xA000);
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64 lparam, u64) -> u64 {
            EXPECT_EQ(target, hwnd);
            if (message == WM::GETMINMAXINFO) {
                EXPECT_TRUE(memory_.write_u32(lparam + 24, 722).is_ok());
                EXPECT_TRUE(memory_.write_u32(lparam + 28, 631).is_ok());
                return 0;
            }
            if (message == WM::WINDOWPOSCHANGING) {
                return dwp.def_window_proc(target, message, wparam, lparam);
            }
            return 0;
        });
    mgr_->set_message_send_manager(&message_send);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 30;
    cpu_.gpr.r9 = 40;
    set_fastcall_arg4_6(724, 489, 0);
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("SetWindowPos").handler(marshaler);

    ASSERT_EQ(cpu_.gpr.rax, 1u);
    const WindowData* changed = hwnd_mgr_.find(hwnd);
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->rect.width(), 724);
    EXPECT_EQ(changed->rect.height(), 631);
}

TEST_F(User32ExtThunkTest, SetWindowPosMinMaxInfoComesOnlyFromDefWindowProc) {
    // Measured (tools/oracle_probe/s1win_msgseq2_probe.c
    // swp_tiny_without_dwp_changing): when the procedure does not pass
    // WM_WINDOWPOSCHANGING to DefWindowProc, USER sends no WM_GETMINMAXINFO
    // and applies no tracking size.
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::OVERLAPPEDWINDOW;
    wd.rect = windowing::Rect{10, 20, 732, 651};
    wd.client_rect = windowing::Rect{0, 0, 722, 631};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<u32> messages;
    message_send.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64, u64) -> u64 {
            messages.push_back(message);
            return 0;
        });
    mgr_->set_message_send_manager(&message_send);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 30;
    cpu_.gpr.r9 = 40;
    set_fastcall_arg4_6(5, 5, 0);
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("SetWindowPos").handler(marshaler);

    const WindowData* changed = hwnd_mgr_.find(hwnd);
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->rect.width(), 5);
    EXPECT_EQ(changed->rect.height(), 5);
    ASSERT_GE(messages.size(), 1u);
    EXPECT_EQ(messages[0], WM::WINDOWPOSCHANGING);
    EXPECT_EQ(std::count(messages.begin(), messages.end(), WM::GETMINMAXINFO), 0);
}

TEST_F(User32ExtThunkTest, SetWindowPos_PostsWindowPosMessagesAndMoveSize) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::OVERLAPPEDWINDOW;
    wd.rect = windowing::Rect{10, 20, 210, 140};
    wd.client_rect = windowing::Rect{0, 0, 184, 92};
    wd.visible = true;
    HWND hwnd = hwnd_mgr_.create(wd);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    queue_.clear();

    // SetWindowPos(hwnd, HWND_TOP, 30, 40, 320, 240, 0)
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8  = 30;
    cpu_.gpr.r9  = 40;
    set_fastcall_arg4_6(320, 240, 0);

    mgr_->impl_SetWindowPos(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    std::vector<u32> msgs;
    MSG msg{};
    while (queue_.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
        if (msg.hwnd == hwnd) {
            msgs.push_back(msg.message);
        }
    }

    auto has = [&](u32 id) {
        return std::find(msgs.begin(), msgs.end(), id) != msgs.end();
    };

    EXPECT_TRUE(has(WM::WINDOWPOSCHANGING));
    EXPECT_TRUE(has(WM::WINDOWPOSCHANGED));
    EXPECT_TRUE(has(WM::MOVE));
    EXPECT_TRUE(has(WM::SIZE));
}

TEST_F(User32ExtThunkTest, SetWindowPosHonorsClientDrawnNcCalcSize) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::CAPTION | WS::THICKFRAME;
    wd.rect = windowing::Rect{10, 20, 210, 140};
    wd.client_rect = windowing::Rect{0, 0, 184, 82};
    wd.client_offset = windowing::Point{8, 38};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    bool saw_nccalcsize = false;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64 lparam, u64) {
            if (message == WM::NCCALCSIZE) {
                EXPECT_EQ(target, hwnd);
                EXPECT_EQ(wparam, 1u);
                EXPECT_NE(lparam, 0u);
                // A custom-frame wndproc returns 0 without changing rgrc[0],
                // making the entire proposed outer rectangle client area.
                saw_nccalcsize = true;
            }
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 30;
    cpu_.gpr.r9 = 40;
    set_fastcall_arg4_6(320, 240, 0);
    mgr_->impl_SetWindowPos(m);

    ASSERT_TRUE(saw_nccalcsize);
    const WindowData* resized = hwnd_mgr_.find(hwnd);
    ASSERT_NE(resized, nullptr);
    EXPECT_EQ(resized->client_rect.width(), 320);
    EXPECT_EQ(resized->client_rect.height(), 240);
    EXPECT_EQ(resized->client_offset.x, 0);
    EXPECT_EQ(resized->client_offset.y, 0);
    EXPECT_TRUE(resized->client_drawn_frame);
}

TEST_F(User32ExtThunkTest, ClientToScreenIncludesParentNcCalcSizeOffset) {
    WindowData parent_data{};
    parent_data.parent = HWND_DESKTOP;
    parent_data.rect = windowing::Rect{100, 200, 420, 440};
    parent_data.client_rect = windowing::Rect{0, 0, 304, 194};
    parent_data.client_offset = windowing::Point{8, 38};
    const HWND parent = hwnd_mgr_.create(parent_data);

    WindowData child_data{};
    child_data.parent = parent;
    child_data.style = WS::CHILD;
    child_data.rect = windowing::Rect{10, 20, 110, 70};
    child_data.client_rect = windowing::Rect{0, 0, 100, 50};
    const HWND child = hwnd_mgr_.create(child_data);

    ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 0, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 4, 0).is_ok());
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = child;
    cpu_.gpr.rdx = BUF_ADDR;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    find_export("ClientToScreen").handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 0).value_or(0), 118u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 4).value_or(0), 258u);
}

TEST_F(User32ExtThunkTest, SetWindowPos_NoSendChangingSuppressesChangingMessage) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::OVERLAPPEDWINDOW;
    wd.rect = windowing::Rect{0, 0, 200, 120};
    wd.client_rect = windowing::Rect{0, 0, 184, 92};
    wd.visible = true;
    HWND hwnd = hwnd_mgr_.create(wd);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    queue_.clear();

    constexpr u32 SWP_NOSIZE = 0x0001;
    constexpr u32 SWP_NOSENDCHANGING = 0x0400;

    // SetWindowPos(hwnd, HWND_TOP, 5, 6, 0, 0, SWP_NOSIZE|SWP_NOSENDCHANGING):
    // a move without WM_WINDOWPOSCHANGING still reports WM_WINDOWPOSCHANGED
    // (measured, tools/oracle_probe/s1win_msgseq_probe.c
    // swp_nosendchanging_move). A call that changes nothing sends no
    // WM_WINDOWPOSCHANGED at all (showwindow_show_popup's second CHANGING).
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8  = 5;
    cpu_.gpr.r9  = 6;
    set_fastcall_arg4_6(0, 0, SWP_NOSIZE | SWP_NOSENDCHANGING);

    mgr_->impl_SetWindowPos(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    bool saw_changing = false;
    bool saw_changed = false;
    MSG msg{};
    while (queue_.peek(msg, NULL_HWND, 0, 0, PM::REMOVE)) {
        if (msg.hwnd != hwnd) {
            continue;
        }
        if (msg.message == WM::WINDOWPOSCHANGING) {
            saw_changing = true;
        }
        if (msg.message == WM::WINDOWPOSCHANGED) {
            saw_changed = true;
        }
    }

    EXPECT_FALSE(saw_changing);
    EXPECT_TRUE(saw_changed);
}

TEST_F(User32ExtThunkTest, SetWindowPos_NoSizePreservesDialogClientArea) {
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    wd.rect = windowing::Rect{0, 0, 385, 349};
    wd.client_rect = windowing::Rect{0, 0, 385, 349};
    wd.visible = true;
    HWND hwnd = hwnd_mgr_.create(wd);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    constexpr u32 SWP_NOSIZE = 0x0001;
    constexpr u32 SWP_NOZORDER = 0x0004;

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 767;
    cpu_.gpr.r9 = 350;
    set_fastcall_arg4_6(0, 0, SWP_NOSIZE | SWP_NOZORDER);

    mgr_->impl_SetWindowPos(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    const WindowData* moved = hwnd_mgr_.find(hwnd);
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->rect.left, 767);
    EXPECT_EQ(moved->rect.top, 350);
    EXPECT_EQ(moved->client_rect.width(), 385);
    EXPECT_EQ(moved->client_rect.height(), 349);
}

TEST_F(User32ExtThunkTest, SetWindowPos_ReordersSiblingZOrder) {
    WindowData a{};
    a.parent = HWND_DESKTOP;
    a.style = WS::OVERLAPPEDWINDOW;
    a.rect = windowing::Rect{0, 0, 120, 80};
    HWND hwnd_a = hwnd_mgr_.create(a);

    WindowData b{};
    b.parent = HWND_DESKTOP;
    b.style = WS::OVERLAPPEDWINDOW;
    b.rect = windowing::Rect{10, 10, 140, 90};
    HWND hwnd_b = hwnd_mgr_.create(b);

    ASSERT_NE(hwnd_a, NULL_HWND);
    ASSERT_NE(hwnd_b, NULL_HWND);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 SWP_NOMOVE = 0x0002;
    constexpr u32 SWP_NOSIZE = 0x0001;

    // Move A to the bottom of desktop z-order.
    cpu_.gpr.rcx = hwnd_a;
    cpu_.gpr.rdx = HWND_BOTTOM;
    cpu_.gpr.r8  = 0;
    cpu_.gpr.r9  = 0;
    set_fastcall_arg4_6(0, 0, SWP_NOMOVE | SWP_NOSIZE);
    mgr_->impl_SetWindowPos(m);

    auto children = hwnd_mgr_.enum_children(HWND_DESKTOP);
    ASSERT_FALSE(children.empty());
    EXPECT_EQ(children.back(), hwnd_a);
    EXPECT_NE(children.front(), hwnd_a);
}

TEST_F(User32ExtThunkTest, SetWindowPos_ApplyOnOwnerRestacksWithoutSend) {
    // Cross-process SetWindowPos is applied on the owner (Learn sends or
    // posts to that thread). The owner-side RPC must succeed without
    // entering a guest wndproc from the IPC thread.
    WindowData parent{};
    parent.parent = HWND_DESKTOP;
    parent.style = WS::OVERLAPPEDWINDOW;
    parent.rect = windowing::Rect{0, 0, 400, 300};
    HWND hwnd_parent = hwnd_mgr_.create(parent);

    WindowData sibling{};
    sibling.parent = hwnd_parent;
    sibling.style = WS::CHILD | WS::VISIBLE;
    sibling.rect = windowing::Rect{0, 0, 100, 100};
    HWND hwnd_sibling = hwnd_mgr_.create(sibling);

    WindowData child{};
    child.parent = hwnd_parent;
    child.style = WS::CHILD | WS::VISIBLE;
    child.rect = windowing::Rect{0, 0, 348, 646};
    HWND hwnd_child = hwnd_mgr_.create(child);
    ASSERT_NE(hwnd_sibling, NULL_HWND);
    ASSERT_NE(hwnd_child, NULL_HWND);

    constexpr u32 SWP_NOSIZE = 0x0001;
    constexpr u32 SWP_NOMOVE = 0x0002;
    EXPECT_TRUE(mgr_->apply_set_window_pos(
        hwnd_child, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE, false));
    auto children = hwnd_mgr_.enum_children(hwnd_parent);
    ASSERT_GE(children.size(), 2u);
    EXPECT_EQ(children.front(), hwnd_child);
}

TEST_F(User32ExtThunkTest, SetWindowPos_UnknownHwndReturnsFalse) {
    // Learn: SetWindowPos fails and GetLastError is
    // ERROR_INVALID_WINDOW_HANDLE for a handle that is not a window.
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    set_last_error(error_codes::ERROR_SUCCESS);

    cpu_.gpr.rcx = 0x999;
    cpu_.gpr.rdx = HWND_TOP;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    set_fastcall_arg4_6(0, 0, 0x0001 | 0x0002); // SWP_NOSIZE | SWP_NOMOVE
    mgr_->impl_SetWindowPos(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

// A window that becomes visible through SetWindowPos(SWP_SHOWWINDOW) exposes
// its visible children too: each needs WM_PAINT, as when ShowWindow shows the
// parent. VCL shows a page's controls before the page itself; they never
// painted (Inno Setup's MainPanel stayed unpainted).
TEST_F(User32ExtThunkTest, SetWindowPosShowWindowInvalidatesVisibleChildren) {
    PaintingManager paint{&hwnd_mgr_};
    mgr_->set_painting_manager(&paint);
    WindowData top{};
    top.parent = HWND_DESKTOP;
    top.style = WS::OVERLAPPEDWINDOW | WS::VISIBLE;
    top.visible = true;
    top.rect = windowing::Rect{0, 0, 400, 300};
    top.client_rect = top.rect;
    const HWND hwnd_top = hwnd_mgr_.create(top);
    WindowData page{};
    page.parent = hwnd_top;
    page.style = WS::CHILD;            // hidden
    page.rect = windowing::Rect{0, 0, 300, 200};
    page.client_rect = page.rect;
    const HWND hwnd_page = hwnd_mgr_.create(page);
    WindowData panel{};
    panel.parent = hwnd_page;
    panel.style = WS::CHILD | WS::VISIBLE;
    panel.visible = true;
    panel.rect = windowing::Rect{0, 0, 300, 50};
    panel.client_rect = panel.rect;
    const HWND hwnd_panel = hwnd_mgr_.create(panel);
    WindowData hidden{};
    hidden.parent = hwnd_page;
    hidden.style = WS::CHILD;
    hidden.rect = windowing::Rect{0, 60, 300, 90};
    hidden.client_rect = hidden.rect;
    const HWND hwnd_hidden = hwnd_mgr_.create(hidden);
    (void)paint.validate_rect(hwnd_panel, nullptr);
    EXPECT_FALSE(paint.needs_paint(hwnd_panel));

    constexpr u32 SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOZORDER = 0x0004,
                  SWP_NOACTIVATE = 0x0010, SWP_SHOWWINDOW = 0x0040;
    EXPECT_TRUE(mgr_->apply_set_window_pos(
        hwnd_page, NULL_HWND, 0, 0, 0, 0,
        SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW, false));
    EXPECT_TRUE(paint.needs_paint(hwnd_page));
    EXPECT_TRUE(paint.needs_paint(hwnd_panel));
    EXPECT_FALSE(paint.needs_paint(hwnd_hidden));
    mgr_->set_painting_manager(nullptr);
}
