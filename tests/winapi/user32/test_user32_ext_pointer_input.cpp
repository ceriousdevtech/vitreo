/// @file test_user32_ext_pointer_input.cpp
/// @brief Pointer, mouse and touch input contracts: EnableMouseInPointer, GetClipCursor,
///        pointer-device notifications and inventory, TrackMouseEvent, touch registration.

#include "user32_ext_thunk_harness.hpp"

// Learn EnableMouseInPointer: success means mouse input is delivered as
// WM_POINTER* messages *instead of* WM_MOUSE*. This host has no WM_POINTER
// pipeline, so asking to enable it has to fail — reporting success sends the
// caller to wait for pointer messages that never arrive, and it stops taking
// mouse input entirely. Unity 6 calls this during input init, and the
// unresolved-import thunk answered 0xFFFFFFFF, which is nonzero, so success.
//
// Disabling is the state a desktop application already starts in, so that
// request is a truthful success and leaves the caller on the legacy path.
TEST_F(User32ExtThunkTest, EnableMouseInPointerStateMachineMatchesWindows) {
    // Values measured on Windows by tools/oracle_probe/mouse_in_pointer_probe.c.
    mouse_in_pointer::reset_for_testing();
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const auto entry = find_export("EnableMouseInPointer");
    ASSERT_TRUE(static_cast<bool>(entry.handler));
    EXPECT_EQ(entry.stdcall_bytes, 4u);
    const auto query = find_export("IsMouseInPointerEnabled");
    ASSERT_TRUE(static_cast<bool>(query.handler));

    query.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    set_last_error(error_codes::ERROR_SUCCESS);
    cpu_.gpr.rcx = 5;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    cpu_.gpr.rcx = 1;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    query.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = 1;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    set_last_error(error_codes::ERROR_SUCCESS);
    cpu_.gpr.rcx = 0;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);

    // No pointer message has been retrieved yet: GetPointerInfo(1) fails with
    // ERROR_INVALID_PARAMETER while GetPointerType(1) reports PT_MOUSE.
    set_last_error(error_codes::ERROR_SUCCESS);
    cpu_.gpr.rcx = 1;
    cpu_.gpr.rdx = BUF_ADDR;
    find_export("GetPointerInfo").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    cpu_.gpr.rcx = 1;
    cpu_.gpr.rdx = BUF_ADDR;
    find_export("GetPointerType").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 4u);

    mouse_in_pointer::reset_for_testing();
}

// GetClipCursor with no clip reports the virtual screen; SetCursorPos then
// confines to that same rectangle. Under virtual full-screen that is the
// (0,0)-(W,H) mode the guest selected, as SM_*VIRTUALSCREEN already report.
TEST_F(User32ExtThunkTest, GetClipCursorUnclippedMatchesVirtualScreenMetrics) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.rect = {0, 0, 1024, 768};
    data.client_rect = {0, 0, 1024, 768};
    const HWND hwnd = hwnd_mgr_.create(data);
    mgr_->apply_virtual_display_mode(hwnd, 1024, 768, 32);
    // A fresh bridge carries no ClipCursor rectangle.

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto get_clip_cursor = find_export("GetClipCursor");
    ASSERT_TRUE(static_cast<bool>(get_clip_cursor.handler));
    cpu_.gpr.rcx = BUF_ADDR;
    get_clip_cursor.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR + 0).value_or(99)), 0);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR + 4).value_or(99)), 0);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR + 8).value_or(0)), 1024);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(BUF_ADDR + 12).value_or(0)), 768);

    const windowing::Rect screen = mgr_->guest_virtual_screen_rect();
    EXPECT_EQ(screen.left, 0);
    EXPECT_EQ(screen.top, 0);
    EXPECT_EQ(screen.right, 1024);
    EXPECT_EQ(screen.bottom, 768);
}

TEST_F(User32ExtThunkTest, PointerDeviceNotificationRegistrationTracksCurrentWindow) {
    auto entry = find_export("RegisterPointerDeviceNotifications");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    WindowData first_window{};
    WindowData second_window{};
    const HWND first = hwnd_mgr_.create(first_window);
    const HWND second = hwnd_mgr_.create(second_window);

    cpu_.gpr.rcx = first;
    cpu_.gpr.rdx = 1;
    entry.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(mgr_->pointer_notification_window(), first);
    EXPECT_TRUE(mgr_->pointer_notification_in_range_enabled());

    cpu_.gpr.rcx = second;
    cpu_.gpr.rdx = 0;
    entry.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(mgr_->pointer_notification_window(), second);
    EXPECT_FALSE(mgr_->pointer_notification_in_range_enabled());

    cpu_.gpr.rcx = 0xBAD;
    entry.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
    EXPECT_EQ(mgr_->pointer_notification_window(), second);
}

TEST_F(User32ExtThunkTest, PointerDeviceInventoryReportsNoTouchOrPenDevices) {
    auto entry = find_export("GetPointerDevices");
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    ASSERT_TRUE(memory_.write_u32(BUF_ADDR, 0xAAAAAAAAu).is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = 0;
    entry.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 0u);

    ASSERT_TRUE(memory_.write_u32(BUF_ADDR, 4u).is_ok());
    cpu_.gpr.rcx = BUF_ADDR;
    cpu_.gpr.rdx = BUF_ADDR + 0x100;
    entry.handler(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 0u);

    cpu_.gpr.rcx = 0;
    entry.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// TrackMouseEvent is implemented now (it used to fail every call with
// ERROR_CALL_NOT_IMPLEMENTED). TRACKMOUSEEVENT is 24 bytes on x64: cbSize,
// dwFlags, pointer-sized hwndTrack, dwHoverTime.
TEST_F(User32ExtThunkTest, TrackMouseEventArmsQueriesAndCancels) {
    constexpr u32 TME_HOVER = 0x00000001;
    constexpr u32 TME_LEAVE = 0x00000002;
    constexpr u32 TME_QUERY = 0x40000000;
    constexpr u32 TME_CANCEL = 0x80000000;

    cpu_.init_long_mode();
    cpu_.set_sp(STACK_BASE + 0x800);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    mgr_->set_is_64bit(true);

    WindowData wd{};
    wd.parent = HWND_DESKTOP;
    wd.style = WS::POPUP;
    const HWND hwnd = hwnd_mgr_.create(wd);

    const auto write_request = [&](u32 size, u32 flags, u64 track,
                                   u32 hover_ms) {
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 0, size).is_ok());
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 4, flags).is_ok());
        ASSERT_TRUE(memory_.write_u64(BUF_ADDR + 8, track).is_ok());
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR + 16, hover_ms).is_ok());
    };

    // A null structure and a wrong cbSize are both ERROR_INVALID_PARAMETER.
    cpu_.gpr.rcx = 0;
    mgr_->impl_TrackMouseEvent(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    cpu_.gpr.rcx = BUF_ADDR;
    write_request(16, TME_LEAVE, hwnd, 0); // x86 size, rejected in long mode
    mgr_->impl_TrackMouseEvent(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    // A well-formed request naming no window is a handle error, not a size one.
    write_request(24, TME_LEAVE, 0, 0);
    mgr_->impl_TrackMouseEvent(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);

    // Arm TME_LEAVE | TME_HOVER with an explicit dwell.
    write_request(24, TME_LEAVE | TME_HOVER, hwnd, 250);
    mgr_->impl_TrackMouseEvent(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    bool leave = false;
    bool hover = false;
    u32 hover_ms = 0;
    bridge_.query_mouse_tracking(hwnd, leave, hover, hover_ms);
    EXPECT_TRUE(leave);
    EXPECT_TRUE(hover);
    EXPECT_EQ(hover_ms, 250u);

    // TME_QUERY reports what is armed by overwriting dwFlags/dwHoverTime.
    write_request(24, TME_QUERY, hwnd, 0);
    mgr_->impl_TrackMouseEvent(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 4).value(), TME_LEAVE | TME_HOVER);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 16).value(), 250u);

    // TME_CANCEL disarms only the requests it names.
    write_request(24, TME_CANCEL | TME_HOVER, hwnd, 0);
    mgr_->impl_TrackMouseEvent(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    bridge_.query_mouse_tracking(hwnd, leave, hover, hover_ms);
    EXPECT_TRUE(leave);
    EXPECT_FALSE(hover);
}

TEST_F(User32ExtThunkTest, TouchRegistrationFlagsOwnershipAndOutputsMatchWindowsBothBitnesses) {
    // Native touch_registration_probe.expected: modifier replacement, owner
    // thread access, query from another thread, no output on absent/bad HWND,
    // preserved success last-error, and idempotent unregister.
    const auto register_touch = find_export("RegisterTouchWindow");
    const auto unregister_touch = find_export("UnregisterTouchWindow");
    const auto query_touch = find_export("IsTouchWindow");
    const u32 owner = ThreadManager::get_current_thread_id_static();
    WindowData data{};
    data.owner_thread_id = owner;
    const HWND hwnd = hwnd_mgr_.create(data);
    for (bool is64 : {false, true}) {
        mgr_->set_is_64bit(is64);
        cpu_ = cpu::CpuState{};
        if (is64) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + 0x800);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        auto invoke = [&](const ExportEntry& entry, u64 window, u64 value) {
            if (is64) { cpu_.gpr.rcx = window; cpu_.gpr.rdx = value; }
            else {
                (void)memory_.write_u32(cpu_.sp()+4, static_cast<u32>(window));
                (void)memory_.write_u32(cpu_.sp()+8, static_cast<u32>(value));
            }
            set_last_error(0xbeef);
            entry.handler(m);
            return cpu_.gpr.rax;
        };
        EXPECT_EQ(invoke(unregister_touch, hwnd, 0), 1u);
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR, 0xdead).is_ok());
        EXPECT_EQ(invoke(query_touch, hwnd, BUF_ADDR), 0u);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 0xdeadu);
        EXPECT_EQ(get_last_error(), 0xbeefu);
        EXPECT_EQ(invoke(register_touch, HWND_DESKTOP, 0), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
        EXPECT_EQ(invoke(unregister_touch, HWND_DESKTOP, 0), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
        for (u32 flags = 0; flags <= 3; ++flags) {
            EXPECT_EQ(invoke(register_touch, hwnd, flags), 1u);
            EXPECT_EQ(get_last_error(), 0xbeefu);
            EXPECT_EQ(invoke(query_touch, hwnd, BUF_ADDR), 1u);
            EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), flags);
            EXPECT_EQ(get_last_error(), 0xbeefu);
            EXPECT_EQ(invoke(query_touch, hwnd, 0), 1u);
        }
        EXPECT_EQ(invoke(register_touch, hwnd, 4), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
        EXPECT_EQ(invoke(query_touch, hwnd, BUF_ADDR), 1u);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 3u);
        EXPECT_EQ(invoke(query_touch, hwnd, 1), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);
        ThreadManager::set_current_thread_id(owner + 1);
        EXPECT_EQ(invoke(register_touch, hwnd, 1), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
        EXPECT_EQ(invoke(unregister_touch, hwnd, 0), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
        EXPECT_EQ(invoke(query_touch, hwnd, BUF_ADDR), 1u);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 3u);
        ThreadManager::set_current_thread_id(owner);
        EXPECT_EQ(invoke(unregister_touch, hwnd, 0), 1u);
        EXPECT_EQ(invoke(unregister_touch, hwnd, 0), 1u);
        ASSERT_TRUE(memory_.write_u32(BUF_ADDR, 0xdead).is_ok());
        EXPECT_EQ(invoke(query_touch, hwnd, BUF_ADDR), 0u);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 0xdeadu);
        EXPECT_EQ(invoke(query_touch, 0, BUF_ADDR), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 0xdeadu);
        EXPECT_EQ(invoke(register_touch, hwnd, 3), 1u);
        mgr_->window_destroyed(hwnd);
        EXPECT_EQ(invoke(query_touch, hwnd, BUF_ADDR), 0u);
        EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), 0xdeadu);
    }
}

TEST_F(User32ExtThunkTest, TouchRegistrationTablesSurviveIndependentOwnerThreadsBothBitnesses) {
    const auto reg = find_export("RegisterTouchWindow");
    const auto query = find_export("IsTouchWindow");
    const auto unreg = find_export("UnregisterTouchWindow");
    std::array<std::array<HWND, 32>, 8> windows{};
    for (unsigned worker = 0; worker < windows.size(); ++worker) {
        for (auto& hwnd : windows[worker]) {
            WindowData data{};
            data.owner_thread_id = 100 + worker;
            hwnd = hwnd_mgr_.create(data);
        }
    }
    for (bool is64 : {false, true}) {
        mgr_->set_is_64bit(is64);
        std::atomic<unsigned> ready{0}, failures{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < windows.size(); ++worker) {
            workers.emplace_back([&, worker] {
                ThreadManager::set_current_thread_id(100 + worker);
                cpu::CpuState cpu;
                if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
                cpu.set_sp(STACK_BASE + 0x2000 + worker * 0x1000);
                Marshaler m(memory_);
                m.attach_cpu(cpu);
                const GuestAddr out = BUF_ADDR + worker * 16;
                auto invoke = [&](const ExportEntry& entry, HWND hwnd, u64 value) {
                    if (is64) { cpu.gpr.rcx = hwnd; cpu.gpr.rdx = value; }
                    else {
                        (void)memory_.write_u32(cpu.sp()+4, hwnd);
                        (void)memory_.write_u32(cpu.sp()+8, static_cast<u32>(value));
                    }
                    entry.handler(m);
                    return cpu.gpr.rax;
                };
                ready.fetch_add(1);
                while (!start.load()) std::this_thread::yield();
                for (unsigned i = 0; i < 3000; ++i) {
                    const HWND hwnd = windows[worker][i % windows[worker].size()];
                    const u32 flags = i & 3;
                    if (invoke(reg, hwnd, flags) != 1 || invoke(query, hwnd, out) != 1 ||
                        memory_.read_u32(out).value_or(0xdead) != flags ||
                        invoke(unreg, hwnd, 0) != 1 || invoke(query, hwnd, 0) != 0)
                        failures.fetch_add(1);
                }
            });
        }
        while (ready.load() != windows.size()) std::this_thread::yield();
        start.store(true);
        for (auto& worker : workers) worker.join();
        EXPECT_EQ(failures.load(), 0u);
    }
}
