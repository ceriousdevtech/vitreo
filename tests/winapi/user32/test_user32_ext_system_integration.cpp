/// @file test_user32_ext_system_integration.cpp
/// @brief Clipboard, WinEvent and shell hooks, registered messages, message filters,
///        power-setting notifications and process DPI awareness.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, ClipboardReadsMovableAllocationAndReleasesOwnership) {
    VirtualMemoryManager vmm(memory_);
    HandleTable handles;
    HeapManager heap(vmm, memory_, handles);
    GlobalLocalManager globals(heap);
    mgr_->set_global_local_manager(&globals);
    const auto previous = bridge_.clipboard_get_text();
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    // SetClipboardData/EmptyClipboard need the clipboard open
    // (tools/oracle_probe/s2msg_input_probe.expected
    // setclipboarddata_not_open: NULL / ERROR_CLIPBOARD_NOT_OPEN).
    cpu_.gpr.rcx = 13;
    cpu_.gpr.rdx = 0;
    find_export("SetClipboardData").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CLIPBOARD_NOT_OPEN);
    cpu_.gpr.rcx = 0;
    find_export("OpenClipboard").handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    for (u32 format : {1u, 13u}) {
        const u64 handle = globals.global_alloc(GHND, 64);
        const GuestAddr ptr = globals.global_lock(handle);
        ASSERT_NE(ptr, 0u);
        ASSERT_NE(ptr, handle);
        if (format == 13) ASSERT_TRUE(m.write_wide_string(ptr, "Clipboard text").is_ok());
        else ASSERT_TRUE(m.write_ansi_string(ptr, "Clipboard text").is_ok());
        (void)globals.global_unlock(handle);
        cpu_.gpr.rcx = format;
        cpu_.gpr.rdx = handle;
        find_export("SetClipboardData").handler(m);
        EXPECT_EQ(cpu_.gpr.rax, handle);
        EXPECT_EQ(bridge_.clipboard_get_text(), "Clipboard text");
        EXPECT_NE(globals.global_size(handle), 0u);
        find_export("EmptyClipboard").handler(m);
        EXPECT_EQ(globals.global_size(handle), 0u);
    }
    cpu_.gpr.rcx = 13;
    cpu_.gpr.rdx = 0xdeadbeef;
    find_export("SetClipboardData").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    find_export("CloseClipboard").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    (void)bridge_.clipboard_set_text(previous);
    mgr_->set_global_local_manager(nullptr);
}

TEST_F(User32ExtThunkTest, WinEventHookReceivesMatchingWindowEvent) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto set_win_event_hook = find_export("SetWinEventHook");
    ASSERT_TRUE(static_cast<bool>(set_win_event_hook.handler));
    GuestAddr called_proc = 0;
    std::vector<u64> called_args;
    mgr_->set_guest_call(
        [&](GuestAddr proc, const u64* args, usize count, bool is_64bit) {
            called_proc = proc;
            called_args.assign(args, args + count);
            EXPECT_TRUE(is_64bit);
            return 0u;
        });

    cpu_.gpr.rcx = 0x0003u; // EVENT_SYSTEM_FOREGROUND
    cpu_.gpr.rdx = 0x8002u; // EVENT_OBJECT_SHOW
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0x12345678u; // WINEVENTPROC
    set_fastcall_arg4_6(0, 0, 0); // all processes/threads, OUTOFCONTEXT
    set_last_error(error_codes::ERROR_SUCCESS);

    set_win_event_hook.handler(marshaler);
    const u64 hook = cpu_.gpr.rax;
    ASSERT_NE(hook, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    mgr_->notify_win_event(0x8002u, 0x2345u);
    // Learn: WINEVENT_OUTOFCONTEXT delivers on the registering thread's
    // message loop, not synchronously on the event-generating thread.
    EXPECT_EQ(called_proc, 0u);
    mgr_->drain_pending_win_events(
        ThreadManager::get_current_thread_id_static());
    EXPECT_EQ(called_proc, 0x12345678u);
    ASSERT_EQ(called_args.size(), 7u);
    EXPECT_EQ(called_args[0], hook);
    EXPECT_EQ(called_args[1], 0x8002u);
    EXPECT_EQ(called_args[2], 0x2345u);
    EXPECT_EQ(called_args[3], 0u); // OBJID_WINDOW
    EXPECT_EQ(called_args[4], 0u); // CHILDID_SELF
}

// tools/oracle_probe/winevent_sequence_probe.expected (Windows 11): a move or
// size raises EVENT_OBJECT_LOCATIONCHANGE, which is 0x800B (WinUser.h; 0x800A
// is STATECHANGE, which Vitreo used to send), and a real sibling-order change
// raises EVENT_OBJECT_REORDER on the desktop with OBJID_CLIENT. Occlusion
// trackers recompute window coverage from exactly these two events.
TEST_F(User32ExtThunkTest, SetWindowPosRaisesLocationChangeAndReorderWinEvents) {
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    std::vector<std::array<u64, 3>> events;  // event, hwnd, idObject
    mgr_->set_guest_call(
        [&](GuestAddr, const u64* args, usize count, bool) {
            if (count == 7) events.push_back({args[1], args[2], args[3]});
            return 0u;
        });
    cpu_.gpr.rcx = 0x00000001u;  // EVENT_MIN
    cpu_.gpr.rdx = 0x7FFFFFFFu;  // EVENT_MAX
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0x12345678u;
    set_fastcall_arg4_6(0, 0, 0);
    find_export("SetWinEventHook").handler(marshaler);
    ASSERT_NE(cpu_.gpr.rax, 0u);

    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.visible = true;
    data.style = 0x10000000u;  // WS_VISIBLE
    data.rect = {100, 100, 300, 200};
    data.client_rect = {0, 0, 200, 100};
    const HWND first = hwnd_mgr_.create(data);
    const HWND second = hwnd_mgr_.create(data);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    mgr_->drain_pending_win_events(tid);
    events.clear();

    const auto set_window_pos = [&](HWND hwnd, u64 after, i32 x, i32 y, u32 flags) {
        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = after;
        cpu_.gpr.r8 = static_cast<u64>(x);
        cpu_.gpr.r9 = static_cast<u64>(y);
        set_fastcall_arg4_6(0, 0, flags);
        find_export("SetWindowPos").handler(marshaler);
        ASSERT_EQ(cpu_.gpr.rax, 1u);
        mgr_->drain_pending_win_events(tid);
    };
    // Move only: SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE.
    set_window_pos(first, 0, 160, 160, 0x0001u | 0x0004u | 0x0010u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0][0], 0x800Bu);
    EXPECT_EQ(events[0][1], first);
    EXPECT_EQ(events[0][2], 0u);  // OBJID_WINDOW
    events.clear();

    // Reorder only: `second` was created last and is on top, so raising
    // `first` (HWND_TOP = 0) changes the order.
    ASSERT_EQ(hwnd_mgr_.enum_children(HWND_DESKTOP).front(), second);
    set_window_pos(first, 0, 0, 0, 0x0001u | 0x0002u | 0x0010u);
    ASSERT_EQ(hwnd_mgr_.enum_children(HWND_DESKTOP).front(), first);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0][0], 0x8004u);  // EVENT_OBJECT_REORDER
    EXPECT_EQ(events[0][1], HWND_DESKTOP);
    EXPECT_EQ(static_cast<u32>(events[0][2]), 0xFFFFFFFCu);  // OBJID_CLIENT
    events.clear();

    // No order change (already on top): no REORDER, as measured
    // ("SetWindowPos(owner, HWND_TOP)" with the owner already top).
    set_window_pos(first, 0, 0, 0, 0x0001u | 0x0002u | 0x0010u);
    EXPECT_TRUE(events.empty());
}

TEST_F(User32ExtThunkTest, RegisteredMessagesShareStableAnsiUnicodeNamespace) {
    constexpr GuestAddr ansi_name = BUF_ADDR;
    constexpr GuestAddr wide_name = BUF_ADDR + 0x100;
    constexpr GuestAddr other_name = BUF_ADDR + 0x200;
    constexpr char shell_hook[] = "SHELLHOOK";
    constexpr char other[] = "Vitreo.Other.Message";
    ASSERT_TRUE(memory_.write_bytes(
        ansi_name, reinterpret_cast<const u8*>(shell_hook),
        sizeof(shell_hook)).is_ok());
    ASSERT_TRUE(memory_.write_bytes(
        other_name, reinterpret_cast<const u8*>(other), sizeof(other)).is_ok());
    for (usize i = 0; i < sizeof(shell_hook); ++i) {
        ASSERT_TRUE(memory_.write_u16(
            wide_name + i * 2,
            static_cast<u16>(shell_hook[i])).is_ok());
    }

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    cpu_.gpr.rcx = ansi_name;
    find_export("RegisterWindowMessageA").handler(marshaler);
    const u64 ansi_id = cpu_.gpr.rax;

    cpu_.gpr.rcx = wide_name;
    find_export("RegisterWindowMessageW").handler(marshaler);
    const u64 wide_id = cpu_.gpr.rax;

    cpu_.gpr.rcx = other_name;
    find_export("RegisterWindowMessageA").handler(marshaler);
    const u64 other_id = cpu_.gpr.rax;

    EXPECT_EQ(ansi_id, 0xC000u);
    EXPECT_EQ(wide_id, ansi_id);
    EXPECT_EQ(other_id, ansi_id + 1);
}

TEST_F(User32ExtThunkTest, ShellHookRegistrationReceivesLifecycleMessage) {
    WindowData receiver_data{};
    const HWND receiver = hwnd_mgr_.create(receiver_data);
    WindowData subject_data{};
    const HWND subject = hwnd_mgr_.create(subject_data);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    cpu_.gpr.rcx = receiver;
    find_export("RegisterShellHookWindow").handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);

    mgr_->notify_shell_event(1, subject);
    MSG message{};
    ASSERT_TRUE(queue_.get(message));
    EXPECT_EQ(message.hwnd, receiver);
    EXPECT_EQ(message.message, 0xC000u);
    EXPECT_EQ(message.wParam, 1u);
    EXPECT_EQ(message.lParam, subject);

    cpu_.gpr.rcx = receiver;
    find_export("DeregisterShellHookWindow").handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    mgr_->notify_shell_event(2, subject);
    EXPECT_TRUE(queue_.empty());
}

TEST_F(User32ExtThunkTest, RegisterShellHookWindowRejectsInvalidHandle) {
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = 0xDEADBEEF;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    find_export("RegisterShellHookWindow").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

TEST_F(User32ExtThunkTest, ClipboardViewerChainPreservesStdcallAndMessageContracts) {
    WindowData first_data{};
    const HWND first = hwnd_mgr_.create(first_data);
    WindowData second_data{};
    const HWND second = hwnd_mgr_.create(second_data);
    ASSERT_NE(first, NULL_HWND);
    ASSERT_NE(second, NULL_HWND);

    auto set_viewer = find_export("SetClipboardViewer");
    auto change_chain = find_export("ChangeClipboardChain");
    EXPECT_EQ(set_viewer.stdcall_bytes, 4u);
    EXPECT_EQ(change_chain.stdcall_bytes, 8u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    cpu_.gpr.rcx = first;
    set_viewer.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, NULL_HWND);

    cpu_.gpr.rcx = second;
    set_viewer.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, first);

    // Removing a non-head viewer asks the current head to update its link.
    cpu_.gpr.rcx = first;
    cpu_.gpr.rdx = NULL_HWND;
    change_chain.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    MSG message{};
    ASSERT_TRUE(queue_.get(message));
    EXPECT_EQ(message.hwnd, second);
    EXPECT_EQ(message.message, WM::CHANGECBCHAIN);
    EXPECT_EQ(message.wParam, first);
    EXPECT_EQ(message.lParam, NULL_HWND);

    // Removing the head is a system-side link update and succeeds directly.
    cpu_.gpr.rcx = second;
    cpu_.gpr.rdx = first;
    change_chain.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_TRUE(queue_.empty());

    // The viewer hears about a change when the changer closes the clipboard
    // (Learn WM_DRAWCLIPBOARD); EmptyClipboard needs it open.
    cpu_.gpr.rcx = 0;
    mgr_->impl_OpenClipboard(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    mgr_->impl_EmptyClipboard(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_TRUE(queue_.empty());
    mgr_->impl_CloseClipboard(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    ASSERT_TRUE(queue_.get(message));
    EXPECT_EQ(message.hwnd, first);
    EXPECT_EQ(message.message, WM::DRAWCLIPBOARD);
    EXPECT_EQ(message.wParam, 0u);
    EXPECT_EQ(message.lParam, 0u);
}

TEST_F(User32ExtThunkTest, SetClipboardViewerRejectsInvalidWindow) {
    auto set_viewer = find_export("SetClipboardViewer");
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = 0xDEADBEEFu;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    set_last_error(0);

    set_viewer.handler(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

TEST_F(User32ExtThunkTest, SuspendResumeNotificationTracksWindowRegistrationLifecycle) {
    auto register_entry = find_export("RegisterSuspendResumeNotification");
    auto unregister_entry = find_export("UnregisterSuspendResumeNotification");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    WindowData window{};
    const HWND hwnd = hwnd_mgr_.create(window);
    ASSERT_NE(hwnd, NULL_HWND);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 0; // DEVICE_NOTIFY_WINDOW_HANDLE
    register_entry.handler(m);
    const u64 registration = cpu_.gpr.rax;
    ASSERT_NE(registration, 0u);
    EXPECT_NE(registration, hwnd);

    cpu_.gpr.rcx = registration;
    unregister_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = registration;
    unregister_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    // Measured: ERROR_INVALID_PARAMETER (s1win_misc_probe.expected
    // unregisterpowersettingnotification_again / unregistersuspendresume_again).
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(User32ExtThunkTest, PowerSettingNotificationTracksWindowAndGuid) {
    auto register_entry = find_export("RegisterPowerSettingNotification");
    auto unregister_entry = find_export("UnregisterPowerSettingNotification");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    WindowData window{};
    const HWND hwnd = hwnd_mgr_.create(window);
    ASSERT_NE(hwnd, NULL_HWND);
    constexpr GuestAddr guid = BUF_ADDR;
    const std::array<u8, 16> setting_guid{
        0x98, 0xA7, 0xC4, 0x2D, 0xB3, 0xF1, 0x59, 0x49,
        0x80, 0xB1, 0x7A, 0xD5, 0xA8, 0x32, 0x38, 0x23,
    };
    ASSERT_TRUE(memory_.write_bytes(
        guid, setting_guid.data(), setting_guid.size()).is_ok());

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = guid;
    cpu_.gpr.r8 = 0; // DEVICE_NOTIFY_WINDOW_HANDLE
    register_entry.handler(m);
    const u64 registration = cpu_.gpr.rax;
    ASSERT_NE(registration, 0u);
    EXPECT_NE(registration, hwnd);

    cpu_.gpr.rcx = registration;
    unregister_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = registration;
    unregister_entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    // Measured: ERROR_INVALID_PARAMETER (s1win_misc_probe.expected
    // unregisterpowersettingnotification_again / unregistersuspendresume_again).
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(User32ExtThunkTest, PowerSettingNotificationRejectsServiceMode) {
    auto register_entry = find_export("RegisterPowerSettingNotification");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0x1234;
    cpu_.gpr.rdx = BUF_ADDR;
    cpu_.gpr.r8 = 1; // DEVICE_NOTIFY_SERVICE_HANDLE
    register_entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOT_SUPPORTED);
}

TEST_F(User32ExtThunkTest, ProcessDpiAwarenessIsSetOnceWithNativeWidthContext) {
    auto set_context = find_export("SetProcessDpiAwarenessContext");
    auto set_legacy = find_export("SetProcessDPIAware");
    auto get_context = find_export("GetThreadDpiAwarenessContext");
    auto set_thread = find_export("SetThreadDpiAwarenessContext");
    auto get_awareness = find_export("GetAwarenessFromDpiAwarenessContext");
    auto is_valid = find_export("IsValidDpiAwarenessContext");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = static_cast<u64>(-4ll);
    set_context.handler(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);

    get_context.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, static_cast<u64>(-4ll));

    cpu_.gpr.rcx = static_cast<u64>(-2ll);
    set_thread.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, static_cast<u64>(-4ll));
    get_context.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, static_cast<u64>(-2ll));

    cpu_.gpr.rcx = static_cast<u64>(-2ll);
    get_awareness.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = static_cast<u64>(-4ll);
    get_awareness.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 2u);
    cpu_.gpr.rcx = 123;
    get_awareness.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, static_cast<u32>(-1));
    is_valid.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    cpu_.gpr.rcx = static_cast<u64>(-4ll);
    set_thread.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, static_cast<u64>(-2ll));
    get_context.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, static_cast<u64>(-4ll));

    // SetProcessDPIAware after the awareness is set still returns TRUE and
    // leaves it (measured, tools/oracle_probe/s1win_display_probe.expected
    // set_process_dpi_aware_after).
    set_last_error(0xDEAD);
    set_legacy.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(User32ExtThunkTest, ChangeWindowMessageFilterExReportsAlwaysAllow) {
    // Qt's tray-icon window calls this right after RegisterWindowMessage
    // ("TaskbarCreated"). As a missing export it returned 0xFFFFFFFF and,
    // worse, cleaned up 0 stdcall bytes instead of 16.
    constexpr u32 MSGFLT_RESET = 0, MSGFLT_ALLOW = 1, MSGFLT_DISALLOW = 2;
    constexpr GuestAddr FILTER = BUF_ADDR;

    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.rect = {0, 0, 100, 100};
    data.client_rect = {0, 0, 100, 100};
    const HWND hwnd = hwnd_mgr_.create(data);

    const auto entry = find_export("ChangeWindowMessageFilterEx");
    ASSERT_TRUE(static_cast<bool>(entry.handler));
    EXPECT_EQ(entry.stdcall_bytes, 16u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // CHANGEFILTERSTRUCT { UINT cbSize; DWORD ExtStatus; }
    ASSERT_TRUE(memory_.write_u32(FILTER, 8).is_ok());
    ASSERT_TRUE(memory_.write_u32(FILTER + 4, 0xDEADBEEF).is_ok());
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 0x0049; // WM_COPYDATA
    cpu_.gpr.r8  = MSGFLT_ALLOW;
    cpu_.gpr.r9  = FILTER;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(FILTER + 4).value_or(0), 1u); // ALWAYS_ALLOW

    cpu_.gpr.r8 = MSGFLT_DISALLOW;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(FILTER + 4).value_or(0), 2u); // ALWAYS_DISALLOW

    cpu_.gpr.r8 = MSGFLT_RESET;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(FILTER + 4).value_or(0), 0u); // STATUS_ERROR

    // A NULL CHANGEFILTERSTRUCT is legal.
    cpu_.gpr.r8 = MSGFLT_ALLOW;
    cpu_.gpr.r9 = 0;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

TEST_F(User32ExtThunkTest, ChangeWindowMessageFilterExRejectsBadInput) {
    constexpr GuestAddr FILTER = BUF_ADDR;
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.rect = {0, 0, 100, 100};
    data.client_rect = {0, 0, 100, 100};
    const HWND hwnd = hwnd_mgr_.create(data);

    const auto entry = find_export("ChangeWindowMessageFilterEx");
    ASSERT_TRUE(static_cast<bool>(entry.handler));

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Invalid window handle.
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 0x0049;
    cpu_.gpr.r8  = 1;
    cpu_.gpr.r9  = 0;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);

    // Unknown action.
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.r8  = 7;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    // Wrong cbSize.
    ASSERT_TRUE(memory_.write_u32(FILTER, 12).is_ok());
    cpu_.gpr.r8 = 1;
    cpu_.gpr.r9 = FILTER;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(User32ExtThunkTest, ChangeWindowMessageFilterValidatesFlag) {
    const auto entry = find_export("ChangeWindowMessageFilter");
    ASSERT_TRUE(static_cast<bool>(entry.handler));
    EXPECT_EQ(entry.stdcall_bytes, 8u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0x0049;
    cpu_.gpr.rdx = 1; // MSGFLT_ADD
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rdx = 9;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}
