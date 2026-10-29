/// @file test_user32_ext_monitors_display.cpp
/// @brief Monitors, virtual-screen metrics and display-mode switching: GetSystemMetrics,
///        MonitorFromPoint, GetMonitorInfo, exclusive display modes, DisplayConfig.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, VirtualScreenMetricsUseDocumentedIndexes) {
    // Microsoft Learn GetSystemMetrics: SM_XVIRTUALSCREEN=76 … SM_CY=79.
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto get_system_metrics = find_export("GetSystemMetrics");
    ASSERT_TRUE(static_cast<bool>(get_system_metrics.handler));

    i32 host_w = 0;
    i32 host_h = 0;
    bridge_.get_screen_size(host_w, host_h);
    ASSERT_GT(host_w, 0);
    ASSERT_GT(host_h, 0);

    cpu_.gpr.rcx = 0; // SM_CXSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), host_w);

    cpu_.gpr.rcx = 78; // SM_CXVIRTUALSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax),
              bridge_.get_virtual_desktop_info().width);
    EXPECT_GT(static_cast<i32>(cpu_.gpr.rax), 0);

    cpu_.gpr.rcx = 79; // SM_CYVIRTUALSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax),
              bridge_.get_virtual_desktop_info().height);
    EXPECT_GT(static_cast<i32>(cpu_.gpr.rax), 0);

    cpu_.gpr.rcx = 45; // SM_CXEDGE
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 2);
    cpu_.gpr.rcx = 46; // SM_CYEDGE
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 2);
}

TEST_F(User32ExtThunkTest, VirtualFullscreenRemapsMonitorInfoAndVirtualMetrics) {
    // Exclusive/virtual mode must make GetMonitorInfo + SM_*VIRTUALSCREEN
    // agree with SM_CX/CYSCREEN, not keep host multi-monitor bounds.
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.rect = {0, 0, 1024, 768};
    data.client_rect = {0, 0, 1024, 768};
    const HWND hwnd = hwnd_mgr_.create(data);
    mgr_->apply_virtual_display_mode(hwnd, 1024, 768, 32);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto get_system_metrics = find_export("GetSystemMetrics");
    ASSERT_TRUE(static_cast<bool>(get_system_metrics.handler));

    cpu_.gpr.rcx = 0; // SM_CXSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 1024);
    cpu_.gpr.rcx = 1; // SM_CYSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 768);
    cpu_.gpr.rcx = 76; // SM_XVIRTUALSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 0);
    cpu_.gpr.rcx = 77; // SM_YVIRTUALSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 0);
    cpu_.gpr.rcx = 78; // SM_CXVIRTUALSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 1024);
    cpu_.gpr.rcx = 79; // SM_CYVIRTUALSCREEN
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 768);
    cpu_.gpr.rcx = 80; // SM_CMONITORS
    get_system_metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 1);

    const GuestAddr info = BUF_ADDR;
    (void)memory_.write_u32(info, 40); // sizeof(MONITORINFO)
    cpu_.gpr.rcx = 0x000D0001u;
    cpu_.gpr.rdx = info;
    mgr_->impl_GetMonitorInfoA(marshaler);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(info + 4).value_or(0)), 0);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(info + 8).value_or(0)), 0);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(info + 12).value_or(0)), 1024);
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(info + 16).value_or(0)), 768);
    EXPECT_EQ(memory_.read_u32(info + 36).value_or(0), 1u); // MONITORINFOF_PRIMARY
}

// MonitorFromPoint(POINT pt, DWORD dwFlags): the 8-byte POINT is passed by
// value -- one packed register on Win64 (RCX, flags in RDX), two stack
// DWORDs on Win32 (12 bytes popped). Learn dwFlags: DEFAULTTONULL (0) ->
// NULL, DEFAULTTOPRIMARY (1) -> primary, DEFAULTTONEAREST (2) -> nearest,
// for a point on no monitor.
TEST_F(User32ExtThunkTest, MonitorFromPointDecodesPointByValueAndFlags) {
    const auto monitors = bridge_.get_all_monitor_info();
    ASSERT_FALSE(monitors.empty());
    u32 primary = 0;
    i32 max_right = monitors[0].right;
    for (usize i = 0; i < monitors.size(); ++i) {
        if (monitors[i].primary && primary == 0) {
            primary = 0x000D0001u + static_cast<u32>(i);
        }
        max_right = std::max(max_right, monitors[i].right);
    }
    ASSERT_NE(primary, 0u);
    const auto& first = monitors[primary - 0x000D0001u];
    const i32 in_x = first.left + 3;
    const i32 in_y = first.top + (first.bottom - first.top) / 2;
    i32 max_bottom = first.bottom;
    for (const auto& mi : monitors) max_bottom = std::max(max_bottom, mi.bottom);
    const i32 off_x = max_right + 100000;
    const i32 off_y = max_bottom + 100000;

    const auto monitor_from_point = find_export("MonitorFromPoint");
    ASSERT_TRUE(static_cast<bool>(monitor_from_point.handler));
    EXPECT_EQ(monitor_from_point.stdcall_bytes, 12u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler m64(memory_);
    m64.attach_cpu(cpu_);
    const auto call64 = [&](i32 x, i32 y, u32 flags) {
        cpu_.gpr.rcx = static_cast<u64>(static_cast<u32>(x)) |
                       (static_cast<u64>(static_cast<u32>(y)) << 32);
        cpu_.gpr.rdx = flags;
        cpu_.gpr.r8 = 0xFFFFFFFFull; // stale register: must not be the flags
        monitor_from_point.handler(m64);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    EXPECT_EQ(call64(in_x, in_y, 0), primary);
    EXPECT_EQ(call64(off_x, off_y, 0), 0u);
    EXPECT_EQ(call64(off_x, off_y, 1), primary);
    // Far right of every monitor: the nearest is one whose right edge is the
    // maximum (distance is measured to the rectangle, not to its centre).
    const u32 nearest = call64(off_x, in_y, 2);
    ASSERT_NE(nearest, 0u);
    EXPECT_EQ(monitors[nearest - 0x000D0001u].right, max_right);
    // A point on no monitor with an undocumented flag value gets NULL.
    EXPECT_EQ(call64(off_x, off_y, 7), 0u);

    cpu_.mode = cpu::CpuMode::Protected;
    Marshaler m32(memory_);
    m32.attach_cpu(cpu_);
    const auto call32 = [&](i32 x, i32 y, u32 flags) {
        (void)memory_.write_u32(cpu_.gpr.rsp + 4, static_cast<u32>(x));
        (void)memory_.write_u32(cpu_.gpr.rsp + 8, static_cast<u32>(y));
        (void)memory_.write_u32(cpu_.gpr.rsp + 12, flags);
        monitor_from_point.handler(m32);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    EXPECT_EQ(call32(in_x, in_y, 0), primary);
    EXPECT_EQ(call32(off_x, off_y, 0), 0u);
    EXPECT_EQ(call32(off_x, off_y, 1), primary);
}

TEST_F(User32ExtThunkTest, DisplayModeSendsTopLevelAndPostsChildBeforePaint) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.visible = true;
    data.wndproc = 0x401000;
    const HWND hwnd = hwnd_mgr_.create(data);

    WindowData peer_data = data;
    peer_data.wndproc = 0x402000;
    const HWND peer = hwnd_mgr_.create(peer_data);

    WindowData child_data = data;
    child_data.parent = hwnd;
    child_data.wndproc = 0x403000;
    const HWND child = hwnd_mgr_.create(child_data);

    PaintingManager painting(&hwnd_mgr_);
    mgr_->set_painting_manager(&painting);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<HWND> sent_to;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64 lparam, u64) {
            EXPECT_EQ(message, 0x007Eu);
            EXPECT_EQ(wparam, 8u);
            EXPECT_EQ(lparam, 0x01E00280u);
            sent_to.push_back(target);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);
    mgr_->apply_virtual_display_mode(hwnd, 640, 480, 8);

    ASSERT_EQ(sent_to.size(), 2u);
    // WM_DISPLAYCHANGE requires sent top-level and posted child delivery;
    // it does not prescribe relative order among the top-level recipients.
    const std::vector<HWND> expected_tops{hwnd, peer};
    EXPECT_TRUE(std::is_permutation(sent_to.begin(), sent_to.end(),
                                    expected_tops.begin(), expected_tops.end()));

    MSG msg{};
    ASSERT_TRUE(queue_.get(msg));
    EXPECT_EQ(msg.hwnd, child);
    EXPECT_EQ(msg.message, 0x007Eu); // child delivery is posted
    ASSERT_TRUE(queue_.get(msg));
    EXPECT_EQ(msg.hwnd, hwnd);
    EXPECT_EQ(msg.message, WM::SIZE);
    EXPECT_TRUE(queue_.empty());
    EXPECT_TRUE(painting.needs_paint(hwnd));
}

TEST_F(User32ExtThunkTest, VirtualFullscreenWindowMovesKeepDesktopOrigin) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.rect = {100, 100, 740, 580};
    data.client_rect = {0, 0, 640, 480};
    const HWND hwnd = hwnd_mgr_.create(data);
    mgr_->apply_virtual_display_mode(hwnd, 640, 480, 32);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = 100;
    cpu_.gpr.r9 = 100;
    set_fastcall_arg4_6(640, 480, 0x0014); // SWP_NOZORDER | SWP_NOACTIVATE
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("SetWindowPos").handler(marshaler);

    ASSERT_EQ(cpu_.gpr.rax, 1u);
    const WindowData* fullscreen = hwnd_mgr_.find(hwnd);
    ASSERT_NE(fullscreen, nullptr);
    EXPECT_EQ(fullscreen->rect.left, 0);
    EXPECT_EQ(fullscreen->rect.top, 0);
    EXPECT_EQ(fullscreen->rect.width(), 640);
    EXPECT_EQ(fullscreen->rect.height(), 480);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = 100;
    cpu_.gpr.r8 = 100;
    cpu_.gpr.r9 = 640;
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, 480);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, 1);
    find_export("MoveWindow").handler(marshaler);

    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(fullscreen->rect.left, 0);
    EXPECT_EQ(fullscreen->rect.top, 0);
    EXPECT_EQ(fullscreen->rect.width(), 640);
    EXPECT_EQ(fullscreen->rect.height(), 480);
}

// DXGI exclusive full-screen with DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH uses
// the virtual display mode DirectDraw/D3D9 use: the mode reaches
// GetSystemMetrics, WM_DISPLAYCHANGE is sent to top-levels (Learn
// WM_DISPLAYCHANGE: wParam bits per pixel, lParam LOWORD width / HIWORD
// height), the window takes the mode and gets WM_SIZE. Leaving restores the
// desktop mode -- a resolution change, so WM_DISPLAYCHANGE again -- and the
// window rectangle through SetWindowPos.
TEST_F(User32ExtThunkTest, ExclusiveDisplayModeSwitchIsPublishedAndRestored) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.wndproc = 0x401000;
    data.style = WS::POPUP | WS::VISIBLE;
    data.visible = true;
    data.rect = {100, 100, 740, 580};
    data.client_rect = {0, 0, 640, 480};
    const HWND hwnd = hwnd_mgr_.create(data);

    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<std::tuple<HWND, u32, u64, u64>> sent;
    message_send.set_wndproc_callback(
        [&](HWND target, u32 message, u64 wparam, u64 lparam, u64) {
            sent.emplace_back(target, message, wparam, lparam);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    const MonitorInfo primary = bridge_.get_primary_monitor_info();
    const i32 desk_w = primary.right - primary.left;
    const i32 desk_h = primary.bottom - primary.top;
    ASSERT_GT(desk_w, 0);
    ASSERT_TRUE(mgr_->enter_exclusive_display(hwnd, {0, 0, desk_w, desk_h},
                                              1280, 720, true));
    ASSERT_TRUE(mgr_->virtual_display_size().has_value());
    EXPECT_EQ(*mgr_->virtual_display_size(), (std::pair<u32, u32>{1280, 720}));
    EXPECT_EQ(sent, (std::vector<std::tuple<HWND, u32, u64, u64>>{
                        {hwnd, 0x007Eu, 32u, (720u << 16) | 1280u}}));
    const WindowData* win = hwnd_mgr_.find(hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_EQ(win->rect.left, 0);
    EXPECT_EQ(win->rect.top, 0);
    EXPECT_EQ(win->rect.width(), 1280);
    EXPECT_EQ(win->client_rect.height(), 720);
    MSG msg{};
    ASSERT_TRUE(queue_.get(msg));
    EXPECT_EQ(msg.hwnd, hwnd);
    EXPECT_EQ(msg.message, WM::SIZE);
    EXPECT_EQ(msg.lParam, (720u << 16) | 1280u);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto metrics = find_export("GetSystemMetrics");
    cpu_.gpr.rcx = 0;  // SM_CXSCREEN
    metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), 1280);

    sent.clear();
    while (queue_.get(msg)) {}
    mgr_->leave_exclusive_display(hwnd, true,
                                  windowing::Rect{100, 100, 740, 580});
    EXPECT_FALSE(mgr_->virtual_display_size().has_value());
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent.front(),
              (std::tuple<HWND, u32, u64, u64>{
                  hwnd, 0x007Eu, 32u,
                  (static_cast<u64>(desk_h) << 16) | static_cast<u64>(desk_w)}));
    EXPECT_EQ(win->rect.left, 100);
    EXPECT_EQ(win->rect.top, 100);
    EXPECT_EQ(win->rect.width(), 640);
    EXPECT_EQ(win->rect.height(), 480);
    cpu_.gpr.rcx = 0;
    metrics.handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu_.gpr.rax), desk_w);
    mgr_->set_message_send_manager(nullptr);
}

// Without DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH "DXGI defaults to the desktop
// resolution" (Learn, DXGI best practices): the window covers the output
// and gets WM_SIZE; the resolution does not change, so no WM_DISPLAYCHANGE.
TEST_F(User32ExtThunkTest, ExclusiveDisplayWithoutModeSwitchOnlyCoversOutput) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.wndproc = 0x401000;
    data.style = WS::POPUP | WS::VISIBLE;
    data.visible = true;
    data.rect = {100, 100, 740, 580};
    data.client_rect = {0, 0, 640, 480};
    const HWND hwnd = hwnd_mgr_.create(data);
    MessageSendManager message_send(hwnd_mgr_, queue_, bridge_);
    std::vector<u32> sent;
    message_send.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64, u64) {
            sent.push_back(message);
            return 0u;
        });
    mgr_->set_message_send_manager(&message_send);

    ASSERT_TRUE(mgr_->enter_exclusive_display(hwnd, {0, 0, 1600, 900}, 1600,
                                              900, false));
    EXPECT_FALSE(mgr_->virtual_display_size().has_value());
    EXPECT_EQ(std::count(sent.begin(), sent.end(), 0x007Eu), 0);
    const WindowData* win = hwnd_mgr_.find(hwnd);
    ASSERT_NE(win, nullptr);
    EXPECT_EQ(win->rect.width(), 1600);
    EXPECT_EQ(win->client_rect.height(), 900);
    MSG msg{};
    ASSERT_TRUE(queue_.get(msg));
    EXPECT_EQ(msg.message, WM::SIZE);
    EXPECT_EQ(msg.lParam, (900u << 16) | 1600u);

    sent.clear();
    mgr_->leave_exclusive_display(hwnd, false,
                                  windowing::Rect{100, 100, 740, 580});
    EXPECT_EQ(std::count(sent.begin(), sent.end(), 0x007Eu), 0);
    EXPECT_EQ(win->rect.left, 100);
    EXPECT_EQ(win->rect.width(), 640);
    EXPECT_FALSE(mgr_->enter_exclusive_display(0xBAD0, {0, 0, 10, 10}, 10, 10,
                                               false))
        << "not a window";
    mgr_->set_message_send_manager(nullptr);
}

TEST_F(User32ExtThunkTest, DisplayConfigBufferSizesMatchActiveMonitors) {
    // Learn: GetDisplayConfigBufferSizes / QueryDisplayConfig must succeed on a
    // WDDM-like desktop and agree with EnumDisplayMonitors occupancy.
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const GuestAddr path_count_addr = BUF_ADDR;
    const GuestAddr mode_count_addr = BUF_ADDR + 4;
    const GuestAddr path_array = BUF_ADDR + 0x100;
    const GuestAddr mode_array = BUF_ADDR + 0x1000;

    cpu_.gpr.rcx = 0x2; // QDC_ONLY_ACTIVE_PATHS
    cpu_.gpr.rdx = path_count_addr;
    cpu_.gpr.r8 = mode_count_addr;
    mgr_->impl_GetDisplayConfigBufferSizes(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u); // ERROR_SUCCESS

    const u32 path_count = memory_.read_u32(path_count_addr).value_or(0);
    const u32 mode_count = memory_.read_u32(mode_count_addr).value_or(0);
    ASSERT_GE(path_count, 1u);
    EXPECT_EQ(mode_count, path_count * 2u);

    (void)memory_.write_u32(path_count_addr, path_count);
    (void)memory_.write_u32(mode_count_addr, mode_count);
    cpu_.gpr.rcx = 0x2;
    cpu_.gpr.rdx = path_count_addr;
    cpu_.gpr.r8 = path_array;
    cpu_.gpr.r9 = mode_count_addr;
    set_fastcall_arg4_6(mode_array, 0, 0);
    mgr_->impl_QueryDisplayConfig(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(memory_.read_u32(path_count_addr).value_or(0), path_count);
    EXPECT_EQ(memory_.read_u32(mode_count_addr).value_or(0), mode_count);

    // First path should be marked ACTIVE (flags at offset 68).
    EXPECT_EQ(memory_.read_u32(path_array + 68).value_or(0), 1u);
}

// GetMonitorInfoA/W(NULL, &mi): FALSE with ERROR_INVALID_MONITOR_HANDLE
// (1461), measured on Windows 11 x64. The thunk used ERROR_INVALID_GROUPNAME,
// whose constant was itself wrong (1453 = ERROR_WORKING_SET_QUOTA; the real
// ERROR_INVALID_GROUPNAME is 1209).
TEST_F(User32ExtThunkTest, GetMonitorInfoNullMonitorIsInvalidMonitorHandle) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    EXPECT_EQ(error_codes::ERROR_INVALID_MONITOR_HANDLE, 1461u);
    EXPECT_EQ(error_codes::ERROR_INVALID_GROUPNAME, 1209u);
    for (const char* name : {"GetMonitorInfoW", "GetMonitorInfoA"}) {
        (void)memory_.write_u32(BUF_ADDR, 40);  // sizeof(MONITORINFO)
        cpu_.gpr.rcx = 0;
        cpu_.gpr.rdx = BUF_ADDR;
        set_last_error(0xDEADu);
        find_export(name).handler(m);
        EXPECT_EQ(cpu_.gpr.rax, 0u) << name;
        EXPECT_EQ(get_last_error(), 1461u) << name;
    }
}

TEST_F(User32ExtThunkTest, GetMonitorInfoExFillsDeviceName) {
    // Learn: MONITORINFOEXW is 104 bytes (MONITORINFO + WCHAR szDevice[32]).
    // Chromium feeds that string to EnumDisplayDevicesW.
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const GuestAddr info = BUF_ADDR;
    (void)memory_.write_u32(info, 104); // sizeof(MONITORINFOEXW)
    // Poison past the struct so a 64-byte WCHAR write into a 72-byte layout
    // would be visible if the size gate regresses.
    (void)memory_.write_u32(info + 104, 0xDEADBEEFu);
    cpu_.gpr.rcx = 0x000D0001u;
    cpu_.gpr.rdx = info;
    mgr_->impl_GetMonitorInfoW(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(info + 104).value_or(0), 0xDEADBEEFu);

    std::string name;
    for (usize i = 0; i < 32; ++i) {
        const u16 ch = memory_.read_u16(info + 40 + i * 2).value_or(0);
        if (ch == 0) break;
        name.push_back(static_cast<char>(ch & 0xFF));
    }
    EXPECT_EQ(name, "\\\\.\\DISPLAY1");
}

TEST_F(User32ExtThunkTest, GetMonitorInfoExAFillsAnsiDeviceNameWithoutOverflow) {
    // Learn: MONITORINFOEXA is 72 bytes (MONITORINFO + CHAR szDevice[32]).
    // Writing UTF-16 here overflows into the caller's stack (TS3 after DeleteDC).
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const GuestAddr info = BUF_ADDR;
    (void)memory_.write_u32(info, 72); // sizeof(MONITORINFOEXA)
    (void)memory_.write_u32(info + 72, 0xDEADBEEFu);
    cpu_.gpr.rcx = 0x000D0001u;
    cpu_.gpr.rdx = info;
    mgr_->impl_GetMonitorInfoA(m);
    ASSERT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(info + 72).value_or(0), 0xDEADBEEFu);

    std::string name;
    for (usize i = 0; i < 32; ++i) {
        const u8 ch = memory_.read_u8(info + 40 + i).value_or(0);
        if (ch == 0) break;
        name.push_back(static_cast<char>(ch));
    }
    EXPECT_EQ(name, "\\\\.\\DISPLAY1");
    // First dword of szDevice must be ASCII, not UTF-16 ("\\.\D..." → 0x5C5C2E5C
    // would be wrong endian; UTF-16 LE starts 5C 00 → dword 0x005C005C).
    EXPECT_NE(memory_.read_u32(info + 40).value_or(0), 0x005C005Cu);
}
