/// @file test_user32_ext_window_queries.cpp
/// @brief Window identity, hit-testing, enumeration and property queries: WindowFromPoint,
///        ChildWindowFromPointEx, GetDlgCtrlID, EnumWindows, GetAncestor, window properties.

#include "user32_ext_thunk_harness.hpp"

TEST_F(User32ExtThunkTest, WindowFromPointDecodesPointByValue) {
    WindowData data{};
    data.rect = windowing::Rect{40, 50, 240, 210};
    data.visible = true;
    const HWND hwnd = hwnd_mgr_.create(data);

    const auto window_from_point = find_export("WindowFromPoint");
    ASSERT_TRUE(static_cast<bool>(window_from_point.handler));
    EXPECT_EQ(window_from_point.stdcall_bytes, 8u);

    cpu_.mode = cpu::CpuMode::Protected;
    Marshaler marshaler32(memory_);
    marshaler32.attach_cpu(cpu_);
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, 80).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, 90).is_ok());
    window_from_point.handler(marshaler32);
    EXPECT_EQ(cpu_.gpr.rax, hwnd);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler64(memory_);
    marshaler64.attach_cpu(cpu_);
    cpu_.gpr.rcx = 80ull | (90ull << 32);
    window_from_point.handler(marshaler64);
    EXPECT_EQ(cpu_.gpr.rax, hwnd);
}

TEST_F(User32ExtThunkTest, ChildWindowFromPointExDecodesWin32PointByValue) {
    WindowData parent_data{};
    parent_data.client_rect = windowing::Rect{0, 0, 320, 240};
    const HWND parent = hwnd_mgr_.create(parent_data);
    WindowData child_data{};
    child_data.parent = parent;
    child_data.rect = windowing::Rect{40, 50, 140, 150};
    child_data.visible = true;
    const HWND child = hwnd_mgr_.create(child_data);

    cpu_.mode = cpu::CpuMode::Protected;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto child_from_point = find_export("ChildWindowFromPointEx");
    ASSERT_TRUE(static_cast<bool>(child_from_point.handler));
    EXPECT_EQ(child_from_point.stdcall_bytes, 16u);
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, parent).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, 80).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 12, 90).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 16, 0).is_ok());

    child_from_point.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, child);
}

TEST_F(User32ExtThunkTest, WindowStateQueriesRoundTripWindowPlacement) {
    WindowData data{};
    data.parent = HWND_DESKTOP;
    data.rect = windowing::Rect{100, 110, 700, 510};
    data.client_rect = windowing::Rect{0, 0, 600, 400};
    const HWND hwnd = hwnd_mgr_.create(data);

    ShowWindowManager show_mgr{hwnd_mgr_, bridge_, queue_};
    mgr_->set_show_window_manager(&show_mgr);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    const auto set_placement = find_export("SetWindowPlacement");
    const auto get_placement = find_export("GetWindowPlacement");
    const auto is_iconic = find_export("IsIconic");
    const auto is_zoomed = find_export("IsZoomed");
    const auto is_arranged = find_export("IsWindowArranged");

    constexpr GuestAddr placement = BUF_ADDR;
    ASSERT_TRUE(memory_.write_u32(placement + 0, 44).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 4, 0x0002).is_ok());
    ASSERT_TRUE(memory_.write_u32(
        placement + 8, static_cast<u32>(SW::SHOWMINIMIZED)).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 12, 3).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 16, 4).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 20, 5).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 24, 6).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 28, 20).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 32, 30).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 36, 820).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 40, 630).is_ok());

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = placement;
    set_placement.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = hwnd;
    is_iconic.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = hwnd;
    is_zoomed.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    cpu_.gpr.rcx = hwnd;
    is_arranged.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    ASSERT_TRUE(memory_.write_u32(placement + 4, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory_.write_u32(placement + 8, 0xDEADBEEFu).is_ok());
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = placement;
    get_placement.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(placement + 4).value(), 0u);
    EXPECT_EQ(memory_.read_u32(placement + 8).value(),
              static_cast<u32>(SW::SHOWMINIMIZED));
    EXPECT_EQ(memory_.read_u32(placement + 12).value(), 3u);
    EXPECT_EQ(memory_.read_u32(placement + 24).value(), 6u);
    EXPECT_EQ(memory_.read_u32(placement + 28).value(), 20u);
    EXPECT_EQ(memory_.read_u32(placement + 40).value(), 630u);

    // GetWindowPlacement ignores the caller's length and writes
    // sizeof(WINDOWPLACEMENT) back (measured,
    // tools/oracle_probe/s1win_api_probe.expected getwindowplacement_length0).
    ASSERT_TRUE(memory_.write_u32(placement, 40).is_ok());
    set_last_error(error_codes::ERROR_SUCCESS);
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = placement;
    get_placement.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(placement).value(), 44u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    // A Dock restore of the minimized window is a shell action: it is queued
    // for the owner thread as WM_SYSCOMMAND SC_RESTORE and changes no state
    // from the platform callback (NativeDeminiaturizeAndMiniaturizeArriveAs-
    // SysCommand).
    bridge_.on_native_show_state(hwnd, false, true);
    cpu_.gpr.rcx = hwnd;
    is_iconic.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    // DefWindowProc carries SC_RESTORE out as ShowWindow(SW_RESTORE). Learn
    // WINDOWPLACEMENT WPF_RESTORETOMAXIMIZED: the restored window is
    // maximized regardless of its state before it was minimized.
    (void)show_mgr.show_window(hwnd, static_cast<int>(SW::RESTORE));
    cpu_.gpr.rcx = hwnd;
    is_zoomed.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = hwnd;
    is_iconic.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    mgr_->set_show_window_manager(nullptr);
}

TEST_F(User32ExtThunkTest, WindowFromDCUsesGdiAssociation) {
    HandleTable handles;
    GdiManager gdi(handles);
    gdi.set_hwnd_manager(&hwnd_mgr_);
    mgr_->set_gdi_manager(&gdi);

    WindowData window{};
    window.parent = HWND_DESKTOP;
    const HWND hwnd = hwnd_mgr_.create(window);
    const Handle hdc = gdi.get_dc(hwnd);
    const Handle memory_dc = gdi.create_compatible_dc(hdc);
    const auto window_from_dc = find_export("WindowFromDC");

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    cpu_.gpr.rcx = hdc;
    window_from_dc.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, hwnd);

    const Handle screen_dc = gdi.get_dc(0);
    cpu_.gpr.rcx = screen_dc;
    window_from_dc.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, hwnd_mgr_.desktop());

    cpu_.gpr.rcx = memory_dc;
    window_from_dc.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    cpu_.gpr.rcx = 0xDEAD;
    window_from_dc.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

TEST_F(User32ExtThunkTest, GetDlgCtrlIDReturnsStoredChildControlId) {
    WindowData child{};
    child.parent = HWND_DESKTOP;
    child.ctrl_id = 1000;
    const HWND hwnd = hwnd_mgr_.create(child);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    mgr_->impl_GetDlgCtrlID(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 1000u);
}

TEST_F(User32ExtThunkTest, GetDlgCtrlIDRejectsInvalidWindow) {
    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = 0xDEADBEEFu;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    set_last_error(error_codes::ERROR_SUCCESS);

    mgr_->impl_GetDlgCtrlID(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_WINDOW_HANDLE);
}

TEST_F(User32ExtThunkTest, WindowProcessIdMatchesKernel32Identity) {
    WindowData data{};
    data.owner_thread_id = 17;
    const HWND hwnd = hwnd_mgr_.create(data);
    ASSERT_NE(hwnd, NULL_HWND);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = BUF_ADDR;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_GetWindowThreadProcessId(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 17u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), process_mgr_.get_current_process_id());
}

// Learn GetWindowThreadProcessId: the return value is the identifier of the
// thread that created the window, and *lpdwProcessId its process. A peer
// process's primary thread carries the system-wide ID its creator reported in
// PROCESS_INFORMATION.dwThreadId (VITREO_PRIMARY_THREAD_ID), so the registry
// stores and returns that ID unchanged; no lookup-time remap.
TEST_F(User32ExtThunkTest, GetWindowThreadProcessIdPeerReportsPublishedThreadId) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-gwtpid-peer-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    constexpr u32 peer_hwnd = 0x00ABCDEF;
    const u32 peer_primary_tid = ipc::allocate_system_thread_id();
    ASSERT_NE(peer_primary_tid, 0u);
    ipc::hwnd_registry_publish(peer_hwnd, 64, 64, peer_primary_tid);
    const auto published = ipc::hwnd_registry_lookup(peer_hwnd);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->owner_guest_thread_id, peer_primary_tid);

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = peer_hwnd;
    cpu_.gpr.rdx = BUF_ADDR;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    mgr_->impl_GetWindowThreadProcessId(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, peer_primary_tid);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), static_cast<u32>(::getpid()));

    ipc::hwnd_registry_unpublish(peer_hwnd);
    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

// GW_ENABLEDPOPUP (Learn + user32_windows_probe gw_enabledpopup_*): first
// visible enabled owned top-level popup in GW_HWNDNEXT order; NULL when none
// (never returns hWnd itself).
TEST_F(User32ExtThunkTest, GetWindowEnabledPopupFindsVisibleOwnedPopup) {
    WindowData owner{};
    owner.parent = HWND_DESKTOP;
    owner.visible = true;
    const HWND owner_hwnd = hwnd_mgr_.create(owner);
    WindowData popup{};
    popup.parent = HWND_DESKTOP;
    popup.owner = owner_hwnd;
    popup.visible = false;
    popup.enabled = true;
    popup.style = windowing::WS::POPUP;
    const HWND popup_hwnd = hwnd_mgr_.create(popup);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    cpu_.gpr.rcx = owner_hwnd;
    cpu_.gpr.rdx = 6;  // GW_ENABLEDPOPUP
    find_export("GetWindow").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);  // hidden → NULL

    hwnd_mgr_.find(popup_hwnd)->visible = true;
    cpu_.gpr.rcx = owner_hwnd;
    cpu_.gpr.rdx = 6;
    find_export("GetWindow").handler(marshaler);
    EXPECT_EQ(static_cast<HWND>(cpu_.gpr.rax), popup_hwnd);

    hwnd_mgr_.find(popup_hwnd)->enabled = false;
    cpu_.gpr.rcx = owner_hwnd;
    cpu_.gpr.rdx = 6;
    find_export("GetWindow").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);

    hwnd_mgr_.find(popup_hwnd)->enabled = true;
    cpu_.gpr.rcx = popup_hwnd;
    cpu_.gpr.rdx = 6;
    find_export("GetWindow").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0u);  // from the popup: no owned popup of its own
}

TEST_F(User32ExtThunkTest, EnumThreadWindowsVisitsOnlyMatchingTopLevelWindows) {
    WindowData first{};
    first.owner_thread_id = 7;
    const HWND first_hwnd = hwnd_mgr_.create(first);
    WindowData second{};
    second.owner_thread_id = 8;
    (void)hwnd_mgr_.create(second);
    WindowData third{};
    third.owner_thread_id = 7;
    const HWND third_hwnd = hwnd_mgr_.create(third);

    std::vector<u64> visited;
    mgr_->set_guest_call(
        [&visited](GuestAddr callback, const u64* args, usize count,
                   bool is_64bit) {
            EXPECT_EQ(callback, 0x12345678u);
            EXPECT_EQ(count, 2u);
            EXPECT_TRUE(is_64bit);
            EXPECT_EQ(args[1], 0xAABBCCDDu);
            visited.push_back(args[0]);
            return 1u;
        });

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = 7;
    cpu_.gpr.rdx = 0x12345678;
    cpu_.gpr.r8 = 0xAABBCCDD;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    find_export("EnumThreadWindows").handler(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    // EnumThreadWindows specifies membership and callback continuation, not
    // creation order. HWND traversal follows the current Z order.
    const std::vector<u64> expected{first_hwnd, third_hwnd};
    EXPECT_TRUE(std::is_permutation(visited.begin(), visited.end(),
                                    expected.begin(), expected.end()));
}

// EnumWindows (Learn): system-wide top-level list. Local desktop children are
// always included; peer tops come from hwnd_registry (user32_windows_probe
// enumwindows_sees_child_window).
TEST_F(User32ExtThunkTest, EnumWindowsVisitsLocalTopLevelWindows) {
    WindowData a{};
    a.parent = HWND_DESKTOP;
    const HWND ha = hwnd_mgr_.create(a);
    WindowData b{};
    b.parent = HWND_DESKTOP;
    const HWND hb = hwnd_mgr_.create(b);

    std::vector<u64> visited;
    mgr_->set_guest_call(
        [&visited](GuestAddr callback, const u64* args, usize count, bool) {
            EXPECT_EQ(callback, 0x55AAu);
            EXPECT_EQ(count, 2u);
            visited.push_back(args[0]);
            return 1u;
        });

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = 0x55AA;
    cpu_.gpr.rdx = 0x42;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    find_export("EnumWindows").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_NE(std::find(visited.begin(), visited.end(), ha), visited.end());
    EXPECT_NE(std::find(visited.begin(), visited.end(), hb), visited.end());
}

// Measured (tools/oracle_probe/s1win_msgseq2_probe.c enumthreadwindows_*):
// a thread without nonchild windows returns TRUE and sets
// ERROR_INVALID_PARAMETER, contrary to Learn's "FALSE".
TEST_F(User32ExtThunkTest, EnumThreadWindowsReturnsTrueWithInvalidParameterWhenThreadHasNoWindows) {
    bool called = false;
    mgr_->set_guest_call(
        [&called](GuestAddr, const u64*, usize, bool) {
            called = true;
            return 1u;
        });

    cpu_.mode = cpu::CpuMode::Long;
    cpu_.gpr.rcx = 99;
    cpu_.gpr.rdx = 0x12345678;
    cpu_.gpr.r8 = 0;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);

    set_last_error(0xDEAD);
    find_export("EnumThreadWindows").handler(marshaler);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_FALSE(called);
}

TEST_F(User32ExtThunkTest, AnsiWindowContractsSharePropertyStateWithUnicode) {
    WindowData data{};
    const HWND hwnd = hwnd_mgr_.create(data);
    ASSERT_NE(hwnd, NULL_HWND);

    constexpr GuestAddr ansi_key = MEM_BASE + 0xF000;
    constexpr GuestAddr wide_key = ansi_key + 0x100;
    constexpr char key[] = "StormProperty";
    ASSERT_TRUE(memory_.write_bytes(
        ansi_key, reinterpret_cast<const u8*>(key), sizeof(key)).is_ok());
    for (usize i = 0; i < sizeof(key); ++i) {
        ASSERT_TRUE(memory_.write_u16(wide_key + i * 2,
                                      static_cast<u16>(key[i])).is_ok());
    }

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    ASSERT_EQ(marshaler.read_ansi_string(ansi_key).value(), "StormProperty");
    ASSERT_EQ(marshaler.read_wide_string(wide_key).value(), "StormProperty");

    auto set_prop_a = find_export("SetPropA");
    auto get_prop_a = find_export("GetPropA");
    auto get_prop_w = find_export("GetPropW");
    auto remove_prop_a = find_export("RemovePropA");
    EXPECT_TRUE(find_export("FindWindowExA").handler);
    // GetClassInfoA is exported by make_window_class_exports (the class
    // manager owns every GetClassInfo form).

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = ansi_key;
    cpu_.gpr.r8 = 0x12345678;
    set_prop_a.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = ansi_key;
    get_prop_a.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0x12345678u);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = wide_key;
    get_prop_w.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0x12345678u);

    cpu_.gpr.rcx = hwnd;
    cpu_.gpr.rdx = ansi_key;
    remove_prop_a.handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0x12345678u);
}

TEST_F(User32ExtThunkTest, WindowPropertiesRemainIndependentAcrossThreads) {
    std::array<HWND, 8> windows{};
    for (auto& hwnd : windows) {
        WindowData data{};
        data.parent = HWND_DESKTOP;
        hwnd = hwnd_mgr_.create(data);
    }
    std::atomic<unsigned> ready{0};
    std::atomic<unsigned> mismatches{0};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < windows.size(); ++index) {
        workers.emplace_back([&, index] {
            cpu::CpuState state;
            state.init_long_mode();
            Marshaler marshaler(memory_);
            marshaler.attach_cpu(state);
            state.gpr.rcx = windows[index];
            state.gpr.rdx = 0xC001; // Atom-form key avoids shared string storage.
            ready.fetch_add(1);
            while (ready.load() != windows.size()) std::this_thread::yield();
            for (u64 iteration = 1; iteration <= 500; ++iteration) {
                const u64 value = (u64{index + 1} << 40) | iteration;
                state.gpr.r8 = value;
                mgr_->impl_SetPropW(marshaler);
                if (state.gpr.rax != 1) ++mismatches;
                mgr_->impl_GetPropW(marshaler);
                if (state.gpr.rax != value) ++mismatches;
                mgr_->impl_RemovePropW(marshaler);
                if (state.gpr.rax != value) ++mismatches;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(mismatches.load(), 0u);
}

TEST_F(User32ExtThunkTest, GetAncestorWalksParentsToDesktop) {
    // Learn: GA_PARENT returns the parent (desktop for top-level), never the
    // owner. Identity stubs make Qt's GetAncestor/GetDesktopWindow walk spin.
    constexpr u32 kGaParent = 1;
    constexpr u32 kGaRoot = 2;
    constexpr u32 kGaRootOwner = 3;

    WindowData top{};
    top.parent = HWND_DESKTOP;
    top.style = WS::OVERLAPPEDWINDOW;
    const HWND top_hwnd = hwnd_mgr_.create(top);
    ASSERT_NE(top_hwnd, NULL_HWND);

    WindowData child{};
    child.parent = top_hwnd;
    child.style = WS::CHILD | WS::VISIBLE;
    const HWND child_hwnd = hwnd_mgr_.create(child);
    ASSERT_NE(child_hwnd, NULL_HWND);

    WindowData owned{};
    owned.parent = HWND_DESKTOP;
    owned.owner = top_hwnd;
    owned.style = WS::POPUP | WS::VISIBLE;
    const HWND owned_hwnd = hwnd_mgr_.create(owned);
    ASSERT_NE(owned_hwnd, NULL_HWND);

    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    auto get_ancestor = find_export("GetAncestor");
    ASSERT_EQ(get_ancestor.stdcall_bytes, 8u);

    auto call = [&](HWND hwnd, u32 flags) -> HWND {
        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = flags;
        get_ancestor.handler(marshaler);
        return static_cast<HWND>(cpu_.gpr.rax);
    };

    EXPECT_EQ(call(top_hwnd, kGaParent), HWND_DESKTOP);
    EXPECT_EQ(call(child_hwnd, kGaParent), top_hwnd);
    EXPECT_EQ(call(HWND_DESKTOP, kGaParent), NULL_HWND);

    EXPECT_EQ(call(child_hwnd, kGaRoot), top_hwnd);
    EXPECT_EQ(call(top_hwnd, kGaRoot), top_hwnd);

    // Owned popup: GA_PARENT → desktop; GA_ROOTOWNER walks owner via GetParent.
    EXPECT_EQ(call(owned_hwnd, kGaParent), HWND_DESKTOP);
    EXPECT_EQ(call(owned_hwnd, kGaRootOwner), top_hwnd);

    // Walking GA_PARENT until desktop terminates (Qt qwindows pattern).
    HWND walk = call(child_hwnd, kGaParent);
    EXPECT_EQ(walk, top_hwnd);
    walk = call(walk, kGaParent);
    EXPECT_EQ(walk, HWND_DESKTOP);
    walk = call(walk, kGaParent);
    EXPECT_EQ(walk, NULL_HWND);
}
