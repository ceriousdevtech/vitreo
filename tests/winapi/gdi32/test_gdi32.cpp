/// @file test_gdi32.cpp
/// @brief Unit tests for GDI32 device context stubs (WIN32-063)

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <windowing/model/window_types.hpp>
#include <windowing/cocoa/cocoa_bridge.hpp>
#include <algorithm>
#include <array>
#include <set>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class GdiTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr = std::make_unique<GdiManager>(*m_handles);
    }

    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<GdiManager> m_mgr;
};

// =============================================================================
// GetDC / ReleaseDC
// =============================================================================

TEST_F(GdiTest, GetDCReturnsNonZeroHandle) {
    Handle hdc = m_mgr->get_dc(0);
    EXPECT_NE(hdc, Handle{0});
}

TEST_F(GdiTest, TextExtentWDoesNotMeasureCharactersBeyondExplicitCount) {
    windowing::CocoaBridge bridge;
    m_mgr->set_cocoa_bridge(&bridge);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const Handle dc = m_mgr->get_dc(0);
    ASSERT_TRUE(m.write_wide_string(0x1000, "9 unrelated trailing text").is_ok());
    cpu.gpr.rcx = dc;
    cpu.gpr.rdx = 0x1000;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = 0x2000;
    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& e) { return e.name == "GetTextExtentPoint32W"; });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(m);
    i32 width = 0, height = 0;
    m_mgr->get_text_extent(dc, "9", width, height);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(0x2000).value(), static_cast<u32>(width));
    EXPECT_EQ(memory.read_u32(0x2004).value(), static_cast<u32>(height));
}

TEST_F(GdiTest, SelectedMonospaceFontControlsTextMeasurement) {
    windowing::CocoaBridge bridge;
    m_mgr->set_cocoa_bridge(&bridge);
    const Handle dc = m_mgr->get_dc(0);
    const Handle font = m_mgr->create_font_w(-16, 400, false, u"Courier");
    (void)m_mgr->select_object(dc, font);
    i32 narrow = 0, wide = 0, height = 0;
    m_mgr->get_text_extent(dc, "iiii", narrow, height);
    m_mgr->get_text_extent(dc, "WWWW", wide, height);
    EXPECT_GT(narrow, 0);
    EXPECT_EQ(narrow, wide);
    const Handle proportional = m_mgr->create_font_w(-16, 400, false, u"Helvetica");
    (void)m_mgr->select_object(dc, proportional);
    m_mgr->get_text_extent(dc, "iiii", narrow, height);
    m_mgr->get_text_extent(dc, "WWWW", wide, height);
    EXPECT_LT(narrow, wide);
}

TEST_F(GdiTest, ExtendedTextExtentUsesProportionalAdvancesAndFitLimit) {
    windowing::CocoaBridge bridge;
    m_mgr->set_cocoa_bridge(&bridge);
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x3000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x1000);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const Handle dc = m_mgr->get_dc(0);
    i32 first_width = 0, height = 0, total_width = 0;
    m_mgr->get_text_extent(dc, "i", first_width, height);
    m_mgr->get_text_extent(dc, "iW", total_width, height);
    const std::array<u32, 8> args{0, static_cast<u32>(dc), 0x2000, 2,
        static_cast<u32>(first_width), 0x3000, 0x3010, 0x3020};
    for (size_t i = 0; i < args.size(); ++i)
        ASSERT_TRUE(memory.write_u32(0x1000 + i * 4, args[i]).is_ok());
    auto exports = make_gdi32_exports(*m_mgr);
    for (bool wide : {false, true}) {
        ASSERT_TRUE((wide ? m.write_wide_string(0x2000, "iW")
                          : m.write_ansi_string(0x2000, "iW")).is_ok());
        const auto name = wide ? "GetTextExtentExPointW" : "GetTextExtentExPointA";
        auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const auto& e) { return e.name == name; });
        ASSERT_NE(entry, exports.entries.end());
        entry->handler(m);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(memory.read_u32(0x3000).value(), 1u);
        EXPECT_EQ(memory.read_u32(0x3010).value(), static_cast<u32>(first_width));
        // With lpnFit the extents stop at the fitting characters: Windows
        // leaves dx[1] alone (s3gdi_text_probe "GetTextExtentExPointW(max20)
        // =1 fit=3 dx=9,16,19,0,0").
        EXPECT_EQ(memory.read_u32(0x3014).value(), 0u);
        EXPECT_EQ(memory.read_u32(0x3020).value(), static_cast<u32>(total_width));
    }
}

TEST_F(GdiTest, BaselineTextIsVisibleInShortCompatibleBitmap) {
    windowing::CocoaBridge bridge;
    m_mgr->set_cocoa_bridge(&bridge);
    const Handle dc = m_mgr->create_compatible_dc(0);
    // Compatible with the screen: a bitmap compatible with a fresh memory DC
    // is monochrome on Windows, where red text is black.
    const Handle bitmap = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 120, 13);
    (void)m_mgr->select_object(dc, bitmap);
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = dc;
    cpu.gpr.rdx = 0x18; // TA_BASELINE
    auto exports = make_gdi32_exports(*m_mgr);
    auto alignment = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& entry) { return entry.name == "SetTextAlign"; });
    ASSERT_NE(alignment, exports.entries.end());
    alignment->handler(marshaler);
    (void)m_mgr->set_text_color(dc, 0x0000FF);
    ASSERT_TRUE(m_mgr->text_out(dc, 0, 10, "Hello"));
    GdiManager::BitmapRgbaCopy copy;
    ASSERT_TRUE(m_mgr->copy_bitmap_rgba(bitmap, copy));
    size_t red_pixels = 0;
    for (size_t i = 0; i + 3 < copy.rgba.size(); i += 4)
        if (copy.rgba[i] > 200 && copy.rgba[i + 1] < 50 &&
            copy.rgba[i + 2] < 50)
            ++red_pixels;
    EXPECT_GT(red_pixels, 20u);
}

TEST_F(GdiTest, ExtTextOutOpaqueClearsRectangleWithoutTextAndPreservesClip) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x1000);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const Handle dc = m_mgr->create_compatible_dc(0);
    const Handle bitmap = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 20, 20);
    (void)m_mgr->select_object(dc, bitmap);
    (void)m_mgr->set_bk_color(dc, 0x0000FF);
    (void)m_mgr->intersect_clip_rect(dc, 3, 3, 15, 15);
    const std::array<u32, 9> args{0, static_cast<u32>(dc), 0, 0, 6, 0x2000, 0, 0, 0};
    for (size_t i = 0; i < args.size(); ++i)
        ASSERT_TRUE(memory.write_u32(0x1000 + i * 4, args[i]).is_ok());
    const std::array<u32, 4> rect{1, 1, 10, 10};
    for (size_t i = 0; i < rect.size(); ++i)
        ASSERT_TRUE(memory.write_u32(0x2000 + i * 4, rect[i]).is_ok());
    auto exports = make_gdi32_exports(*m_mgr);
    for (const auto* name : {"ExtTextOutA", "ExtTextOutW"}) {
        SCOPED_TRACE(name);
        ASSERT_TRUE(m_mgr->fill_rect_gdi(dc, 0, 0, 20, 20, 0xFFFFFF));
        auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const auto& e) { return e.name == name; });
        ASSERT_NE(entry, exports.entries.end());
        entry->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(m_mgr->get_pixel(dc, 4, 4), 0x0000FFu);
        EXPECT_EQ(m_mgr->get_pixel(dc, 10, 4), 0xFFFFFFu);
        // The temporary ETO_CLIPPED rectangle must not survive the call.
        ASSERT_TRUE(m_mgr->fill_rect_gdi(dc, 12, 12, 14, 14, 0x00FF00));
        EXPECT_EQ(m_mgr->get_pixel(dc, 12, 12), 0x00FF00u);
    }
}

TEST_F(GdiTest, CreateHalftonePaletteReturns256EntryPalette) {
    Handle hdc = m_mgr->get_dc(0);
    Handle pal = m_mgr->create_halftone_palette(hdc);
    ASSERT_NE(pal, Handle{0});
    EXPECT_EQ(m_mgr->get_palette_entries(pal, 0, 0, 0), 256u);
    EXPECT_EQ(m_mgr->create_halftone_palette(0xDEAD), Handle{0});
}

TEST_F(GdiTest, GetDIBColorTableFailsOnDisplayDc) {
    Handle hdc = m_mgr->get_dc(0);
    EXPECT_EQ(m_mgr->get_dib_color_table(hdc, 0, 1, 0x1000), 0u);
}

TEST_F(GdiTest, ReleaseDCReturnsOne) {
    Handle hdc = m_mgr->get_dc(0);
    EXPECT_EQ(m_mgr->release_dc(0, hdc), 1);
}

TEST_F(GdiTest, WindowFromDCReturnsAssociatedWindow) {
    constexpr Handle hwnd = 0x1234;
    const Handle hdc = m_mgr->get_dc(hwnd);
    EXPECT_EQ(m_mgr->window_from_dc(hdc), hwnd);
}

TEST_F(GdiTest, WindowFromDCScreenDCReturnsDesktopWindow) {
    const Handle screen_dc = m_mgr->get_dc(0);
    EXPECT_EQ(m_mgr->window_from_dc(screen_dc),
              windowing::HWND_DESKTOP);
}

TEST_F(GdiTest, WindowFromDCReturnsNullForMemoryAndUnknownDCs) {
    const Handle display_dc = m_mgr->get_dc(0x1234);
    const Handle memory_dc = m_mgr->create_compatible_dc(display_dc);
    EXPECT_EQ(m_mgr->window_from_dc(memory_dc), Handle{0});
    EXPECT_EQ(m_mgr->window_from_dc(0xDEAD), Handle{0});
}

TEST_F(GdiTest, CreateInformationDCReturnsTrackedDisplayDC) {
    Handle hdc = m_mgr->create_ic_w();
    ASSERT_NE(hdc, Handle{0});
    EXPECT_TRUE(m_handles->is_valid(hdc));
    EXPECT_EQ(m_mgr->get_device_caps(hdc, TECHNOLOGY), 1);
    EXPECT_TRUE(m_mgr->delete_dc(hdc));
}

// =============================================================================
// GetDeviceCaps
// =============================================================================

TEST_F(GdiTest, GetDeviceCapsHorzRes1920) {
    Handle hdc = m_mgr->get_dc(0);
    EXPECT_EQ(m_mgr->get_device_caps(hdc, HORZRES), 1920);
}

TEST_F(GdiTest, GetDeviceCapsBitsPixel32) {
    Handle hdc = m_mgr->get_dc(0);
    EXPECT_EQ(m_mgr->get_device_caps(hdc, BITSPIXEL), 32);
}

TEST_F(GdiTest, GetSystemPaletteUseTracksCurrentMode) {
    Handle hdc = m_mgr->get_dc(0);
    ASSERT_NE(hdc, Handle{0});
    EXPECT_EQ(m_mgr->get_system_palette_use(hdc), 1u);
    EXPECT_EQ(m_mgr->set_system_palette_use(hdc, 2), 1u);
    EXPECT_EQ(m_mgr->get_system_palette_use(hdc), 2u);
}

TEST_F(GdiTest, GetSystemPaletteUseRejectsInvalidDC) {
    EXPECT_EQ(m_mgr->get_system_palette_use(0xDEAD), 0u);
}

// =============================================================================
// Stock Objects
// =============================================================================

TEST_F(GdiTest, GetStockObjectWhiteBrush) {
    Handle h = m_mgr->get_stock_object(WHITE_BRUSH_ID);
    EXPECT_NE(h, Handle{0});
}

TEST_F(GdiTest, GetStockObjectInvalidIdReturnsZero) {
    Handle h = m_mgr->get_stock_object(999);
    EXPECT_EQ(h, Handle{0});
}

TEST_F(GdiTest, StockFontAndPaletteIdsMatchWin32) {
    EXPECT_NE(m_mgr->get_stock_object(SYSTEM_FONT_ID), Handle{0});
    EXPECT_NE(m_mgr->get_stock_object(DEFAULT_PALETTE_ID), Handle{0});
    EXPECT_NE(m_mgr->get_stock_object(DEFAULT_GUI_FONT_ID), Handle{0});
    EXPECT_NE(m_mgr->get_stock_object(SYSTEM_FONT_ID),
              m_mgr->get_stock_object(DEFAULT_PALETTE_ID));
    EXPECT_NE(m_mgr->get_stock_object(DEFAULT_GUI_FONT_ID),
              m_mgr->get_stock_object(DEFAULT_PALETTE_ID));
}

// =============================================================================
// SelectObject
// =============================================================================

TEST_F(GdiTest, SelectObjectReturnsPreviousObject) {
    Handle hdc = m_mgr->get_dc(0);
    Handle brush1 = m_mgr->get_stock_object(WHITE_BRUSH_ID);
    Handle brush2 = m_mgr->get_stock_object(BLACK_BRUSH_ID);

    // A display DC starts with WHITE_BRUSH selected.
    Handle prev1 = m_mgr->select_object(hdc, brush1);
    EXPECT_EQ(prev1, brush1);

    // Second select — previous should be brush1
    Handle prev2 = m_mgr->select_object(hdc, brush2);
    EXPECT_EQ(prev2, brush1);
}

TEST_F(GdiTest, SelectFontReturnsDefaultSystemFont) {
    Handle hdc = m_mgr->get_dc(0);
    Handle font = m_mgr->create_font_w(-14, 400, false);

    EXPECT_EQ(m_mgr->select_object(hdc, font),
              m_mgr->get_stock_object(SYSTEM_FONT_ID));
}

TEST_F(GdiTest, SelectPaletteReturnsDefaultPalette) {
    Handle hdc = m_mgr->get_dc(0);
    Handle palette = m_mgr->get_stock_object(DEFAULT_PALETTE_ID);

    EXPECT_EQ(m_mgr->select_palette(hdc, palette, false), palette);
}

// =============================================================================
// DeleteObject
// =============================================================================

TEST_F(GdiTest, DeleteObjectReturnsTrueForUserObject) {
    Handle user_obj = m_mgr->create_solid_brush(0x123456);
    EXPECT_TRUE(m_mgr->delete_object(user_obj));
    // The second delete of the same handle fails (s3gdi_bitmap_probe
    // "delete-after-deselect=0").
    EXPECT_FALSE(m_mgr->delete_object(user_obj));
    // A handle that was never a GDI object is not one now.
    EXPECT_FALSE(m_mgr->delete_object(m_handles->allocate(kHandleTypeGdiObject, nullptr)));
}

TEST_F(GdiTest, DeleteObjectOnStockObjectSucceedsWithoutDeleting) {
    // s3gdi_bitmap_probe: DeleteObject(GetStockObject(WHITE_BRUSH)) is TRUE
    // and the stock brush stays usable.
    Handle stock = m_mgr->get_stock_object(WHITE_BRUSH_ID);
    EXPECT_TRUE(m_mgr->delete_object(stock));
    EXPECT_EQ(m_mgr->object_type(stock), OBJ_BRUSH_TYPE);
}

// =============================================================================
// CreateCompatibleDC
// =============================================================================

TEST_F(GdiTest, CreateCompatibleDCReturnsNewHandle) {
    Handle hdc = m_mgr->get_dc(0);
    Handle compat = m_mgr->create_compatible_dc(hdc);
    EXPECT_NE(compat, Handle{0});
    EXPECT_NE(compat, hdc);
}

// =============================================================================
// GetSystemMetrics
// =============================================================================

TEST_F(GdiTest, GetSystemMetricsCxScreen1920) {
    EXPECT_EQ(m_mgr->get_system_metrics(SM_CXSCREEN), 1920);
}

// =============================================================================
// SetBkMode
// =============================================================================

TEST_F(GdiTest, SetBkModeReturnsPreviousMode) {
    Handle hdc = m_mgr->get_dc(0);

    // Default bk_mode is OPAQUE (2) per Win32 (TRANSPARENT=1, OPAQUE=2)
    i32 prev = m_mgr->set_bk_mode(hdc, 1); // set to TRANSPARENT
    EXPECT_EQ(prev, 2); // was OPAQUE

    // Now previous should be TRANSPARENT (1)
    i32 prev2 = m_mgr->set_bk_mode(hdc, 2); // set back to OPAQUE
    EXPECT_EQ(prev2, 1); // was TRANSPARENT
}

// =============================================================================
// Thunk Creation
// =============================================================================

TEST_F(GdiTest, ThunksContainExpectedEntries) {
    DllExports exports = make_gdi32_exports(*m_mgr);
    EXPECT_GE(exports.entries.size(), 12u);

    // Presence, not position. These assertions previously used fixed indices
    // and broke as soon as the D3DKMT adapter exports were registered ahead of
    // them, even though every named export was still present and correct.
    std::set<std::string> seen;
    for (const auto& e : exports.entries) {
        EXPECT_TRUE(seen.insert(e.name).second)
            << "duplicate export registration shadows an earlier one: "
            << e.name;
    }

    for (const char* required : {
            "GetDC",
            "GetDeviceCaps",
            "GetStockObject",
            "GetSystemMetrics",
            "GetCharABCWidthsFloatW",
            "GetCharABCWidthsFloatA",
            "GetCharWidthW",
            "GetCharWidth32W",
            "AddFontResourceExW",
            "RemoveFontResourceExW",
            "GetTextFaceA",
            "GetGlyphOutlineW",
            "GetGlyphOutlineA",
            "GetOutlineTextMetricsW",
            "GetOutlineTextMetricsA",
            "GetCharABCWidthsI",
            "CreateICA",
            "CreateICW",
            "CreateHalftonePalette",
            "GetDIBColorTable",
            "ChoosePixelFormat",
            "DescribePixelFormat",
        }) {
        EXPECT_TRUE(seen.count(required) != 0)
            << "missing gdi32 export: " << required;
    }
}

TEST_F(GdiTest, GetOutlineTextMetricsWSizeQueryAndFill) {
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr buf = 0x2000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x4000, MemoryProtection::ReadWrite).is_ok());
    m_mgr->set_guest_memory(&memory);

    auto exports = make_gdi32_exports(*m_mgr);
    auto* entry = static_cast<ExportEntry*>(nullptr);
    for (auto& e : exports.entries) {
        if (e.name == "GetOutlineTextMetricsW") {
            entry = &e;
            break;
        }
    }
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12);

    Handle hdc = m_mgr->get_dc(0);
    ASSERT_NE(hdc, Handle{0});

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    entry->handler(marshaler);
    const u32 needed = static_cast<u32>(cpu.gpr.rax);
    EXPECT_GE(needed, 216u);
    EXPECT_LT(needed, 2048u);

    ASSERT_TRUE(memory.write_u32(stack + 8, needed).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, static_cast<u32>(buf)).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), needed);
    EXPECT_EQ(memory.read_u32(buf).value_or(0), needed);
    EXPECT_NE(memory.read_u32(buf + 4).value_or(0), 0u);  // tmHeight
    EXPECT_EQ(memory.read_u8(buf + 4 + 55).value_or(0) & 0x04, 0x04);  // TMPF_TRUETYPE
    const u32 family_off = memory.read_u32(buf + 200).value_or(0);
    EXPECT_GE(family_off, 200u);
    EXPECT_LT(family_off, needed);
}

TEST_F(GdiTest, GetOutlineTextMetricsWRejectsNullDcAndShortBuffer) {
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr buf = 0x2000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x4000, MemoryProtection::ReadWrite).is_ok());
    m_mgr->set_guest_memory(&memory);

    auto exports = make_gdi32_exports(*m_mgr);
    auto* entry = static_cast<ExportEntry*>(nullptr);
    for (auto& e : exports.entries) {
        if (e.name == "GetOutlineTextMetricsW") {
            entry = &e;
            break;
        }
    }
    ASSERT_NE(entry, nullptr);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    // A buffer shorter than the structure is not rejected: Windows copies
    // cjCopy bytes and returns cjCopy (s3gdi_otm_probe "small(100) r=100
    // err=57005 size=342").
    Handle hdc = m_mgr->get_dc(0);
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, static_cast<u32>(buf)).is_ok());
    ASSERT_TRUE(memory.write_u32(buf + 8, 0xCCCCCCCCu).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 8u);
    EXPECT_GT(memory.read_u32(buf).value_or(0), 200u);  // otmSize: the whole size
    EXPECT_EQ(memory.read_u32(buf + 8).value_or(0), 0xCCCCCCCCu);
}

TEST_F(GdiTest, CreateICAReturnsTrackedInformationDC) {
    // Learn: CreateICA creates an information context (ANSI twin of CreateICW).
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x2000, MemoryProtection::ReadWrite).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "CreateICA"; });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // A NULL driver is no device (s3gdi_print_probe "CreateICA(NULL)=NULL");
    // "DISPLAY" is the display.
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());  // driver
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());  // device
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_bytes(mem + 0x100, reinterpret_cast<const u8*>("DISPLAY"), 8).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, mem + 0x100).is_ok());  // driver
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok()); // port
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok()); // DEVMODEA*

    entry->handler(marshaler);
    const auto hdc = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(hdc, Handle{0});
    EXPECT_TRUE(m_handles->is_valid(hdc));
    EXPECT_EQ(m_mgr->get_device_caps(hdc, TECHNOLOGY), 1);
    EXPECT_TRUE(m_mgr->delete_dc(hdc));
}

TEST_F(GdiTest, DescribePixelFormatFillsOpenGlWindowDescriptor) {
    // Learn: DescribePixelFormat writes PIXELFORMATDESCRIPTOR members and
    // returns the maximum format index. SDL/GL probes reject a zeroed PFD.
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr pfd = 0x1A00;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x4000, MemoryProtection::ReadWrite).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto describe = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "DescribePixelFormat"; });
    ASSERT_NE(describe, exports.entries.end());

    const Handle hdc = m_mgr->get_dc(0);
    ASSERT_NE(hdc, Handle{0});

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    std::array<u8, 40> zero{};
    ASSERT_TRUE(memory.write_bytes(pfd, zero.data(), zero.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    // Format 6 of Vitreo's ICD list: double buffered, 24-bit depth, no stencil.
    ASSERT_TRUE(memory.write_u32(stack + 8, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 40).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(pfd)).is_ok());

    describe->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 16u);  // DescribePixelFormat returns the format count
    EXPECT_EQ(memory.read_u16(pfd).value_or(0), 40u);
    EXPECT_EQ(memory.read_u16(pfd + 2).value_or(0), 1u);
    const u32 flags = memory.read_u32(pfd + 4).value_or(0);
    EXPECT_EQ(flags & 0x00000004u, 0x00000004u); // PFD_DRAW_TO_WINDOW
    EXPECT_EQ(flags & 0x00000020u, 0x00000020u); // PFD_SUPPORT_OPENGL
    EXPECT_EQ(flags & 0x00000001u, 0x00000001u); // PFD_DOUBLEBUFFER
    EXPECT_EQ(memory.read_u8(pfd + 9).value_or(0), 32u);  // cColorBits
    EXPECT_EQ(memory.read_u8(pfd + 23).value_or(0), 24u); // cDepthBits
}

TEST_F(GdiTest, GetCharABCWidthsFloatWWritesAbcFloatRange) {
    // Qt qwindows.dll queries first=0x20..last=0xff; a miss returned
    // 0xFFFFFFFF (BOOL TRUE) with an empty buffer and corrupted control flow.
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr abc = 0x1A00;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x4000, MemoryProtection::ReadWrite).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "GetCharABCWidthsFloatW";
        });
    ASSERT_NE(entry, exports.entries.end());

    const Handle hdc = m_mgr->get_dc(0);
    ASSERT_NE(hdc, Handle{0});

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr u32 first = 0x20;
    constexpr u32 last = 0x22; // 3 glyphs
    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, first).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, last).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(abc)).is_ok());

    // The DC's font decides the widths; the fixed 8.0 advances this pinned
    // described no font. Windows reports the device ABC widths as whole-pixel
    // floats (s3gdi_otm_probe "GetCharABCWidthsFloatW(A)=1 0.000,9.000,0.000").
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    float fabc[9] = {};
    ASSERT_TRUE(memory.read_bytes(abc, reinterpret_cast<u8*>(fabc), sizeof(fabc)).is_ok());
    for (int i = 0; i < 3; ++i) {
        const float sum = fabc[i * 3] + fabc[i * 3 + 1] + fabc[i * 3 + 2];
        EXPECT_GT(sum, 0.0f);
        EXPECT_EQ(sum, static_cast<float>(static_cast<int>(sum)));  // whole pixels
    }
}

TEST_F(GdiTest, GetCharWidthWWritesIntegerAdvances) {
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr widths = 0x1A00;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x4000, MemoryProtection::ReadWrite).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "GetCharWidthW";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 16);

    const Handle hdc = m_mgr->get_dc(0);
    ASSERT_NE(hdc, Handle{0});

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x20).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x22).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(widths)).is_ok());
    ASSERT_TRUE(memory.write_u32(widths, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(widths + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(widths + 8, 0xFFFFFFFFu).is_ok());

    // Arial -13's device advances for ' ', '!' and '"' (hdmx: 4, 3, 5 -
    // the fixed 8 this pinned was no font's widths; s3gdi_otm_probe
    // "GetCharWidthW(a..c)=1 w=7,7,7" measures the same table).
    const Handle font = m_mgr->create_font_w(-13, 400, false, u"Arial");
    (void)m_mgr->select_object(hdc, font);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(widths).value_or(0), 4u);
    EXPECT_EQ(memory.read_u32(widths + 4).value_or(0), 3u);
    EXPECT_EQ(memory.read_u32(widths + 8).value_or(0), 5u);
}

TEST_F(GdiTest, AddFontResourceExWUsesStdcall12) {
    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "AddFontResourceExW";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 12);
}

TEST_F(GdiTest, GetCharABCWidthsFloatWRejectsNullPageBuffer) {
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x2000, MemoryProtection::ReadWrite).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "GetCharABCWidthsFloatW";
        });
    ASSERT_NE(entry, exports.entries.end());

    const Handle hdc = m_mgr->get_dc(0);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x20).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x21).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, 0).is_ok()); // null lpabc

    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(GdiTest, GetGlyphOutlineWMetricsWritesGlyphMetrics) {
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr gm = 0x1A00;
    constexpr GuestAddr mat2 = 0x1B00;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x4000, MemoryProtection::ReadWrite).is_ok());
    // Identity FIXED MAT2: eM11=1.0 (FIXED 0x00010000), others 0.
    ASSERT_TRUE(memory.write_u32(mat2 + 0, 0x00010000u).is_ok());
    ASSERT_TRUE(memory.write_u32(mat2 + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(mat2 + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(mat2 + 12, 0x00010000u).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetGlyphOutlineW"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 28u);

    const Handle hdc = m_mgr->get_dc(0);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x41).is_ok()); // 'A'
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());    // GGO_METRICS
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(gm)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 28, static_cast<u32>(mat2)).is_ok());

    entry->handler(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
    // 'A' is an inked glyph in any text font, so its black box and advance are
    // both non-empty and no larger than a few ems at the default size.
    const u32 black_box_x = memory.read_u32(gm + 0).value_or(0);
    const u32 black_box_y = memory.read_u32(gm + 4).value_or(0);
    EXPECT_GT(black_box_x, 0u);
    EXPECT_LT(black_box_x, 128u);
    EXPECT_GT(black_box_y, 0u);
    EXPECT_LT(black_box_y, 128u);
    EXPECT_GT(memory.read_u16(gm + 16).value_or(0), 0u);
}

TEST_F(GdiTest, GetGlyphOutlineWGray8BitmapRasterizesCoverage) {
    constexpr GuestAddr mem = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr gm = 0x1A00;
    constexpr GuestAddr mat2 = 0x1B00;
    constexpr GuestAddr bits = 0x2000;
    constexpr u32 GGO_GRAY8_BITMAP = 6;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(mem, 0x8000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(mat2 + 0, 0x00010000u).is_ok());
    ASSERT_TRUE(memory.write_u32(mat2 + 12, 0x00010000u).is_ok());

    auto exports = make_gdi32_exports(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetGlyphOutlineW"; });
    ASSERT_NE(entry, exports.entries.end());

    const Handle hdc = m_mgr->get_dc(0);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hdc)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0x41).is_ok());  // 'A'
    ASSERT_TRUE(memory.write_u32(stack + 12, GGO_GRAY8_BITMAP).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(gm)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0).is_ok());  // size query
    ASSERT_TRUE(memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 28, static_cast<u32>(mat2)).is_ok());

    entry->handler(marshaler);
    const u32 required = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(required, 0xFFFFFFFFu);
    ASSERT_GT(required, 0u);

    ASSERT_TRUE(memory.write_u32(stack + 20, required).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 24, static_cast<u32>(bits)).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), required);

    // GGO_GRAY8_BITMAP counts 65 coverage levels, 0 through 64, and an inked
    // glyph has to cover at least one pixel.
    u32 inked = 0;
    for (u32 index = 0; index < required; ++index) {
        const u8 value = memory.read_u8(bits + index).value_or(0xFF);
        ASSERT_LE(value, 64u);
        if (value != 0) ++inked;
    }
    EXPECT_GT(inked, 0u);
}

// =============================================================================
// SystemParametersInfoW Contract Tests
// =============================================================================

class GdiSpiTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr BUF_ADDR   = 0x2000;

    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr = std::make_unique<GdiManager>(*m_handles);
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(BUF_ADDR, 0x10000, MemoryProtection::ReadWrite);
        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    ExportEntry find_export(const std::string& name) {
        DllExports exports = make_gdi32_exports(*m_mgr);
        for (const auto& e : exports.entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name << " not found";
        return {};
    }

    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<GdiManager> m_mgr;
    GuestMemory memory_;
    cpu::CpuState cpu_;
};

TEST_F(GdiSpiTest, SpiGetWorkAreaReturnsTrueAndWritesRect) {
    auto entry = find_export("SystemParametersInfoW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 SPI_GETWORKAREA = 0x0030;
    cpu_.gpr.rcx = SPI_GETWORKAREA;  // uiAction
    cpu_.gpr.rdx = 0;                // uiParam
    cpu_.gpr.r8  = BUF_ADDR;        // pvParam = RECT*
    cpu_.gpr.r9  = 0;                // fWinIni

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u); // TRUE

    // RECT: left=0, top=0, right=SM_CXSCREEN, bottom=SM_CYSCREEN
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 0).value(), 0u);    // left
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 4).value(), 0u);    // top
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 8).value(), 1920u); // right
    EXPECT_EQ(memory_.read_u32(BUF_ADDR + 12).value(), 1080u);// bottom
}

TEST_F(GdiSpiTest, SpiGetNonClientMetricsReturnsTrueAndSetsCbSize) {
    auto entry = find_export("SystemParametersInfoW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 SPI_GETNONCLIENTMETRICS = 0x0029;
    constexpr u32 NCM_SIZE = 504; // NONCLIENTMETRICSW size
    cpu_.gpr.rcx = SPI_GETNONCLIENTMETRICS;
    cpu_.gpr.rdx = NCM_SIZE;             // uiParam = sizeof(NONCLIENTMETRICSW)
    cpu_.gpr.r8  = BUF_ADDR;            // pvParam
    cpu_.gpr.r9  = 0;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    // cbSize should be written
    EXPECT_EQ(memory_.read_u32(BUF_ADDR).value(), NCM_SIZE);
}

TEST_F(GdiSpiTest, SpiDefaultReturnsTrueForUnknownAction) {
    auto entry = find_export("SystemParametersInfoW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0x9999;  // bogus SPI action
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8  = 0;
    cpu_.gpr.r9  = 0;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u); // TRUE — best-effort
}

TEST_F(GdiSpiTest, GetFontDataRegistersFiveArgumentStdcallSignature) {
    const auto entry = find_export("GetFontData");
    EXPECT_EQ(entry.stdcall_bytes, 20u);
}

// A font engine that shapes text itself reads the font's own tables through
// GetFontData; without them it maps every character to glyph 0 and draws
// nothing.
TEST_F(GdiSpiTest, GetFontDataReturnsTheSelectedFontsOwnTable) {
    const auto entry = find_export("GetFontData");
    const Handle hdc = m_mgr->get_dc(0);
    ASSERT_NE(hdc, Handle{0});

    cpu_.init_protected_mode();
    cpu_.gpr.rsp = STACK_BASE + 0x800;
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 CMAP_TABLE = 0x70616D63u;  // 'cmap', low byte first
    (void)memory_.write_u32(cpu_.gpr.rsp + 4, hdc);
    (void)memory_.write_u32(cpu_.gpr.rsp + 8, CMAP_TABLE);
    (void)memory_.write_u32(cpu_.gpr.rsp + 12, 0);
    (void)memory_.write_u32(cpu_.gpr.rsp + 16, 0);  // size query
    (void)memory_.write_u32(cpu_.gpr.rsp + 20, 0);

    entry.handler(m);
    const u32 table_size = static_cast<u32>(cpu_.gpr.rax);
    ASSERT_NE(table_size, 0xFFFFFFFFu);
    ASSERT_GT(table_size, 4u);

    (void)memory_.write_u32(cpu_.gpr.rsp + 16, BUF_ADDR);
    (void)memory_.write_u32(cpu_.gpr.rsp + 20, 4);
    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 4u);
    // A cmap table starts with version 0 followed by its subtable count.
    EXPECT_EQ(memory_.read_u16(BUF_ADDR).value(), 0u);
    EXPECT_NE(memory_.read_u16(BUF_ADDR + 2).value(), 0u);
}

// =============================================================================
// CreateDIBSection Tests
// =============================================================================

class GdiDIBTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE = 0x1000;
    static constexpr GuestAddr DIB_SCRATCH = 0x5E100000; // GDI_DIB_SCRATCH_BASE

    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr = std::make_unique<GdiManager>(*m_handles);
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(DIB_SCRATCH,
                                 GdiManager::GDI_DIB_SCRATCH_SIZE,
                                 MemoryProtection::ReadWrite);
        m_mgr->set_guest_memory(&memory_);
    }

    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<GdiManager> m_mgr;
    GuestMemory memory_;
};

TEST_F(GdiDIBTest, CopyBitmapRgbaSyncsGuestDibPixels) {
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;
    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 2);
    (void)memory_.write_u32(BMI + 8, 2);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 32);
    (void)memory_.write_u32(BMI + 16, 0);

    Handle h = m_mgr->create_dib_section(0, BMI, 0, PPVBITS);
    ASSERT_NE(h, Handle{0});
    const u32 bits = memory_.read_u32(PPVBITS).value();
    // Bottom-up: last row is image y=0. BGRA red + opaque.
    const std::array<u8, 4> bgra{0x11, 0x22, 0x33, 0xFF};
    // 2x2 32bpp stride is 8. Bottom-up: row 1 in memory is image y=0.
    ASSERT_TRUE(memory_.write_bytes(bits + 8, bgra.data(), bgra.size()).is_ok());

    GdiManager::BitmapRgbaCopy copy{};
    ASSERT_TRUE(m_mgr->copy_bitmap_rgba(h, copy));
    EXPECT_EQ(copy.width, 2);
    EXPECT_EQ(copy.height, 2);
    EXPECT_EQ(copy.bpp, 32);
    EXPECT_TRUE(copy.from_dib);
    ASSERT_EQ(copy.rgba.size(), 16u);
    EXPECT_EQ(copy.rgba[0], 0x33);
    EXPECT_EQ(copy.rgba[1], 0x22);
    EXPECT_EQ(copy.rgba[2], 0x11);
    EXPECT_EQ(copy.rgba[3], 0xFF);
}

TEST_F(GdiDIBTest, CreateDIBSectionReturnsValidHandle) {
    // Write a BITMAPINFOHEADER at MEM_BASE
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;

    // BITMAPINFOHEADER: biSize=40, biWidth=10, biHeight=10, biPlanes=1, biBitCount=32
    (void)memory_.write_u32(BMI + 0,  40);   // biSize
    (void)memory_.write_u32(BMI + 4,  10);   // biWidth (i32)
    (void)memory_.write_u32(BMI + 8,  10);   // biHeight (i32, positive = bottom-up)
    (void)memory_.write_u16(BMI + 12, 1);    // biPlanes
    (void)memory_.write_u16(BMI + 14, 32);   // biBitCount
    (void)memory_.write_u32(BMI + 16, 0);    // biCompression = BI_RGB

    Handle h = m_mgr->create_dib_section(0, BMI, 0, PPVBITS);
    EXPECT_NE(h, Handle{0});

    // Check that ppvBits was written with a valid guest address
    auto bits_addr = memory_.read_u32(PPVBITS);
    ASSERT_TRUE(bits_addr.is_ok());
    EXPECT_GE(bits_addr.value(), DIB_SCRATCH);
}

TEST_F(GdiDIBTest, CreateDIBSectionWorksWithTopDown) {
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;

    (void)memory_.write_u32(BMI + 0,  40);
    (void)memory_.write_u32(BMI + 4,  8);     // width=8
    // biHeight = -16 (negative = top-down)
    (void)memory_.write_u32(BMI + 8,  static_cast<u32>(-16));
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 24);    // 24bpp
    (void)memory_.write_u32(BMI + 16, 0);

    Handle h = m_mgr->create_dib_section(0, BMI, 0, PPVBITS);
    EXPECT_NE(h, Handle{0});
}

TEST_F(GdiDIBTest, SupportsMultipleFullLayeredWindowFrames) {
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;

    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 738);
    (void)memory_.write_u32(BMI + 8, 468);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 32);
    (void)memory_.write_u32(BMI + 16, 0);

    // Battle.net Setup retains multiple BGRA frames while composing the next
    // UpdateLayeredWindow submission. Four frames require about 5.3 MB.
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(m_mgr->create_dib_section(0, BMI, 0, PPVBITS), Handle{0});
    }
}

TEST_F(GdiDIBTest, CreateDIBSectionFailsWithZeroBmi) {
    Handle h = m_mgr->create_dib_section(0, 0, 0, 0);
    EXPECT_EQ(h, Handle{0});
}

TEST_F(GdiDIBTest, SetDIBitsToDeviceSupportsRgb555) {
    // InstallShield skin assets include BI_RGB 16bpp BMPs (e.g. _is_.bmp).
    Handle hdc = m_mgr->create_compatible_dc(0);
    // A fresh memory DC is compatible with its 1x1 monochrome default
    // bitmap (s3gdi_bitmap_probe "ccb fresh-memdc"); take the display format.
    Handle bmp = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 2, 1);
    ASSERT_NE(bmp, Handle{0});
    (void)m_mgr->select_object(hdc, bmp);

    constexpr GuestAddr BMI = MEM_BASE + 0x200;
    constexpr GuestAddr BITS = MEM_BASE + 0x280;
    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 2);   // width
    (void)memory_.write_u32(BMI + 8, 1);   // height bottom-up
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 16);
    (void)memory_.write_u32(BMI + 16, 0);  // BI_RGB
    // RGB555 pure red: R=31 → 0x7C00
    (void)memory_.write_u16(BITS + 0, 0x7C00);
    (void)memory_.write_u16(BITS + 2, 0x7C00);
    (void)memory_.write_u32(BITS + 4, 0); // stride pad to 8 bytes

    EXPECT_EQ(m_mgr->set_dib_bits_to_device(
                  hdc, 0, 0, 2, 1, 0, 0, 0, 1, BITS, BMI, 0),
              1);
    const u32 pixel = m_mgr->get_pixel(hdc, 0, 0);
    // COLORREF is 0x00BBGGRR
    EXPECT_EQ(pixel & 0xFF, 0xFFu);         // R
    EXPECT_EQ((pixel >> 8) & 0xFF, 0u);     // G
    EXPECT_EQ((pixel >> 16) & 0xFF, 0u);    // B
}

TEST_F(GdiDIBTest, CreateDIBSectionBitmapSelectableIntoDC) {
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;

    (void)memory_.write_u32(BMI + 0,  40);
    (void)memory_.write_u32(BMI + 4,  4);
    (void)memory_.write_u32(BMI + 8,  4);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 32);
    (void)memory_.write_u32(BMI + 16, 0);

    Handle hBitmap = m_mgr->create_dib_section(0, BMI, 0, PPVBITS);
    ASSERT_NE(hBitmap, Handle{0});

    // Select into a compatible DC
    Handle hdc = m_mgr->create_compatible_dc(0);
    Handle prev = m_mgr->select_object(hdc, hBitmap);
    // select_object should succeed (returns previous stock bitmap)
    EXPECT_NE(prev, hBitmap); // previous should be the stock bitmap, not our DIB
}

TEST_F(GdiDIBTest, CreateDIBSectionPalColorsResolvesLogicalPaletteIndices) {
    constexpr GuestAddr LOG_PALETTE = MEM_BASE + 0x200;
    constexpr GuestAddr BMI = MEM_BASE + 0x300;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x400;

    (void)memory_.write_u16(LOG_PALETTE + 0, 0x0300);
    (void)memory_.write_u16(LOG_PALETTE + 2, 2);
    const u8 palette_entries[] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    (void)memory_.write_bytes(LOG_PALETTE + 4, palette_entries,
                              sizeof(palette_entries));

    Handle palette = m_mgr->create_palette(LOG_PALETTE);
    ASSERT_NE(palette, Handle{0});
    Handle source_dc = m_mgr->create_compatible_dc(0);
    (void)m_mgr->select_palette(source_dc, palette, false);

    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 1);
    (void)memory_.write_u32(BMI + 8, 1);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 8);
    (void)memory_.write_u32(BMI + 16, 0);
    (void)memory_.write_u32(BMI + 32, 2);
    (void)memory_.write_u16(BMI + 40, 0);
    (void)memory_.write_u16(BMI + 42, 1);

    // The DIB takes the palette's colours as they are at creation; changing
    // the palette afterwards does not reach it (s3gdi_misc_probe
    // "palcolors after SetPaletteEntries -> 00000000").
    const u8 updated_entry[] = {0x12, 0x34, 0x56, 0x00};
    (void)memory_.write_bytes(LOG_PALETTE + 4, updated_entry,
                              sizeof(updated_entry));
    ASSERT_EQ(m_mgr->set_palette_entries(palette, 1, 1, LOG_PALETTE + 4), 1u);
    Handle dib = m_mgr->create_dib_section(source_dc, BMI, 1, PPVBITS);
    ASSERT_NE(dib, Handle{0});
    const u8 later_entry[] = {0x01, 0x02, 0x03, 0x00};
    (void)memory_.write_bytes(LOG_PALETTE + 4, later_entry, sizeof(later_entry));
    ASSERT_EQ(m_mgr->set_palette_entries(palette, 1, 1, LOG_PALETTE + 4), 1u);
    GuestAddr bits = memory_.read_u32(PPVBITS).value();
    (void)memory_.write_u8(bits, 1);
    (void)m_mgr->select_object(source_dc, dib);

    Handle dest_dc = m_mgr->create_compatible_dc(0);
    Handle dest_bitmap = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 1, 1);
    (void)m_mgr->select_object(dest_dc, dest_bitmap);
    ASSERT_TRUE(m_mgr->bit_blt(dest_dc, 0, 0, 1, 1, source_dc, 0, 0,
                               0x00CC0020));
    EXPECT_EQ(m_mgr->get_pixel(dest_dc, 0, 0), 0x00563412u);
}

TEST_F(GdiDIBTest, StretchDIBitsPalColorsUsesDestinationLogicalPalette) {
    constexpr GuestAddr LOG_PALETTE = MEM_BASE + 0x900;
    constexpr GuestAddr BMI = MEM_BASE + 0xA00;
    constexpr GuestAddr BITS = MEM_BASE + 0xB00;

    (void)memory_.write_u16(LOG_PALETTE + 0, 0x0300);
    (void)memory_.write_u16(LOG_PALETTE + 2, 2);
    const u8 palette_entries[] = {
        0x00, 0x00, 0x00, 0x00,
        0x12, 0x34, 0x56, 0x00,
    };
    (void)memory_.write_bytes(LOG_PALETTE + 4, palette_entries,
                              sizeof(palette_entries));

    Handle palette = m_mgr->create_palette(LOG_PALETTE);
    ASSERT_NE(palette, Handle{0});

    Handle dest_dc = m_mgr->create_compatible_dc(0);
    Handle dest_bitmap = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 1, 1);
    (void)m_mgr->select_object(dest_dc, dest_bitmap);
    (void)m_mgr->select_palette(dest_dc, palette, false);

    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 1);
    (void)memory_.write_u32(BMI + 8, 1);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 8);
    (void)memory_.write_u32(BMI + 16, 0);
    (void)memory_.write_u32(BMI + 32, 2);
    // DIB_PAL_COLORS: bitmap color index 0 resolves through palette entry 1.
    (void)memory_.write_u16(BMI + 40, 1);
    (void)memory_.write_u16(BMI + 42, 0);
    (void)memory_.write_u32(BITS, 0);

    Marshaler marshaler(memory_);
    ASSERT_EQ(m_mgr->stretch_dib_bits(
                  marshaler, dest_dc, 0, 0, 1, 1, 0, 0, 1, 1,
                  BITS, BMI, 1, 0x00CC0020),
              1);
    EXPECT_EQ(m_mgr->get_pixel(dest_dc, 0, 0), 0x00563412u);
}

TEST_F(GdiDIBTest, StretchDIBitsUsesLowerLeftOriginForBottomUpSourceSlice) {
    constexpr GuestAddr BMI = MEM_BASE + 0xC00;
    constexpr GuestAddr BITS = MEM_BASE + 0xD00;

    Handle dest_dc = m_mgr->create_compatible_dc(0);
    Handle dest_bitmap = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 1, 2);
    (void)m_mgr->select_object(dest_dc, dest_bitmap);

    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 1);
    (void)memory_.write_u32(BMI + 8, 4); // positive: bottom-up
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 32);
    (void)memory_.write_u32(BMI + 16, 0);

    // Buffer rows are ordered bottom to top. Select rows 1..2 with ySrc=1;
    // the top destination pixel must come from row 2, followed by row 1.
    const u8 rows[] = {
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x20, 0x00, 0x00,
        0x30, 0x00, 0x00, 0x00,
        0x40, 0x40, 0x40, 0x00,
    };
    (void)memory_.write_bytes(BITS, rows, sizeof(rows));

    Marshaler marshaler(memory_);
    ASSERT_EQ(m_mgr->stretch_dib_bits(
                  marshaler, dest_dc, 0, 0, 1, 2, 0, 1, 1, 2,
                  BITS, BMI, 0, 0x00CC0020),
              4);
    EXPECT_EQ(m_mgr->get_pixel(dest_dc, 0, 0), 0x00300000u);
    EXPECT_EQ(m_mgr->get_pixel(dest_dc, 0, 1), 0x00002000u);
}

TEST_F(GdiDIBTest, BitBltIntoIndexedDibUpdatesGuestBitsAndSubsequentBlits) {
    constexpr GuestAddr LOG_PALETTE = MEM_BASE + 0x500;
    constexpr GuestAddr SOURCE_BMI = MEM_BASE + 0x600;
    constexpr GuestAddr DEST_BMI = MEM_BASE + 0x700;
    constexpr GuestAddr SOURCE_BITS_PTR = MEM_BASE + 0x800;
    constexpr GuestAddr DEST_BITS_PTR = MEM_BASE + 0x804;

    (void)memory_.write_u16(LOG_PALETTE, 0x0300);
    (void)memory_.write_u16(LOG_PALETTE + 2, 2);
    const u8 palette_entries[] = {
        0x00, 0x00, 0x00, 0x00,
        0x56, 0x34, 0x12, 0x00,
    };
    (void)memory_.write_bytes(LOG_PALETTE + 4, palette_entries, sizeof(palette_entries));
    Handle palette = m_mgr->create_palette(LOG_PALETTE);

    auto initialize_bmi = [&](GuestAddr bmi) {
        (void)memory_.write_u32(bmi, 40);
        (void)memory_.write_u32(bmi + 4, 1);
        (void)memory_.write_u32(bmi + 8, 1);
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 8);
        (void)memory_.write_u32(bmi + 16, 0);
        (void)memory_.write_u32(bmi + 32, 2);
        (void)memory_.write_u16(bmi + 40, 0);
        (void)memory_.write_u16(bmi + 42, 1);
    };
    initialize_bmi(SOURCE_BMI);
    initialize_bmi(DEST_BMI);

    Handle source_dc = m_mgr->create_compatible_dc(0);
    Handle intermediate_dc = m_mgr->create_compatible_dc(0);
    (void)m_mgr->select_palette(source_dc, palette, false);
    (void)m_mgr->select_palette(intermediate_dc, palette, false);
    Handle source_dib = m_mgr->create_dib_section(source_dc, SOURCE_BMI, 1, SOURCE_BITS_PTR);
    Handle intermediate_dib = m_mgr->create_dib_section(intermediate_dc, DEST_BMI, 1, DEST_BITS_PTR);
    (void)m_mgr->select_object(source_dc, source_dib);
    (void)m_mgr->select_object(intermediate_dc, intermediate_dib);
    const GuestAddr source_bits = memory_.read_u32(SOURCE_BITS_PTR).value();
    const GuestAddr intermediate_bits = memory_.read_u32(DEST_BITS_PTR).value();
    (void)memory_.write_u8(source_bits, 1);
    (void)memory_.write_u8(intermediate_bits, 0);

    ASSERT_TRUE(m_mgr->bit_blt(intermediate_dc, 0, 0, 1, 1, source_dc, 0, 0, 0x00CC0020));
    EXPECT_EQ(memory_.read_u8(intermediate_bits).value(), 1u);

    Handle final_dc = m_mgr->create_compatible_dc(0);
    Handle final_bitmap = m_mgr->create_compatible_bitmap(m_mgr->get_dc(0), 1, 1);
    (void)m_mgr->select_object(final_dc, final_bitmap);
    ASSERT_TRUE(m_mgr->bit_blt(final_dc, 0, 0, 1, 1, intermediate_dc, 0, 0, 0x00CC0020));
    EXPECT_EQ(m_mgr->get_pixel(final_dc, 0, 0), 0x00123456u);
}

// =============================================================================
// Ellipse Tests
// =============================================================================

TEST_F(GdiDIBTest, EllipseDrawsOnBitmap) {
    // Create a DC with a bitmap
    Handle hdc = m_mgr->get_dc(0);
    Handle bmp_handle = m_mgr->create_compatible_bitmap(hdc, 100, 100);
    Handle memDC = m_mgr->create_compatible_dc(hdc);
    m_mgr->select_object(memDC, bmp_handle);

    // Create a brush
    Handle hBrush = m_mgr->create_solid_brush(0x0000FF); // red
    m_mgr->select_object(memDC, hBrush);

    bool ok = m_mgr->ellipse(memDC, 10, 10, 90, 90);
    EXPECT_TRUE(ok);
}

TEST_F(GdiDIBTest, EllipseReturnsFalseForInvalidDC) {
    bool ok = m_mgr->ellipse(Handle{0xDEAD}, 0, 0, 100, 100);
    EXPECT_FALSE(ok);
}

// =============================================================================
// AlphaBlend (msimg32 entry point, implemented in GdiManager because it needs
// DC/bitmap internals). Previously a fake-success stub that returned TRUE
// without touching a pixel, which rendered Battle.net-Setup.exe's layered
// window permanently blank.
//
// The source is driven through a real guest-backed DIB section, which is how
// the app actually supplies pixels: it software-renders its skin into the DIB
// and AlphaBlend syncs those bytes in.
// =============================================================================

class GdiAlphaBlendTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE    = 0x1000;
    static constexpr GuestAddr DIB_SCRATCH = 0x5E100000; // GDI_DIB_SCRATCH_BASE
    static constexpr i32 W = 4;
    static constexpr i32 H = 4;

    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr = std::make_unique<GdiManager>(*m_handles);
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(DIB_SCRATCH,
                                 GdiManager::GDI_DIB_SCRATCH_SIZE,
                                 MemoryProtection::ReadWrite);
        m_mgr->set_guest_memory(&memory_);
    }

    void write_bmi(GuestAddr bmi) {
        (void)memory_.write_u32(bmi + 0,  40);
        (void)memory_.write_u32(bmi + 4,  static_cast<u32>(W));
        (void)memory_.write_u32(bmi + 8,  static_cast<u32>(H));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 32);
        (void)memory_.write_u32(bmi + 16, 0);
    }

    /// DIB-backed DC whose pixel bytes the test can write as the guest would.
    /// @param out_bits receives the guest address of the pixel buffer.
    Handle make_dib_dc(GuestAddr bmi, GuestAddr ppv, GuestAddr& out_bits) {
        write_bmi(bmi);
        Handle bmp = m_mgr->create_dib_section(0, bmi, 0, ppv);
        EXPECT_NE(bmp, Handle{0});
        out_bits = memory_.read_u32(ppv).value_or(0);
        EXPECT_NE(out_bits, GuestAddr{0});
        Handle dc = m_mgr->create_compatible_dc(0);
        EXPECT_NE(dc, Handle{0});
        (void)m_mgr->select_object(dc, bmp);
        return dc;
    }

    /// Plain memory DC used as a blend destination.
    Handle make_dst_dc(GuestAddr bmi) {
        write_bmi(bmi);
        Handle bmp = m_mgr->create_dib_section(0, bmi, 0, 0);
        EXPECT_NE(bmp, Handle{0});
        Handle dc = m_mgr->create_compatible_dc(0);
        (void)m_mgr->select_object(dc, bmp);
        return dc;
    }

    /// Fill a guest DIB with one BGRA value.
    void fill_guest(GuestAddr bits, u8 b, u8 g, u8 r, u8 a) {
        for (i32 i = 0; i < W * H; ++i) {
            (void)memory_.write_u8(bits + i * 4 + 0, b);
            (void)memory_.write_u8(bits + i * 4 + 1, g);
            (void)memory_.write_u8(bits + i * 4 + 2, r);
            (void)memory_.write_u8(bits + i * 4 + 3, a);
        }
    }

    void fill_dst(Handle dc, u32 colorref) {
        for (i32 y = 0; y < H; ++y)
            for (i32 x = 0; x < W; ++x)
                (void)m_mgr->set_pixel(dc, x, y, colorref);
    }

    static u32 rgb(u8 r, u8 g, u8 b) {
        return static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
               (static_cast<u32>(b) << 16);
    }

    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<GdiManager> m_mgr;
    GuestMemory memory_;
};

TEST_F(GdiAlphaBlendTest, OpaqueSourceReplacesDestination) {
    Handle dst = make_dst_dc(MEM_BASE);
    GuestAddr bits = 0;
    Handle src = make_dib_dc(MEM_BASE + 0x100, MEM_BASE + 0x200, bits);
    fill_dst(dst, rgb(0, 0, 0));
    fill_guest(bits, 255, 255, 255, 255);   // opaque white

    ASSERT_TRUE(m_mgr->alpha_blend(dst, 0, 0, W, H, src, 0, 0, W, H, 255, true));
    EXPECT_EQ(m_mgr->get_pixel(dst, 1, 1), rgb(255, 255, 255));
}

TEST_F(GdiAlphaBlendTest, FullyTransparentSourceLeavesDestination) {
    Handle dst = make_dst_dc(MEM_BASE);
    GuestAddr bits = 0;
    Handle src = make_dib_dc(MEM_BASE + 0x100, MEM_BASE + 0x200, bits);
    fill_dst(dst, rgb(30, 20, 10));
    fill_guest(bits, 0, 0, 0, 0);           // premultiplied, zero coverage

    ASSERT_TRUE(m_mgr->alpha_blend(dst, 0, 0, W, H, src, 0, 0, W, H, 255, true));
    EXPECT_EQ(m_mgr->get_pixel(dst, 2, 2), rgb(30, 20, 10));
}

TEST_F(GdiAlphaBlendTest, PremultipliedSourceIsAddedNotRescaled) {
    // Half-covered white, premultiplied: colour is already 128 with alpha 128.
    // Over black the result must be ~128, not 128*128/255 == 64. Rescaling a
    // premultiplied source is the classic double-multiply that darkens edges.
    Handle dst = make_dst_dc(MEM_BASE);
    GuestAddr bits = 0;
    Handle src = make_dib_dc(MEM_BASE + 0x100, MEM_BASE + 0x200, bits);
    fill_dst(dst, rgb(0, 0, 0));
    fill_guest(bits, 128, 128, 128, 128);

    ASSERT_TRUE(m_mgr->alpha_blend(dst, 0, 0, W, H, src, 0, 0, W, H, 255, true));
    const u32 got = m_mgr->get_pixel(dst, 0, 0);
    EXPECT_NEAR(static_cast<int>(got & 0xFF), 128, 1);
    EXPECT_NEAR(static_cast<int>((got >> 8) & 0xFF), 128, 1);
    EXPECT_NEAR(static_cast<int>((got >> 16) & 0xFF), 128, 1);
}

TEST_F(GdiAlphaBlendTest, ConstantAlphaWithoutPerPixelBlendsHalfway) {
    // No AC_SRC_ALPHA: the source colour is scaled by the constant alpha and
    // blended against the destination.
    Handle dst = make_dst_dc(MEM_BASE);
    GuestAddr bits = 0;
    Handle src = make_dib_dc(MEM_BASE + 0x100, MEM_BASE + 0x200, bits);
    fill_dst(dst, rgb(0, 0, 0));
    fill_guest(bits, 255, 255, 255, 255);

    ASSERT_TRUE(m_mgr->alpha_blend(dst, 0, 0, W, H, src, 0, 0, W, H, 128, false));
    const u32 got = m_mgr->get_pixel(dst, 3, 3);
    EXPECT_NEAR(static_cast<int>(got & 0xFF), 128, 2);
    EXPECT_NEAR(static_cast<int>((got >> 8) & 0xFF), 128, 2);
    EXPECT_NEAR(static_cast<int>((got >> 16) & 0xFF), 128, 2);
}

TEST_F(GdiAlphaBlendTest, RejectsUnknownDcAndDegenerateExtents) {
    Handle dst = make_dst_dc(MEM_BASE);
    GuestAddr bits = 0;
    Handle src = make_dib_dc(MEM_BASE + 0x100, MEM_BASE + 0x200, bits);
    // A DC that was never created must not report a successful blend.
    EXPECT_FALSE(m_mgr->alpha_blend(0xDEAD, 0, 0, W, H, src, 0, 0, W, H, 255, true));
    EXPECT_FALSE(m_mgr->alpha_blend(dst, 0, 0, W, H, 0xDEAD, 0, 0, W, H, 255, true));
    // Zero extents succeed and draw nothing on Windows 11 (s3gdi_misc_probe
    // "AB zero case0..3").
    EXPECT_TRUE(m_mgr->alpha_blend(dst, 0, 0, 0, H, src, 0, 0, W, H, 255, true));
    EXPECT_TRUE(m_mgr->alpha_blend(dst, 0, 0, W, H, src, 0, 0, W, 0, 255, true));
}

// =============================================================================
// DIB <-> host surface conversion
// =============================================================================
//
// Every blit that touches a DIB section converts the guest's pixel buffer into
// the host surface and back. These pin the conversion itself — byte order, row
// order, and indexed-colour matching — so the fast paths in the sync routines
// stay honest about what they produce.

class GdiDibConversionTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE = 0x1000;
    static constexpr GuestAddr DIB_SCRATCH = 0x5E100000; // GDI_DIB_SCRATCH_BASE
    static constexpr GuestAddr BMI = MEM_BASE;
    static constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;

    void SetUp() override {
        m_handles = std::make_unique<HandleTable>();
        m_mgr = std::make_unique<GdiManager>(*m_handles);
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(DIB_SCRATCH,
                                 GdiManager::GDI_DIB_SCRATCH_SIZE,
                                 MemoryProtection::ReadWrite);
        m_mgr->set_guest_memory(&memory_);
    }

    /// Create a DIB section of @p bpp and select it into a fresh memory DC.
    /// @p height is negative for a top-down DIB, matching the Win32 convention.
    struct Surface {
        Handle dc = 0;
        Handle bitmap = 0;
        GuestAddr bits = 0;
    };

    Surface make_surface(i32 width, i32 height, u16 bpp,
                         const std::vector<u32>& color_table = {}) {
        (void)memory_.write_u32(BMI + 0, 40);
        (void)memory_.write_u32(BMI + 4, static_cast<u32>(width));
        (void)memory_.write_u32(BMI + 8, static_cast<u32>(height));
        (void)memory_.write_u16(BMI + 12, 1);
        (void)memory_.write_u16(BMI + 14, bpp);
        (void)memory_.write_u32(BMI + 16, 0); // BI_RGB
        (void)memory_.write_u32(BMI + 32,
                                static_cast<u32>(color_table.size())); // biClrUsed
        for (size_t i = 0; i < color_table.size(); ++i) {
            // RGBQUAD: blue, green, red, reserved — packed here as 0x00RRGGBB.
            const u32 rgb = color_table[i];
            (void)memory_.write_u32(
                BMI + 40 + static_cast<GuestAddr>(i) * 4,
                ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu));
        }

        Surface s;
        s.bitmap = m_mgr->create_dib_section(0, BMI, 0, PPVBITS);
        EXPECT_NE(s.bitmap, Handle{0});
        s.bits = memory_.read_u32(PPVBITS).value_or(0);
        s.dc = m_mgr->create_compatible_dc(0);
        (void)m_mgr->select_object(s.dc, s.bitmap);
        return s;
    }

    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<GdiManager> m_mgr;
    GuestMemory memory_;
};

TEST_F(GdiDibConversionTest, Bgra32RoundTripsThroughABlitUnchanged) {
    // Top-down 32bpp: the exact bytes the guest wrote must come back out.
    constexpr i32 W = 4, H = 2;
    Surface src = make_surface(W, -H, 32);
    Surface dst = make_surface(W, -H, 32);
    ASSERT_NE(src.bits, 0u);
    ASSERT_NE(dst.bits, 0u);

    const u32 pixels[W * H] = {
        0x00112233u, 0x44556677u, 0x8899AABBu, 0xCCDDEEFFu,
        0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu,
    };
    for (u32 i = 0; i < W * H; ++i) {
        (void)memory_.write_u32(src.bits + i * 4, pixels[i]);
    }

    ASSERT_TRUE(m_mgr->bit_blt(dst.dc, 0, 0, W, H, src.dc, 0, 0, 0x00CC0020));

    // SRCCOPY between 32bpp memory bitmaps copies all 32 bits, the reserved
    // byte included (s3gdi_blit_probe "BitBlt SRCCOPY": 40112233 lands as
    // 40112233).
    for (u32 i = 0; i < W * H; ++i) {
        const u32 got = memory_.read_u32(dst.bits + i * 4).value_or(0);
        EXPECT_EQ(got, pixels[i]) << "32bpp pixel " << i << " changed crossing the host surface";
    }
}

TEST_F(GdiDibConversionTest, BottomUpDibKeepsItsRowOrderAcrossABlit) {
    // A positive height means row 0 in memory is the bottom scanline. Reading
    // and writing must apply that flip symmetrically, or the image inverts.
    constexpr i32 W = 2, H = 2;
    Surface src = make_surface(W, H, 32);
    Surface dst = make_surface(W, H, 32);

    const u32 pixels[W * H] = {
        0x00AAAAAAu, 0x00BBBBBBu,   // memory row 0 = bottom scanline
        0x00CCCCCCu, 0x00DDDDDDu,   // memory row 1 = top scanline
    };
    for (u32 i = 0; i < W * H; ++i) {
        (void)memory_.write_u32(src.bits + i * 4, pixels[i]);
    }

    ASSERT_TRUE(m_mgr->bit_blt(dst.dc, 0, 0, W, H, src.dc, 0, 0, 0x00CC0020));

    for (u32 i = 0; i < W * H; ++i) {
        EXPECT_EQ(memory_.read_u32(dst.bits + i * 4).value_or(0) & 0x00FFFFFFu,
                  pixels[i] & 0x00FFFFFFu)
            << "bottom-up row order was not preserved at pixel " << i;
    }
}

TEST_F(GdiDibConversionTest, Rgb24BlitPreservesColourAndIgnoresPadding) {
    constexpr i32 W = 2, H = 1;
    Surface src = make_surface(W, -H, 24);
    Surface dst = make_surface(W, -H, 24);

    // BGR byte order in the DIB.
    const u8 bytes[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    for (u32 i = 0; i < 6; ++i) {
        (void)memory_.write_u8(src.bits + i, bytes[i]);
    }

    ASSERT_TRUE(m_mgr->bit_blt(dst.dc, 0, 0, W, H, src.dc, 0, 0, 0x00CC0020));

    for (u32 i = 0; i < 6; ++i) {
        EXPECT_EQ(memory_.read_u8(dst.bits + i).value_or(0), bytes[i])
            << "24bpp byte " << i << " changed crossing the host surface";
    }
}

TEST_F(GdiDibConversionTest, IndexedDibMatchesNearestPaletteColour) {
    // 8bpp both ways: the read resolves indices through the colour table, and
    // the write picks the nearest entry back. Exact palette colours must map
    // back to their own index.
    constexpr i32 W = 4, H = 1;
    const std::vector<u32> table = {0x00000000u, 0x00FF0000u,
                                    0x0000FF00u, 0x000000FFu};
    Surface src = make_surface(W, -H, 8, table);
    Surface dst = make_surface(W, -H, 8, table);

    const u8 indices[W] = {0, 1, 2, 3};
    for (u32 i = 0; i < W; ++i) {
        (void)memory_.write_u8(src.bits + i, indices[i]);
    }

    ASSERT_TRUE(m_mgr->bit_blt(dst.dc, 0, 0, W, H, src.dc, 0, 0, 0x00CC0020));

    for (u32 i = 0; i < W; ++i) {
        EXPECT_EQ(memory_.read_u8(dst.bits + i).value_or(0xFF), indices[i])
            << "8bpp index " << i << " did not survive the palette round trip";
    }
}

TEST_F(GdiDibConversionTest, RepeatedColoursResolveToTheSameIndex) {
    // The nearest-colour search is memoised; a repeated colour must still
    // produce the index the search would have returned.
    constexpr i32 W = 8, H = 1;
    const std::vector<u32> table = {0x00000000u, 0x00FF0000u};
    Surface src = make_surface(W, -H, 8, table);
    Surface dst = make_surface(W, -H, 8, table);

    const u8 indices[W] = {1, 1, 0, 1, 0, 0, 1, 0};
    for (u32 i = 0; i < W; ++i) {
        (void)memory_.write_u8(src.bits + i, indices[i]);
    }

    ASSERT_TRUE(m_mgr->bit_blt(dst.dc, 0, 0, W, H, src.dc, 0, 0, 0x00CC0020));

    for (u32 i = 0; i < W; ++i) {
        EXPECT_EQ(memory_.read_u8(dst.bits + i).value_or(0xFF), indices[i])
            << "memoised palette match diverged at pixel " << i;
    }
}

TEST_F(GdiDIBTest, GetDIBColorTableCopiesIndexedRgbQuads) {
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;
    constexpr GuestAddr OUT = MEM_BASE + 0x200;

    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 1);
    (void)memory_.write_u32(BMI + 8, 1);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 8);
    (void)memory_.write_u32(BMI + 16, 0);
    (void)memory_.write_u32(BMI + 32, 2); // biClrUsed
    const u8 rgbquads[] = {
        0x11, 0x22, 0x33, 0x00,
        0x44, 0x55, 0x66, 0x00,
    };
    (void)memory_.write_bytes(BMI + 40, rgbquads, sizeof(rgbquads));

    Handle hdc = m_mgr->create_compatible_dc(0);
    Handle dib = m_mgr->create_dib_section(hdc, BMI, 0, PPVBITS);
    ASSERT_NE(dib, Handle{0});
    (void)m_mgr->select_object(hdc, dib);

    EXPECT_EQ(m_mgr->get_dib_color_table(hdc, 0, 2, OUT), 2u);
    EXPECT_EQ(memory_.read_u8(OUT + 0).value_or(0), 0x11);
    EXPECT_EQ(memory_.read_u8(OUT + 1).value_or(0), 0x22);
    EXPECT_EQ(memory_.read_u8(OUT + 2).value_or(0), 0x33);
    EXPECT_EQ(memory_.read_u8(OUT + 4).value_or(0), 0x44);
    EXPECT_EQ(m_mgr->get_dib_color_table(hdc, 1, 1, OUT), 1u);
    EXPECT_EQ(memory_.read_u8(OUT + 0).value_or(0), 0x44);
}

TEST_F(GdiDIBTest, GetDIBColorTableFailsForTrueColorDib) {
    constexpr GuestAddr BMI = MEM_BASE;
    constexpr GuestAddr PPVBITS = MEM_BASE + 0x100;
    constexpr GuestAddr OUT = MEM_BASE + 0x200;

    (void)memory_.write_u32(BMI + 0, 40);
    (void)memory_.write_u32(BMI + 4, 4);
    (void)memory_.write_u32(BMI + 8, 4);
    (void)memory_.write_u16(BMI + 12, 1);
    (void)memory_.write_u16(BMI + 14, 32);
    (void)memory_.write_u32(BMI + 16, 0);

    Handle hdc = m_mgr->create_compatible_dc(0);
    Handle dib = m_mgr->create_dib_section(hdc, BMI, 0, PPVBITS);
    ASSERT_NE(dib, Handle{0});
    (void)m_mgr->select_object(hdc, dib);
    EXPECT_EQ(m_mgr->get_dib_color_table(hdc, 0, 1, OUT), 0u);
}

// Independent HGDIOBJs may be used on different Windows threads. Sharing or
// deleting the same object concurrently is deliberately outside this test.
// Learn: Multiple Threads and GDI Objects, GetObjectType, SaveDC, RestoreDC,
// DeleteObject. Constants below were generated from the MinGW SDK wingdi.h.
TEST_F(GdiTest, IndependentObjectsSurviveConcurrentGuestExportsBothBitnesses) {
    constexpr u32 sdk_OBJ_BITMAP = 7;
    constexpr u32 sdk_OBJ_BRUSH = 2;
    constexpr u32 sdk_OBJ_PEN = 1;
    constexpr u32 sdk_OBJ_FONT = 6;
    constexpr u32 sdk_OBJ_PAL = 5;
    constexpr u32 sdk_OBJ_REGION = 8;
    constexpr u32 sdk_OBJ_MEMDC = 10;
    constexpr u32 sdk_PS_SOLID = 0;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x80000, MemoryProtection::ReadWrite).is_ok());
    m_mgr->set_guest_memory(&memory);
    const auto exports = make_gdi32_exports(*m_mgr);
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        std::atomic<unsigned> failures{0};
        std::barrier start(8);
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < 8; ++worker) {
            workers.emplace_back([&, worker] {
                cpu::CpuState cpu;
                if (!is64) cpu.init_protected_mode();
                const GuestAddr stack = 0x10000 + worker * 0x10000;
                cpu.set_sp(stack);
                Marshaler marshaler(memory);
                marshaler.attach_cpu(cpu);
                auto call = [&](const char* name, std::initializer_list<u64> arguments) -> u64 {
                    const std::vector<u64> args(arguments);
                    if (is64) {
                        cpu.gpr.rcx = args.size() > 0 ? args[0] : 0;
                        cpu.gpr.rdx = args.size() > 1 ? args[1] : 0;
                        cpu.gpr.r8 = args.size() > 2 ? args[2] : 0;
                        cpu.gpr.r9 = args.size() > 3 ? args[3] : 0;
                        for (usize i = 4; i < args.size(); ++i)
                            (void)memory.write_u64(stack + 0x28 + (i - 4) * 8, args[i]);
                    } else {
                        for (usize i = 0; i < args.size(); ++i)
                            (void)memory.write_u32(stack + 4 + i * 4, static_cast<u32>(args[i]));
                    }
                    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                        [&](const auto& value) { return value.name == name; });
                    if (entry == exports.entries.end()) { ++failures; return 0; }
                    entry->handler(marshaler);
                    return is64 ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
                };
                auto check = [&](bool ok) { if (!ok) ++failures; };
                start.arrive_and_wait();
                for (unsigned round = 0; round < 256; ++round) {
                    const Handle dc = static_cast<Handle>(call("CreateCompatibleDC", {0}));
                    const Handle bitmap = static_cast<Handle>(call("CreateBitmap", {4, 4, 1, 32, 0}));
                    const Handle brush = static_cast<Handle>(call("CreateSolidBrush", {worker * 256 + round}));
                    const Handle pen = static_cast<Handle>(call("CreatePen", {sdk_PS_SOLID, 1, round}));
                    const Handle region = static_cast<Handle>(call("CreateRectRgn", {1, 2, 4, 5}));
                    const Handle palette = static_cast<Handle>(call("CreateHalftonePalette", {dc}));
                    // Direct host callers use the same owner as export thunks.
                    const Handle font = m_mgr->create_font_w(-12, 400, false, u"Arial");
                    const std::array<std::pair<Handle, u32>, 7> objects{{
                        {dc, sdk_OBJ_MEMDC}, {bitmap, sdk_OBJ_BITMAP},
                        {brush, sdk_OBJ_BRUSH}, {pen, sdk_OBJ_PEN},
                        {region, sdk_OBJ_REGION}, {palette, sdk_OBJ_PAL}, {font, sdk_OBJ_FONT}}};
                    for (const auto& [object, type] : objects) {
                        check(object != 0);
                        check(call("GetObjectType", {object}) == type);
                    }
                    check(call("SelectObject", {dc, bitmap}) != 0);
                    check(call("SelectObject", {dc, brush}) != 0);
                    check(call("SelectObject", {dc, pen}) != 0);
                    check(call("SaveDC", {dc}) != 0);
                    check(call("RestoreDC", {dc, static_cast<u32>(-1)}) != 0);
                    check(call("DeleteDC", {dc}) != 0);
                    for (usize i = 1; i < objects.size(); ++i) {
                        check(call("DeleteObject", {objects[i].first}) != 0);
                        check(call("GetObjectType", {objects[i].first}) == 0);
                    }
                    check(call("GetObjectType", {dc}) == 0);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        EXPECT_EQ(failures.load(), 0u);
    }
}

TEST_F(GdiTest, SystemBrushCallbackReleasesAllNestedOwnerLocks) {
    using namespace std::chrono_literals;
    const Handle system_brush = m_handles->allocate(kHandleTypeGdiObject, nullptr);
    std::vector<std::future<bool>> tasks;
    bool callback_completed = false;
    m_mgr->set_system_brush_resolver([&](u64 handle) -> std::optional<u32> {
        if (handle != system_brush) return std::nullopt;
        tasks.push_back(std::async(std::launch::async, [&] {
            const Handle dc = m_mgr->create_compatible_dc(0);
            const bool exists = dc != 0 && m_mgr->has_dc(dc);
            return m_mgr->delete_dc(dc) && exists;
        }));
        callback_completed = tasks.back().wait_for(500ms) == std::future_status::ready;
        return 0x123456;
    });
    {
        GdiManager::ApiLock outer(*m_mgr);
        GdiManager::ApiLock nested(*m_mgr);
        u32 color = 0;
        EXPECT_TRUE(m_mgr->lookup_brush_color(system_brush, &color));
        EXPECT_EQ(color, 0x123456u);
    }
    EXPECT_TRUE(callback_completed);
    // Join after the owner API returned, so a regression reports a timeout
    // rather than hanging in an async future's destructor under the lock.
    for (auto& task : tasks) EXPECT_TRUE(task.get());
    (void)m_handles->close(system_brush);
}

TEST_F(GdiTest, CallbackSuspensionDoesNotReleaseAnotherManagersLock) {
    using namespace std::chrono_literals;
    GdiManager other(*m_handles);
    std::future<bool> own_task, other_task;
    bool own_completed = false, other_blocked = false;
    const Handle system_brush = m_handles->allocate(kHandleTypeGdiObject, nullptr);
    m_mgr->set_system_brush_resolver([&](u64 handle) -> std::optional<u32> {
        if (handle != system_brush) return std::nullopt;
        own_task = std::async(std::launch::async, [&] { return m_mgr->has_dc(0); });
        other_task = std::async(std::launch::async, [&] { return other.has_dc(0); });
        own_completed = own_task.wait_for(500ms) == std::future_status::ready;
        other_blocked = other_task.wait_for(20ms) == std::future_status::timeout;
        return 0;
    });
    {
        GdiManager::ApiLock other_lock(other);
        GdiManager::ApiLock own_lock(*m_mgr);
        u32 color = 1;
        EXPECT_TRUE(m_mgr->lookup_brush_color(system_brush, &color));
        EXPECT_EQ(color, 0u);
    }
    EXPECT_TRUE(own_completed);
    EXPECT_TRUE(other_blocked);
    EXPECT_FALSE(own_task.get());
    EXPECT_FALSE(other_task.get());
    (void)m_handles->close(system_brush);
}

TEST_F(GdiTest, MetafileEnumerationReleasesNestedLocksBothBitnesses) {
    using namespace std::chrono_literals;
    // Generated from gdi_enum_callback_probe.expected, captured on Windows
    // in both bitnesses. A native empty EMF has HEADER and EOF records.
    const std::array<u8, 128> fixture{
        0x01, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xe6, 0xff, 0xff, 0xff, 0xe6, 0xff, 0xff, 0xff, 0x20, 0x45, 0x4d, 0x46, 0x00, 0x00, 0x01, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x0f, 0x01, 0x00, 0x00, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x55, 0x22, 0x04, 0x00, 0xc0, 0x19, 0x03, 0x00, 0x0e, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
    };
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(GdiManager::GDI_DIB_SCRATCH_BASE,
        GdiManager::GDI_DIB_SCRATCH_SIZE, MemoryProtection::ReadWrite).is_ok());
    m_mgr->set_guest_memory(&memory);
    const Handle emf = m_mgr->set_enh_metafile_bits(fixture);
    ASSERT_NE(emf, 0u);
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        std::vector<std::future<bool>> tasks;
        unsigned calls = 0;
        m_mgr->set_guest_call([&](GuestAddr proc, const u64* args, usize n, bool callback64) -> u64 {
            EXPECT_EQ(proc, 0x1000u);
            EXPECT_EQ(n, 5u);
            EXPECT_EQ(callback64, is64);
            EXPECT_EQ(args[0], 0u);
            EXPECT_EQ(args[3], 1u);
            EXPECT_EQ(args[4], 0x1234u);
            EXPECT_EQ(memory.read_u32(args[2]).value(), calls == 0 ? 1u : 14u);
            EXPECT_EQ(memory.read_u32(args[2] + 4).value(), calls == 0 ? 108u : 20u);
            ++calls;
            tasks.push_back(std::async(std::launch::async, [&] {
                const Handle brush = m_mgr->create_solid_brush(0x030201);
                u32 color = 0;
                const bool matches = m_mgr->lookup_brush_color(brush, &color) && color == 0x030201;
                return m_mgr->delete_object(brush) && matches;
            }));
            EXPECT_EQ(tasks.back().wait_for(500ms), std::future_status::ready);
            return 1;
        });
        {
            GdiManager::ApiLock outer(*m_mgr);
            EXPECT_EQ(m_mgr->enum_enh_metafile(0, emf, 0x1000, 0x1234, is64), 1);
        }
        EXPECT_EQ(calls, 2u);
        for (auto& task : tasks) EXPECT_TRUE(task.get());
    }
    EXPECT_TRUE(m_mgr->delete_enh_metafile(emf));
}

TEST_F(GdiTest, LoadedBitmapPreservesDepthForCursorMaskSelection) {
    windowing::CocoaBridge bridge;
    m_mgr->set_cocoa_bridge(&bridge);
    // BITMAPFILEHEADER + BITMAPINFOHEADER, 1x1 24bpp blue pixel, padded row.
    std::vector<u8> bmp(58, 0);
    auto word = [&](size_t at, u32 v, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) bmp[at + i] = static_cast<u8>(v >> (8 * i));
    };
    bmp[0] = 'B'; bmp[1] = 'M';
    word(2, 58, 4); word(10, 54, 4); word(14, 40, 4);
    word(18, 1, 4); word(22, 1, 4); word(26, 1, 2); word(28, 24, 2);
    word(34, 4, 4); bmp[54] = 255;
    const auto handle = bridge.register_bitmap_bmp(bmp);
    ASSERT_NE(handle, 0u);
    GdiManager::BitmapRgbaCopy copy;
    ASSERT_TRUE(m_mgr->copy_bitmap_rgba(handle, copy));
    EXPECT_EQ(copy.bpp, 24u);
    EXPECT_EQ(copy.width, 1);
    EXPECT_EQ(copy.height, 1);
    EXPECT_EQ(copy.rgba[2], 255u);
}

TEST_F(GdiTest, WindowStorageResizePreservesValidClientPixels) {
    const auto dc = m_mgr->get_dc(123);
    ASSERT_NE(dc, 0u);
    m_mgr->set_pixel(dc, 1, 1, 0x00332211);
    m_mgr->set_pixel(dc, 2, 2, 0x00665544);
    m_mgr->ensure_framebuffer_size(123, 400, 350);
    EXPECT_EQ(m_mgr->get_pixel(dc, 1, 1), 0x00332211u);
    EXPECT_EQ(m_mgr->get_pixel(dc, 2, 2), 0x00665544u);
    EXPECT_EQ(m_mgr->get_pixel(dc, 399, 349), 0u);
    m_mgr->ensure_framebuffer_size(123, 3, 3);
    EXPECT_EQ(m_mgr->get_pixel(dc, 1, 1), 0x00332211u);
    EXPECT_EQ(m_mgr->get_pixel(dc, 2, 2), 0x00665544u);
}

TEST_F(GdiDIBTest, ComposeLayeredWindowReplacesSurfaceWithBlendFunctionAlpha) {
    // Learn UpdateLayeredWindow/BLENDFUNCTION: the window content is replaced
    // by hdcSrc. AC_SRC_ALPHA: premultiplied colour and alpha both scale by
    // SourceConstantAlpha. Constant only: colour scales, opacity = constant.
    // ULW_OPAQUE: colour as is, opaque. Uncovered window pixels are holes.
    const Handle screen = m_mgr->get_dc(0);
    const Handle win = m_mgr->create_compatible_dc(0);
    const Handle src = m_mgr->create_compatible_dc(0);
    (void)m_mgr->select_object(win, m_mgr->create_compatible_bitmap(screen, 3, 1));
    (void)m_mgr->select_object(src, m_mgr->create_compatible_bitmap(screen, 2, 1));
    const auto fill = [&](Handle dc, std::initializer_list<u8> bytes) {
        ASSERT_TRUE(m_mgr->with_device_surface(dc, [&](GdiManager::DeviceSurface& s) {
            std::copy(bytes.begin(), bytes.end(), s.rgba);
        }));
    };
    const auto read = [&]() {
        std::array<u8, 12> out{};
        EXPECT_TRUE(m_mgr->read_device_surface(win, [&](const GdiManager::ReadOnlyDeviceSurface& s) {
            std::copy_n(s.rgba, out.size(), out.begin());
        }));
        return out;
    };
    // Source: half-covered premultiplied red and quarter-covered green.
    fill(src, {128, 0, 0, 128, 0, 64, 0, 64});
    // The previous window content (opaque white) must not survive anywhere.
    fill(win, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255});
    ASSERT_TRUE(m_mgr->compose_layered_window(win, src, 0, 0, 3, 1, 255, true, false));
    EXPECT_EQ(read(), (std::array<u8, 12>{128, 0, 0, 128, 0, 64, 0, 64, 0, 0, 0, 0}));
    ASSERT_TRUE(m_mgr->compose_layered_window(win, src, 0, 0, 2, 1, 128, true, false));
    EXPECT_EQ(read(), (std::array<u8, 12>{64, 0, 0, 64, 0, 32, 0, 32, 0, 0, 0, 0}));
    ASSERT_TRUE(m_mgr->compose_layered_window(win, src, 0, 0, 2, 1, 128, false, false));
    EXPECT_EQ(read(), (std::array<u8, 12>{64, 0, 0, 128, 0, 32, 0, 128, 0, 0, 0, 0}));
    ASSERT_TRUE(m_mgr->compose_layered_window(win, src, 0, 0, 2, 1, 0, false, true));
    EXPECT_EQ(read(), (std::array<u8, 12>{128, 0, 0, 255, 0, 64, 0, 255, 0, 0, 0, 0}));
    // pptSrc offsets into the source.
    ASSERT_TRUE(m_mgr->compose_layered_window(win, src, 1, 0, 2, 1, 255, true, false));
    EXPECT_EQ(read(), (std::array<u8, 12>{0, 64, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0}));
}
