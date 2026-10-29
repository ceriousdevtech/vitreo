/// @file test_user32_ext_painting_controls.cpp
/// @brief Painting, colours, regions, menus and scroll bars: DrawState, system colours,
///        GetMenu/RemoveMenu, scroll info, window regions.

#include "user32_ext_thunk_harness.hpp"
#include <winapi/ui/comctl32/comctl32.hpp>
#include <winapi/user32/windows/sdk_creation_layouts.hpp>

TEST_F(User32ExtThunkTest, AnsiDrawingAndMenuExportsAreRegistered) {
    struct Expected {
        const char* name;
        u32 stdcall_bytes;
    };
    for (const Expected expected : {
             Expected{"ModifyMenuA", 20u},
             Expected{"GrayStringA", 36u},
             Expected{"DrawTextExA", 24u},
             Expected{"TabbedTextOutA", 32u},
             Expected{"CharNextExA", 12u},
         }) {
        const auto entry = find_export(expected.name);
        EXPECT_TRUE(static_cast<bool>(entry.handler)) << expected.name;
        EXPECT_EQ(entry.stdcall_bytes, expected.stdcall_bytes) << expected.name;
    }
}

// =============================================================================
// GetMenu Tests
// =============================================================================

TEST_F(User32ExtThunkTest, GetMenu_ReturnsZeroForUnknownWindow) {
    auto entry = find_export("GetMenu");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0x999; // unknown HWND
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

TEST_F(User32ExtThunkTest, CheckMenuRadioItemHasStdcallFiveArgs) {
    // Learn: BOOL CheckMenuRadioItem(hmenu, first, last, check, flags)
    auto entry = find_export("CheckMenuRadioItem");
    EXPECT_EQ(entry.stdcall_bytes, 20u);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 0xDEAD; // unknown HMENU
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8 = 3;
    cpu_.gpr.r9 = 2;
    // 5th arg (flags) is on stack in x64 after home space — set_return path
    // still consumes via m.arg(); zero flags is fine for the miss path.
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

TEST_F(User32ExtThunkTest, GetSubMenuExportConsumesTwoArgs) {
    auto entry = find_export("GetSubMenu");
    EXPECT_EQ(entry.stdcall_bytes, 8u);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 0xDEAD;
    cpu_.gpr.rdx = 0;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

TEST_F(User32ExtThunkTest, RemoveMenuDetachesAndPreservesSubmenuIdentity) {
    Marshaler m(memory_); m.attach_cpu(cpu_);
    GuestAddr cursor = BUF_ADDR;
    auto word = [&](u16 value) { EXPECT_TRUE(memory_.write_u16(cursor, value).is_ok()); cursor += 2; };
    auto label = [&](const char* text) { while (*text) word(static_cast<u8>(*text++)); word(0); };
    word(0); word(0);
    word(0x10); label("First"); word(0x80); word(101); label("One");
    word(0x90); label("Second"); word(0x80); word(102); label("Two");
    // A minimal PE resource directory: RT_MENU / ID 1 / language 1033.
    ASSERT_TRUE(memory_.write_u16(0x400e, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x4010, 4).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x4014, 0x80000018).is_ok());
    ASSERT_TRUE(memory_.write_u16(0x4026, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x4028, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x402c, 0x80000030).is_ok());
    ASSERT_TRUE(memory_.write_u16(0x403e, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x4040, 1033).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x4044, 0x48).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x4048, BUF_ADDR - MEM_BASE).is_ok());
    ASSERT_TRUE(memory_.write_u32(0x404c, cursor - BUF_ADDR).is_ok());
    mgr_->set_resource_info(MEM_BASE, 0x4000 - MEM_BASE, 0x1000);
    cpu_.gpr.rcx = MEM_BASE; cpu_.gpr.rdx = 1; find_export("LoadMenuW").handler(m);
    const u64 menu = cpu_.gpr.rax;
    ASSERT_NE(menu, 0u);
    cpu_.gpr.rcx = menu; cpu_.gpr.rdx = 0; find_export("GetSubMenu").handler(m);
    const u64 first = cpu_.gpr.rax;
    cpu_.gpr.rcx = menu; cpu_.gpr.rdx = 1; find_export("GetSubMenu").handler(m);
    const u64 second = cpu_.gpr.rax;
    ASSERT_NE(first, 0u); ASSERT_NE(second, 0u);
    cpu_.gpr.rcx = menu; cpu_.gpr.rdx = 0; cpu_.gpr.r8 = 0x400; find_export("RemoveMenu").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = menu; find_export("GetMenuItemCount").handler(m); EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = menu; cpu_.gpr.rdx = 0; find_export("GetSubMenu").handler(m); EXPECT_EQ(cpu_.gpr.rax, second);
    cpu_.gpr.rcx = first; find_export("GetMenuItemCount").handler(m); EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = second; cpu_.gpr.rdx = 102; cpu_.gpr.r8 = 0; find_export("RemoveMenu").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    cpu_.gpr.rcx = second; find_export("GetMenuItemCount").handler(m); EXPECT_EQ(cpu_.gpr.rax, 0u);
}

TEST_F(User32ExtThunkTest, ScrollInfoClampsRangeAndKeepsAxesIndependent) {
    WindowData window; window.style = 0x00300000;
    const auto hwnd = hwnd_mgr_.create(window);
    Marshaler m(memory_); m.attach_cpu(cpu_);
    (void)memory_.write_u32(BUF_ADDR, 28);
    (void)memory_.write_u32(BUF_ADDR + 4, 7);
    (void)memory_.write_u32(BUF_ADDR + 8, 10);
    (void)memory_.write_u32(BUF_ADDR + 12, 109);
    (void)memory_.write_u32(BUF_ADDR + 16, 30);
    (void)memory_.write_u32(BUF_ADDR + 20, 1000);
    cpu_.gpr.rcx = hwnd; cpu_.gpr.rdx = 1; cpu_.gpr.r8 = BUF_ADDR; cpu_.gpr.r9 = 1;
    find_export("SetScrollInfo").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 80u);
    cpu_.gpr.r8 = 5;
    find_export("SetScrollPos").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 80u);
    find_export("GetScrollPos").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 10u);
    cpu_.gpr.rdx = 0;
    find_export("GetScrollPos").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    cpu_.gpr.rdx = 1; cpu_.gpr.r8 = BUF_ADDR;
    (void)memory_.write_u32(BUF_ADDR + 4, 0x17);
    find_export("GetScrollInfo").handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 16).value(), 30u);
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 20).value(), 10u);
    (void)memory_.write_u32(BUF_ADDR + 4, 0xa); // page + disable-no-scroll
    (void)memory_.write_u32(BUF_ADDR + 16, 500);
    find_export("SetScrollInfo").handler(m);
    const auto& state = hwnd_mgr_.find(hwnd)->scroll_bars[1];
    EXPECT_EQ(state.page, 100u); EXPECT_EQ(state.position, 10);
    EXPECT_TRUE(state.visible); EXPECT_FALSE(state.enabled);
}

// =============================================================================
// DrawStateW / DrawStateA against tools/oracle_probe/draw_state_probe.expected
// (Windows, x86 == x64). Unreal's splash screen draws its image with
// DrawStateW(DST_BITMAP); the stub returned TRUE and drew nothing, leaving a
// white window where the logo belongs.
// =============================================================================

namespace {

constexpr i32 kDsW = 20;
constexpr i32 kDsH = 10;
constexpr u32 kDsBackground = 0x112233;  // as 0xRRGGBB

/// One probe case: its return, last error, and the 20x10 grid as 0xRRGGBB.
struct DrawStateRow {
    u32 ret = 0;
    u32 err = 0;
    std::vector<u32> grid;
};

std::map<std::string, DrawStateRow> load_draw_state_expected() {
    std::map<std::string, DrawStateRow> rows;
    std::ifstream in(VITREO_ORACLE_PROBE_DIR "/draw_state_probe.expected");
    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        unsigned ret = 0;
        unsigned long err = 0;
        char label[64] = {};
        if (std::sscanf(line.c_str(), "%63s ret=%u err=%lu", label, &ret, &err) == 3) {
            current = label;
            rows[current] = DrawStateRow{ret, static_cast<u32>(err), {}};
            continue;
        }
        if (current.empty() || line.size() < 2 || line[0] != ' ' || line[1] != ' ') continue;
        auto& grid = rows[current].grid;
        if (grid.size() >= static_cast<usize>(kDsW * kDsH)) continue;
        const std::string cells = line.substr(2);
        if (cells.size() < static_cast<usize>(kDsW * 6)) continue;
        for (i32 i = 0; i < kDsW; ++i) {
            const std::string cell = cells.substr(static_cast<usize>(i) * 6, 6);
            grid.push_back(cell.find('.') != std::string::npos
                               ? kDsBackground
                               : static_cast<u32>(std::stoul(cell, nullptr, 16)));
        }
    }
    return rows;
}

}  // namespace

class DrawStateTest : public User32ExtThunkTest {
protected:
    void SetUp() override {
        User32ExtThunkTest::SetUp();
        gdi_ = std::make_unique<GdiManager>(handles_);
        mgr_->set_gdi_manager(gdi_.get());
        expected_ = load_draw_state_expected();
        // The probe's 6x4 source: B = 0x10 + x*0x20, G = 0x40 + y*0x30, R = 0x90.
        std::vector<u8> src(6 * 4 * 4);
        for (i32 yy = 0; yy < 4; ++yy) {
            for (i32 xx = 0; xx < 6; ++xx) {
                u8* p = src.data() + (yy * 6 + xx) * 4;
                p[0] = 0x90;
                p[1] = static_cast<u8>(0x40 + yy * 0x30);
                p[2] = static_cast<u8>(0x10 + xx * 0x20);
                p[3] = 0xFF;
            }
        }
        source_ = gdi_->create_bitmap_from_rgba(6, 4, src);
        ASSERT_NE(source_, 0u);
        dc_ = gdi_->create_compatible_dc(0);
    }

    /// Fresh 20x10 destination filled with the probe's background.
    void reset_destination() {
        std::vector<u8> px(kDsW * kDsH * 4);
        for (i32 i = 0; i < kDsW * kDsH; ++i) {
            px[i * 4 + 0] = 0x11;
            px[i * 4 + 1] = 0x22;
            px[i * 4 + 2] = 0x33;
            px[i * 4 + 3] = 0xFF;
        }
        if (dest_ != 0) {
            (void)gdi_->select_object(dc_, old_);
            (void)gdi_->delete_object(dest_);
        }
        dest_ = gdi_->create_bitmap_from_rgba(kDsW, kDsH, px);
        old_ = gdi_->select_object(dc_, dest_);
    }

    std::vector<u32> destination_grid() {
        GdiManager::BitmapRgbaCopy copy;
        EXPECT_TRUE(gdi_->copy_bitmap_rgba(dest_, copy));
        std::vector<u32> grid;
        for (i32 i = 0; i < kDsW * kDsH && static_cast<usize>(i * 4 + 2) < copy.rgba.size(); ++i) {
            const u8* p = copy.rgba.data() + i * 4;
            grid.push_back((static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2]);
        }
        return grid;
    }

    /// DrawStateW/A with 32-bit stdcall arguments; returns {ret, last error}.
    std::pair<u32, u32> draw_state(bool wide, u64 hdc, u64 brush, u64 ldata, i32 x, i32 y,
                                   i32 cx, i32 cy, u32 flags) {
        cpu_.init_protected_mode();
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        const u32 args[10] = {static_cast<u32>(hdc), static_cast<u32>(brush), 0,
                              static_cast<u32>(ldata), 0, static_cast<u32>(x),
                              static_cast<u32>(y), static_cast<u32>(cx),
                              static_cast<u32>(cy), flags};
        (void)memory_.write_u32(cpu_.gpr.rsp, 0xDEADBEEF);
        for (usize i = 0; i < 10; ++i) {
            (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4, args[i]);
        }
        Marshaler marshaler(memory_);
        marshaler.attach_cpu(cpu_);
        set_last_error(57005);
        find_export(wide ? "DrawStateW" : "DrawStateA").handler(marshaler);
        return {static_cast<u32>(cpu_.gpr.rax), get_last_error()};
    }

    void expect_case(const char* label, bool wide, u64 brush, i32 x, i32 y, i32 cx, i32 cy,
                     u32 flags) {
        ASSERT_TRUE(expected_.count(label)) << label;
        const DrawStateRow& want = expected_.at(label);
        ASSERT_EQ(want.grid.size(), static_cast<usize>(kDsW * kDsH)) << label;
        reset_destination();
        const auto [ret, err] = draw_state(wide, dc_, brush, source_, x, y, cx, cy, flags);
        EXPECT_EQ(ret, want.ret) << label;
        EXPECT_EQ(err, want.err) << label;
        const auto got = destination_grid();
        ASSERT_EQ(got.size(), want.grid.size()) << label;
        for (i32 i = 0; i < kDsW * kDsH; ++i) {
            EXPECT_EQ(got[i], want.grid[i]) << label << " pixel (" << i % kDsW << ","
                                            << i / kDsW << ")";
        }
    }

    HandleTable handles_;
    std::unique_ptr<GdiManager> gdi_;
    std::map<std::string, DrawStateRow> expected_;
    Handle source_{0};
    Handle dc_{0};
    Handle dest_{0};
    Handle old_{0};
};

TEST_F(DrawStateTest, BitmapIsAnUnscaledCopyClippedToTheBitmap) {
    constexpr u32 DST_BITMAP = 0x4;
    expect_case("bitmap32_cx0", true, 0, 2, 3, 0, 0, DST_BITMAP);
    expect_case("bitmap32_cx4cy2", true, 0, 2, 3, 4, 2, DST_BITMAP);
    expect_case("bitmap32_cx9cy6", true, 0, 2, 3, 9, 6, DST_BITMAP);
    expect_case("bitmap32_cx0cy2", true, 0, 2, 3, 0, 2, DST_BITMAP);
    expect_case("bitmap32_neg_origin", true, 0, -2, -1, 0, 0, DST_BITMAP);
    expect_case("bitmap32_right", true, 0, 2, 3, 9, 0, DST_BITMAP | 0x8000);
    expect_case("A_bitmap32_cx0", false, 0, 2, 3, 0, 0, DST_BITMAP);
}

TEST_F(DrawStateTest, TransformsPaintThroughTheWholeColourImage) {
    constexpr u32 DST_BITMAP = 0x4;
    const Handle red = gdi_->create_solid_brush(0x0000FF);  // COLORREF red
    expect_case("bitmap32_mono_red", true, red, 2, 3, 0, 0, DST_BITMAP | 0x80);
    expect_case("bitmap32_mono_nullbrush", true, 0, 2, 3, 0, 0, DST_BITMAP | 0x80);
    expect_case("bitmap32_union", true, 0, 2, 3, 0, 0, DST_BITMAP | 0x10);
    expect_case("bitmap32_union_even", true, 0, 2, 2, 0, 0, DST_BITMAP | 0x10);
    expect_case("bitmap32_union_red", true, red, 2, 3, 0, 0, DST_BITMAP | 0x10);
    expect_case("bitmap32_disabled", true, 0, 2, 3, 0, 0, DST_BITMAP | 0x20);
}

TEST_F(DrawStateTest, FailuresMatchWindows) {
    constexpr u32 DST_BITMAP = 0x4;
    reset_destination();
    auto r = draw_state(true, 0, 0, source_, 2, 3, 0, 0, DST_BITMAP);  // null_hdc
    EXPECT_EQ(r.first, expected_.at("null_hdc").ret);
    EXPECT_EQ(r.second, expected_.at("null_hdc").err);
    r = draw_state(true, dc_, 0, 0, 2, 3, 0, 0, DST_BITMAP);  // null_bitmap
    EXPECT_EQ(r.first, expected_.at("null_bitmap").ret);
    EXPECT_EQ(r.second, expected_.at("null_bitmap").err);
    r = draw_state(true, dc_, 0, 0x12345, 2, 3, 0, 0, DST_BITMAP);  // bogus_bitmap
    EXPECT_EQ(r.first, expected_.at("bogus_bitmap").ret);
    EXPECT_EQ(r.second, expected_.at("bogus_bitmap").err);
    for (u32 type : {5u, 15u}) {  // bad_type5 / bad_type15
        r = draw_state(true, dc_, 0, source_, 2, 3, 0, 0, type);
        EXPECT_EQ(r.first, 0u) << type;
        EXPECT_EQ(r.second, 57005u) << type;
    }
    EXPECT_EQ(destination_grid(), std::vector<u32>(kDsW * kDsH, kDsBackground));
}

TEST_F(User32ExtThunkTest, SysColorsAreTheMeasuredWindows11Defaults) {
    std::ifstream in(VITREO_ORACLE_PROBE_DIR "/sys_color_probe.expected");
    ASSERT_TRUE(in.good());
    std::string line;
    int rows = 0;
    while (std::getline(in, line)) {
        int index = 0;
        unsigned color = 0;
        if (std::sscanf(line.c_str(), "%d %x", &index, &color) != 2) continue;
        ++rows;
        EXPECT_EQ(mgr_->sys_color(static_cast<u32>(index)), color) << "index " << index;
    }
    EXPECT_EQ(rows, 33);
}

TEST_F(User32ExtThunkTest, SysColorAndBrushRejectUndefinedIndices) {
    // sys_color_brush_probe: GetSysColor(31+) = 0 with ERROR_INVALID_PARAMETER;
    // GetSysColorBrush(31+) = NULL with the last error untouched.
    cpu_.mode = cpu::CpuMode::Long;
    Marshaler marshaler(memory_);
    marshaler.attach_cpu(cpu_);
    for (u64 index : {31ull, 32ull, 100ull}) {
        cpu_.gpr.rcx = index;
        set_last_error(57005);
        find_export("GetSysColor").handler(marshaler);
        EXPECT_EQ(cpu_.gpr.rax, 0u) << index;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER) << index;
        set_last_error(57005);
        find_export("GetSysColorBrush").handler(marshaler);
        EXPECT_EQ(cpu_.gpr.rax, 0u) << index;
        EXPECT_EQ(get_last_error(), 57005u) << index;
    }
    cpu_.gpr.rcx = 15;  // COLOR_BTNFACE
    find_export("GetSysColor").handler(marshaler);
    EXPECT_EQ(cpu_.gpr.rax, 0xF0F0F0u);
    find_export("GetSysColorBrush").handler(marshaler);
    EXPECT_NE(cpu_.gpr.rax, 0u);
    // A system-colour brush resolves to its colour wherever a brush is taken.
    EXPECT_EQ(sys_colors::from_brush(cpu_.gpr.rax), std::optional<u32>{0xF0F0F0u});
    EXPECT_EQ(sys_colors::from_brush(16), std::optional<u32>{0xF0F0F0u});  // COLOR_BTNFACE + 1
    EXPECT_EQ(sys_colors::from_brush(0), std::nullopt);
}

TEST_F(User32ExtThunkTest, NewWindowFramebufferIsUnpaintedBlack) {
    HandleTable handles;
    GdiManager gdi(handles);
    gdi.set_hwnd_manager(&hwnd_mgr_);
    WindowData window{};
    window.parent = HWND_DESKTOP;
    window.rect = {0, 0, 16, 8};
    window.client_rect = window.rect;
    const HWND hwnd = hwnd_mgr_.create(window);
    const Handle hdc = gdi.get_dc(hwnd);
    ASSERT_NE(hdc, 0u);
    EXPECT_EQ(gdi.get_pixel(hdc, 1, 1), 0x000000u);
}

TEST_F(User32ExtThunkTest, WindowRegionSnapshotsSurviveConcurrentQueryAndReplaceBothBitnesses) {
    // Learn SetWindowRgn/GetWindowRgn: ownership transfer, copied region and
    // SIMPLEREGION for one rectangle. Table mutation must not invalidate the
    // copy while another guest thread queries it.
    HandleTable handles;
    GdiManager gdi(handles);
    mgr_->set_gdi_manager(&gdi);
    const auto set_region = find_export("SetWindowRgn");
    const auto get_region = find_export("GetWindowRgn");
    std::array<HWND, 1024> windows{};
    for (auto& hwnd : windows) {
        WindowData data{};
        hwnd = hwnd_mgr_.create(data);
        ASSERT_NE(hwnd, NULL_HWND);
    }
    for (bool is64 : {false, true}) {
        mgr_->set_is_64bit(is64);
        std::atomic<bool> start{false}, stop{false};
        std::atomic<unsigned> ready{0}, queries{0}, failures{0};
        std::array<Handle, 8> outputs{};
        for (auto& h : outputs) h = gdi.create_rect_rgn(0, 0, 0, 0);
        std::vector<std::thread> readers;
        for (unsigned worker = 0; worker < outputs.size(); ++worker) {
            readers.emplace_back([&, worker] {
                cpu::CpuState cpu;
                if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
                cpu.set_sp(STACK_BASE + 0x2000 + worker * 0x1000);
                Marshaler m(memory_);
                m.attach_cpu(cpu);
                ready.fetch_add(1);
                while (!start.load()) std::this_thread::yield();
                unsigned iteration = worker;
                while (!stop.load()) {
                    const HWND hwnd = windows[iteration % windows.size()];
                    const i32 left = static_cast<i32>(iteration++ % windows.size());
                    if (is64) { cpu.gpr.rcx = hwnd; cpu.gpr.rdx = outputs[worker]; }
                    else {
                        (void)memory_.write_u32(cpu.sp()+4, hwnd);
                        (void)memory_.write_u32(cpu.sp()+8, outputs[worker]);
                    }
                    get_region.handler(m);
                    if (cpu.gpr.rax != 0) {
                        ClipRect copy{};
                        if (cpu.gpr.rax != 2 || !gdi.get_region_rect(outputs[worker], copy) ||
                            copy.left != left || copy.top != 0 || copy.right != left + 10 || copy.bottom != 20)
                            failures.fetch_add(1);
                    }
                    queries.fetch_add(1);
                }
            });
        }
        while (ready.load() != outputs.size()) std::this_thread::yield();
        start.store(true);
        cpu_ = cpu::CpuState{};
        if (is64) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + 0x800);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        for (unsigned i = 0; i < 8192; ++i) {
            const HWND hwnd = windows[i % windows.size()];
            const i32 left = static_cast<i32>(i % windows.size());
            const Handle input = i % 3 == 0 ? 0 : gdi.create_rect_rgn(left, 0, left + 10, 20);
            if (is64) { cpu_.gpr.rcx = hwnd; cpu_.gpr.rdx = input; cpu_.gpr.r8 = 0; }
            else {
                (void)memory_.write_u32(cpu_.sp()+4, hwnd);
                (void)memory_.write_u32(cpu_.sp()+8, input);
                (void)memory_.write_u32(cpu_.sp()+12, 0);
            }
            set_region.handler(m);
            if (cpu_.gpr.rax == 0) failures.fetch_add(1);
        }
        stop.store(true);
        for (auto& reader : readers) reader.join();
        EXPECT_GT(queries.load(), 0u);
        EXPECT_EQ(failures.load(), 0u);
        for (auto hwnd : windows) mgr_->window_destroyed(hwnd);
        if (is64) { cpu_.gpr.rcx = windows[1]; cpu_.gpr.rdx = outputs[0]; }
        else {
            (void)memory_.write_u32(cpu_.sp()+4, windows[1]);
            (void)memory_.write_u32(cpu_.sp()+8, outputs[0]);
        }
        get_region.handler(m);
        EXPECT_EQ(cpu_.gpr.rax, 0u);
        for (auto h : outputs) EXPECT_TRUE(gdi.delete_object(h));
    }
}

// FlatSB_SetScrollInfo must call the standard implementation for an
// uninitialized window. The position bound is from SetScrollInfo's Learn
// contract: nMax - max(nPage - 1, 0), hence 109 - 29 = 80 here.
TEST_F(User32ExtThunkTest, FlatScrollInfoUpdatesStandardWindowState) {
    ComCtl32Manager comctl;
    auto exports = create_comctl32_thunks(comctl, {},
        [&](Marshaler& m) { mgr_->impl_SetScrollInfo(m); },
        FlatScrollHandlers{[&](Marshaler& m) { mgr_->impl_GetScrollPos(m); }, [&](Marshaler& m) { mgr_->impl_SetScrollPos(m); }, [&](Marshaler& m) { mgr_->impl_GetScrollInfo(m); }, [&](Marshaler& m) { mgr_->impl_GetScrollRange(m); }, [&](Marshaler& m) { mgr_->impl_SetScrollRange(m); }, [&](Marshaler& m) { mgr_->impl_EnableScrollBar(m); }, [&](Marshaler& m) { mgr_->impl_ShowScrollBar(m); }});
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "FlatSB_SetScrollInfo"; });
    ASSERT_NE(entry, exports.entries.end());
    for (auto mode : {cpu::CpuMode::Protected, cpu::CpuMode::Long}) {
        cpu_.mode = mode;
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        for (u32 bar : {0u, 1u}) {
            WindowData window;
            const auto hwnd = hwnd_mgr_.create(window);
            using sdk_creation::SCROLLINFO;
            const SCROLLINFO info{sizeof(SCROLLINFO),
                sdk_creation::SIF_RANGE | sdk_creation::SIF_PAGE | sdk_creation::SIF_POS,
                10, 109, 30, 1000, 0};
            ASSERT_TRUE(memory_.write_bytes(BUF_ADDR,
                reinterpret_cast<const Byte*>(&info), sizeof(info)).is_ok());
            cpu_.gpr.rcx = hwnd; cpu_.gpr.rdx = bar;
            cpu_.gpr.r8 = BUF_ADDR; cpu_.gpr.r9 = 0;
            (void)memory_.write_u32(cpu_.gpr.rsp + 4, hwnd);
            (void)memory_.write_u32(cpu_.gpr.rsp + 8, bar);
            (void)memory_.write_u32(cpu_.gpr.rsp + 12, BUF_ADDR);
            (void)memory_.write_u32(cpu_.gpr.rsp + 16, 0);
            entry->handler(m);
            EXPECT_EQ(cpu_.gpr.rax, 80u);
            const auto& scroll = hwnd_mgr_.find(hwnd)->scroll_bars[bar];
            EXPECT_EQ(scroll.minimum, 10);
            EXPECT_EQ(scroll.maximum, 109);
            EXPECT_EQ(scroll.page, 30u);
            EXPECT_EQ(scroll.position, 80);
            EXPECT_TRUE(scroll.visible);
            EXPECT_TRUE(scroll.enabled);
            EXPECT_FALSE(hwnd_mgr_.find(hwnd)->scroll_bars[1 - bar].initialized);
            // The structure remains an input, including the unclamped nPos.
            EXPECT_EQ(memory_.read_u32(BUF_ADDR + offsetof(SCROLLINFO, nPos)).value(), 1000u);
            auto call = [&](const char* name, std::initializer_list<u64> args) {
                u32 i = 0;
                for (auto value : args) {
                    if (i == 0) cpu_.gpr.rcx = value;
                    if (i == 1) cpu_.gpr.rdx = value;
                    if (i == 2) cpu_.gpr.r8 = value;
                    if (i == 3) cpu_.gpr.r9 = value;
                    (void)memory_.write_u32(cpu_.gpr.rsp + 4 + 4 * i, value);
                    if (i >= 4) (void)memory_.write_u64(cpu_.gpr.rsp + 40 + 8 * (i - 4), value);
                    ++i;
                }
                auto found = std::find_if(exports.entries.begin(), exports.entries.end(),
                    [&](const ExportEntry& e) { return e.name == name; });
                EXPECT_NE(found, exports.entries.end());
                if (found != exports.entries.end()) found->handler(m);
                return cpu_.gpr.rax;
            };
            EXPECT_EQ(call("FlatSB_GetScrollPos", {hwnd, bar}), 80u);
            EXPECT_EQ(call("FlatSB_SetScrollPos", {hwnd, bar, 50, 0}), 80u);
            EXPECT_EQ(call("FlatSB_GetScrollPos", {hwnd, bar}), 50u);
            EXPECT_EQ(call("FlatSB_SetScrollRange", {hwnd, bar, 20, 200, 0}), 1u);
            EXPECT_EQ(call("FlatSB_GetScrollRange", {hwnd, bar, BUF_ADDR + 128, BUF_ADDR + 132}), 1u);
            EXPECT_EQ(memory_.read_u32(BUF_ADDR + 128).value(), 20u);
            EXPECT_EQ(memory_.read_u32(BUF_ADDR + 132).value(), 200u);
            EXPECT_EQ(call("FlatSB_GetScrollInfo", {hwnd, bar, BUF_ADDR}), 1u);
            EXPECT_EQ(memory_.read_u32(BUF_ADDR + offsetof(SCROLLINFO, nPos)).value(), 50u);
            EXPECT_EQ(call("FlatSB_ShowScrollBar", {hwnd, bar, 0}), 1u);
            EXPECT_FALSE(scroll.visible);
            EXPECT_EQ(call("FlatSB_EnableScrollBar", {hwnd, bar, 0}), 0u);
            // Restore state for the independent unsupported-control check.
            EXPECT_EQ(call("FlatSB_SetScrollPos", {hwnd, bar, 80, 0}), 50u);
            cpu_.gpr.rcx = hwnd;
            (void)memory_.write_u32(cpu_.gpr.rsp + 4, hwnd);
            // SB_CTL is an explicit unsupported user32 path, not a third axis.
            cpu_.gpr.rdx = 2;
            (void)memory_.write_u32(cpu_.gpr.rsp + 8, 2);
            entry->handler(m);
            EXPECT_EQ(cpu_.gpr.rax, 0u);
            EXPECT_EQ(hwnd_mgr_.find(hwnd)->scroll_bars[bar].position, 80);
        }
    }
}
