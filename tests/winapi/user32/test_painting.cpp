#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <ipc/hwnd_registry.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <windowing/model/window_types.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <filesystem>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::windowing::HwndManager;
using vitreo::windowing::WindowData;

class PaintingTest : public ::testing::Test {
protected:
    PaintingManager mgr;
};

class PaintingWithHwndTest : public ::testing::Test {
protected:
    HwndManager hwnd_mgr;
    PaintingManager mgr{&hwnd_mgr};

    windowing::HWND create_client(i32 w, i32 h) {
        WindowData data{};
        data.parent = windowing::HWND_DESKTOP;
        data.rect = {0, 0, w, h};
        data.client_rect = {0, 0, w, h};
        data.visible = true;
        return hwnd_mgr.create(data);
    }
};

// The union/validation values are native s3gdi_userpaint_probe expectations.
// Each worker owns independent windows; only the manager's host map is shared.
TEST_F(PaintingWithHwndTest, IndependentUpdateRegionsSurviveConcurrentCalls) {
    auto exports = create_painting_thunks(mgr);
    auto find = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [&](const auto& entry) { return entry.name == name; })->handler;
    };
    auto invalidate = find("InvalidateRect");
    auto validate = find("ValidateRect");
    auto client_rect = find("GetClientRect");
    for (bool wide : {false, true}) {
        constexpr unsigned workers = 8;
        std::barrier start(workers);
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&] {
                GuestMemory memory;
                ASSERT_TRUE(memory.map_region(0x10000, 0x4000,
                    MemoryProtection::ReadWrite).is_ok());
                cpu::CpuState cpu;
                if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                cpu.set_sp(0x13000);
                Marshaler m(memory);
                m.attach_cpu(cpu);
                auto args = [&](u64 a, u64 b, u64 c) {
                    if (wide) {
                        cpu.gpr.rcx = a; cpu.gpr.rdx = b; cpu.gpr.r8 = c;
                    } else {
                        EXPECT_TRUE(memory.write_u32(cpu.sp() + 4, a).is_ok());
                        EXPECT_TRUE(memory.write_u32(cpu.sp() + 8, b).is_ok());
                        EXPECT_TRUE(memory.write_u32(cpu.sp() + 12, c).is_ok());
                    }
                };
                const Rect update{10, 10, 30, 20};
                ASSERT_TRUE(memory.write_bytes(0x10000,
                    reinterpret_cast<const Byte*>(&update), sizeof(update)).is_ok());
                start.arrive_and_wait();
                for (unsigned n = 0; n < 256; ++n) {
                    const auto hwnd = create_client(80, 60);
                    args(hwnd, 0x10000, 0);
                    invalidate(m);
                    EXPECT_NE(cpu.gpr.rax, 0u);
                    Rect got{};
                    ASSERT_TRUE(mgr.get_update_rect(hwnd, got, false));
                    EXPECT_EQ(got.left, 10); EXPECT_EQ(got.top, 10);
                    EXPECT_EQ(got.right, 30); EXPECT_EQ(got.bottom, 20);
                    args(hwnd, 0x10100, 0);
                    client_rect(m);
                    EXPECT_NE(cpu.gpr.rax, 0u);
                    EXPECT_EQ(memory.read_u32(0x10108).value_or(0), 80u);
                    EXPECT_EQ(memory.read_u32(0x1010c).value_or(0), 60u);
                    EXPECT_NE(mgr.find_window_needing_paint(), 0u);
                    args(hwnd, 0, 0);
                    validate(m);
                    EXPECT_NE(cpu.gpr.rax, 0u);
                    EXPECT_FALSE(mgr.needs_paint(hwnd));
                }
            });
        }
        for (auto& thread : threads) thread.join();
    }
}

TEST_F(PaintingWithHwndTest, PaintCallbacksReleaseStateLock) {
    const auto hwnd = create_client(80, 60);
    std::future<bool> work;
    mgr.set_invalidate_callback([&](u64 target) {
        work = std::async(std::launch::async,
            [&, target] { return mgr.validate_rect(target, nullptr); });
        EXPECT_EQ(work.wait_for(std::chrono::milliseconds(200)),
                  std::future_status::ready);
    });
    EXPECT_TRUE(mgr.invalidate_rect(hwnd, nullptr, true));
    EXPECT_TRUE(work.get());
    mgr.set_invalidate_callback({});
    EXPECT_TRUE(mgr.invalidate_rect(hwnd, nullptr, true));
    mgr.set_erase_background_callback([&](u64 target, u64) -> u64 {
        work = std::async(std::launch::async,
            [&, target] { Rect rc{}; (void)mgr.get_update_rect(target, rc, false); return true; });
        EXPECT_EQ(work.wait_for(std::chrono::milliseconds(200)),
                  std::future_status::ready);
        return 1;
    });
    EXPECT_TRUE(mgr.erase_now(hwnd));
    EXPECT_TRUE(work.get());
    EXPECT_TRUE(mgr.invalidate_rect(hwnd, nullptr, true));
    PaintStruct ps{};
    EXPECT_NE(mgr.begin_paint(hwnd, ps), 0u);
    EXPECT_TRUE(work.get());
    mgr.set_erase_background_callback({});
}

// ---- BeginPaint / EndPaint ----

TEST_F(PaintingTest, BeginPaint_ReturnsValidHDC) {
    PaintStruct ps{};
    u64 hdc = mgr.begin_paint(0x100, ps);
    EXPECT_NE(hdc, 0u);
}

// rcPaint is the update region's box: empty when nothing was invalidated,
// the invalidated rectangle otherwise (s3gdi_userpaint_probe "BeginPaint clean
// rc=0,0,0,0", "BeginPaint direct ... rc=10,10,30,20").
TEST_F(PaintingTest, BeginPaint_FillsPaintStructWithTheUpdateBox) {
    PaintStruct ps{};
    mgr.begin_paint(0x100, ps);
    EXPECT_EQ(ps.rcPaint.right, 0);
    EXPECT_EQ(ps.rcPaint.bottom, 0);
    const Rect r{10, 10, 30, 20};
    EXPECT_TRUE(mgr.invalidate_rect(0x100, &r, false));
    mgr.begin_paint(0x100, ps);
    EXPECT_EQ(ps.rcPaint.left, 10);
    EXPECT_EQ(ps.rcPaint.top, 10);
    EXPECT_EQ(ps.rcPaint.right, 30);
    EXPECT_EQ(ps.rcPaint.bottom, 20);
}

TEST_F(PaintingTest, EndPaint_ReturnsTrue) {
    PaintStruct ps{};
    mgr.begin_paint(0x100, ps);
    EXPECT_TRUE(mgr.end_paint(0x100, ps));
}

TEST_F(PaintingTest, BeginPaintSynchronouslyOffersBackgroundEraseToWindow) {
    bool called = false;
    u64 callback_hdc = 0;
    mgr.set_erase_background_callback([&](u64 hwnd, u64 hdc) {
        EXPECT_EQ(hwnd, 0x100u);
        called = true;
        callback_hdc = hdc;
        return 1u;
    });
    ASSERT_TRUE(mgr.invalidate_rect(0x100, nullptr, true));

    PaintStruct ps{};
    const u64 hdc = mgr.begin_paint(0x100, ps);

    EXPECT_TRUE(called);
    EXPECT_EQ(callback_hdc, hdc);
    EXPECT_EQ(ps.fErase, 0);
}

TEST_F(PaintingTest, BeginPaintPreservesEraseFlagWhenWindowDoesNotErase) {
    mgr.set_erase_background_callback([](u64, u64) { return 0u; });
    ASSERT_TRUE(mgr.invalidate_rect(0x100, nullptr, true));

    PaintStruct ps{};
    (void)mgr.begin_paint(0x100, ps);

    EXPECT_EQ(ps.fErase, 1);
}

// ---- InvalidateRect / ValidateRect ----

TEST_F(PaintingTest, InvalidateRect_ReturnsTrue) {
    EXPECT_TRUE(mgr.invalidate_rect(0x100, nullptr, true));
}

TEST_F(PaintingTest, ValidateRect_ReturnsTrue) {
    EXPECT_TRUE(mgr.validate_rect(0x100, nullptr));
}

TEST_F(PaintingTest, GetUpdateRect_ReportsInvalidRegionUntilValidated) {
    Rect update{};
    EXPECT_FALSE(mgr.get_update_rect(0x100, update, false));

    Rect dirty{10, 20, 110, 120};
    ASSERT_TRUE(mgr.invalidate_rect(0x100, &dirty, true));
    ASSERT_TRUE(mgr.get_update_rect(0x100, update, false));
    EXPECT_EQ(update.left, 10);
    EXPECT_EQ(update.top, 20);
    EXPECT_EQ(update.right, 110);
    EXPECT_EQ(update.bottom, 120);

    ASSERT_TRUE(mgr.validate_rect(0x100, nullptr));
    EXPECT_FALSE(mgr.get_update_rect(0x100, update, false));
}

// ---- InvalidateRgn / ValidateRgn ----
//
// The region forms of the invalidation contract. A NULL region means the whole
// client area; a real region contributes its box, in client coordinates. The
// documented return of InvalidateRgn is always nonzero.

class PaintingWithGdiTest : public ::testing::Test {
protected:
    HandleTable handles;
    GdiManager  gdi{handles};
    PaintingManager mgr;

    void SetUp() override { mgr.set_gdi_manager(&gdi); }
};

TEST_F(PaintingWithGdiTest, InvalidateRgn_UsesRegionBoxAsUpdateRegion) {
    const auto hrgn = gdi.create_rect_rgn(10, 20, 110, 120);
    ASSERT_NE(hrgn, 0u);

    EXPECT_TRUE(mgr.invalidate_rgn(0x100, static_cast<u64>(hrgn), true));

    Rect update{};
    ASSERT_TRUE(mgr.get_update_rect(0x100, update, false));
    EXPECT_EQ(update.left, 10);
    EXPECT_EQ(update.top, 20);
    EXPECT_EQ(update.right, 110);
    EXPECT_EQ(update.bottom, 120);
}

TEST_F(PaintingWithGdiTest, InvalidateRgn_NullRegionInvalidatesWholeClientArea) {
    EXPECT_TRUE(mgr.invalidate_rgn(0x100, 0, true));

    Rect update{};
    ASSERT_TRUE(mgr.get_update_rect(0x100, update, false));
    EXPECT_GT(update.right, 0);
    EXPECT_GT(update.bottom, 0);
}

TEST_F(PaintingWithGdiTest, ValidateRgn_ClearsThePendingPaint) {
    const auto hrgn = gdi.create_rect_rgn(0, 0, 50, 50);
    ASSERT_NE(hrgn, 0u);
    ASSERT_TRUE(mgr.invalidate_rgn(0x100, static_cast<u64>(hrgn), false));
    ASSERT_TRUE(mgr.needs_paint(0x100));

    EXPECT_TRUE(mgr.validate_rgn(0x100, static_cast<u64>(hrgn)));

    Rect update{};
    EXPECT_FALSE(mgr.get_update_rect(0x100, update, false));
    EXPECT_FALSE(mgr.needs_paint(0x100));
}

TEST_F(PaintingWithGdiTest, InvalidateRgn_UnknownRegionStillSchedulesAPaint) {
    // A region this process never created must not silently drop the repaint —
    // InvalidateRgn's documented return is always nonzero.
    EXPECT_TRUE(mgr.invalidate_rgn(0x100, 0xDEAD0000u, false));
    EXPECT_TRUE(mgr.needs_paint(0x100));
}

TEST_F(PaintingWithHwndTest,
       InvalidateNullRect_UsesRealClientSizeNotDefault800x600) {
    // ShowWindow's first-show InvalidateRect(NULL) must seed the update region
    // from the live client rect. Defaults of 800x600 previously leaked into
    // GetUpdateRect for Battle.net Login (~354x681).
    const auto hwnd = create_client(354, 681);
    ASSERT_NE(hwnd, windowing::NULL_HWND);

    ASSERT_TRUE(mgr.invalidate_rect(static_cast<u64>(hwnd), nullptr, true));

    Rect update{};
    ASSERT_TRUE(mgr.get_update_rect(static_cast<u64>(hwnd), update, false));
    EXPECT_EQ(update.left, 0);
    EXPECT_EQ(update.top, 0);
    EXPECT_EQ(update.right, 354);
    EXPECT_EQ(update.bottom, 681);
}

TEST_F(PaintingWithHwndTest, GetUpdateRect_ClampsStaleOversizedInvalidRect) {
    const auto hwnd = create_client(200, 100);
    ASSERT_NE(hwnd, windowing::NULL_HWND);

    // Record an oversized dirty region, then shrink the live client — GetUpdateRect
    // must clamp to the current client, not keep the stale extents.
    Rect dirty{0, 0, 800, 600};
    ASSERT_TRUE(mgr.invalidate_rect(static_cast<u64>(hwnd), &dirty, false));

    auto* win = hwnd_mgr.find(hwnd);
    ASSERT_NE(win, nullptr);
    win->client_rect = {0, 0, 200, 100};

    Rect update{};
    ASSERT_TRUE(mgr.get_update_rect(static_cast<u64>(hwnd), update, false));
    EXPECT_EQ(update.right, 200);
    EXPECT_EQ(update.bottom, 100);
}

// A hidden window keeps no update region and reports no QS_PAINT; one
// hidden after invalidation stops reporting it (s3gdi_userpaint_probe
// "hidden: invalidate pending=0"; ShowWindow invalidates on show).
TEST_F(PaintingWithHwndTest, HiddenWindowsHaveNoUpdateRegion) {
    const auto hwnd = create_client(100, 50);
    auto* win = hwnd_mgr.find(hwnd);
    ASSERT_NE(win, nullptr);
    win->visible = false;
    EXPECT_TRUE(mgr.invalidate_rect(static_cast<u64>(hwnd), nullptr, true));
    Rect update{};
    EXPECT_FALSE(mgr.get_update_rect(static_cast<u64>(hwnd), update, false));
    EXPECT_EQ(mgr.find_window_needing_paint(), 0u);
    win->visible = true;
    EXPECT_TRUE(mgr.invalidate_rect(static_cast<u64>(hwnd), nullptr, true));
    EXPECT_EQ(mgr.find_window_needing_paint(), static_cast<u64>(hwnd));
    win->visible = false;
    EXPECT_EQ(mgr.find_window_needing_paint(), 0u);
    EXPECT_FALSE(mgr.needs_paint(static_cast<u64>(hwnd)));
}

// ---- GetClientRect / GetWindowRect ----

TEST_F(PaintingTest, GetClientRect_UnknownHwndFails) {
    // Learn GetClientRect: invalid HWND returns FALSE — never invent 800x600.
    Rect rc{};
    EXPECT_FALSE(mgr.get_client_rect(0x100, rc));
}

TEST_F(PaintingWithHwndTest, GetClientRect_UsesLocalClientSize) {
    const auto hwnd = create_client(348, 646);
    ASSERT_NE(hwnd, windowing::NULL_HWND);

    Rect rc{};
    ASSERT_TRUE(mgr.get_client_rect(static_cast<u64>(hwnd), rc));
    EXPECT_EQ(rc.left, 0);
    EXPECT_EQ(rc.top, 0);
    EXPECT_EQ(rc.right, 348);
    EXPECT_EQ(rc.bottom, 646);
}

TEST_F(PaintingWithHwndTest, GetClientRect_LocalZeroClientReportsZero) {
    // Learn: a window created at 0×0 reports {0,0,0,0} — not a coerced 1×1.
    const auto hwnd = create_client(0, 0);
    ASSERT_NE(hwnd, windowing::NULL_HWND);

    Rect rc{};
    ASSERT_TRUE(mgr.get_client_rect(static_cast<u64>(hwnd), rc));
    EXPECT_EQ(rc.left, 0);
    EXPECT_EQ(rc.top, 0);
    EXPECT_EQ(rc.right, 0);
    EXPECT_EQ(rc.bottom, 0);
}

TEST_F(PaintingTest, GetClientRect_PeerHwndUsesRegistryClientSize) {
    // CEF GPU GetClientRect on the browser Login child must match the
    // published client size, not the historical 800x600 paint default.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-getclientrect-peer-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    constexpr u32 hwnd = 0x001073bu;
    ipc::hwnd_registry_publish(hwnd, 348, 646, 1);

    Rect rc{};
    ASSERT_TRUE(mgr.get_client_rect(hwnd, rc));
    EXPECT_EQ(rc.right, 348);
    EXPECT_EQ(rc.bottom, 646);

    ipc::hwnd_registry_unpublish(hwnd);
    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

TEST_F(PaintingTest, GetClientRect_PeerHwndZeroClientStillSucceeds) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-getclientrect-zero-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    constexpr u32 hwnd = 0x0010b4fu;
    ipc::hwnd_registry_publish(hwnd, 0, 0, 1);

    Rect rc{};
    ASSERT_TRUE(mgr.get_client_rect(hwnd, rc));
    EXPECT_EQ(rc.left, 0);
    EXPECT_EQ(rc.top, 0);
    EXPECT_GE(rc.right, 1);
    EXPECT_GE(rc.bottom, 1);

    ipc::hwnd_registry_unpublish(hwnd);
    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

TEST_F(PaintingTest, GetWindowRect_FailsForUnknownWindow) {
    // Measured (tools/oracle_probe/s1win_api_probe.expected,
    // getwindowrect_invalid): an unknown HWND fails and the output is left
    // alone; the thunk reports ERROR_INVALID_WINDOW_HANDLE.
    Rect rc{7, 7, 7, 7};
    EXPECT_FALSE(mgr.get_window_rect(0x100, rc));
    EXPECT_EQ(rc.left, 7);
    EXPECT_EQ(rc.bottom, 7);
}

// ---- SetRect ----

TEST_F(PaintingTest, SetRect_FillsCorrectly) {
    Rect r{};
    EXPECT_TRUE(PaintingManager::set_rect(r, 10, 20, 110, 220));
    EXPECT_EQ(r.left, 10);
    EXPECT_EQ(r.top, 20);
    EXPECT_EQ(r.right, 110);
    EXPECT_EQ(r.bottom, 220);
}

// ---- InflateRect ----

TEST_F(PaintingTest, InflateRect_ExpandsRect) {
    Rect r{10, 10, 100, 100};
    EXPECT_TRUE(PaintingManager::inflate_rect(r, 5, 5));
    EXPECT_EQ(r.left, 5);
    EXPECT_EQ(r.top, 5);
    EXPECT_EQ(r.right, 105);
    EXPECT_EQ(r.bottom, 105);
}

// ---- OffsetRect ----

TEST_F(PaintingTest, OffsetRect_MovesRect) {
    Rect r{10, 20, 50, 60};
    EXPECT_TRUE(PaintingManager::offset_rect(r, 5, -5));
    EXPECT_EQ(r.left, 15);
    EXPECT_EQ(r.top, 15);
    EXPECT_EQ(r.right, 55);
    EXPECT_EQ(r.bottom, 55);
}

// ---- PtInRect ----

TEST_F(PaintingTest, PtInRect_TrueForInside) {
    Rect r{0, 0, 100, 100};
    Point pt{50, 50};
    EXPECT_TRUE(PaintingManager::pt_in_rect(r, pt));
}

TEST_F(PaintingTest, PtInRect_FalseForOutside) {
    Rect r{0, 0, 100, 100};
    Point pt{150, 150};
    EXPECT_FALSE(PaintingManager::pt_in_rect(r, pt));
}

TEST_F(PaintingTest, PtInRect_FalseForRightEdge) {
    // Win32 PtInRect: right/bottom edges are exclusive
    Rect r{0, 0, 100, 100};
    Point pt{100, 50};
    EXPECT_FALSE(PaintingManager::pt_in_rect(r, pt));
}

// ---- IsRectEmpty ----

TEST_F(PaintingTest, IsRectEmpty_TrueForZeroArea) {
    Rect r{0, 0, 0, 0};
    EXPECT_TRUE(PaintingManager::is_rect_empty(r));
}

TEST_F(PaintingTest, IsRectEmpty_FalseForValidRect) {
    Rect r{0, 0, 100, 100};
    EXPECT_FALSE(PaintingManager::is_rect_empty(r));
}

// ---- IntersectRect ----

TEST_F(PaintingTest, IntersectRect_OverlappingRects) {
    Rect a{0, 0, 100, 100};
    Rect b{50, 50, 150, 150};
    Rect dst{};
    EXPECT_TRUE(PaintingManager::intersect_rect(dst, a, b));
    EXPECT_EQ(dst.left, 50);
    EXPECT_EQ(dst.top, 50);
    EXPECT_EQ(dst.right, 100);
    EXPECT_EQ(dst.bottom, 100);
}

TEST_F(PaintingTest, IntersectRect_NonOverlappingReturnsFalse) {
    Rect a{0, 0, 50, 50};
    Rect b{100, 100, 200, 200};
    Rect dst{};
    EXPECT_FALSE(PaintingManager::intersect_rect(dst, a, b));
}

// ---- Stubs ----

// A DC nobody created fails ("FillRect(NULL dc)=0" in s3gdi_userpaint_probe).
TEST_F(PaintingTest, FillRect_UnknownDcFails) {
    Rect rc{0, 0, 100, 100};
    EXPECT_EQ(mgr.fill_rect(0x1000, rc, 0x1), 0);
}

// DrawText on a DC nobody created draws nothing and returns 0, as a NULL DC
// does on Windows 11 (s3gdi_drawtext_probe "NULL dc ret=0"); the old fixed 16
// described text that was never drawn.
TEST_F(PaintingTest, DrawText_UnknownDcReturnsZero) {
    Rect rc{0, 0, 200, 200};
    EXPECT_EQ(mgr.draw_text(0x1000, "Hello", rc, 0), 0);
}

// ---- PtInRect export: POINT by value ----
//
// PtInRect(const RECT*, POINT). The 8-byte POINT travels by value: Win64 packs
// it into RDX (x low dword, y high dword); PE32 stdcall spreads it over two
// stack DWORDs (12 bytes popped in all). RECT right/bottom are exclusive.
TEST(PtInRectExportTest, DecodesPointByValueInBothBitnesses) {
    PaintingManager mgr;
    DllExports exports = create_painting_thunks(mgr);
    ExportEntry pt_in_rect{};
    for (const auto& e : exports.entries) {
        if (e.name == "PtInRect") pt_in_rect = e;
    }
    ASSERT_TRUE(static_cast<bool>(pt_in_rect.handler));
    EXPECT_EQ(pt_in_rect.stdcall_bytes, 12u);

    GuestMemory memory;
    constexpr GuestAddr kRect = 0x2000;
    constexpr GuestAddr kStack = 0x8000;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kRect + 0, 10).is_ok());
    ASSERT_TRUE(memory.write_u32(kRect + 4, 20).is_ok());
    ASSERT_TRUE(memory.write_u32(kRect + 8, 110).is_ok());
    ASSERT_TRUE(memory.write_u32(kRect + 12, 220).is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    Marshaler m64(memory);
    m64.attach_cpu(cpu);
    const auto call64 = [&](i32 x, i32 y) {
        cpu.gpr.rcx = kRect;
        cpu.gpr.rdx = static_cast<u64>(static_cast<u32>(x)) |
                      (static_cast<u64>(static_cast<u32>(y)) << 32);
        cpu.gpr.r8 = 0xFFFFFFFFFFFFFFFFull; // must not be read as y
        pt_in_rect.handler(m64);
        return cpu.gpr.rax;
    };
    EXPECT_EQ(call64(50, 100), 1u);
    EXPECT_EQ(call64(10, 20), 1u);
    EXPECT_EQ(call64(110, 100), 0u);  // right edge is outside
    EXPECT_EQ(call64(50, 220), 0u);   // bottom edge is outside
    EXPECT_EQ(call64(50, 5), 0u);     // y above the rect
    EXPECT_EQ(call64(-1, 100), 0u);

    cpu.mode = cpu::CpuMode::Protected;
    cpu.gpr.rsp = kStack;
    Marshaler m32(memory);
    m32.attach_cpu(cpu);
    const auto call32 = [&](i32 x, i32 y) {
        (void)memory.write_u32(kStack + 4, static_cast<u32>(kRect));
        (void)memory.write_u32(kStack + 8, static_cast<u32>(x));
        (void)memory.write_u32(kStack + 12, static_cast<u32>(y));
        pt_in_rect.handler(m32);
        return cpu.gpr.rax;
    };
    EXPECT_EQ(call32(50, 100), 1u);
    EXPECT_EQ(call32(110, 100), 0u);
    EXPECT_EQ(call32(50, 5), 0u);
}

// Learn (WNDCLASSEXW hbrBackground, FillRect): an HBRUSH of 1..31 is a system
// colour, COLOR_xxx + 1. No GDI handle takes those values, so the stock objects
// (created first, on a fresh handle table) cannot shadow one: with DKGRAY_BRUSH
// at handle 16, a COLOR_BTNFACE + 1 class background painted 404040.
TEST_F(PaintingWithHwndTest, SystemColourBrushValuesAreNotGdiHandles) {
    HandleTable handles;
    GdiManager gdi(handles);
    gdi.set_hwnd_manager(&hwnd_mgr);
    mgr.set_gdi_manager(&gdi);
    for (i32 id = 0; id <= 19; ++id) {  // WHITE_BRUSH .. DC_PEN
        const Handle stock = gdi.get_stock_object(id);
        if (stock != 0) EXPECT_GE(stock, kFirstGdiHandle) << "stock object " << id;
    }
    EXPECT_GE(gdi.create_solid_brush(0x0000FF), kFirstGdiHandle);

    const auto hwnd = create_client(8, 4);
    const Handle hdc = gdi.get_dc(hwnd);
    ASSERT_NE(hdc, 0u);
    EXPECT_GE(hdc, kFirstGdiHandle);
    // sys_color_probe.expected: COLOR_BTNFACE (15) is F0F0F0.
    EXPECT_EQ(mgr.brush_color(15 + 1), std::optional<u32>{0xF0F0F0u});
    EXPECT_EQ(mgr.fill_rect(hdc, Rect{0, 0, 8, 4}, 15 + 1), 1);
    EXPECT_EQ(gdi.get_pixel(hdc, 1, 1), 0xF0F0F0u);
    // A real brush still paints itself (s3gdi_bitmap_probe: DKGRAY_BRUSH 404040).
    EXPECT_EQ(mgr.fill_rect(hdc, Rect{0, 0, 8, 4}, gdi.get_stock_object(3)), 1);
    EXPECT_EQ(gdi.get_pixel(hdc, 1, 1), 0x404040u);
}

// ---- Child window surfaces ----
//
// A WS_CHILD window has no pixels of its own on Windows: its DC draws on the
// parent's surface, so what it never paints still shows the parent beneath
// (an owner-draw button whose WM_DRAWITEM draws nothing is invisible). The
// child's surface therefore starts transparent; what it paints is opaque. A
// top-level window's surface starts opaque.

class WindowSurfaceTest : public ::testing::Test {
protected:
    HandleTable handles;
    GdiManager gdi{handles};
    HwndManager hwnd_mgr;

    void SetUp() override { gdi.set_hwnd_manager(&hwnd_mgr); }

    windowing::HWND create(u32 style, windowing::HWND parent) {
        WindowData data{};
        data.parent = parent;
        data.style = style;
        data.rect = {0, 0, 40, 30};
        data.client_rect = {0, 0, 40, 30};
        data.visible = true;
        return hwnd_mgr.create(data);
    }

    u8 alpha_at(Handle hdc, i32 x, i32 y) {
        u8 a = 0x7F;
        EXPECT_TRUE(gdi.with_device_surface(hdc, [&](GdiManager::DeviceSurface& s) {
            a = s.rgba[(static_cast<usize>(y) * static_cast<usize>(s.width) + static_cast<usize>(x)) * 4 + 3];
        }));
        return a;
    }
};

TEST_F(WindowSurfaceTest, ChildWindowUnpaintedPixelsAreTransparent) {
    constexpr u32 WS_POPUP = 0x80000000u, WS_CHILD = 0x40000000u;
    const auto top = create(WS_POPUP, windowing::HWND_DESKTOP);
    const auto child = create(WS_CHILD, top);

    const Handle top_dc = gdi.get_dc(static_cast<Handle>(top));
    const Handle child_dc = gdi.get_dc(static_cast<Handle>(child));
    ASSERT_NE(top_dc, 0u);
    ASSERT_NE(child_dc, 0u);

    EXPECT_EQ(alpha_at(top_dc, 5, 5), 0xFF);
    EXPECT_EQ(alpha_at(child_dc, 5, 5), 0x00);

    ASSERT_TRUE(gdi.fill_rect_gdi(child_dc, 0, 0, 10, 10, 0x00FF0000u));
    EXPECT_EQ(alpha_at(child_dc, 5, 5), 0xFF);
    EXPECT_EQ(alpha_at(child_dc, 20, 20), 0x00);
}
