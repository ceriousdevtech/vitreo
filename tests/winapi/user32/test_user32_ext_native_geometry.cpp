/// @file test_user32_ext_native_geometry.cpp
/// @brief Host-driven (native) resize and gesture geometry, plus bitness-specific
///        window-position notifications: native resize invalidation, SetWindowPos64.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, SubclassingVisibleNativeControlSchedulesInitialPaint) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.visible = true;
    data.style = WS::CHILD | WS::VISIBLE;
    data.wndproc = 0;
    const HWND hwnd = hwnd_mgr_.create(data);

    PaintingManager painting(&hwnd_mgr_);
    mgr_->set_painting_manager(&painting);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = static_cast<u64>(static_cast<i64>(-4)); // GWLP_WNDPROC
    cpu_.gpr.r8 = 0x401000;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_SetWindowLongPtrW(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->wndproc, 0x401000u);
    EXPECT_TRUE(painting.needs_paint(hwnd));
}

TEST_F(User32ExtThunkTest, SetWindowPosShowingPopupInvalidatesUnlessNoRedraw) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.style = WS::POPUP;
    data.rect = {10, 20, 110, 100};
    data.client_rect = {0, 0, 100, 80};
    const HWND hwnd = hwnd_mgr_.create(data);
    PaintingManager painting(&hwnd_mgr_);
    mgr_->set_painting_manager(&painting);
    constexpr u32 placement = 0x0001 | 0x0002 | 0x0004 | 0x0010;
    ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, 0, 0, 0, 0, 0,
                                         placement | 0x0040 | 0x0008));
    EXPECT_NE(hwnd_mgr_.find(hwnd)->style & WS::VISIBLE, 0u);
    EXPECT_FALSE(painting.needs_paint(hwnd));
    ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, 0, 0, 0, 0, 0,
                                         placement | 0x0080));
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->style & WS::VISIBLE, 0u);
    ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, 0, 0, 0, 0, 0,
                                         placement | 0x0040));
    vitreo::winapi::Rect update{};
    ASSERT_TRUE(painting.get_update_rect(hwnd, update, false));
    EXPECT_EQ(update.right - update.left, 100);
    EXPECT_EQ(update.bottom - update.top, 80);
    ASSERT_TRUE(painting.validate_rect(hwnd, nullptr));
    constexpr u32 resize = 0x0002 | 0x0004 | 0x0010;
    ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, 0, 0, 0, 140, 80, resize));
    ASSERT_TRUE(painting.get_update_rect(hwnd, update, false));
    EXPECT_EQ(update.left, 100);
    EXPECT_EQ(update.top, 0);
    EXPECT_EQ(update.right, 140);
    EXPECT_EQ(update.bottom, 80);
    ASSERT_TRUE(painting.validate_rect(hwnd, nullptr));
    ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, 0, 0, 0, 140, 80, resize));
    EXPECT_FALSE(painting.needs_paint(hwnd));
}

TEST_F(User32ExtThunkTest, NativeResizeInvalidatesWithoutPostingPaint) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.visible = true;
    data.rect = windowing::Rect{10, 20, 330, 260};
    data.client_rect = windowing::Rect{0, 0, 320, 240};
    const HWND hwnd = hwnd_mgr_.create(data);

    PaintingManager painting(&hwnd_mgr_);
    mgr_->set_painting_manager(&painting);

    bridge_.on_native_resize(hwnd, 640, 480);

    const WindowData* resized = hwnd_mgr_.find(hwnd);
    ASSERT_NE(resized, nullptr);
    EXPECT_EQ(resized->client_rect.width(), 640);
    EXPECT_EQ(resized->client_rect.height(), 480);
    EXPECT_TRUE(painting.needs_paint(hwnd));
    EXPECT_TRUE(queue_.empty());
}

TEST_F(User32ExtThunkTest,
       ProgrammaticResizeSuppressionDoesNotFeedBackHostFrameGeometry) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.visible = true;
    data.style = WS::CAPTION | WS::THICKFRAME;
    data.rect = windowing::Rect{10, 20, 338, 306};
    data.client_rect = windowing::Rect{0, 0, 320, 240};
    const HWND hwnd = hwnd_mgr_.create(data);

    PaintingManager painting(&hwnd_mgr_);
    mgr_->set_painting_manager(&painting);

    bridge_.set_suppress_resize_notify(true);
    bridge_.on_native_resize(hwnd, 320, 266); // host frame included 26px title bar
    bridge_.on_native_show_state(hwnd, false, true);
    bridge_.set_suppress_resize_notify(false);

    const WindowData* unchanged = hwnd_mgr_.find(hwnd);
    ASSERT_NE(unchanged, nullptr);
    EXPECT_EQ(unchanged->rect.width(), 328);
    EXPECT_EQ(unchanged->rect.height(), 286);
    EXPECT_EQ(unchanged->client_rect.width(), 320);
    EXPECT_EQ(unchanged->client_rect.height(), 240);
    EXPECT_FALSE(unchanged->maximized);
    EXPECT_EQ(unchanged->style & WS::MAXIMIZE, 0u);
    EXPECT_FALSE(painting.needs_paint(hwnd));
    EXPECT_TRUE(queue_.empty());
}

// A Dock restore is the macOS counterpart of a taskbar click: Windows sends
// the minimized window WM_SYSCOMMAND SC_RESTORE so DefWindowProc runs
// ShowWindow(SW_RESTORE) and the guest rect leaves the parked position. A
// host miniaturize the guest did not ask for arrives as SC_MINIMIZE. Both are
// delivered on the owner thread, never from the platform callback.
TEST_F(User32ExtThunkTest, NativeDeminiaturizeAndMiniaturizeArriveAsSysCommand) {
    constexpr u32 sdk_WM_SYSCOMMAND = 0x0112;
    constexpr u64 sdk_SC_MINIMIZE = 0xF020;
    constexpr u64 sdk_SC_RESTORE = 0xF120;
    const u32 thread = ThreadManager::get_current_thread_id_static();
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.owner_thread_id = thread;
    wd.visible = true;
    wd.style = WS::POPUP | WS::THICKFRAME;
    wd.client_drawn_frame = true;
    wd.rect = {10, 20, 330, 260};
    wd.client_rect = {0, 0, 320, 240};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);
    std::vector<u64> commands;
    MessageSendManager send(hwnd_mgr_, queue_, bridge_);
    send.set_wndproc_callback([&](HWND target, u32 message, u64 wp, u64 lp, u64) -> u64 {
        if (message == sdk_WM_SYSCOMMAND) {
            EXPECT_EQ(target, hwnd);
            EXPECT_EQ(lp, 0u);
            commands.push_back(wp);
        }
        return 0;
    });
    mgr_->set_message_send_manager(&send);

    bridge_.on_native_show_state(hwnd, true, false);  // Not minimized in Win32.
    hwnd_mgr_.find(hwnd)->minimized = true;
    bridge_.on_native_show_state(hwnd, false, false);  // Dock restore.
    EXPECT_TRUE(commands.empty());
    mgr_->drain_pending_native_resizes(thread + 1);
    EXPECT_TRUE(commands.empty());
    mgr_->drain_pending_native_resizes(thread);
    EXPECT_EQ(commands, (std::vector<u64>{sdk_SC_MINIMIZE, sdk_SC_RESTORE}));
    mgr_->set_message_send_manager(nullptr);
}

TEST_F(User32ExtThunkTest, SetWindowPos64MarshalsPointerSizedHandlesAndOverrides) {
    mgr_->set_is_64bit(true);
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::POPUP;
    wd.rect = {10,20,210,140}; wd.client_rect = {0,0,200,120};
    wd.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(wd);
    bool changed = false;
    MessageSendManager send(hwnd_mgr_, queue_, bridge_);
    send.set_wndproc_callback([&](HWND, u32 msg, u64, u64 p, u64) -> u64 {
        if (msg == WM::WINDOWPOSCHANGING) {
            EXPECT_EQ(memory_.read_u64(p).value(), hwnd);
            EXPECT_EQ(memory_.read_u64(p+8).value(), 0u);
            EXPECT_EQ(memory_.read_u32(p+16).value(),30u);
            EXPECT_EQ(memory_.read_u32(p+20).value(),40u);
            EXPECT_EQ(memory_.read_u32(p+24).value(),320u);
            EXPECT_EQ(memory_.read_u32(p+28).value(),240u);
            (void)memory_.write_u32(p+24,640);
            (void)memory_.write_u32(p+28,480);
        }
        if (msg == WM::WINDOWPOSCHANGED) {
            changed = true;
            EXPECT_EQ(memory_.read_u64(p).value(),hwnd);
            EXPECT_EQ(memory_.read_u32(p+24).value(),640u);
            EXPECT_EQ(memory_.read_u32(p+28).value(),480u);
            // A hidden window's move + size reports SWP_NOREDRAW too
            // (measured, s1win_msgseq2_probe swp_changing_edits: 0x14 -> 0x1c).
            EXPECT_EQ(memory_.read_u32(p+32).value(),0x1cu);
        }
        return 0;
    });
    mgr_->set_message_send_manager(&send);
    ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, HWND_TOP,30,40,320,240,0x14));
    EXPECT_TRUE(changed);
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->client_rect.width(),640);
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->client_rect.height(),480);
}

TEST_F(User32ExtThunkTest, NestedWindowPositionPreservesOuterNotification) {
    mgr_->set_is_64bit(true);
    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::POPUP;
    wd.rect = {0,0,512,512}; wd.client_rect = wd.rect;
    wd.wndproc = 0x401000;
    const HWND outer = hwnd_mgr_.create(wd);
    const HWND inner = hwnd_mgr_.create(wd);
    MessageSendManager send(hwnd_mgr_, queue_, bridge_);
    bool nested = false;
    send.set_wndproc_callback([&](HWND h, u32 msg, u64, u64 p, u64) -> u64 {
        if (h == outer && msg == WM::WINDOWPOSCHANGED) {
            nested = true;
            EXPECT_TRUE(mgr_->apply_set_window_pos(inner, HWND_TOP,
                                                   0,0,20,30,0x14));
            EXPECT_EQ(memory_.read_u64(p).value(), outer);
            EXPECT_EQ(memory_.read_u32(p+24).value(), 102u);
            EXPECT_EQ(memory_.read_u32(p+28).value(), 100u);
        }
        return 0;
    });
    mgr_->set_message_send_manager(&send);
    EXPECT_TRUE(mgr_->apply_set_window_pos(outer, HWND_TOP,0,0,102,100,0x14));
    EXPECT_TRUE(nested);
}

TEST_F(User32ExtThunkTest, NativeResize64NotifiesWindowPosChangedForGuestFrame) {
    mgr_->set_is_64bit(true);
    WindowData wd{};
    wd.parent=HWND_DESKTOP; wd.style=WS::POPUP | WS::THICKFRAME;
    wd.client_drawn_frame=true; wd.rect={10,20,330,260};
    wd.client_rect={0,0,320,240}; wd.wndproc=0x401000;
    const HWND hwnd=hwnd_mgr_.create(wd);
    MessageSendManager send(hwnd_mgr_,queue_,bridge_);
    bool changed=false;
    send.set_wndproc_callback([&](HWND,u32 msg,u64,u64 p,u64)->u64 {
        if (msg==WM::WINDOWPOSCHANGED) {
            changed=true;
            EXPECT_EQ(memory_.read_u32(p+24).value(),640u);
            EXPECT_EQ(memory_.read_u32(p+28).value(),480u);
        }
        return 0;
    });
    mgr_->set_message_send_manager(&send);
    EXPECT_TRUE(bridge_.on_native_resize(hwnd,600,450));
    EXPECT_TRUE(bridge_.on_native_resize(hwnd,640,480));
    EXPECT_FALSE(changed); // AppKit must never directly enter the guest CPU.
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->client_rect.width(),320);
    const auto owner = hwnd_mgr_.find(hwnd)->owner_thread_id;
    mgr_->drain_pending_native_resizes(owner + 1);
    EXPECT_FALSE(changed);
    mgr_->drain_pending_native_resizes(owner);
    EXPECT_TRUE(changed);
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->rect.width(),640);
    EXPECT_EQ(hwnd_mgr_.find(hwnd)->client_rect.width(),640);
}

TEST_F(User32ExtThunkTest, NativeGestureBoundariesPreserveGeometryAndOwnerBothBitnesses) {
    // Generated SDK constants; move_size_lifecycle_probe.expected confirms
    // one enter/exit and MOVESIZESTART/END, unused parameters zero, both ABIs.
    constexpr u32 sdk_WM_ENTERSIZEMOVE = 0x0231;
    constexpr u32 sdk_WM_EXITSIZEMOVE = 0x0232;
    constexpr u32 sdk_EVENT_SYSTEM_MOVESIZESTART = 0x000A;
    constexpr u32 sdk_EVENT_SYSTEM_MOVESIZEEND = 0x000B;
    const u32 thread = ThreadManager::get_current_thread_id_static();
    for (bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        mgr_->set_is_64bit(is64);
        cpu_ = cpu::CpuState{};
        if (!is64) cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + 0x800);
        Marshaler marshaler(memory_);
        marshaler.attach_cpu(cpu_);
        auto call = [&](const char* name, const std::vector<u64>& args) -> u64 {
            if (is64) {
                cpu_.gpr.rcx = args.size() > 0 ? args[0] : 0;
                cpu_.gpr.rdx = args.size() > 1 ? args[1] : 0;
                cpu_.gpr.r8 = args.size() > 2 ? args[2] : 0;
                cpu_.gpr.r9 = args.size() > 3 ? args[3] : 0;
                for (usize i = 4; i < args.size(); ++i)
                    (void)memory_.write_u64(cpu_.gpr.rsp + 0x28 + (i - 4) * 8, args[i]);
            } else {
                for (usize i = 0; i < args.size(); ++i)
                    (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i]));
            }
            find_export(name).handler(marshaler);
            return cpu_.gpr.rax;
        };
        std::vector<u32> events, messages;
        mgr_->set_guest_call([&](GuestAddr, const u64* args, usize n, bool callback64) -> u64 {
            EXPECT_EQ(n, 7u);
            EXPECT_EQ(callback64, is64);
            EXPECT_EQ(args[3], 0u); // OBJID_WINDOW
            EXPECT_EQ(args[4], 0u); // CHILDID_SELF
            events.push_back(static_cast<u32>(args[1]));
            return 0;
        });
        const u64 hook = call("SetWinEventHook", {sdk_EVENT_SYSTEM_MOVESIZESTART,
            sdk_EVENT_SYSTEM_MOVESIZEEND, 0, 0x12345678, 0, thread, 0});
        ASSERT_NE(hook, 0u);
        WindowData wd{};
        wd.parent = HWND_DESKTOP;
        wd.owner_thread_id = thread;
        wd.style = WS::POPUP | WS::THICKFRAME;
        wd.client_drawn_frame = true;
        wd.rect = {10, 20, 330, 260};
        wd.client_rect = {0, 0, 320, 240};
        wd.wndproc = 0x401000;
        const HWND hwnd = hwnd_mgr_.create(wd);
        MessageSendManager send(hwnd_mgr_, queue_, bridge_);
        send.set_wndproc_callback([&](HWND target, u32 message, u64 wp, u64 lp, u64) -> u64 {
            EXPECT_EQ(target, hwnd);
            if (message == sdk_WM_ENTERSIZEMOVE || message == sdk_WM_EXITSIZEMOVE) {
                EXPECT_EQ(wp, 0u);
                EXPECT_EQ(lp, 0u);
                messages.push_back(message);
            } else if (message == WM::WINDOWPOSCHANGED) messages.push_back(message);
            return 0;
        });
        mgr_->set_message_send_manager(&send);
        bridge_.on_native_move_size(hwnd, true);
        bridge_.on_native_move_size(hwnd, true); // Duplicate host notifications.
        EXPECT_TRUE(bridge_.on_native_resize(hwnd, 600, 450));
        EXPECT_TRUE(bridge_.on_native_resize(hwnd, 640, 480)); // Coalesces within gesture.
        bridge_.on_native_move_size(hwnd, false);
        bridge_.on_native_move_size(hwnd, false);
        bridge_.on_native_move_size(hwnd, true);
        EXPECT_TRUE(bridge_.on_native_resize(hwnd, 300, 200));
        bridge_.on_native_move_size(hwnd, false);
        EXPECT_TRUE(messages.empty()); // Platform callbacks never enter guest code.
        mgr_->drain_pending_native_resizes(thread + 1);
        EXPECT_TRUE(messages.empty());
        mgr_->drain_pending_native_resizes(thread);
        EXPECT_EQ(messages, (std::vector<u32>{sdk_WM_ENTERSIZEMOVE, WM::WINDOWPOSCHANGED,
            sdk_WM_EXITSIZEMOVE, sdk_WM_ENTERSIZEMOVE, WM::WINDOWPOSCHANGED, sdk_WM_EXITSIZEMOVE}));
        EXPECT_EQ(hwnd_mgr_.find(hwnd)->client_rect.width(), 300);
        mgr_->drain_pending_win_events(thread);
        EXPECT_EQ(events, (std::vector<u32>{sdk_EVENT_SYSTEM_MOVESIZESTART,
            sdk_EVENT_SYSTEM_MOVESIZEEND, sdk_EVENT_SYSTEM_MOVESIZESTART, sdk_EVENT_SYSTEM_MOVESIZEEND}));
        EXPECT_EQ(call("UnhookWinEvent", {hook}), 1u);
        mgr_->set_message_send_manager(nullptr);
    }
}

// Microsoft Learn Window Class Styles: redraw only for a changed dimension
// selected by the class; NOREDRAW suppresses it. Both growth and shrink count.
TEST_F(User32ExtThunkTest, ResizeHonorsClassRedrawDimensionsBothBitnesses) {
    for (bool is64 : {false, true}) {
        mgr_->set_is_64bit(is64);
        for (u32 style : {0u, CS::HREDRAW, CS::VREDRAW, CS::HREDRAW | CS::VREDRAW}) {
            for (bool horizontal : {false, true}) {
                for (int extent : {60, 140}) {
                    for (bool no_redraw : {false, true}) {
                        WindowData data{};
                        data.parent = HWND_DESKTOP;
                        data.style = WS::POPUP | WS::VISIBLE;
                        data.visible = true;
                        data.class_style = style;
                        data.client_drawn_frame = true;
                        data.rect = {0, 0, 100, 100};
                        data.client_rect = data.rect;
                        const HWND hwnd = hwnd_mgr_.create(data);
                        PaintingManager painting(&hwnd_mgr_);
                        mgr_->set_painting_manager(&painting);
                        const int w = horizontal ? extent : 100;
                        const int h = horizontal ? 100 : extent;
                        ASSERT_TRUE(mgr_->apply_set_window_pos(hwnd, 0, 0, 0, w, h,
                            0x0002 | 0x0004 | 0x0010 | (no_redraw ? 0x0008 : 0)));
                        vitreo::winapi::Rect update{};
                        const bool full = (style & (horizontal ? CS::HREDRAW : CS::VREDRAW)) != 0;
                        const bool dirty = painting.get_update_rect(hwnd, update, false);
                        EXPECT_EQ(dirty, !no_redraw && (full || extent > 100));
                        if (dirty) {
                            EXPECT_EQ(update.left, full || !horizontal ? 0 : 100);
                            EXPECT_EQ(update.top, full || horizontal ? 0 : 100);
                            EXPECT_EQ(update.right, w);
                            EXPECT_EQ(update.bottom, h);
                        }
                        mgr_->set_painting_manager(nullptr);
                    }
                }
            }
        }
    }
}
