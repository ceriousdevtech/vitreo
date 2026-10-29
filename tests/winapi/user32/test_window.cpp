/// @file test_window.cpp
/// @brief Tests for WindowManager / CreateWindowExW / DestroyWindow (WIN32-051)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/windows/window.hpp>
#include <winapi/user32/windows/sdk_creation_layouts.hpp>
#include <winapi/user32/classes/system_classes.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/user32_internal.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

class WindowManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register a default window class
        WindowClassData wc;
        wc.class_name = L"TestClass";
        wc.wndproc    = 0x1000;
        wc.style      = 0;
        class_mgr.register_class(wc);
    }

    HwndManager     hwnd_mgr;
    WindowClassManager class_mgr;
    CocoaBridge     bridge;
    MessageQueue    queue;

    WindowManager make_wm() {
        WindowManager wm(hwnd_mgr, class_mgr, bridge, queue);
        // This fixture's TestClass procedure accepts creation. Individual
        // callback regressions replace it with the behavior they exercise.
        wm.set_wndproc_callback([](HWND, u32 message, u64, u64, u64) -> u64 {
            return message == WM::NCCREATE ? 1u : 0u;
        });
        return wm;
    }

    // Helper: create a basic window with defaults
    HWND create_default(WindowManager& wm, u32 style = WS::OVERLAPPEDWINDOW) {
        return wm.create_window(
            0,                    // ex_style
            L"TestClass",         // class_name
            L"Test Window",       // window_name
            style,                // style
            CW_USEDEFAULT, CW_USEDEFAULT,  // x, y
            CW_USEDEFAULT, CW_USEDEFAULT,  // width, height
            NULL_HWND,            // parent
            0, 0, 0              // menu, hinstance, param
        );
    }
};

// ---------------------------------------------------------------------------
// CreateWindowExW
// ---------------------------------------------------------------------------

TEST_F(WindowManagerTest, CreateWithValidClassReturnsValidHwnd) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    EXPECT_NE(hwnd, NULL_HWND);
}

TEST_F(WindowManagerTest, CreateMessageOnlyWindowKeepsZeroSizeAndNoDesktopParent) {
    // Learn: CreateWindow(..., 0,0,0,0, HWND_MESSAGE) — Chromium MessagePumpForUI.
    auto wm = make_wm();
    HWND hwnd = wm.create_window(
        0, L"TestClass", L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, 0, 0, 0);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->is_message_only);
    EXPECT_EQ(data->parent, HWND_MESSAGE);
    EXPECT_EQ(data->owner, NULL_HWND);
    EXPECT_EQ(data->rect.width(), 0);
    EXPECT_EQ(data->rect.height(), 0);
    // Must not appear as a desktop top-level window.
    auto tops = hwnd_mgr.enum_children(HWND_DESKTOP);
    EXPECT_EQ(std::find(tops.begin(), tops.end(), hwnd), tops.end());
}

TEST_F(WindowManagerTest, CreateWithUnknownClassReturnsNull) {
    auto wm = make_wm();
    HWND hwnd = wm.create_window(
        0, L"NonExistentClass", L"Title", WS::OVERLAPPEDWINDOW,
        100, 100, 640, 480, NULL_HWND, 0, 0, 0
    );
    EXPECT_EQ(hwnd, NULL_HWND);
}

TEST_F(WindowManagerTest, CreateNetworkAddressControlUsesBuiltinClass) {
    auto wm = make_wm();
    HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);

    HWND hwnd = wm.create_window(
        0, L"msctls_netaddress", L"", WS::CHILD | WS::VISIBLE,
        10, 10, 180, 24, parent, 1001, 0, 0
    );
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->parent, parent);
    EXPECT_EQ(data->ctrl_id, 1001u);
}

TEST_F(WindowManagerTest, CreatedWindowIsFindable) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_NE(hwnd_mgr.find(hwnd), nullptr);
}

TEST_F(WindowManagerTest, CreatedWindowHasCorrectTitle) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->title, L"Test Window");
}

TEST_F(WindowManagerTest, CreatedWindowHasCorrectStyle) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm, WS::OVERLAPPEDWINDOW | WS::VISIBLE);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    // Every top-level window keeps WS_CLIPSIBLINGS (measured,
    // tools/oracle_probe/s1win_style_probe.expected "c 00cf0000 ...").
    EXPECT_EQ(data->style, WS::OVERLAPPEDWINDOW | WS::VISIBLE | WS::CLIPSIBLINGS);
}

TEST_F(WindowManagerTest, CreateHonorsClientDrawnNcCalcSizeBeforeCreate) {
    auto wm = make_wm();
    constexpr GuestAddr scratch = 0x3000;
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Protected;
    ASSERT_TRUE(memory.map_region(scratch, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    wm.set_createstruct_scratch(scratch);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    std::vector<u32> messages;
    wm.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64, u64) {
            messages.push_back(message);
            // Leave NCCALCSIZE rgrc[0] unchanged: full-window client area.
            return message == static_cast<u32>(WM::NCCREATE) ? 1u : 0u;
        });

    const HWND hwnd = wm.create_window(
        0, L"TestClass", L"Custom frame",
        WS::CAPTION | WS::THICKFRAME,
        10, 20, 320, 240, NULL_HWND, 0, 0, 0, 0, 0, &marshaler);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->client_rect.width(), 320);
    EXPECT_EQ(data->client_rect.height(), 240);
    EXPECT_EQ(data->client_offset.x, 0);
    EXPECT_EQ(data->client_offset.y, 0);
    EXPECT_TRUE(data->client_drawn_frame);
    // An overlapped window gets WM_GETMINMAXINFO before WM_NCCREATE
    // (measured, tools/oracle_probe/s1win_msgseq_probe.c
    // create_overlapped_hidden).
    EXPECT_EQ(messages,
              (std::vector<u32>{WM::GETMINMAXINFO, WM::NCCREATE, WM::NCCALCSIZE,
                                WM::CREATE}));
}

// Microsoft Learn, SetWindowLongPtrW: replacing GWLP_WNDPROC causes the
// system to call the replacement instead of the previous procedure. This
// applies at each creation-message boundary, not only after WM_NCCREATE.
TEST_F(WindowManagerTest, CreationMessagesUseCurrentSubclassProcedure) {
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        for (const u32 replace_on : {WM::GETMINMAXINFO, WM::NCCREATE,
                                     WM::NCCALCSIZE}) {
            SCOPED_TRACE(static_cast<int>(mode));
            SCOPED_TRACE(replace_on);
            auto wm = make_wm();
            GuestMemory memory;
            cpu::CpuState cpu;
            cpu.mode = mode;
            constexpr GuestAddr scratch = 0x3000;
            ASSERT_TRUE(memory.map_region(scratch, 0x1000,
                                          MemoryProtection::ReadWrite).is_ok());
            wm.set_createstruct_scratch(scratch);
            Marshaler marshaler(memory);
            marshaler.attach_cpu(cpu);
            const u64 replacement = mode == cpu::CpuMode::Long
                ? 0x1234567887654321ull : 0x76543210ull;
            bool replaced = false;
            std::vector<u32> messages;
            wm.set_wndproc_callback(
                [&](HWND hwnd, u32 message, u64, u64, u64 proc) -> u64 {
                    messages.push_back(message);
                    EXPECT_EQ(proc, replaced ? replacement : 0x1000);
                    if (message == replace_on) {
                        hwnd_mgr.find(hwnd)->wndproc = replacement;
                        replaced = true;
                    }
                    return message == WM::NCCREATE ? 1 : 0;
                });
            const HWND hwnd = wm.create_window(
                0, L"TestClass", L"Subclass", WS::OVERLAPPEDWINDOW,
                0, 0, 320, 240, NULL_HWND, 0, 0, 0, 0, 0, &marshaler);
            ASSERT_NE(hwnd, NULL_HWND);
            EXPECT_TRUE(replaced);
            EXPECT_EQ(messages, (std::vector<u32>{WM::GETMINMAXINFO,
                WM::NCCREATE, WM::NCCALCSIZE, WM::CREATE}));
            EXPECT_EQ(hwnd_mgr.find(hwnd)->wndproc, replacement);
        }
    }
}

TEST_F(WindowManagerTest, ReplacementProcedureCanRejectCreation) {
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        for (const u32 reject_on : {WM::NCCREATE, WM::CREATE}) {
            auto wm = make_wm();
            GuestMemory memory;
            cpu::CpuState cpu;
            cpu.mode = mode;
            constexpr GuestAddr scratch = 0x3000;
            ASSERT_TRUE(memory.map_region(scratch, 0x1000,
                                          MemoryProtection::ReadWrite).is_ok());
            wm.set_createstruct_scratch(scratch);
            Marshaler marshaler(memory);
            marshaler.attach_cpu(cpu);
            HWND created = NULL_HWND;
            bool rejected = false;
            wm.set_wndproc_callback(
                [&](HWND hwnd, u32 message, u64, u64, u64 proc) -> u64 {
                    created = hwnd;
                    if (message == WM::GETMINMAXINFO) {
                        hwnd_mgr.find(hwnd)->wndproc = 0x2000;
                    }
                    if (message == reject_on && proc == 0x2000) {
                        rejected = true;
                        return message == WM::NCCREATE ? 0 : ~u64{0};
                    }
                    return message == WM::NCCREATE ? 1 : 0;
                });
            EXPECT_EQ(wm.create_window(
                0, L"TestClass", L"Rejected subclass", WS::OVERLAPPEDWINDOW,
                0, 0, 320, 240, NULL_HWND, 0, 0, 0, 0, 0, &marshaler), NULL_HWND);
            EXPECT_TRUE(rejected);
            EXPECT_EQ(hwnd_mgr.find(created), nullptr);
        }
    }
}

TEST_F(WindowManagerTest, ConcurrentCreateCannotClearOuterMarshaler) {
    auto wm = make_wm();
    GuestMemory memory;
    cpu::CpuState outer_cpu;
    cpu::CpuState inner_cpu;
    ASSERT_TRUE(memory.map_region(0x3000, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    wm.set_createstruct_scratch(0x3000);
    outer_cpu.mode = inner_cpu.mode = cpu::CpuMode::Protected;
    inner_cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::ESP), 0x4000);
    Marshaler outer(memory);
    Marshaler inner(memory);
    outer.attach_cpu(outer_cpu);
    inner.attach_cpu(inner_cpu);
    bool calculated = false;
    wm.set_wndproc_callback([&](HWND, u32 message, u64, u64, u64) {
        if (message == static_cast<u32>(WM::NCCREATE)) {
            // A concurrent failed call used to clear the manager's shared
            // marshaler, dropping the outer call's remaining creation work.
            std::thread other([&] { impl_CreateWindowExW(inner, wm); });
            other.join();
            return 1u;
        }
        if (message == static_cast<u32>(WM::NCCALCSIZE)) calculated = true;
        return 0u;
    });
    EXPECT_NE(wm.create_window(0, L"TestClass", L"Outer", WS::CAPTION,
                               0, 0, 100, 100, NULL_HWND, 0, 0, 0,
                               0, 0, &outer), NULL_HWND);
    EXPECT_TRUE(calculated);
}

TEST_F(WindowManagerTest, GuestChromePopupKeepsFullClientRect) {
    // SplashSkin / guest-owned frames paint their own chrome. Subtracting
    // Cocoa caption or menu from the client clips guest pixels and text.
    auto wm = make_wm();
    const HWND hwnd = wm.create_window(
        0, L"TestClass", L"Splash", WS::POPUP,
        10, 20, 400, 300, NULL_HWND, 0, 0, 0);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.width(), 400);
    EXPECT_EQ(data->rect.height(), 300);
    EXPECT_EQ(data->client_rect.width(), 400);
    EXPECT_EQ(data->client_rect.height(), 300);
    EXPECT_EQ(data->client_offset.x, 0);
    EXPECT_EQ(data->client_offset.y, 0);
}

TEST_F(WindowManagerTest, X64CreateStructUsesNativePointerLayout) {
    auto wm = make_wm();
    constexpr GuestAddr scratch = 0x3000;
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    ASSERT_TRUE(memory.map_region(scratch, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    wm.set_createstruct_scratch(scratch);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // The CREATESTRUCT is valid while WM_NCCREATE / WM_CREATE run (on
    // Windows it lives on the creating thread's stack), so snapshot it there.
    GuestAddr create_struct = 0;
    std::array<u8, 80> snapshot{};
    wm.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64 lparam, u64) {
            if (message == static_cast<u32>(WM::NCCREATE)) {
                create_struct = lparam;
                (void)memory.read_bytes(lparam, snapshot.data(), snapshot.size());
            }
            return message == static_cast<u32>(WM::NCCREATE) ? 1u : 0u;
        });

    constexpr u64 param = 0x1234567887654321ull;
    constexpr u64 instance = 0x140000000ull;
    constexpr u64 menu = 0xabcdef0123456789ull;
    // A local class is found only for the instance that registered it
    // (Learn: About Window Classes; measured, s1win_class_probe
    // create_local_other_instance).
    {
        WindowClassData wc;
        wc.class_name = L"TestClass";
        wc.wndproc = 0x1000;
        wc.hinstance = instance;
        ASSERT_NE(class_mgr.register_class(wc), 0);
    }
    constexpr GuestAddr class_ptr = 0x123400001000ull;
    constexpr GuestAddr name_ptr = 0x123400002000ull;
    ASSERT_NE(wm.create_window(0x10, L"TestClass", L"", 0x20,
                               1, 2, 3, 4, NULL_HWND, menu, instance, param,
                               class_ptr, name_ptr, &marshaler), NULL_HWND);

    ASSERT_EQ(create_struct, scratch);
    auto u64_at = [&](usize off) { u64 v; std::memcpy(&v, snapshot.data() + off, 8); return v; };
    auto u32_at = [&](usize off) { u32 v; std::memcpy(&v, snapshot.data() + off, 4); return v; };
    EXPECT_EQ(u64_at(0), param);
    EXPECT_EQ(u64_at(8), instance);
    EXPECT_EQ(u64_at(16), menu);
    EXPECT_EQ(u32_at(32), 4u);
    EXPECT_EQ(u32_at(36), 3u);
    EXPECT_EQ(u64_at(56), name_ptr);
    EXPECT_EQ(u64_at(64), class_ptr);
    // CREATESTRUCT.dwExStyle gets WS_EX_WINDOWEDGE only from a dialog or
    // sizing frame, not the one every overlapped window is given (measured,
    // s1win_msgseq2_probe create_overlapped_plain_hidden: ex=08000000;
    // s1win_msgseq_probe create_overlapped_hidden, WS_OVERLAPPEDWINDOW:
    // ex=00000100).
    EXPECT_EQ(u32_at(72), 0x10u);
}

TEST_F(WindowManagerTest, CwUseDefaultResolvesToValidGeometry) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    // CW_USEDEFAULT -> x=100, y=100, w=640, h=480
    EXPECT_EQ(data->rect.left, 100);
    EXPECT_EQ(data->rect.top, 100);
    EXPECT_EQ(data->rect.right, 740);   // 100 + 640
    EXPECT_EQ(data->rect.bottom, 580);  // 100 + 480
}

TEST_F(WindowManagerTest, CwUseDefaultWidthIgnoresExplicitHeight) {
    // Learn (CreateWindowExW): "If the nWidth parameter is set to
    // CW_USEDEFAULT, the system ignores nHeight" and selects a default width
    // and height; a CW_USEDEFAULT x likewise ignores y. Battle.net's
    // AgentDummyWindow passes (CW_USEDEFAULT, 0, CW_USEDEFAULT, 0), which must
    // not produce a zero-height overlapped window.
    auto wm = make_wm();
    HWND hwnd = wm.create_window(
        0, L"TestClass", L"Agent", WS::OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
        NULL_HWND, 0, 0, 0
    );
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.left, 100);
    EXPECT_EQ(data->rect.top, 100);
    EXPECT_EQ(data->rect.width(), 640);
    EXPECT_EQ(data->rect.height(), 480);
    EXPECT_GT(data->client_rect.height(), 0);
}

TEST_F(WindowManagerTest, ExplicitHeightSurvivesExplicitWidth) {
    // The ignore rule is scoped to defaulted extents: an explicit nWidth keeps
    // an explicit nHeight, including a deliberate zero extent.
    auto wm = make_wm();
    HWND hwnd = wm.create_window(
        0, L"TestClass", L"Sized", WS::OVERLAPPEDWINDOW,
        10, 20, 300, 0, NULL_HWND, 0, 0, 0
    );
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.left, 10);
    EXPECT_EQ(data->rect.top, 20);
    EXPECT_EQ(data->rect.width(), 300);
    EXPECT_EQ(data->rect.height(), 0);
}

TEST_F(WindowManagerTest, ZeroSizedOverlappedIsRaisedToMinTrackSize) {
    // Measured (tools/oracle_probe/s1win_api_probe.c,
    // create_overlapped_zero_mintrack): a 0x0 WS_OVERLAPPEDWINDOW becomes
    // SM_CXMINTRACK x SM_CYMINTRACK (136x39): USER pre-fills MINMAXINFO before
    // WM_GETMINMAXINFO, so the bound applies even when the procedure ignores
    // the message. The procedure below also leaves WM_NCCALCSIZE alone, so
    // the whole window is client area.
    auto wm = make_wm();
    constexpr GuestAddr scratch = 0x3000;
    GuestMemory memory;
    cpu::CpuState cpu;
    ASSERT_TRUE(memory.map_region(scratch, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());
    wm.set_createstruct_scratch(scratch);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    wm.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64, u64) {
            return message == static_cast<u32>(WM::NCCREATE) ? 1u : 0u;
        });

    HWND hwnd = wm.create_window(
        0, L"TestClass", L"Zero", WS::OVERLAPPEDWINDOW,
        0, 0, 0, 0, NULL_HWND, 0, 0, 0, 0, 0, &marshaler);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.width(), 136);
    EXPECT_EQ(data->rect.height(), 39);
    EXPECT_EQ(data->client_rect.width(), 136);
    EXPECT_EQ(data->client_rect.height(), 39);
}

TEST_F(WindowManagerTest, ZeroSizedChildKeepsZeroExtent) {
    auto wm = make_wm();
    HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);

    // Chromium creates Chrome Legacy Window with an explicit zero extent and
    // sizes it from the view afterwards, so the extent must stay zero rather
    // than becoming the overlapped default.
    HWND child = wm.create_window(
        0, L"TestClass", L"Zero", WS::CHILD | WS::VISIBLE,
        0, 0, 0, 0, parent, 0, 0, 0
    );
    ASSERT_NE(child, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(child);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.width(), 0);
    EXPECT_EQ(data->rect.height(), 0);
    EXPECT_EQ(data->client_rect.width(), 0);
    EXPECT_EQ(data->client_rect.height(), 0);
}

TEST_F(WindowManagerTest, CwUseDefaultOnPopupResolvesToZero) {
    auto wm = make_wm();
    HWND owner = create_default(wm);
    ASSERT_NE(owner, NULL_HWND);

    // CW_USEDEFAULT is defined only for overlapped windows; a pop-up or child
    // window resolves it to zero position and extent instead.
    HWND popup = wm.create_window(
        0, L"TestClass", L"Popup", WS::POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        owner, 0, 0, 0
    );
    ASSERT_NE(popup, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(popup);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->rect.left, 0);
    EXPECT_EQ(data->rect.top, 0);
    EXPECT_EQ(data->rect.width(), 0);
    EXPECT_EQ(data->rect.height(), 0);
}

TEST_F(WindowManagerTest, ChildWindowIsParentedCorrectly) {
    auto wm = make_wm();
    // Create parent first
    HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);

    // Create child
    HWND child = wm.create_window(
        0, L"TestClass", L"Child", WS::CHILD | WS::VISIBLE,
        10, 20, 200, 100, parent, 0, 0, 0
    );
    ASSERT_NE(child, NULL_HWND);
    const WindowData* child_data = hwnd_mgr.find(child);
    ASSERT_NE(child_data, nullptr);
    EXPECT_EQ(child_data->parent, parent);
}

TEST_F(WindowManagerTest, TopLevelPopupUsesOwnerNotParent) {
    auto wm = make_wm();
    HWND owner = create_default(wm);
    ASSERT_NE(owner, NULL_HWND);

    HWND popup = wm.create_window(
        0, L"TestClass", L"Popup", WS::POPUP | WS::VISIBLE,
        20, 30, 200, 120, owner, 0, 0, 0
    );
    ASSERT_NE(popup, NULL_HWND);

    const WindowData* popup_data = hwnd_mgr.find(popup);
    ASSERT_NE(popup_data, nullptr);
    EXPECT_EQ(popup_data->parent, HWND_DESKTOP);
    EXPECT_EQ(popup_data->owner, owner);
}

TEST_F(WindowManagerTest, TopLevelWindowCreatesNativeWindow) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_TRUE(bridge.has_native_window(hwnd));
}

TEST_F(WindowManagerTest, ChildWindowDoesNotCreateNativeWindow) {
    auto wm = make_wm();
    HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);

    HWND child = wm.create_window(
        0, L"TestClass", L"Child", WS::CHILD,
        10, 20, 200, 100, parent, 0, 0, 0
    );
    ASSERT_NE(child, NULL_HWND);
    EXPECT_FALSE(bridge.has_native_window(child));
}

// ---------------------------------------------------------------------------
// DestroyWindow
// ---------------------------------------------------------------------------

TEST_F(WindowManagerTest, DestroyWindowRemovesFromHwndManager) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_TRUE(wm.destroy_window(hwnd));
    EXPECT_EQ(hwnd_mgr.find(hwnd), nullptr);
}

TEST_F(WindowManagerTest, DestroyWindowDestroysNativeWindow) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_TRUE(bridge.has_native_window(hwnd));
    wm.destroy_window(hwnd);
    EXPECT_FALSE(bridge.has_native_window(hwnd));
}

TEST_F(WindowManagerTest, DestroyOwnerAlsoDestroysOwnedPopups) {
    auto wm = make_wm();
    HWND owner = create_default(wm);
    ASSERT_NE(owner, NULL_HWND);

    HWND popup = wm.create_window(
        0, L"TestClass", L"Popup", WS::POPUP | WS::VISIBLE,
        20, 30, 200, 120, owner, 0, 0, 0
    );
    ASSERT_NE(popup, NULL_HWND);
    ASSERT_NE(hwnd_mgr.find(popup), nullptr);

    EXPECT_TRUE(wm.destroy_window(owner));
    EXPECT_EQ(hwnd_mgr.find(owner), nullptr);
    EXPECT_EQ(hwnd_mgr.find(popup), nullptr);
}

TEST_F(WindowManagerTest, CreateToolbarWindow32WithoutRegisterClass) {
    // Learn (InitCommonControlsEx / ICC_BAR): ToolbarWindow32 is a comctl32
    // system class. CreateWindowEx must accept it without an app RegisterClass.
    auto wm = make_wm();
    HWND parent = create_default(wm, WS::OVERLAPPEDWINDOW);
    ASSERT_NE(parent, NULL_HWND);

    HWND toolbar = wm.create_window(
        0x188, L"ToolbarWindow32", L"",
        WS::CHILD | WS::VISIBLE | 0x95d,
        0, 0, 0, 0, parent, 0, 0, 0);
    ASSERT_NE(toolbar, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(toolbar);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->class_name, L"ToolbarWindow32");
}

TEST_F(WindowManagerTest, DestroyInvalidHwndReturnsFalse) {
    auto wm = make_wm();
    EXPECT_FALSE(wm.destroy_window(0xDEAD));
}

TEST_F(WindowManagerTest, ClassMenuLoadsBeforeWmCreate) {
    // Learn (CreateWindowExW): NULL hMenu uses the class menu; GetMenu must
    // already succeed by the time WM_CREATE runs.
    WindowClassData wc;
    wc.class_name = L"MenuClass";
    wc.wndproc = 0x2000;
    wc.menu_name = L"1500";
    class_mgr.register_class(wc);

    auto wm = make_wm();
    bool menu_loaded = false;
    bool saw_create = false;
    wm.set_menu_load_callback(
        [&](HWND, const std::wstring& name) {
            EXPECT_FALSE(saw_create);
            EXPECT_EQ(name, L"1500");
            menu_loaded = true;
        });
    wm.set_wndproc_callback(
        [&](HWND, u32 msg, u64, u64, u64) -> u64 {
            if (msg == static_cast<u32>(WM::CREATE)) {
                saw_create = true;
                EXPECT_TRUE(menu_loaded);
            }
            // WM_NCCREATE requires TRUE to continue creation.
            return 1;
        });

    HWND hwnd = wm.create_window(
        0, L"MenuClass", L"WithMenu", WS::OVERLAPPEDWINDOW,
        0, 0, 200, 100, NULL_HWND, 0, 0, 0);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_TRUE(menu_loaded);
    EXPECT_TRUE(saw_create);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->has_menu);
}

TEST_F(WindowManagerTest, CreateAndDestroySendCorrectMessagesBeforeReturning) {
    auto wm = make_wm();
    std::vector<u32> delivered;
    wm.set_wndproc_callback([&](HWND, u32 message, u64, u64, u64) -> u64 {
        delivered.push_back(message);
        return message == WM::NCCREATE ? 1u : 0u;
    });
    const HWND hwnd = create_default(wm);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_EQ(delivered, (std::vector<u32>{WM::NCCREATE, WM::CREATE}));
    EXPECT_TRUE(queue.empty());
    delivered.clear();
    wm.destroy_window(hwnd);
    EXPECT_EQ(delivered, (std::vector<u32>{WM::DESTROY, WM::NCDESTROY}));
    EXPECT_TRUE(queue.empty());
}

TEST_F(WindowManagerTest, VisibleWindowIsMarkedVisible) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm, WS::OVERLAPPEDWINDOW | WS::VISIBLE);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->visible);
}

TEST_F(WindowManagerTest, VisibleCreationSendsShowWindowBeforeReturning) {
    auto wm = make_wm();
    std::vector<u32> messages;
    wm.set_wndproc_callback(
        [&](HWND, u32 message, u64 wparam, u64 lparam, u64) {
            messages.push_back(message);
            if (message == static_cast<u32>(WM::SHOWWINDOW)) {
                EXPECT_EQ(wparam, 1u);
                EXPECT_EQ(lparam, 0u);
            }
            return message == static_cast<u32>(WM::NCCREATE) ? 1u : 0u;
        });
    wm.set_activation_callback(
        [&](HWND target) {
            EXPECT_NE(target, NULL_HWND);
            messages.push_back(WM::ACTIVATEAPP);
        });

    const HWND hwnd = create_default(
        wm, WS::OVERLAPPEDWINDOW | WS::VISIBLE);
    ASSERT_NE(hwnd, NULL_HWND);

    // This procedure does not call DefWindowProc, so it must not receive
    // default WM_SIZE/WM_MOVE notifications.
    // WM_WINDOWPOSCHANGED needs a guest WINDOWPOS buffer and is therefore only
    // sent when a marshaler is wired up, which this fixture does not do.
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages[0], WM::NCCREATE);
    EXPECT_EQ(messages[1], WM::CREATE);
    EXPECT_EQ(messages[2], WM::SHOWWINDOW);
    EXPECT_EQ(messages[3], WM::ACTIVATEAPP);

    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(queue.empty());
}

TEST_F(WindowManagerTest, HiddenCreationDoesNotReportPlacement) {
    auto wm = make_wm();
    std::vector<u32> messages;
    wm.set_wndproc_callback(
        [&](HWND, u32 message, u64, u64, u64) {
            messages.push_back(message);
            return message == static_cast<u32>(WM::NCCREATE) ? 1u : 0u;
        });

    const HWND hwnd = create_default(wm, WS::OVERLAPPEDWINDOW);
    ASSERT_NE(hwnd, NULL_HWND);

    // Without WS_VISIBLE nothing is displayed, so Windows reports no show,
    // size, or move; the app learns its geometry from its first ShowWindow.
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], WM::NCCREATE);
    EXPECT_EQ(messages[1], WM::CREATE);
}

TEST_F(WindowManagerTest, VisibleCreationEstablishesInitialUpdateRegion) {
    auto wm = make_wm();
    PaintingManager painting(&hwnd_mgr);
    wm.set_painting_manager(&painting);

    const HWND hwnd = create_default(
        wm, WS::OVERLAPPEDWINDOW | WS::VISIBLE);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_TRUE(painting.needs_paint(hwnd));
}

TEST_F(WindowManagerTest, HiddenCreationDoesNotEstablishUpdateRegion) {
    auto wm = make_wm();
    PaintingManager painting(&hwnd_mgr);
    wm.set_painting_manager(&painting);

    const HWND hwnd = create_default(wm, WS::OVERLAPPEDWINDOW);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_FALSE(painting.needs_paint(hwnd));
}

TEST_F(WindowManagerTest, NonVisibleWindowIsNotMarkedVisible) {
    auto wm = make_wm();
    HWND hwnd = create_default(wm, WS::OVERLAPPEDWINDOW);
    ASSERT_NE(hwnd, NULL_HWND);
    const WindowData* data = hwnd_mgr.find(hwnd);
    ASSERT_NE(data, nullptr);
    EXPECT_FALSE(data->visible);
}

TEST_F(WindowManagerTest, MakeWindowExportsRegistersEntries) {
    auto wm = make_wm();
    DllExports exports = make_window_exports(wm);
    ASSERT_EQ(exports.entries.size(), 3u);
    EXPECT_EQ(exports.entries[0].name, "CreateWindowExW");
    EXPECT_EQ(exports.entries[1].name, "CreateWindowExA");
    EXPECT_EQ(exports.entries[2].name, "DestroyWindow");
}

TEST_F(WindowManagerTest, SuperclassEditInitializesRealNativeControlAndKeepsIdentity) {
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        for (const bool ansi : {false, true}) {
            auto wm = make_wm();
            const HWND parent = create_default(wm);
            ASSERT_NE(parent, NULL_HWND);
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
            cpu::CpuState cpu;
            cpu.mode = mode;
            Marshaler m(memory);
            m.attach_cpu(cpu);
            capsule::Vfs vfs;
            User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
            ext.set_system_control_initializer(
                [&](Marshaler& marshaler, HWND hwnd, std::wstring_view name, u64 cs, bool is_ansi) {
                    return wm.initialize_system_control(marshaler, hwnd, name, cs, is_ansi);
                });
            WindowClassData base;
            ASSERT_TRUE(fill_system_class(L"Edit", base));
            EXPECT_NE(base.wndproc, system_class_wndproc(L"Button"));
            const u64 saved_proc = system_class_wndproc(L"Edit", ansi);
            base.system = false;
            base.unicode = !ansi;
            base.class_name = L"DerivedTextControl";
            base.hinstance = 0x400000;
            base.wndproc = 0x2000;
            base.wnd_extra += sizeof(u64);
            const ATOM atom = class_mgr.register_class(base);
            ASSERT_NE(atom, 0u);
            wm.set_createstruct_scratch(0x3000);
            HWND descendant = NULL_HWND;
            HWND later_sibling = NULL_HWND;
            wm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64 wp, u64 lp, u64 proc) {
                if (hwnd == parent) return u64{0}; // Parent notifications use its own test proc.
                if (proc != 0x2000u) {
                    EXPECT_TRUE(is_native_control_wndproc(proc));
                    ext.call_window_proc_values(m, proc, hwnd, msg, wp, lp, false);
                    return cpu.gpr.rax;
                }
                EXPECT_EQ(proc, 0x2000u);
                if (msg == WM::NCCREATE) {
                    descendant = wm.create_window(0, L"Static", L"Nested", WS::CHILD,
                        1, 1, 20, 10, hwnd, 8, 0, 0, 0, 0, &m);
                    later_sibling = wm.create_window(0, L"Edit", L"Sibling", WS::CHILD | sdk_creation::WS_TABSTOP,
                        0, 45, 180, 30, parent, 9, 0, 0, 0, 0, &m);
                    EXPECT_NE(descendant, NULL_HWND);
                    EXPECT_NE(later_sibling, NULL_HWND);
                }
                ext.call_window_proc_values(m, saved_proc, hwnd, msg, wp, lp, ansi);
                return cpu.gpr.rax;
            });
            if (ansi) ASSERT_TRUE(m.write_ansi_string(0x3400, "Initial edit text \xc3\xa9").is_ok());
            else ASSERT_TRUE(m.write_wide_string(0x3400, "Initial edit text \xc3\xa9").is_ok());
            const HWND child = wm.create_window(
                0, base.class_name, L"Initial edit text", WS::CHILD | sdk_creation::WS_TABSTOP,
                0, 0, 180, 40, parent, 7, base.hinstance, 0,
                0, 0x3400, &m, ansi);
            ASSERT_NE(child, NULL_HWND);
            ASSERT_TRUE(bridge.has_control_view(child));
            EXPECT_EQ(bridge.get_control_text(child), ansi ? L"Initial edit text \u00c3\u00a9" : L"Initial edit text \u00e9");
            EXPECT_TRUE(bridge.has_control_view(descendant));
            EXPECT_EQ(hwnd_mgr.find(descendant)->parent, child);
            EXPECT_FALSE(bridge.control_view_is_above(child, later_sibling));
            const auto* live = hwnd_mgr.find(child);
            ASSERT_NE(live, nullptr);
            EXPECT_EQ(live->class_name, base.class_name);
            EXPECT_EQ(live->class_atom, atom);
            EXPECT_EQ(live->wndproc, 0x2000u);
            EXPECT_EQ(live->native_control_class, L"Edit");
            EXPECT_EQ(live->wnd_extra_size, static_cast<u32>(base.wnd_extra));
            ASSERT_TRUE(wm.destroy_window(child));
            EXPECT_FALSE(bridge.has_control_view(child));
            EXPECT_EQ(hwnd_mgr.find(child), nullptr);
            EXPECT_FALSE(bridge.has_control_view(descendant));
            wm.set_wndproc_callback({});
            EXPECT_TRUE(wm.destroy_window(parent));
            ClassError error{};
            EXPECT_TRUE(class_mgr.unregister_class(base.class_name, base.hinstance, error));
        }
    }
}

// A window OF a system class (CreateWindowEx "Static") gets its native
// control at creation. Its WM_NCCREATE through the class procedure must find
// that control initialized, not rebind a second presentation: NSIS (Steam
// setup) drew its page header twice, the stale copy in the creation font
// under the WM_SETFONT one.
TEST_F(WindowManagerTest, SystemClassControlKeepsOneNativeViewThroughItsClassProc) {
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        for (const bool ansi : {false, true}) {
            auto wm = make_wm();
            const HWND parent = create_default(wm);
            ASSERT_NE(parent, NULL_HWND);
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
            cpu::CpuState cpu;
            cpu.mode = mode;
            Marshaler m(memory);
            m.attach_cpu(cpu);
            capsule::Vfs vfs;
            User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
            ext.set_system_control_initializer(
                [&](Marshaler& marshaler, HWND hwnd, std::wstring_view name, u64 cs, bool is_ansi) {
                    return wm.initialize_system_control(marshaler, hwnd, name, cs, is_ansi);
                });
            wm.set_createstruct_scratch(0x3000);
            u32 class_proc_calls = 0;
            wm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64 wp, u64 lp, u64 proc) {
                if (hwnd == parent) return u64{0};
                EXPECT_TRUE(is_native_control_wndproc(proc));
                ++class_proc_calls;
                ext.call_window_proc_values(m, proc, hwnd, msg, wp, lp, ansi);
                return cpu.gpr.rax;
            });
            const std::wstring title = L"Welcome to Setup";
            if (ansi) ASSERT_TRUE(m.write_ansi_string(0x3400, "Welcome to Setup").is_ok());
            else ASSERT_TRUE(m.write_wide_string(0x3400, "Welcome to Setup").is_ok());
            constexpr u32 kWsExTransparent = 0x20;
            const HWND label = wm.create_window(
                kWsExTransparent, L"Static", title, WS::CHILD | WS::VISIBLE,
                180, 16, 293, 46, parent, 1201, 0, 0, 0, 0x3400, &m, ansi);
            ASSERT_NE(label, NULL_HWND);
            EXPECT_GT(class_proc_calls, 0u);  // NCCREATE/CREATE went through the class proc
            ASSERT_TRUE(bridge.has_control_view(label));
            EXPECT_EQ(bridge.sibling_control_views(label), 1u);  // the parent's only child
            const auto* live = hwnd_mgr.find(label);
            ASSERT_NE(live, nullptr);
            EXPECT_TRUE(live->native_control_class.empty());
            wm.set_wndproc_callback({});
            ASSERT_TRUE(wm.destroy_window(label));
            EXPECT_FALSE(bridge.has_control_view(label));
            EXPECT_TRUE(wm.destroy_window(parent));
        }
    }
}

// VCL's TEdit.Text reads WM_GETTEXTLENGTH / WM_GETTEXT through
// CallWindowProc(EDIT class proc). Learn WM_GETTEXT: wParam = buffer size in
// characters incl. the NUL; copies at most wParam-1 and NUL-terminates;
// returns the count copied. WM_GETTEXTLENGTH: characters (bytes for A).
// Regression: both fell through as "unhandled -> 0", so Inno Setup read an
// empty install folder and refused it.
TEST_F(WindowManagerTest, SubclassedEditGetTextAnswersFromBaseProc) {
    auto wm = make_wm();
    const HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Protected;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    MessageSendManager msm(hwnd_mgr, queue, bridge);
    capsule::Vfs vfs;
    User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
    ext.set_message_send_manager(&msm);

    const HWND edit = wm.create_window(
        0, L"Edit", L"", WS::CHILD, 0, 0, 200, 20, parent, 1, 0, 0);
    ASSERT_NE(edit, NULL_HWND);
    hwnd_mgr.find(edit)->wndproc = 0x2000;
    const std::wstring path = L"C:\\Program Files\\Microsoft VS Code";
    (void)bridge.set_control_text(edit, path);
    const u32 len = static_cast<u32>(path.size());
    constexpr u32 kGetText = 0x000D;
    constexpr u32 kGetTextLength = 0x000E;

    auto call = [&](bool ansi, u32 msg, u64 wp, u64 lp) {
        ext.call_window_proc_values(m, system_class_wndproc(L"Edit", ansi),
                                    edit, msg, wp, lp, ansi);
        return cpu.gpr.rax;
    };

    // W: length, full copy, truncation, zero-size buffer.
    EXPECT_EQ(call(false, kGetTextLength, 0, 0), len);
    EXPECT_EQ(call(false, kGetText, 64, 0x3000), len);
    for (u32 i = 0; i < len; ++i) {
        EXPECT_EQ(memory.read_u16(0x3000 + i * 2).value(), static_cast<u16>(path[i]));
    }
    EXPECT_EQ(memory.read_u16(0x3000 + len * 2).value(), 0u);
    EXPECT_EQ(call(false, kGetText, 4, 0x3100), 3u);
    EXPECT_EQ(memory.read_u16(0x3100).value(), u'C');
    EXPECT_EQ(memory.read_u16(0x3104).value(), u'\\');
    EXPECT_EQ(memory.read_u16(0x3106).value(), 0u);
    ASSERT_TRUE(memory.write_u16(0x3200, 0xBEEF).is_ok());
    EXPECT_EQ(call(false, kGetText, 0, 0x3200), 0u);
    EXPECT_EQ(memory.read_u16(0x3200).value(), 0xBEEFu);

    // A: byte length and CP_ACP bytes.
    EXPECT_EQ(call(true, kGetTextLength, 0, 0), len);
    EXPECT_EQ(call(true, kGetText, 64, 0x3300), len);
    EXPECT_EQ(memory.read_u8(0x3300).value(), 'C');
    EXPECT_EQ(memory.read_u8(0x3300 + len).value(), 0u);

    EXPECT_TRUE(wm.destroy_window(parent));
}

TEST_F(WindowManagerTest, SubclassedEditStreamInLoadsWhenBaseProcRuns) {
    auto wm = make_wm();
    const HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Protected;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    MessageSendManager msm(hwnd_mgr, queue, bridge);
    capsule::Vfs vfs;
    User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
    ext.set_message_send_manager(&msm);

    constexpr u32 kEsMultiline = 0x0004;
    constexpr u32 kEmStreamIn = 0x0449;
    constexpr u32 kSfRtf = 0x0002;
    const HWND edit = wm.create_window(
        0, L"Edit", L"", WS::CHILD | kEsMultiline,
        0, 0, 200, 80, parent, 1, 0, 0);
    ASSERT_NE(edit, NULL_HWND);
    auto* live = hwnd_mgr.find(edit);
    ASSERT_NE(live, nullptr);
    live->wndproc = 0x2000;

    ASSERT_TRUE(m.write_u32(0x3000, 0x1111).is_ok());
    ASSERT_TRUE(m.write_u32(0x3004, 0).is_ok());
    ASSERT_TRUE(m.write_u32(0x3008, 0x8888).is_ok());

    int calls = 0;
    msm.set_guest_call([&](GuestAddr proc, const u64* args, usize n, bool is64) -> u64 {
        EXPECT_EQ(proc, 0x8888u);
        EXPECT_FALSE(is64);
        EXPECT_EQ(n, 4u);
        const GuestAddr pcb = static_cast<GuestAddr>(args[3]);
        if (calls++ > 0) {
            EXPECT_TRUE(m.write_u32(pcb, 0).is_ok());
            return 0;
        }
        const char rtf[] = "{\\rtf1\\ansi License text}";
        const u32 nbytes = static_cast<u32>(sizeof(rtf) - 1);
        EXPECT_LE(nbytes, static_cast<u32>(args[2]));
        EXPECT_TRUE(m.memory().write_bytes(
            static_cast<GuestAddr>(args[1]),
            reinterpret_cast<const u8*>(rtf), nbytes).is_ok());
        EXPECT_TRUE(m.write_u32(pcb, nbytes).is_ok());
        return 0;
    });

    EXPECT_FALSE(msm.dispatch_marshaled_control(
        m, edit, kEmStreamIn, kSfRtf, 0x3000, false, false));
    EXPECT_EQ(calls, 0);

    ext.call_window_proc_values(
        m, system_class_wndproc(L"Edit"), edit, kEmStreamIn, kSfRtf, 0x3000, false);
    EXPECT_GT(calls, 0);
    EXPECT_NE(bridge.get_control_text(edit).find(L"License text"), std::wstring::npos);
    EXPECT_TRUE(wm.destroy_window(parent));
}

TEST_F(WindowManagerTest, SuperclassRejectsFailedNativeInitialization) {
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        auto wm = make_wm();
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        cpu.mode = mode;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        capsule::Vfs vfs;
        User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
        ext.set_system_control_initializer(
            [&](Marshaler& marshaler, HWND hwnd, std::wstring_view name, u64 cs, bool ansi) {
                return wm.initialize_system_control(marshaler, hwnd, name, cs, ansi);
            });
        // A live HWND without native backing cannot host the control. The
        // base proc must reject creation, not report a fictitious initialized edit.
        WindowData parent_data;
        const HWND parent = hwnd_mgr.create(parent_data);
        HWND attempted = NULL_HWND;
        wm.set_createstruct_scratch(0x3000);
        wm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64 wp, u64 lp, u64) {
            attempted = hwnd;
            ext.call_window_proc_values(m, system_class_wndproc(L"Edit"), hwnd, msg, wp, lp, false);
            return cpu.gpr.rax;
        });
        EXPECT_EQ(wm.create_window(0, L"TestClass", L"Text", WS::CHILD,
            0, 0, 80, 20, parent, 1, 0, 0, 0, 0, &m), NULL_HWND);
        EXPECT_NE(attempted, NULL_HWND);
        EXPECT_EQ(hwnd_mgr.find(attempted), nullptr);
        EXPECT_FALSE(bridge.has_control_view(attempted));
        EXPECT_NE(hwnd_mgr.find(parent), nullptr);
    }
}

TEST_F(WindowManagerTest, MixedEncodingSuperclassCreationFailsExplicitly) {
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    capsule::Vfs vfs;
    User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
    bool called = false;
    ext.set_system_control_initializer([&](Marshaler&, HWND, std::wstring_view, u64, bool) {
        called = true;
        return true;
    });
    testing::internal::CaptureStderr();
    for (auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        cpu.mode = mode;
        for (bool caller_ansi : {false, true}) {
            ext.call_window_proc_values(m, system_class_wndproc(L"Edit", !caller_ansi),
                NULL_HWND, WM::NCCREATE, 0, 0, caller_ansi);
            EXPECT_EQ(cpu.gpr.rax, 0u);
        }
    }
    const auto diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_NE(diagnostic.find("UNIMPLEMENTED user32!CallWindowProc: mixed A/W superclass creation"), std::string::npos);
    EXPECT_FALSE(called);
}

// SysLink class procedure. Expected values are Windows' results in
// tools/oracle_probe/syslink_probe{32,64}.expected (line names in comments).
namespace {

struct SysLinkHarness {
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler m{memory};
    struct Notify { u64 wparam; u64 hwnd_from; u64 id_from; u32 code; u32 mask;
                    i32 link; u32 state; std::wstring id; std::wstring url; };
    std::vector<Notify> notifies;
};

std::wstring read_w(GuestMemory& mem, GuestAddr at, u32 max) {
    std::wstring s;
    for (u32 i = 0; i < max; ++i) {
        const u16 c = mem.read_u16(at + i * 2).value_or(0);
        if (c == 0) break;
        s.push_back(static_cast<wchar_t>(c));
    }
    return s;
}

void run_syslink_contract(bool is64, HwndManager& hwnd_mgr, MessageQueue& queue,
                          CocoaBridge& bridge, WindowManager& wm, HWND parent) {
    namespace sl = vitreo::windowing::syslink;
    SysLinkHarness h;
    ASSERT_TRUE(h.memory.map_region(0x3000, 0x4000, MemoryProtection::ReadWrite).is_ok());
    h.cpu.mode = is64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
    h.m.attach_cpu(h.cpu);
    MessageSendManager msm(hwnd_mgr, queue, bridge);
    bool shift = false;
    msm.set_key_state_provider([&](u32 vk) -> i16 {
        return (vk == 0x10 && shift) ? static_cast<i16>(0x8000) : 0;
    });
    hwnd_mgr.find(parent)->wndproc = 0x1000;
    msm.set_wndproc_callback([&](HWND, u32 msg, u64 wp, u64 lp, u64) -> u64 {
        if (msg != 0x004E) return 0;
        SysLinkHarness::Notify n{};
        const GuestAddr p = static_cast<GuestAddr>(lp);
        n.wparam = wp;
        n.hwnd_from = is64 ? h.memory.read_u64(p).value_or(0) : h.memory.read_u32(p).value_or(0);
        n.id_from = is64 ? h.memory.read_u64(p + 8).value_or(0) : h.memory.read_u32(p + 4).value_or(0);
        n.code = h.memory.read_u32(p + (is64 ? 16 : 8)).value_or(0);
        const GuestAddr item = p + sl::nmlink_item_offset(is64);
        n.mask = h.memory.read_u32(item + sl::kLitemMask).value_or(0xFF);
        n.link = static_cast<i32>(h.memory.read_u32(item + sl::kLitemLink).value_or(0));
        n.state = h.memory.read_u32(item + sl::kLitemState).value_or(0xFF);
        n.id = read_w(h.memory, item + sl::kLitemId, sl::kMaxLinkIdText);
        n.url = read_w(h.memory, item + sl::kLitemUrl, sl::kMaxUrlLength);
        h.notifies.push_back(n);
        return 0;
    });

    const HWND link = wm.create_window(
        0, L"SysLink", L"one <a id=\"x\" href=\"h1\">first</a> two <a href=\"h2\">second</a>",
        WS::CHILD | WS::VISIBLE, 10, 10, 400, 60, parent, 7, 0, 0);
    ASSERT_NE(link, NULL_HWND);
    hwnd_mgr.find(link)->ctrl_id = 7;

    auto call = [&](u32 msg, u64 wp, u64 lp, bool ansi = false) -> u64 {
        EXPECT_TRUE(msm.dispatch_syslink_control(h.m, link, msg, wp, lp, ansi, false)) << msg;
        return h.cpu.gpr.rax;
    };
    constexpr GuestAddr kBuf = 0x3000;
    constexpr GuestAddr kItem = 0x4000;   // LITEM / LHITTESTINFO (4288 bytes)
    auto item_reset = [&](u32 mask, i32 index, u32 state = 0, u32 state_mask = 0) {
        for (u32 off = 0; off < sl::kLitemSize + 8; off += 4) (void)h.memory.write_u32(kItem + off, 0);
        (void)h.memory.write_u32(kItem + sl::kLitemMask, mask);
        (void)h.memory.write_u32(kItem + sl::kLitemLink, static_cast<u32>(index));
        (void)h.memory.write_u32(kItem + sl::kLitemState, state);
        (void)h.memory.write_u32(kItem + sl::kLitemStateMask, state_mask);
    };
    auto state_of = [&](i32 index) {
        item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, index, 0, 0xFFFFFFFFu);
        EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 1u);
        return h.memory.read_u32(kItem + sl::kLitemState).value_or(0xFF);
    };

    // Text (settext gettextlength=61 gettext_ret=62; A 62/62; trunc*).
    EXPECT_EQ(call(0x000E, 0, 0), 61u);
    EXPECT_EQ(call(0x000E, 0, 0, true), 62u);
    EXPECT_EQ(call(0x000D, 512, kBuf), 62u);
    EXPECT_EQ(read_w(h.memory, kBuf, 512), L"one <a id=\"x\" href=\"h1\">first</a> two <a href=\"h2\">second</a>");
    (void)h.memory.write_u16(kBuf + 10, 0x7777);
    EXPECT_EQ(call(0x000D, 5, kBuf), 0u);
    EXPECT_EQ(read_w(h.memory, kBuf, 16), L"one ");
    EXPECT_EQ(h.memory.read_u16(kBuf + 10).value_or(0), 0x7777u);
    (void)h.memory.write_u8(kBuf, 0x77); (void)h.memory.write_u8(kBuf + 1, 0x77);
    EXPECT_EQ(call(0x000D, 3, kBuf, true), 0u);
    EXPECT_EQ(h.memory.read_u8(kBuf).value_or(1), 0u);
    EXPECT_EQ(h.memory.read_u8(kBuf + 1).value_or(0), 0x77u);
    EXPECT_EQ(call(0x000D, 0, kBuf), 0u);
    EXPECT_EQ(call(0x0087, 0, 0), 0x100u);  // getdlgcode

    // LM_GETITEM (settext link0/link1, getitem_*).
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_ITEMID | sl::LIF_URL | sl::LIF_STATE, 0, 0, 0xFFFFFFFFu);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 1u);
    EXPECT_EQ(h.memory.read_u32(kItem + sl::kLitemState).value_or(0), 0x2u);
    EXPECT_EQ(read_w(h.memory, kItem + sl::kLitemId, 48), L"x");
    EXPECT_EQ(read_w(h.memory, kItem + sl::kLitemUrl, 2084), L"h1");
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL, 5);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL, -1);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_URL, 1);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_ITEMID | sl::LIF_URL, 1);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_ITEMINDEX, 0);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 0, 0xFF, sl::LIS_ENABLED);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 1u);
    EXPECT_EQ(h.memory.read_u32(kItem + sl::kLitemState).value_or(0), sl::LIS_ENABLED);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, 0), 0u);

    // LM_SETITEM (setitem_*, after*).
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL | sl::LIF_ITEMID, 1);
    for (u32 i = 0; i < 6; ++i) (void)h.memory.write_u16(kItem + sl::kLitemUrl + i * 2, u"newurl"[i]);
    for (u32 i = 0; i < 5; ++i) (void)h.memory.write_u16(kItem + sl::kLitemId + i * 2, u"newid"[i]);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 1u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 1, sl::LIS_VISITED | sl::LIS_FOCUSED,
               sl::LIS_VISITED | sl::LIS_FOCUSED);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 1u);
    EXPECT_EQ(state_of(1), 0x7u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 0, sl::LIS_FOCUSED, sl::LIS_FOCUSED);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 1u);
    EXPECT_EQ(state_of(0), 0x3u);
    EXPECT_EQ(state_of(1), 0x6u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 0, 0x20, 0x20);  // statebit5
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 0, 0x100, 0);    // state_undefined_unmasked
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 1u);
    EXPECT_EQ(state_of(0), 0x3u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL, 1);
    for (u32 i = 0; i < 7; ++i) (void)h.memory.write_u16(kItem + sl::kLitemUrl + i * 2, u"another"[i]);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 1u);
    EXPECT_EQ(state_of(1), 0x6u);  // afterurl2: LIS_VISITED kept
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL, 9);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 0u);
    item_reset(sl::LIF_ITEMINDEX, 0);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 0u);

    // LM_GETIDEALSIZE (ideal_*, narrow_*, zero_width).
    const u64 wide = call(sl::LM_GETIDEALHEIGHT, 1000, 0);
    EXPECT_GT(wide, 0u);
    EXPECT_EQ(call(sl::LM_GETIDEALSIZE, 1000, kBuf), wide);
    const i32 cx = static_cast<i32>(h.memory.read_u32(kBuf).value_or(0));
    EXPECT_GT(cx, 0);
    EXPECT_LE(cx, 1000);
    EXPECT_EQ(h.memory.read_u32(kBuf + 4).value_or(0), wide);
    EXPECT_GT(call(sl::LM_GETIDEALSIZE, 40, kBuf), wide);
    EXPECT_EQ(call(sl::LM_GETIDEALSIZE, 0, kBuf), wide);

    // LM_HITTEST (hit first0_gt_0, hit_link0, hit_plaintext, hit_outside).
    i32 first0 = -1;
    i32 first1 = -1;
    const i32 row = static_cast<i32>(wide / 2);
    for (i32 x = 0; x < 400; ++x) {
        item_reset(0, 0);
        (void)h.memory.write_u32(kItem, static_cast<u32>(x));
        (void)h.memory.write_u32(kItem + 4, static_cast<u32>(row));
        if (call(sl::LM_HITTEST, 0, kItem) == 1) {
            const i32 idx = static_cast<i32>(h.memory.read_u32(kItem + 8 + sl::kLitemLink).value_or(9));
            if (idx == 0 && first0 < 0) first0 = x;
            if (idx == 1 && first1 < 0) first1 = x;
        }
    }
    ASSERT_GT(first0, 0);
    EXPECT_GT(first1, first0);
    for (u32 off = 0; off < sl::kLitemSize + 8; off += 4) (void)h.memory.write_u32(kItem + off, 0xCCCCCCCCu);
    (void)h.memory.write_u32(kItem, static_cast<u32>(first0));
    (void)h.memory.write_u32(kItem + 4, static_cast<u32>(row));
    EXPECT_EQ(call(sl::LM_HITTEST, 0, kItem), 1u);
    EXPECT_EQ(h.memory.read_u32(kItem + 8 + sl::kLitemLink).value_or(9), 0u);
    EXPECT_EQ(read_w(h.memory, kItem + 8 + sl::kLitemId, 48), L"x");
    EXPECT_EQ(h.memory.read_u32(kItem + 8 + sl::kLitemMask).value_or(0), 0xCCCCCCCCu);
    EXPECT_EQ(h.memory.read_u16(kItem + 8 + sl::kLitemUrl).value_or(0), 0xCCCCu);
    (void)h.memory.write_u32(kItem, 1);
    EXPECT_EQ(call(sl::LM_HITTEST, 0, kItem), 0u);
    EXPECT_EQ(h.memory.read_u32(kItem + 8 + sl::kLitemLink).value_or(0), 0xFFFFFFFFu);
    EXPECT_EQ(call(sl::LM_HITTEST, 0, 0), 0u);

    // Mouse: nothing on down, NM_CLICK on up (after_down, after_up, click).
    const u64 pt = static_cast<u64>(first0) | (static_cast<u64>(row) << 16);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 1, sl::LIS_FOCUSED, sl::LIS_FOCUSED);
    EXPECT_EQ(call(sl::LM_SETITEM, 0, kItem), 1u);
    call(0x0201, 1, pt);
    EXPECT_TRUE(h.notifies.empty());
    call(0x0202, 0, pt);
    ASSERT_EQ(h.notifies.size(), 1u);
    EXPECT_EQ(h.notifies[0].wparam, 7u);
    EXPECT_EQ(h.notifies[0].hwnd_from, link);
    EXPECT_EQ(h.notifies[0].id_from, 7u);
    EXPECT_EQ(h.notifies[0].code, static_cast<u32>(-2));
    EXPECT_EQ(h.notifies[0].link, 0);
    EXPECT_EQ(h.notifies[0].mask, 0u);
    EXPECT_EQ(h.notifies[0].state, 0u);
    EXPECT_EQ(h.notifies[0].id, L"x");
    EXPECT_EQ(h.notifies[0].url, L"h1");
    EXPECT_EQ(state_of(0), 0x3u);  // afterclick
    EXPECT_EQ(state_of(1), 0x6u);

    // Keys (enter_*, space_*, aftertab, tablast, shifttab).
    EXPECT_FALSE(msm.dispatch_syslink_control(h.m, link, 0x0102, 0x0D, 0, false, false));  // WM_CHAR
    h.notifies.clear();
    call(0x0100, 0x0D, 0);
    ASSERT_EQ(h.notifies.size(), 1u);
    EXPECT_EQ(h.notifies[0].code, static_cast<u32>(-4));
    EXPECT_EQ(h.notifies[0].link, 0);
    call(0x0100, 0x20, 0);
    EXPECT_EQ(h.notifies.size(), 2u);
    call(0x0100, 0x09, 0);
    EXPECT_EQ(state_of(0), 0x2u);
    EXPECT_EQ(state_of(1), 0x7u);
    EXPECT_EQ(call(0x0100, 0x09, 0), 1u);
    EXPECT_EQ(state_of(1), 0x6u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 1, sl::LIS_FOCUSED, sl::LIS_FOCUSED);
    call(sl::LM_SETITEM, 0, kItem);
    shift = true;
    call(0x0100, 0x09, 0);
    shift = false;
    EXPECT_EQ(state_of(0), 0x3u);
    h.notifies.clear();
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_STATE, 0, 0, sl::LIS_FOCUSED);
    call(sl::LM_SETITEM, 0, kItem);
    call(0x0100, 0x0D, 0);
    EXPECT_TRUE(h.notifies.empty());  // enter_nofocus

    // WM_SETTEXT: same text keeps items (resettext); new text re-parses
    // (fewer); NULL clears the text and keeps the items (settextnull).
    const char16_t same[] = u"one <a id=\"x\" href=\"h1\">first</a> two <a href=\"h2\">second</a>";
    for (u32 i = 0; i < sizeof(same) / 2; ++i) (void)h.memory.write_u16(kBuf + i * 2, same[i]);
    EXPECT_EQ(call(0x000C, 0, kBuf), 1u);
    EXPECT_EQ(state_of(1), 0x6u);
    const char16_t fewer[] = u"<a href=\"R2\">c</a>";
    for (u32 i = 0; i < sizeof(fewer) / 2; ++i) (void)h.memory.write_u16(kBuf + i * 2, fewer[i]);
    EXPECT_EQ(call(0x000C, 0, kBuf), 1u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL | sl::LIF_STATE, 0, 0, 0xFFFFFFFFu);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 1u);
    EXPECT_EQ(read_w(h.memory, kItem + sl::kLitemUrl, 2084), L"R2");
    EXPECT_EQ(h.memory.read_u32(kItem + sl::kLitemState).value_or(0), 0x2u);
    item_reset(sl::LIF_ITEMINDEX | sl::LIF_URL, 1);
    EXPECT_EQ(call(sl::LM_GETITEM, 0, kItem), 0u);
    EXPECT_EQ(call(0x000C, 0, 0), 1u);
    EXPECT_EQ(call(0x000E, 0, 0), 0u);
    EXPECT_EQ(state_of(0), 0x2u);

    // Destruction drops the state.
    msm.control_destroyed(link);
    EXPECT_TRUE(wm.destroy_window(link));
}

}  // namespace

TEST_F(WindowManagerTest, SysLinkMatchesWindowsContractX86) {
    auto wm = make_wm();
    const HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);
    run_syslink_contract(false, hwnd_mgr, queue, bridge, wm, parent);
    EXPECT_TRUE(wm.destroy_window(parent));
}

TEST_F(WindowManagerTest, SysLinkMatchesWindowsContractX64) {
    auto wm = make_wm();
    const HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);
    run_syslink_contract(true, hwnd_mgr, queue, bridge, wm, parent);
    EXPECT_TRUE(wm.destroy_window(parent));
}

// A custom class that superclasses SysLink (VCL TNewLinkLabel) forwards
// WM_NCCREATE to the SysLink class procedure. Creation must succeed with a
// real SysLink behind the guest procedure, and the class procedure must then
// answer LM_* for the parsed markup.
TEST_F(WindowManagerTest, SuperclassSysLinkInitializesAndAnswersLinkMessages) {
    namespace sl = vitreo::windowing::syslink;
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        auto wm = make_wm();
        const HWND parent = create_default(wm);
        ASSERT_NE(parent, NULL_HWND);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 0x3000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        cpu.mode = mode;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        capsule::Vfs vfs;
        User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
        MessageSendManager msm(hwnd_mgr, queue, bridge);
        ext.set_message_send_manager(&msm);
        ext.set_system_control_initializer(
            [&](Marshaler& marshaler, HWND hwnd, std::wstring_view name, u64 cs, bool is_ansi) {
                return wm.initialize_system_control(marshaler, hwnd, name, cs, is_ansi);
            });
        WindowClassData base;
        ASSERT_TRUE(fill_system_class(L"SysLink", base));
        const u64 saved_proc = system_class_wndproc(L"SysLink");
        base.system = false;
        base.unicode = true;
        base.class_name = L"TNewLinkLabel";
        base.hinstance = 0x400000;
        base.wndproc = 0x2000;
        ASSERT_NE(class_mgr.register_class(base), 0u);
        wm.set_createstruct_scratch(0x3000);
        wm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64 wp, u64 lp, u64) {
            if (hwnd == parent) return u64{0};
            ext.call_window_proc_values(m, saved_proc, hwnd, msg, wp, lp, false);
            return cpu.gpr.rax;
        });
        const char* markup = "Read the <a id=\"t\" href=\"https://example.test/terms\">terms</a>.";
        ASSERT_TRUE(m.write_wide_string(0x3400, markup).is_ok());
        const HWND label = wm.create_window(
            0, base.class_name, L"Read the <a id=\"t\" href=\"https://example.test/terms\">terms</a>.",
            WS::CHILD | WS::VISIBLE, 0, 0, 300, 20, parent, 7, base.hinstance, 0, 0, 0x3400, &m, false);
        ASSERT_NE(label, NULL_HWND);
        const auto* live = hwnd_mgr.find(label);
        ASSERT_NE(live, nullptr);
        EXPECT_EQ(live->class_name, L"TNewLinkLabel");
        EXPECT_EQ(live->wndproc, 0x2000u);
        EXPECT_EQ(live->native_control_class, L"SysLink");
        ASSERT_TRUE(bridge.has_control_view(label));

        constexpr GuestAddr kItem = 0x4000;
        for (u32 off = 0; off < sl::kLitemSize; off += 4) (void)memory.write_u32(kItem + off, 0);
        (void)memory.write_u32(kItem + sl::kLitemMask, sl::LIF_ITEMINDEX | sl::LIF_URL | sl::LIF_ITEMID);
        (void)memory.write_u32(kItem + sl::kLitemLink, 0);
        ext.call_window_proc_values(m, saved_proc, label, sl::LM_GETITEM, 0, kItem, false);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(read_w(memory, kItem + sl::kLitemUrl, sl::kMaxUrlLength), L"https://example.test/terms");
        EXPECT_EQ(read_w(memory, kItem + sl::kLitemId, sl::kMaxLinkIdText), L"t");

        wm.set_wndproc_callback({});
        ASSERT_TRUE(wm.destroy_window(label));
        EXPECT_TRUE(wm.destroy_window(parent));
        ClassError error{};
        EXPECT_TRUE(class_mgr.unregister_class(base.class_name, base.hinstance, error));
    }
}

// A superclassed msctls_progress32 (VCL TNewProgressBar) is created through
// the class procedure and answers every PBM_* (Learn PBM_* pages).
TEST_F(WindowManagerTest, SuperclassProgressBarInitializesAndAnswersEveryPbm) {
    for (const auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        auto wm = make_wm();
        const HWND parent = create_default(wm);
        ASSERT_NE(parent, NULL_HWND);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        cpu.mode = mode;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        capsule::Vfs vfs;
        User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
        MessageSendManager msm(hwnd_mgr, queue, bridge);
        ext.set_message_send_manager(&msm);
        ext.set_system_control_initializer(
            [&](Marshaler& marshaler, HWND hwnd, std::wstring_view name, u64 cs, bool is_ansi) {
                return wm.initialize_system_control(marshaler, hwnd, name, cs, is_ansi);
            });
        WindowClassData base;
        ASSERT_TRUE(fill_system_class(L"msctls_progress32", base));
        const u64 saved_proc = system_class_wndproc(L"msctls_progress32");
        base.system = false;
        base.unicode = true;
        base.class_name = L"TNewProgressBar";
        base.hinstance = 0x400000;
        base.wndproc = 0x2000;
        ASSERT_NE(class_mgr.register_class(base), 0u);
        wm.set_createstruct_scratch(0x3000);
        wm.set_wndproc_callback([&](HWND hwnd, u32 msg, u64 wp, u64 lp, u64) {
            if (hwnd == parent) return u64{0};
            ext.call_window_proc_values(m, saved_proc, hwnd, msg, wp, lp, false);
            return cpu.gpr.rax;
        });
        ASSERT_TRUE(m.write_wide_string(0x3400, "").is_ok());
        const HWND bar = wm.create_window(0, base.class_name, L"", WS::CHILD | WS::VISIBLE,
                                          0, 42, 516, 21, parent, 0, base.hinstance, 0,
                                          0, 0x3400, &m, false);
        ASSERT_NE(bar, NULL_HWND);
        EXPECT_EQ(hwnd_mgr.find(bar)->native_control_class, L"msctls_progress32");
        EXPECT_EQ(hwnd_mgr.find(bar)->wndproc, 0x2000u);

        auto pbm = [&](u32 msg, u64 wp, u64 lp) {
            ext.call_window_proc_values(m, saved_proc, bar, msg, wp, lp, false);
            return cpu.gpr.rax;
        };
        EXPECT_EQ(pbm(PBM_GETSTEP, 0, 0), 10u);                 // default step
        EXPECT_EQ(pbm(PBM_GETSTATE, 0, 0), 1u);                 // PBST_NORMAL
        EXPECT_EQ(pbm(PBM_GETBARCOLOR, 0, 0), 0xFF000000u);     // CLR_DEFAULT
        EXPECT_EQ(pbm(PBM_GETBKCOLOR, 0, 0), 0xFF000000u);
        pbm(PBM_SETRANGE32, 0, 200);
        constexpr GuestAddr kRange = 0x4000;
        (void)memory.write_u32(kRange, 0x7777);
        (void)memory.write_u32(kRange + 4, 0x7777);
        EXPECT_EQ(pbm(PBM_GETRANGE, 1, kRange), 0u);
        EXPECT_EQ(memory.read_u32(kRange).value_or(9), 0u);
        EXPECT_EQ(memory.read_u32(kRange + 4).value_or(9), 200u);
        EXPECT_EQ(pbm(PBM_GETRANGE, 0, 0), 200u);
        EXPECT_EQ(pbm(PBM_SETBARCOLOR, 0, 0x00FF00), 0xFF000000u);
        EXPECT_EQ(pbm(PBM_GETBARCOLOR, 0, 0), 0x00FF00u);
        EXPECT_EQ(pbm(PBM_SETBKCOLOR, 0, 0x0000FF), 0xFF000000u);
        EXPECT_EQ(pbm(PBM_GETBKCOLOR, 0, 0), 0x0000FFu);
        EXPECT_EQ(pbm(PBM_SETSTATE, 3, 0), 1u);                 // previous: PBST_NORMAL
        EXPECT_EQ(pbm(PBM_GETSTATE, 0, 0), 3u);
        pbm(PBM_SETSTEP, 25, 0);
        EXPECT_EQ(pbm(PBM_GETSTEP, 0, 0), 25u);
        pbm(PBM_SETPOS, 150, 0);
        EXPECT_EQ(pbm(PBM_GETPOS, 0, 0), 150u);

        wm.set_wndproc_callback({});
        ASSERT_TRUE(wm.destroy_window(bar));
        EXPECT_TRUE(wm.destroy_window(parent));
        ClassError error{};
        EXPECT_TRUE(class_mgr.unregister_class(base.class_name, base.hinstance, error));
    }
}

// EDIT class procedure. Expected values: tools/oracle_probe/
// edit_messages_probe{32,64}.expected and edit_messages2_probe*.expected
// (Windows 11 comctl32 v6, identical on x86 and x64 for these lines).
namespace {

struct EditHarness {
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler m{memory};
    std::vector<u32> notes;
};

void run_edit_contract(bool multi, HwndManager& hwnd_mgr, MessageQueue& queue,
                       CocoaBridge& bridge, WindowManager& wm, HWND parent) {
    EditHarness h;
    ASSERT_TRUE(h.memory.map_region(0x3000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    h.cpu.mode = cpu::CpuMode::Protected;
    h.m.attach_cpu(h.cpu);
    MessageSendManager msm(hwnd_mgr, queue, bridge);
    hwnd_mgr.find(parent)->wndproc = 0x1000;
    msm.set_wndproc_callback([&](HWND, u32 msg, u64 wp, u64, u64) -> u64 {
        if (msg == 0x0111) h.notes.push_back(static_cast<u32>(wp >> 16));
        return 0;
    });
    const u32 style = multi ? (0x0004 | 0x0040 | 0x0080 | 0x00200000 | 0x00100000) : 0x0080;
    const HWND edit = wm.create_window(0, L"Edit", L"", WS::CHILD | WS::VISIBLE | style,
                                       0, 0, 300, 100, parent, 5, 0, 0);
    ASSERT_NE(edit, NULL_HWND);
    hwnd_mgr.find(edit)->ctrl_id = 5;

    auto S = [&](u32 msg, u64 wp, u64 lp) -> u64 {
        EXPECT_TRUE(msm.dispatch_edit_control(h.m, edit, msg, wp, lp, false, false)) << std::hex << msg;
        return h.cpu.gpr.rax & 0xFFFFFFFFu;
    };
    constexpr GuestAddr kStr = 0x3000;
    constexpr GuestAddr kBuf = 0x3800;
    auto set_text = [&](const char16_t* t) {
        u32 i = 0;
        for (; t[i]; ++i) (void)h.memory.write_u16(kStr + i * 2, t[i]);
        (void)h.memory.write_u16(kStr + i * 2, 0);
        S(0x000C, 0, kStr);
    };
    auto text = [&]() {
        S(0x000D, 128, kBuf);
        std::u16string out;
        for (u32 i = 0; i < 128; ++i) {
            const u16 c = h.memory.read_u16(kBuf + i * 2).value_or(0);
            if (c == 0) break;
            out.push_back(static_cast<char16_t>(c));
        }
        return out;
    };
    auto getline = [&](u32 line, u16 size) -> std::pair<u64, std::u16string> {
        for (u32 i = 0; i < 64; ++i) (void)h.memory.write_u16(kBuf + i * 2, 0x7777);
        (void)h.memory.write_u16(kBuf, size);
        const u64 got = S(0x00C4, line, kBuf);
        std::u16string out;
        for (u32 i = 0; i < got; ++i) out.push_back(static_cast<char16_t>(h.memory.read_u16(kBuf + i * 2).value_or(0)));
        return {got, out};
    };
    auto sel = [&]() {
        (void)h.memory.write_u32(kBuf, 0xCCCC);
        (void)h.memory.write_u32(kBuf + 4, 0xCCCC);
        const u64 r = S(0x00B0, kBuf, kBuf + 4);
        return std::tuple<u64, u32, u32>(r, h.memory.read_u32(kBuf).value_or(9), h.memory.read_u32(kBuf + 4).value_or(9));
    };
    auto replace = [&](const char16_t* t, bool undo) {
        u32 i = 0;
        for (; t[i]; ++i) (void)h.memory.write_u16(kStr + i * 2, t[i]);
        (void)h.memory.write_u16(kStr + i * 2, 0);
        S(0x00C2, undo ? 1 : 0, kStr);
    };

    // "empty"
    EXPECT_EQ(S(0x00BA, 0, 0), 1u);
    EXPECT_EQ(S(0x00BB, static_cast<u64>(-1), 0), 0u);
    EXPECT_EQ(S(0x00BB, 1, 0), multi ? 0xFFFFFFFFu : 0u);
    EXPECT_EQ(getline(0, 40).first, 0u);
    EXPECT_EQ(h.memory.read_u16(kBuf).value_or(0), 40u);  // untouched ("after=0x0028")
    EXPECT_EQ(S(0x00B8, 0, 0), 0u);   // modify
    EXPECT_EQ(S(0x00C6, 0, 0), 0u);   // canundo
    EXPECT_EQ(S(0x00D5, 0, 0), 30000u);
    EXPECT_EQ(S(0x00D2, 0, 0), 0u);   // pwchar
    EXPECT_EQ(S(0x00D4, 0, 0), 0u);   // margins

    // "crlf"
    h.notes.clear();
    set_text(u"ab\r\ncd\r\n\r\nxyz");
    EXPECT_EQ(h.notes, (multi ? std::vector<u32>{} : std::vector<u32>{0x400, 0x300}));
    EXPECT_EQ(S(0x000E, 0, 0), 13u);
    if (multi) {
        EXPECT_EQ(S(0x00BA, 0, 0), 4u);
        const u32 idx[] = {0, 4, 8, 10};
        const u32 lens[] = {2, 2, 0, 3};
        for (u32 i = 0; i < 4; ++i) {
            EXPECT_EQ(S(0x00BB, i, 0), idx[i]);
            EXPECT_EQ(S(0x00C1, idx[i], 0), lens[i]);
        }
        EXPECT_EQ(S(0x00BB, 4, 0), 0xFFFFFFFFu);
        EXPECT_EQ(getline(1, 40).second, u"cd");
        EXPECT_EQ(getline(2, 40).first, 0u);
        EXPECT_EQ(getline(4, 40).first, 0u);
        EXPECT_EQ(S(0x00C9, 999, 0), 3u);  // lfc(999)
    } else {
        EXPECT_EQ(S(0x00BA, 0, 0), 1u);
        EXPECT_EQ(S(0x00BB, 1, 0), 0u);
        EXPECT_EQ(S(0x00C1, 4, 0), 13u);
        EXPECT_EQ(getline(1, 40).second, u"ab\r\ncd\r\n\r\nxyz");
    }
    EXPECT_EQ(getline(0, 2).second, u"ab");  // getline_small
    EXPECT_EQ(S(0x00B8, 0, 0), 0u);

    // "lonelf", "trailing"
    set_text(u"a\nb\rc");
    EXPECT_EQ(S(0x00BA, 0, 0), 1u);
    EXPECT_EQ(text(), u"a\nb\rc");
    set_text(u"ab\r\ncd\r\n");
    EXPECT_EQ(S(0x00BA, 0, 0), multi ? 3u : 1u);
    if (multi) EXPECT_EQ(S(0x00BB, 3, 0), 0xFFFFFFFFu);

    // Selection ("setsel(*)", "getsel_null", "linelength(-1)").
    set_text(u"hello world");
    S(0x00B1, 7, 2);
    EXPECT_EQ(sel(), std::make_tuple(u64{0x00070002}, 2u, 7u));
    S(0x00B1, 0, static_cast<u64>(-1));
    EXPECT_EQ(sel(), std::make_tuple(u64{0x000b0000}, 0u, 11u));
    S(0x00B1, 3, 3);
    S(0x00B1, static_cast<u64>(static_cast<u32>(-1)), 0);
    EXPECT_EQ(sel(), std::make_tuple(u64{0x00030003}, 3u, 3u));
    S(0x00B1, 50, 60);
    EXPECT_EQ(sel(), std::make_tuple(u64{0x000b000b}, 11u, 11u));
    S(0x00B1, static_cast<u64>(static_cast<u32>(-5)), 4);
    EXPECT_EQ(sel(), std::make_tuple(u64{0x000b0004}, 4u, 11u));
    S(0x00B1, 2, 5);
    EXPECT_EQ(S(0x00B0, 0, 0), 0x00050002u);
    EXPECT_EQ(S(0x00C1, static_cast<u64>(-1), 0), multi ? 8u : 11u);
    // Caret ("caret=5 caret_rev=" 5 single / 2 multi; "setcaret").
    EXPECT_EQ(S(0x1512, 0, 0), 5u);
    S(0x00B1, 5, 2);
    EXPECT_EQ(S(0x1512, 0, 0), multi ? 2u : 5u);
    EXPECT_EQ(S(0x1511, 4, 0), 1u);
    EXPECT_EQ(sel(), std::make_tuple(u64{0x00040004}, 4u, 4u));

    // REPLACESEL / undo / redo ("replacesel", "undo", "redo").
    S(0x00B9, 0, 0);
    S(0x00CD, 0, 0);
    S(0x00B1, 6, 11);
    h.notes.clear();
    replace(u"there", true);
    EXPECT_EQ(h.notes, (multi ? std::vector<u32>{0x400, 0x300, 0x400, 0x300}
                              : std::vector<u32>{0x400, 0x300}));
    EXPECT_EQ(text(), u"hello there");
    EXPECT_EQ(sel(), std::make_tuple(u64{0x000b000b}, 11u, 11u));
    EXPECT_EQ(S(0x00B8, 0, 0), 1u);
    EXPECT_EQ(S(0x00C6, 0, 0), 1u);
    EXPECT_EQ(S(0x00C7, 0, 0), 1u);
    EXPECT_EQ(text(), u"hello world");
    EXPECT_EQ(S(0x00C6, 0, 0), 1u);
    EXPECT_EQ(S(0x00B8, 0, 0), 1u);
    EXPECT_EQ(sel(), std::make_tuple(u64{0x000b0006}, 6u, 11u));
    EXPECT_EQ(S(0x00C7, 0, 0), 1u);
    EXPECT_EQ(text(), u"hello there");
    S(0x00CD, 0, 0);
    S(0x00B1, 0, 0);
    replace(u"X", false);
    EXPECT_EQ(S(0x00C6, 0, 0), 0u);  // replacesel_noundo
    set_text(u"abc");
    EXPECT_EQ(S(0x00C6, 0, 0), 0u);  // settext_resets
    EXPECT_EQ(S(0x00B8, 0, 0), 0u);

    // Limits ("limit5", "settext_over_limit", "replacesel_over_limit",
    // "limit0", "limitmax") and EN_MAXTEXT order ("maxtext").
    S(0x00C5, 5, 0);
    EXPECT_EQ(S(0x00D5, 0, 0), 5u);
    set_text(u"0123456789");
    EXPECT_EQ(S(0x000E, 0, 0), 10u);
    set_text(u"012");
    S(0x00B1, 3, 3);
    h.notes.clear();
    replace(u"ABCDEF", false);
    EXPECT_EQ(text(), u"012AB");
    EXPECT_EQ(h.notes, (multi ? std::vector<u32>{0x400, 0x300, 0x501}
                              : std::vector<u32>{0x501, 0x400, 0x300}));
    h.notes.clear();
    replace(u"Q", false);
    EXPECT_EQ(h.notes, (std::vector<u32>{0x501}));  // maxtext_full
    S(0x00C5, 0, 0);
    EXPECT_EQ(S(0x00D5, 0, 0), multi ? 0xFFFFFFFFu : 0x7FFFFFFEu);
    S(0x00C5, 0x7FFFFFFF, 0);
    EXPECT_EQ(S(0x00D5, 0, 0), multi ? 0x7FFFFFFFu : 0x7FFFFFFEu);

    // Style bits ("setreadonly", "pwchar").
    EXPECT_EQ(S(0x00CF, 1, 0), 1u);
    EXPECT_NE(hwnd_mgr.find(edit)->style & 0x0800u, 0u);
    EXPECT_EQ(S(0x00CF, 0, 0), 1u);
    EXPECT_EQ(hwnd_mgr.find(edit)->style & 0x0800u, 0u);
    S(0x00CC, '*', 0);
    EXPECT_EQ(S(0x00D2, 0, 0), 42u);
    EXPECT_NE(hwnd_mgr.find(edit)->style & 0x0020u, 0u);
    S(0x00CC, 0, 0);
    EXPECT_EQ(S(0x00D2, 0, 0), 0u);
    EXPECT_EQ(hwnd_mgr.find(edit)->style & 0x0020u, 0u);

    // Multiline-only messages, margins, cue banner.
    EXPECT_EQ(S(0x00CB, 0, 0), multi ? 1u : 0u);  // settabstops0
    S(0x00D3, 3, (7u << 16) | 3u);
    EXPECT_EQ(S(0x00D4, 0, 0), 0x00070003u);
    for (u32 i = 0; i < 10; ++i) (void)h.memory.write_u16(kStr + i * 2, u"Type here"[i]);
    EXPECT_EQ(S(0x1501, 0, kStr), multi ? 0u : 1u);
    for (u32 i = 0; i < 32; ++i) (void)h.memory.write_u16(kBuf + i * 2, 0x7777);
    EXPECT_EQ(S(0x1502, kBuf, 4), multi ? 0u : 1u);
    if (!multi) {
        EXPECT_EQ(h.memory.read_u16(kBuf + 4).value_or(0), u'p');
        EXPECT_EQ(h.memory.read_u16(kBuf + 6).value_or(1), 0u);  // "Typ" + NUL
    } else {
        EXPECT_EQ(h.memory.read_u16(kBuf).value_or(0), 0x7777u);
    }
    set_text(u"l1\r\nl2\r\nl3\r\nl4");
    EXPECT_EQ(S(0x00C8, 1, 0), multi ? 1u : 0u);   // fmtlines
    EXPECT_EQ(S(0x00B6, 0, 1), multi ? 1u : 0u);   // linescroll
    EXPECT_EQ(S(0x00B7, 0, 0), multi ? 1u : 0u);   // scrollcaret
    EXPECT_EQ(S(0x1506, 0, 0), multi ? 0u : 0xFFFFFFFFu);  // gethilite
    EXPECT_EQ(S(0x1517, 0, 0), multi ? 4u : 1u);   // getfilelinecount
    EXPECT_EQ(S(0x1514, 1, 0), multi ? 4u : 0u);   // filelineindex(1)

    // WM_CLEAR ("wm_clear=0", undoable).
    set_text(u"hello world");
    S(0x00B1, 0, 6);
    EXPECT_EQ(S(0x0303, 0, 0), 0u);
    EXPECT_EQ(text(), u"world");
    EXPECT_EQ(S(0x00C6, 0, 0), 1u);
    EXPECT_EQ(S(0x00B8, 0, 0), 1u);

    msm.control_destroyed(edit);
    EXPECT_TRUE(wm.destroy_window(edit));
}

}  // namespace

TEST_F(WindowManagerTest, EditSingleLineMatchesWindows) {
    auto wm = make_wm();
    const HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);
    run_edit_contract(false, hwnd_mgr, queue, bridge, wm, parent);
    EXPECT_TRUE(wm.destroy_window(parent));
}

TEST_F(WindowManagerTest, EditMultiLineMatchesWindows) {
    auto wm = make_wm();
    const HWND parent = create_default(wm);
    ASSERT_NE(parent, NULL_HWND);
    run_edit_contract(true, hwnd_mgr, queue, bridge, wm, parent);
    EXPECT_TRUE(wm.destroy_window(parent));
}

TEST_F(WindowManagerTest, NativeCreationCompletesSynchronouslyBeforeSubclassInstallation) {
    for (bool x64 : {false, true}) {
        auto wm = make_wm();
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 0x4000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        wm.set_createstruct_scratch(0x3000);
        std::vector<u32> creation;
        bool returned = false;
        HWND child = NULL_HWND;
        constexpr GuestAddr title = 0x5000, class_name = 0x5100;
        ASSERT_TRUE(m.write_wide_string(title, "Native label").is_ok());
        ASSERT_TRUE(m.write_wide_string(class_name, "Button").is_ok());
        wm.set_wndproc_callback([&](HWND hwnd, u32 message, u64 w, u64 l, u64 proc) -> u64 {
            const auto* live = hwnd_mgr.find(hwnd);
            if (live && live->class_name == L"Button") {
                EXPECT_FALSE(returned);
                EXPECT_EQ(proc, effective_wndproc(*live));
                EXPECT_NE(proc, 0u);
                EXPECT_EQ(w, 0u);
                if (message == WM::NCCREATE || message == WM::CREATE) {
                    creation.push_back(message);
                    EXPECT_NE(l, 0u);
                    const usize name_offset = x64
                        ? offsetof(sdk_creation::CREATESTRUCTW<u64>, lpszName)
                        : offsetof(sdk_creation::CREATESTRUCTW<u32>, lpszName);
                    EXPECT_EQ(m.read_ptr(l + name_offset).value(), title);
                    EXPECT_EQ(m.read_wide_string(m.read_ptr(l + name_offset).value()).value(), "Native label");
                }
            }
            return message == WM::NCCREATE ? 1u : 0u;
        });
        const HWND parent = create_default(wm);
        ASSERT_NE(parent, NULL_HWND);
        queue.clear();
        child = wm.create_window(0, L"Button", L"Native label", WS::CHILD,
            5, 6, 120, 24, parent, 42, 0, 0, class_name, title, &m);
        returned = true;
        ASSERT_NE(child, NULL_HWND);
        EXPECT_EQ(creation, (std::vector<u32>{WM::NCCREATE, WM::CREATE}));
        EXPECT_TRUE(queue.empty());
        // A subclass installed after CreateWindowEx must never receive a late
        // creation message with a stale or fabricated CREATESTRUCT.
        hwnd_mgr.find(child)->wndproc = 0x2000;
        MSG pending{};
        while (queue.peek(pending, child, 0, 0, PM::REMOVE)) {
            EXPECT_NE(pending.message, WM::NCCREATE);
            EXPECT_NE(pending.message, WM::CREATE);
        }
        EXPECT_EQ(hwnd_mgr.find(child)->title, L"Native label");
    }
}

TEST_F(WindowManagerTest, MissingGuestDispatcherFailsWithoutPretendingCreationRan) {
    WindowManager wm(hwnd_mgr, class_mgr, bridge, queue);
    set_last_error(0);
    EXPECT_EQ(create_default(wm), NULL_HWND);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_TRUE(queue.empty());
}

TEST_F(WindowManagerTest, InternalNativeModelWithoutDispatcherDoesNotQueueFictionalCreation) {
    WindowManager wm(hwnd_mgr, class_mgr, bridge, queue);
    const HWND hwnd = wm.create_window(0, L"Button", L"Native model", WS::POPUP,
        0, 0, 120, 24, NULL_HWND, 0, 0, 0);
    ASSERT_NE(hwnd, NULL_HWND);
    EXPECT_EQ(hwnd_mgr.find(hwnd)->title, L"Native model");
    EXPECT_TRUE(queue.empty());
}

TEST_F(WindowManagerTest, CreationRejectsMinusOneAtGuestLresultWidth) {
    for (bool x64 : {false, true}) {
        auto wm = make_wm();
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        wm.set_createstruct_scratch(0x3000);
        HWND allocated = NULL_HWND;
        wm.set_wndproc_callback([&](HWND hwnd, u32 message, u64, u64, u64) -> u64 {
            allocated = hwnd;
            if (message == WM::NCCREATE) return 1;
            if (message == WM::CREATE) return x64 ? ~u64{0} : u64{0xffffffffu};
            return 0;
        });
        set_last_error(0xdead);
        EXPECT_EQ(wm.create_window(0, L"TestClass", L"Rejected", WS::POPUP,
            0, 0, 120, 24, NULL_HWND, 0, 0, 0, 0, 0, &m), NULL_HWND);
        EXPECT_NE(allocated, NULL_HWND);
        EXPECT_EQ(hwnd_mgr.find(allocated), nullptr);
        EXPECT_EQ(get_last_error(), 0u); // measured s1win_api_probe create_create_minus1
        EXPECT_TRUE(queue.empty());
    }
}


TEST_F(WindowManagerTest, GenericNativeCreationRequiresRealBackingAndReadableCreateStruct) {
    for (bool x64 : {false, true}) {
        auto wm = make_wm();
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x3000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory); m.attach_cpu(cpu);
        wm.set_createstruct_scratch(0x3000);
        capsule::Vfs vfs;
        User32ExtManager ext(memory, vfs, hwnd_mgr, class_mgr, bridge, queue);
        unsigned superclass_initializers = 0;
        ext.set_system_control_initializer([&](Marshaler&, HWND, std::wstring_view, u64, bool) {
            ++superclass_initializers; return false;
        });
        wm.set_wndproc_callback([&](HWND hwnd, u32 message, u64 wp, u64 lp, u64 proc) -> u64 {
            if (proc == 0x1000) return message == WM::NCCREATE ? 1u : 0u;
            ext.call_window_proc_values(m, proc, hwnd, message, wp, lp, false);
            return cpu.gpr.rax;
        });
        const HWND parent = create_default(wm);
        ASSERT_NE(parent, NULL_HWND);
        ASSERT_TRUE(m.write_wide_string(0x3400, "Terms content").is_ok());
        ASSERT_TRUE(m.write_wide_string(0x3500, "RichEdit50W").is_ok());
        const HWND child = wm.create_window(0, L"RichEdit50W", L"Terms content", WS::CHILD,
            0, 0, 200, 80, parent, 7, 0, 0, 0x3500, 0x3400, &m);
        ASSERT_NE(child, NULL_HWND);
        EXPECT_TRUE(bridge.has_control_view(child));
        EXPECT_EQ(hwnd_mgr.find(child)->title, L"Terms content");
        EXPECT_EQ(superclass_initializers, 0u);
        const auto call = [&](HWND hwnd, GuestAddr cs) {
            ext.call_window_proc_values(m, native_control_wndproc_address(),
                hwnd, WM::NCCREATE, 0, cs, false);
            return cpu.gpr.rax;
        };
        EXPECT_EQ(call(child, 0), 0u);
        EXPECT_EQ(call(child, 0xdead0000), 0u);
        const usize bytes = x64 ? sizeof(sdk_creation::CREATESTRUCTW<u64>)
                                 : sizeof(sdk_creation::CREATESTRUCTW<u32>);
        EXPECT_EQ(call(child, 0x4000 - bytes + 1), 0u);
        EXPECT_EQ(call(child, 0x3000), 1u);
        WindowData missing{};
        missing.parent = parent; missing.class_name = L"RichEdit50W";
        const HWND no_backing = hwnd_mgr.create(missing);
        EXPECT_FALSE(bridge.has_control_view(no_backing));
        EXPECT_EQ(call(no_backing, 0x3000), 0u);
        EXPECT_EQ(call(0xbad, 0x3000), 0u);
        const HWND placeholder = wm.create_window(0, L"TestClass", L"Pixel placeholder", WS::CHILD,
            0, 0, 100, 50, parent, 8, 0, 0, 0, 0, &m);
        ASSERT_NE(placeholder, NULL_HWND);
        EXPECT_TRUE(bridge.has_control_view(placeholder));
        EXPECT_FALSE(is_native_host_control(*hwnd_mgr.find(placeholder)));
        EXPECT_TRUE(hwnd_mgr.find(placeholder)->native_control_class.empty());
        EXPECT_EQ(call(placeholder, 0x3000), 0u);
    }
}
