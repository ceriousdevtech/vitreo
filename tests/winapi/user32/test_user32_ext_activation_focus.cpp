/// @file test_user32_ext_activation_focus.cpp
/// @brief Window activation and keyboard-focus sequences: SetActiveWindow, WM_ACTIVATE*,
///        WM_SETFOCUS/WM_KILLFOCUS, dialog focus restore, SetFocus.

#include "user32_ext_thunk_harness.hpp"

// Expected sequences: tools/oracle_probe/activation_order_probe{64,32}.expected
// (same-thread activation). The oracle session had no foreground window, so
// WM_NCACTIVATE wParam there is always 0; the values below use Learn
// WM_NCACTIVATE (TRUE when the caption is drawn active).
TEST_F(User32ExtThunkTest, SetActiveWindowAndDestroyFollowOracleSequence) {
    WindowData a_data{};
    a_data.parent = HWND_DESKTOP;
    a_data.wndproc = 0x401000;
    a_data.visible = true;
    a_data.enabled = true;
    const HWND a = hwnd_mgr_.create(a_data);

    WindowData child_data = a_data;
    child_data.parent = a;
    const HWND child = hwnd_mgr_.create(child_data);

    DefWindowProcManager dwp(hwnd_mgr_, queue_, memory_);
    dwp.set_focus_window(
        [&](HWND hwnd) { (void)mgr_->set_keyboard_focus(hwnd, true); });

    struct Sent {
        HWND hwnd;
        u32 message;
        u64 wparam;
        u64 lparam;
        bool operator==(const Sent&) const = default;
    };
    std::vector<Sent> sent;
    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64 lparam, u64) -> u64 {
            sent.push_back({target, message, wparam, lparam});
            if (message == WM::ACTIVATE) {
                return dwp.def_window_proc(target, message, wparam, lparam);
            }
            return 0;
        });
    mgr_->set_message_send_manager(&message_send);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    const auto set_active = [&](u64 hwnd) {
        cpu_.gpr.rcx = hwnd;
        mgr_->impl_SetActiveWindow(m);
        return cpu_.gpr.rax;
    };

    EXPECT_EQ(set_active(a), NULL_HWND);
    EXPECT_EQ(sent, (std::vector<Sent>{
                        {a, WM::ACTIVATEAPP, 1, 0},
                        {a, WM::NCACTIVATE, 1, 0},
                        {a, WM::ACTIVATE, WA::ACTIVE, 0},
                        {a, WM::SETFOCUS, 0, 0}}));

    WindowData b_data = a_data;
    b_data.wndproc = 0x402000;
    const HWND b = hwnd_mgr_.create(b_data);

    sent.clear();
    EXPECT_EQ(set_active(b), a);
    EXPECT_EQ(sent, (std::vector<Sent>{
                        {a, WM::NCACTIVATE, 0, b},
                        {a, WM::ACTIVATE, WA::INACTIVE, b},
                        {b, WM::NCACTIVATE, 1, a},
                        {b, WM::ACTIVATE, WA::ACTIVE, a},
                        {a, WM::KILLFOCUS, b, 0},
                        {b, WM::SETFOCUS, a, 0}}));

    sent.clear();
    set_last_error(0xDEAD);
    EXPECT_EQ(set_active(0x7FFF0), NULL_HWND);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    EXPECT_EQ(set_active(child), b);
    EXPECT_EQ(set_active(NULL_HWND), NULL_HWND);
    EXPECT_TRUE(sent.empty());

    sent.clear();
    mgr_->release_activation(b, true);
    hwnd_mgr_.destroy(b);
    EXPECT_EQ(sent, (std::vector<Sent>{
                        {b, WM::NCACTIVATE, 0, a},
                        {b, WM::ACTIVATE, WA::INACTIVE, a},
                        {a, WM::NCACTIVATE, 1, b},
                        {a, WM::ACTIVATE, WA::ACTIVE, b},
                        {b, WM::KILLFOCUS, a, 0},
                        {a, WM::SETFOCUS, b, 0}}));

    sent.clear();
    mgr_->release_activation(a, true);
    EXPECT_TRUE(sent.empty());
    mgr_->impl_GetActiveWindow(m);
    EXPECT_EQ(cpu_.gpr.rax, NULL_HWND);
    mgr_->impl_GetFocus(m);
    EXPECT_EQ(cpu_.gpr.rax, NULL_HWND);
    mgr_->set_message_send_manager(nullptr);
    (void)bridge_.exchange_app_active(false);
}

// Native Windows winevent_sequence_probe.expected keeps the owner active
// when its never-active popup is destroyed. Check the guest state in both modes.
TEST_F(User32ExtThunkTest, InactivePopupDestructionKeepsOracleActivation) {
    WindowManager windows(hwnd_mgr_, class_mgr_, bridge_, queue_);
    windows.set_destroy_activation_callback(
        [&](HWND hwnd) { mgr_->release_activation(hwnd, true); });
    MessageSendManager send(hwnd_mgr_, queue_, bridge_);
    send.set_wndproc_callback([](HWND, u32, u64, u64, u64) -> u64 { return 0; });
    mgr_->set_message_send_manager(&send);
    for (bool wide : {false, true}) {
        if (wide) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        WindowData data{};
        data.parent = HWND_DESKTOP;
        data.style = WS::OVERLAPPEDWINDOW | WS::VISIBLE;
        data.visible = true;
        data.enabled = true;
        data.wndproc = 0x401000;
        const HWND owner = hwnd_mgr_.create(data);
        const HWND other = hwnd_mgr_.create(data);
        data.style = WS::POPUP | WS::VISIBLE;
        data.owner = owner;
        // SDK winuser.h WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW.
        data.ex_style = 0x08000000u | 0x00000080u;
        const HWND popup = hwnd_mgr_.create(data);
        ASSERT_TRUE(mgr_->change_active_window(other, WA::ACTIVE));
        ASSERT_TRUE(windows.destroy_window(popup));
        mgr_->impl_GetActiveWindow(m);
        EXPECT_EQ(cpu_.gpr.rax, other);
        ASSERT_TRUE(windows.destroy_window(owner));
        ASSERT_TRUE(windows.destroy_window(other));
    }
    mgr_->set_message_send_manager(nullptr);
    (void)bridge_.exchange_app_active(false);
}

// Learn DefDlgProc WM_ACTIVATE: focus returns to the control that had it when
// the dialog was deactivated.
TEST_F(User32ExtThunkTest, DialogActivationRestoresSavedControlFocus) {
    WindowData dlg_data{};
    dlg_data.parent = HWND_DESKTOP;
    dlg_data.wndproc = 0x401000;
    dlg_data.visible = true;
    dlg_data.enabled = true;
    dlg_data.is_dialog = true;
    const HWND dlg = hwnd_mgr_.create(dlg_data);
    WindowData edit_data = dlg_data;
    edit_data.is_dialog = false;
    edit_data.parent = dlg;
    const HWND edit = hwnd_mgr_.create(edit_data);
    WindowData other_data = dlg_data;
    other_data.is_dialog = false;
    const HWND other = hwnd_mgr_.create(other_data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    message_send.set_wndproc_callback(
        [](HWND, u32, u64, u64, u64) -> u64 { return 0; });
    mgr_->set_message_send_manager(&message_send);

    ASSERT_TRUE(mgr_->change_active_window(dlg, WA::ACTIVE));
    ASSERT_TRUE(mgr_->set_keyboard_focus(edit, true));
    ASSERT_TRUE(mgr_->change_active_window(other, WA::ACTIVE));
    (void)mgr_->set_keyboard_focus(other, true);
    ASSERT_TRUE(mgr_->change_active_window(dlg, WA::ACTIVE));

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    mgr_->impl_GetFocus(m);
    EXPECT_EQ(cpu_.gpr.rax, edit);
    mgr_->set_message_send_manager(nullptr);
    (void)bridge_.exchange_app_active(false);
}

TEST_F(User32ExtThunkTest,
       ApplicationActivationIsSentOnceToEveryTopLevelWindow) {
    WindowData first_data{};
    first_data.parent = HWND_DESKTOP;
    first_data.wndproc = 0x401000;
    const HWND first = hwnd_mgr_.create(first_data);

    WindowData second_data = first_data;
    second_data.wndproc = 0x402000;
    const HWND second = hwnd_mgr_.create(second_data);

    WindowData child_data = first_data;
    child_data.parent = first;
    child_data.wndproc = 0x403000;
    (void)hwnd_mgr_.create(child_data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<std::tuple<HWND, u32, u64>> recipients;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64 lparam, u64) {
            EXPECT_EQ(lparam, 0u);
            recipients.emplace_back(target, message, wparam);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    EXPECT_TRUE(mgr_->activate_application(first));
    EXPECT_FALSE(mgr_->activate_application(first));
    EXPECT_TRUE(mgr_->application_active());
    EXPECT_TRUE(mgr_->set_application_active(first, false));
    EXPECT_FALSE(mgr_->set_application_active(first, false));
    EXPECT_FALSE(mgr_->application_active());
    // WM_ACTIVATEAPP reaches top-levels in Z-order (newest on top).
    EXPECT_EQ(recipients,
              (std::vector<std::tuple<HWND, u32, u64>>{
                  {second, WM::ACTIVATEAPP, 1},
                  {first, WM::ACTIVATEAPP, 1},
                  {first, WM::NCACTIVATE, 1},
                  {first, WM::ACTIVATE, WA::ACTIVE},
                  {first, WM::NCACTIVATE, 0},
                  {first, WM::ACTIVATE, WA::INACTIVE},
                  {second, WM::ACTIVATEAPP, 0},
                  {first, WM::ACTIVATEAPP, 0}}));
    EXPECT_TRUE(queue_.empty());
    EXPECT_FALSE(bridge_.exchange_app_active(false));
}

// An AppKit-originated activation change is delivered from the host main
// thread, which must never block on a guest thread: the guest thread may be in
// WaitForSingleObject and not pumping (a deactivation during Satisfactory's
// load spun the main thread forever in that send). Under NativeDeliveryScope,
// another thread's window gets the sequence queued as sent messages
// (SendNotifyMessage semantics, Learn nf-winuser-sendnotifymessagew): nothing
// runs during the call, and draining the owner's sent messages delivers the
// same sequence, in the same order, as the synchronous path.
TEST_F(User32ExtThunkTest, NativeActivationQueuesSentMessagesForAnotherThread) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();
    ThreadManager::set_current_thread_id(5);
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.wndproc = 0x401000;
    data.owner_thread_id = 5;
    const HWND window = hwnd_mgr_.create(data);
    ASSERT_NE(window, NULL_HWND);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<std::tuple<HWND, u32, u64>> recipients;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            EXPECT_EQ(ThreadManager::get_current_thread_id_static(), 5u)
                << "a queued activation message ran off its owner thread";
            recipients.emplace_back(target, message, wparam);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    ThreadManager::set_current_thread_id(9);  // the delivering host thread
    {
        User32ExtManager::NativeDeliveryScope native;
        EXPECT_TRUE(mgr_->set_application_active(window, true));
        EXPECT_TRUE(mgr_->set_application_active(window, false));
    }
    EXPECT_TRUE(recipients.empty())
        << "a native activation waited on another thread's window";

    ThreadManager::set_current_thread_id(5);
    EXPECT_TRUE(queue_.dispatch_sent_messages(5));
    EXPECT_EQ(recipients,
              (std::vector<std::tuple<HWND, u32, u64>>{
                  {window, WM::ACTIVATEAPP, 1},
                  {window, WM::NCACTIVATE, 1},
                  {window, WM::ACTIVATE, WA::ACTIVE},
                  {window, WM::NCACTIVATE, 0},
                  {window, WM::ACTIVATE, WA::INACTIVE},
                  {window, WM::ACTIVATEAPP, 0}}));

    mgr_->set_message_send_manager(nullptr);
    (void)bridge_.exchange_app_active(false);
    ThreadManager::set_current_thread_id(previous_tid);
}

TEST_F(User32ExtThunkTest, ActivatingShowWindowUpdatesGetActiveWindowSynchronously) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.enabled = true;
    data.visible = false;
    const HWND hwnd = hwnd_mgr_.create(data);
    ASSERT_NE(hwnd, NULL_HWND);

    ShowWindowManager show_mgr(hwnd_mgr_, bridge_, queue_);
    show_mgr.set_user32_ext_manager(mgr_.get());
    show_mgr.show_window(hwnd, SW::SHOWNORMAL);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_GetActiveWindow(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, hwnd);
}

TEST_F(User32ExtThunkTest,
       NativeKeyboardFocusTransitionsAreSentAndDeduplicated) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<std::pair<u32, u64>> messages;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            EXPECT_EQ(target, hwnd);
            messages.emplace_back(message, wparam);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    EXPECT_TRUE(mgr_->set_keyboard_focus(hwnd, true));
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), hwnd);
    EXPECT_FALSE(mgr_->set_keyboard_focus(hwnd, true));
    EXPECT_TRUE(mgr_->set_keyboard_focus(hwnd, false));
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), NULL_HWND);
    EXPECT_FALSE(mgr_->set_keyboard_focus(hwnd, false));
    EXPECT_EQ(messages,
              (std::vector<std::pair<u32, u64>>{
                  {WM::SETFOCUS, 0}, {WM::KILLFOCUS, 0}}));
    EXPECT_TRUE(queue_.empty());
}

TEST_F(User32ExtThunkTest,
       NativeKeyboardFocusRestoresFocusedChildOfReactivatedTopLevel) {
    WindowData top_data{};
    top_data.parent = HWND_DESKTOP;
    top_data.wndproc = 0x401000;
    const HWND top = hwnd_mgr_.create(top_data);

    WindowData child_data{};
    child_data.parent = top;
    child_data.wndproc = 0x402000;
    const HWND child = hwnd_mgr_.create(child_data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    struct FocusMessage {
        HWND target;
        u32 message;
        u64 other;
        bool operator==(const FocusMessage&) const = default;
    };
    std::vector<FocusMessage> messages;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            messages.push_back({target, message, wparam});
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    ASSERT_TRUE(mgr_->set_keyboard_focus(child, true));
    ASSERT_TRUE(mgr_->set_native_keyboard_focus(top, false));
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), NULL_HWND);
    ASSERT_TRUE(mgr_->set_native_keyboard_focus(top, true));
    EXPECT_EQ(mgr_->m_focus_hwnd, child);
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), child);
    EXPECT_EQ(messages,
              (std::vector<FocusMessage>{
                  {child, WM::SETFOCUS, NULL_HWND},
                  {child, WM::KILLFOCUS, NULL_HWND},
                  {child, WM::SETFOCUS, NULL_HWND}}));
    EXPECT_TRUE(queue_.empty());
}

TEST_F(User32ExtThunkTest,
       NativeKeyboardFocusDoesNotStealExistingChildFocus) {
    // Re-entrant become-key (pending activation / restack) must not force
    // SetFocus(top) when a child already owns keyboard focus — that would
    // send WM_KILLFOCUS and drop edit/omnibox caret focus on keypress.
    WindowData top_data{};
    top_data.parent = HWND_DESKTOP;
    top_data.wndproc = 0x401000;
    const HWND top = hwnd_mgr_.create(top_data);

    WindowData child_data{};
    child_data.parent = top;
    child_data.wndproc = 0x402000;
    const HWND child = hwnd_mgr_.create(child_data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    struct FocusMessage {
        HWND target;
        u32 message;
        u64 other;
        bool operator==(const FocusMessage&) const = default;
    };
    std::vector<FocusMessage> messages;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            messages.push_back({target, message, wparam});
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    ASSERT_TRUE(mgr_->set_keyboard_focus(child, true));
    messages.clear();
    EXPECT_FALSE(mgr_->set_native_keyboard_focus(top, true));
    EXPECT_EQ(mgr_->m_focus_hwnd, child);
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), child);
    EXPECT_TRUE(messages.empty());
}

TEST_F(User32ExtThunkTest,
       NativeKeyboardFocusDoesNotStealOwnedPopupFocus) {
    // Owned WS_POPUP windows are top-level (parent = desktop). Become-key
    // on the owner must not SetFocus(owner) while the popup has focus.
    // Become-key on the popup itself still activates it (SetFocus).
    WindowData top_data{};
    top_data.parent = HWND_DESKTOP;
    top_data.wndproc = 0x401000;
    const HWND top = hwnd_mgr_.create(top_data);

    WindowData child_data{};
    child_data.parent = top;
    child_data.wndproc = 0x402000;
    const HWND child = hwnd_mgr_.create(child_data);

    WindowData popup_data{};
    popup_data.parent = HWND_DESKTOP;
    popup_data.owner = top;
    popup_data.style = WS::POPUP | WS::VISIBLE;
    popup_data.wndproc = 0x403000;
    const HWND popup = hwnd_mgr_.create(popup_data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    struct FocusMessage {
        HWND target;
        u32 message;
        u64 other;
        bool operator==(const FocusMessage&) const = default;
    };
    std::vector<FocusMessage> messages;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            messages.push_back({target, message, wparam});
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    ASSERT_TRUE(mgr_->set_keyboard_focus(popup, true));
    messages.clear();
    EXPECT_FALSE(mgr_->set_native_keyboard_focus(top, true));
    EXPECT_EQ(mgr_->m_focus_hwnd, popup);
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), popup);
    EXPECT_TRUE(messages.empty());

    ASSERT_TRUE(mgr_->set_keyboard_focus(child, true));
    messages.clear();
    EXPECT_TRUE(mgr_->set_native_keyboard_focus(popup, true));
    EXPECT_EQ(mgr_->m_focus_hwnd, popup);
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), popup);
    EXPECT_EQ(messages,
              (std::vector<FocusMessage>{
                  {child, WM::KILLFOCUS, popup},
                  {popup, WM::SETFOCUS, child}}));
}

// A click on the window that is already active: Learn WM_MOUSEACTIVATE is
// "sent when the cursor is in an inactive window", and WM_ACTIVATE /
// WM_ACTIVATEAPP / WM_SETFOCUS accompany an activation change only. The
// native click path (AppKit key window unchanged, or a repeated become-key)
// reaches USER32 as set_application_active(hwnd, true, by_mouse): it must
// send nothing and keep the focus.
TEST_F(User32ExtThunkTest, ClickOnTheActiveWindowSendsNoActivationMessages) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.wndproc = 0x401000;
    data.style = WS::OVERLAPPEDWINDOW | WS::VISIBLE;
    data.visible = true;
    const HWND window = hwnd_mgr_.create(data);
    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<std::pair<u32, u64>> sent;
    message_send.set_wndproc_callback(
        [&](HWND, u32 message, u64 wparam, u64, u64) {
            sent.emplace_back(message, wparam);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    EXPECT_TRUE(mgr_->set_application_active(window, true, 0, true));
    EXPECT_EQ(sent, (std::vector<std::pair<u32, u64>>{
                        {WM::ACTIVATEAPP, 1},
                        {WM::NCACTIVATE, 1},
                        {WM::ACTIVATE, WA::CLICKACTIVE}}));
    EXPECT_TRUE(mgr_->set_keyboard_focus(window, true));
    sent.clear();

    EXPECT_FALSE(mgr_->set_application_active(window, true, 0, true));
    EXPECT_FALSE(mgr_->set_application_active(window, true, 0, false));
    EXPECT_TRUE(sent.empty());
    EXPECT_TRUE(mgr_->application_active());
    EXPECT_EQ(mgr_->keyboard_focus_window(), window);
    EXPECT_TRUE(queue_.empty());
    (void)mgr_->set_application_active(window, false);
    mgr_->set_message_send_manager(nullptr);
    (void)bridge_.exchange_app_active(false);
}

// =============================================================================
// SetFocus Tests
// =============================================================================

TEST_F(User32ExtThunkTest, SetFocus_ReturnsAndTracksPreviousHwnd) {
    WindowData first_data{};
    first_data.parent = HWND_DESKTOP;
    first_data.enabled = true;
    first_data.wndproc = 0x401000;
    const HWND first = hwnd_mgr_.create(first_data);

    WindowData second_data = first_data;
    second_data.wndproc = 0x402000;
    const HWND second = hwnd_mgr_.create(second_data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    struct FocusMessage {
        HWND target;
        u32 message;
        u64 other;
        bool operator==(const FocusMessage&) const = default;
    };
    std::vector<FocusMessage> messages;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            messages.push_back({target, message, wparam});
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    auto entry = find_export("SetFocus");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // First call: no previous focus → returns 0
    cpu_.gpr.rcx = first;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u); // no previous
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), first);

    // Second call returns the old focus and performs the synchronous handoff.
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = second;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, first);
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), second);

    // NULL clears focus and returns the window that lost it.
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = NULL_HWND;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, second);
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), NULL_HWND);
    EXPECT_EQ(messages,
              (std::vector<FocusMessage>{
                  {first, WM::SETFOCUS, NULL_HWND},
                  {first, WM::KILLFOCUS, second},
                  {second, WM::SETFOCUS, first},
                  {second, WM::KILLFOCUS, NULL_HWND}}));
    EXPECT_TRUE(queue_.empty());
}

// Focus belongs to a thread's input queue: a guest thread moving focus to its
// own window never waits on the thread that owned the previous focus window
// (that thread may be in WaitForSingleObject and not retrieving messages).
// The new window gets WM_SETFOCUS synchronously; the other thread's window
// gets WM_KILLFOCUS when its owner next retrieves sent messages
// (SendNotifyMessage semantics, Learn nf-winuser-sendnotifymessagew).
TEST_F(User32ExtThunkTest, SetFocusDoesNotWaitOnPreviousFocusOwnerThread) {
    const u32 previous_tid = ThreadManager::get_current_thread_id_static();

    WindowData other_data{};
    other_data.parent = HWND_DESKTOP;
    other_data.enabled = true;
    other_data.wndproc = 0x401000;
    other_data.owner_thread_id = 5;
    const HWND other = hwnd_mgr_.create(other_data);
    WindowData mine_data = other_data;
    mine_data.wndproc = 0x402000;
    mine_data.owner_thread_id = 7;
    const HWND mine = hwnd_mgr_.create(mine_data);
    ASSERT_NE(other, NULL_HWND);
    ASSERT_NE(mine, NULL_HWND);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<std::tuple<HWND, u32, u64, u32>> delivered;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64, u64) {
            delivered.emplace_back(target, message, wparam,
                                   ThreadManager::get_current_thread_id_static());
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    ThreadManager::set_current_thread_id(5);
    ASSERT_TRUE(mgr_->set_keyboard_focus(other, true));
    delivered.clear();

    ThreadManager::set_current_thread_id(7);
    ASSERT_TRUE(mgr_->set_keyboard_focus(mine, true));
    EXPECT_EQ(bridge_.keyboard_focus_hwnd(), mine);
    EXPECT_EQ(delivered,
              (std::vector<std::tuple<HWND, u32, u64, u32>>{
                  {mine, WM::SETFOCUS, other, 7u}}))
        << "SetFocus waited on another thread's window";

    delivered.clear();
    ThreadManager::set_current_thread_id(5);
    EXPECT_TRUE(queue_.dispatch_sent_messages(5));
    EXPECT_EQ(delivered,
              (std::vector<std::tuple<HWND, u32, u64, u32>>{
                  {other, WM::KILLFOCUS, mine, 5u}}));

    mgr_->set_message_send_manager(nullptr);
    ThreadManager::set_current_thread_id(previous_tid);
}
