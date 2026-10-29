/// @file test_gdi32_cpuz.cpp
/// @brief Unit tests for CPU-Z GDI32 API stubs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class Gdi32CpuzTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        gdi.set_guest_memory(&memory);

        // Obtain an HDC for testing DC-related operations
        hdc = gdi.get_dc(0);
        ASSERT_NE(hdc, Handle{0});

        // Build exports for testing lambda-based stubs
        exports = make_gdi32_exports(
            gdi, [] { return std::string{"Test Apple GPU"}; });
    }

    /// Find an export entry by name and invoke it with current marshaler state.
    void call_export(const std::string& name) {
        for (auto& e : exports.entries) {
            if (e.name == name) {
                e.handler(marshaler);
                return;
            }
        }
        FAIL() << "Export not found: " << name;
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HandleTable handles;
    GdiManager gdi{handles};
    DllExports exports;
    Handle hdc{0};
};

TEST_F(Gdi32CpuzTest, D3DKMTOpenAdapterReturnsMetalBackedIdentity) {
    constexpr GuestAddr open = DATA_BASE + 0x500;
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, open).is_ok());

    call_export("D3DKMTOpenAdapterFromGdiDisplayName");

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(open + 64).value(), 0x4D544C01u);
    EXPECT_EQ(memory.read_u32(open + 68).value(), 0x5654524Du);
    EXPECT_EQ(memory.read_u32(open + 72).value(), 1u);
    EXPECT_EQ(memory.read_u32(open + 76).value(), 0u);
}

TEST_F(Gdi32CpuzTest, TranslateCharsetInfoFailsWithoutInventingSignature) {
    cpu.init_long_mode();
    cpu.gpr.rcx = 1252; // TCI_SRCCODEPAGE takes a value, not a pointer.
    cpu.gpr.rdx = DATA_BASE;
    cpu.gpr.r8 = 2;
    ASSERT_TRUE(memory.write_u64(DATA_BASE, UINT64_MAX).is_ok());
    call_export("TranslateCharsetInfo");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_EQ(memory.read_u64(DATA_BASE).value(), UINT64_MAX);
    cpu.gpr.r8 = 0;
    call_export("TranslateCharsetInfo");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(Gdi32CpuzTest, GetNearestColorPreservesRgbOnTrueColorDevice) {
    cpu.init_long_mode();
    cpu.gpr.rcx = hdc;
    cpu.gpr.rdx = 0x0055AA77;
    call_export("GetNearestColor");
    EXPECT_EQ(cpu.gpr.rax, 0x0055AA77u);
    cpu.gpr.rcx = 0xDEADBEEF;
    call_export("GetNearestColor");
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFFFu);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(Gdi32CpuzTest, D3DKMTQueryAdapterInfoReportsVirtualRootAddress) {
    constexpr GuestAddr query = DATA_BASE + 0x500;
    constexpr GuestAddr address = DATA_BASE + 0x600;
    ASSERT_TRUE(memory.write_u32(query, 0x4D544C01u).is_ok());
    ASSERT_TRUE(memory.write_u32(query + 4, 6u).is_ok());
    ASSERT_TRUE(memory.write_u32(query + 8, address).is_ok());
    ASSERT_TRUE(memory.write_u32(query + 12, 12u).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, query).is_ok());

    call_export("D3DKMTQueryAdapterInfo");

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(address).value(), 0u);
    EXPECT_EQ(memory.read_u32(address + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(address + 8).value(), 0u);
}

TEST_F(Gdi32CpuzTest, MemoryMetaFileLifecycleOwnsBytesAndInvalidatesHandle) {
    constexpr GuestAddr metafile_data = DATA_BASE + 0x800;
    constexpr std::array<u8, 8> bytes{
        0x01, 0x00, 0x09, 0x00, 0x00, 0x03, 0x10, 0x00};
    ASSERT_TRUE(memory.write_bytes(
        metafile_data, reinterpret_cast<const Byte*>(bytes.data()),
        bytes.size()).is_ok());

    (void)memory.write_u32(
        static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(bytes.size()));
    (void)memory.write_u32(
        static_cast<u32>(cpu.gpr.rsp + 8),
        static_cast<u32>(metafile_data));
    call_export("SetMetaFileBitsEx");
    const Handle hmf = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(hmf, Handle{0});

    (void)memory.write_u32(
        static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(
        static_cast<u32>(cpu.gpr.rsp + 8), hmf);

    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "PlayMetaFile";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), hmf);
    call_export("DeleteMetaFile");
    EXPECT_EQ(cpu.gpr.rax, 1u);

    call_export("DeleteMetaFile");
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// =============================================================================
// SetViewportOrgEx
// =============================================================================

TEST_F(Gdi32CpuzTest, SetViewportOrgEx_ReturnsTRUE_WritesPreviousOrigin) {
    constexpr GuestAddr pt_addr = DATA_BASE + 0x100;
    // Pre-zero the output POINT
    (void)memory.write_u32(pt_addr, 0xDEAD);
    (void)memory.write_u32(pt_addr + 4, 0xDEAD);

    // SetViewportOrgEx(hdc, x=10, y=20, lppt)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  10);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 20);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), pt_addr);
    call_export("SetViewportOrgEx");

    EXPECT_EQ(cpu.gpr.rax, 1u);  // TRUE

    // Previous origin should be (0,0) for a fresh DC
    auto ox = memory.read_u32(pt_addr);
    auto oy = memory.read_u32(pt_addr + 4);
    ASSERT_TRUE(ox.is_ok() && oy.is_ok());
    EXPECT_EQ(ox.value(), 0u);
    EXPECT_EQ(oy.value(), 0u);
}

// =============================================================================
// SetMapMode
// =============================================================================

TEST_F(Gdi32CpuzTest, SetMapMode_ReturnsPreviousMode) {
    // SetMapMode(hdc, fnMapMode=2 [MM_LOMETRIC])
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 2);
    call_export("SetMapMode");

    // Previous mode should be 1 (MM_TEXT) for a fresh DC
    EXPECT_EQ(cpu.gpr.rax, 1u);

    // Setting again should return 2
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 7);  // MM_ISOTROPIC
    call_export("SetMapMode");
    EXPECT_EQ(cpu.gpr.rax, 2u);
}

// =============================================================================
// SetGraphicsMode / GetGraphicsMode
// =============================================================================

TEST_F(Gdi32CpuzTest, SetGraphicsMode_IsStdcall8AndTracksMode) {
    // Real export must be stdcall 8 — missing stub with 0 cleanup corrupts ANGLE.
    bool found = false;
    for (const auto& e : exports.entries) {
        if (e.name == "SetGraphicsMode") {
            EXPECT_EQ(e.stdcall_bytes, 8u);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    // Default is GM_COMPATIBLE=1
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    call_export("GetGraphicsMode");
    EXPECT_EQ(cpu.gpr.rax, 1u);

    // SetGraphicsMode(hdc, GM_ADVANCED=2) -> previous 1
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 2);
    call_export("SetGraphicsMode");
    EXPECT_EQ(cpu.gpr.rax, 1u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    call_export("GetGraphicsMode");
    EXPECT_EQ(cpu.gpr.rax, 2u);

    // Invalid mode fails with 0
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 3);
    call_export("SetGraphicsMode");
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Gdi32CpuzTest, SetWorldTransform_IsStdcall8RequiresAdvancedMode) {
    for (const char* name : {"SetWorldTransform", "GetWorldTransform"}) {
        bool found = false;
        for (const auto& e : exports.entries) {
            if (e.name == name) {
                EXPECT_EQ(e.stdcall_bytes, 8u) << name;
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found) << name;
    }
    bool modify_found = false;
    for (const auto& e : exports.entries) {
        if (e.name == "ModifyWorldTransform") {
            EXPECT_EQ(e.stdcall_bytes, 12u);
            modify_found = true;
            break;
        }
    }
    ASSERT_TRUE(modify_found);

    constexpr GuestAddr xform = DATA_BASE + 0x200;
    // Identity-ish scale: eM11=2, eM22=2
    float vals[6] = {2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    ASSERT_TRUE(memory.write_bytes(xform, reinterpret_cast<const Byte*>(vals), sizeof(vals)).is_ok());

    // GM_COMPATIBLE rejects SetWorldTransform
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), static_cast<u32>(xform));
    call_export("SetWorldTransform");
    EXPECT_EQ(cpu.gpr.rax, 0u);

    // Switch to GM_ADVANCED
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 2);
    call_export("SetGraphicsMode");
    EXPECT_EQ(cpu.gpr.rax, 1u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), static_cast<u32>(xform));
    call_export("SetWorldTransform");
    EXPECT_EQ(cpu.gpr.rax, 1u);

    float out[6] = {};
    constexpr GuestAddr out_xform = DATA_BASE + 0x280;
    ASSERT_TRUE(memory.write_bytes(out_xform, reinterpret_cast<const Byte*>(out), sizeof(out)).is_ok());
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), static_cast<u32>(out_xform));
    call_export("GetWorldTransform");
    EXPECT_EQ(cpu.gpr.rax, 1u);
    ASSERT_TRUE(memory.read_bytes(out_xform, reinterpret_cast<Byte*>(out), sizeof(out)).is_ok());
    EXPECT_FLOAT_EQ(out[0], 2.0f);
    EXPECT_FLOAT_EQ(out[3], 2.0f);

    // MWT_IDENTITY resets
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 1); // MWT_IDENTITY
    call_export("ModifyWorldTransform");
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

// =============================================================================
// SetTextAlign
// =============================================================================

TEST_F(Gdi32CpuzTest, SetTextAlign_ReturnsPreviousAlign) {
    // SetTextAlign(hdc, fMode=0x18 [TA_CENTER | TA_BASELINE])
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0x18);
    call_export("SetTextAlign");

    // Previous align should be 0 for a fresh DC
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// =============================================================================
// CreatePatternBrush
// =============================================================================

TEST_F(Gdi32CpuzTest, CreatePatternBrushCopiesAndTilesBitmap) {
    // CreateCompatibleBitmap needs a real DC; the screen DC gives the 32bpp
    // display format (s3gdi_bitmap_probe "ccb NULL dc", "ccb screen").
    const Handle screen = gdi.get_dc(0);
    const Handle source_dc = gdi.create_compatible_dc(0);
    const Handle source_bitmap = gdi.create_compatible_bitmap(screen, 2, 2);
    ASSERT_NE(source_dc, Handle{0});
    ASSERT_NE(source_bitmap, Handle{0});
    (void)gdi.select_object(source_dc, source_bitmap);
    ASSERT_NE(gdi.set_pixel(source_dc, 0, 0, 0x000000FF), 0xFFFFFFFFu);
    ASSERT_NE(gdi.set_pixel(source_dc, 1, 0, 0x0000FF00), 0xFFFFFFFFu);
    ASSERT_NE(gdi.set_pixel(source_dc, 0, 1, 0x00FF0000), 0xFFFFFFFFu);
    ASSERT_NE(gdi.set_pixel(source_dc, 1, 1, 0x0000FFFF), 0xFFFFFFFFu);

    (void)memory.write_u32(
        static_cast<u32>(cpu.gpr.rsp + 4), source_bitmap);
    call_export("CreatePatternBrush");
    const Handle brush = static_cast<Handle>(cpu.gpr.rax);
    ASSERT_NE(brush, Handle{0});
    ASSERT_TRUE(gdi.delete_object(source_bitmap));

    const Handle destination_dc = gdi.create_compatible_dc(0);
    const Handle destination_bitmap = gdi.create_compatible_bitmap(screen, 4, 4);
    ASSERT_NE(destination_dc, Handle{0});
    ASSERT_NE(destination_bitmap, Handle{0});
    (void)gdi.select_object(destination_dc, destination_bitmap);
    ASSERT_TRUE(gdi.fill_rect_with_brush(
        destination_dc, 0, 0, 4, 4, brush));

    EXPECT_EQ(gdi.get_pixel(destination_dc, 0, 0), 0x000000FFu);
    EXPECT_EQ(gdi.get_pixel(destination_dc, 1, 0), 0x0000FF00u);
    EXPECT_EQ(gdi.get_pixel(destination_dc, 2, 0), 0x000000FFu);
    EXPECT_EQ(gdi.get_pixel(destination_dc, 0, 1), 0x00FF0000u);
    EXPECT_EQ(gdi.get_pixel(destination_dc, 3, 3), 0x0000FFFFu);
}

TEST_F(Gdi32CpuzTest, CreatePatternBrushRejectsInvalidBitmap) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0);
    call_export("CreatePatternBrush");
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// =============================================================================
// CreateHatchBrush
// =============================================================================

TEST_F(Gdi32CpuzTest, CreateHatchBrush_ReturnsNonZero) {
    // CreateHatchBrush(iHatch=0, color=0x00FF0000 [blue])
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0x00FF0000);
    call_export("CreateHatchBrush");
    EXPECT_NE(cpu.gpr.rax, 0u);
}

// =============================================================================
// Region APIs
// =============================================================================

TEST_F(Gdi32CpuzTest, SetRectRgn_ReturnsTRUE) {
    // Create a region first
    Handle hrgn = gdi.create_rect_rgn(0, 0, 100, 100);
    ASSERT_NE(hrgn, Handle{0});

    // SetRectRgn(hrgn, 10, 20, 200, 300)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  static_cast<u32>(hrgn));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  10);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 20);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 200);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), 300);
    call_export("SetRectRgn");
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(Gdi32CpuzTest, CombineRgn_OverlappingUnionIsComplex) {
    Handle src1 = gdi.create_rect_rgn(0, 0, 50, 50);
    Handle src2 = gdi.create_rect_rgn(10, 10, 60, 60);
    Handle dest = gdi.create_rect_rgn(0, 0, 1, 1);

    // CombineRgn(dest, src1, src2, RGN_OR=2)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  static_cast<u32>(dest));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  static_cast<u32>(src1));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), static_cast<u32>(src2));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 2);  // RGN_OR
    call_export("CombineRgn");
    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(COMPLEXREGION));
}

TEST_F(Gdi32CpuzTest, CreateEllipticRgn_ReturnsNonZero) {
    // CreateEllipticRgn(left=0, top=0, right=100, bottom=100)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 100);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 100);
    call_export("CreateEllipticRgn");
    EXPECT_NE(cpu.gpr.rax, 0u);
}

TEST_F(Gdi32CpuzTest, CreateRoundRectRgn_ReturnsNonZero) {
    // CreateRoundRectRgn(left=0, top=0, right=200, bottom=200, cx=10, cy=10)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),  0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 200);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), 200);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), 10);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 24), 10);
    call_export("CreateRoundRectRgn");
    EXPECT_NE(cpu.gpr.rax, 0u);
}

TEST_F(Gdi32CpuzTest, GetRgnBox_WritesRegionBoundingRect) {
    Handle hrgn = gdi.create_rect_rgn(5, 10, 50, 80);
    ASSERT_NE(hrgn, Handle{0});

    constexpr GuestAddr rect_addr = DATA_BASE + 0x200;

    // GetRgnBox(hrgn, lprc)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hrgn));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), rect_addr);
    call_export("GetRgnBox");

    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(SIMPLEREGION));
    auto left   = memory.read_u32(rect_addr);
    auto top    = memory.read_u32(rect_addr + 4);
    auto right  = memory.read_u32(rect_addr + 8);
    auto bottom = memory.read_u32(rect_addr + 12);
    ASSERT_TRUE(left.is_ok() && top.is_ok() && right.is_ok() && bottom.is_ok());
    EXPECT_EQ(static_cast<i32>(left.value()), 5);
    EXPECT_EQ(static_cast<i32>(top.value()), 10);
    EXPECT_EQ(static_cast<i32>(right.value()), 50);
    EXPECT_EQ(static_cast<i32>(bottom.value()), 80);
}

TEST_F(Gdi32CpuzTest, GetRegionDataSerializesRectangularRegion) {
    Handle hrgn = gdi.create_rect_rgn(5, 10, 50, 80);
    constexpr GuestAddr data_addr = DATA_BASE + 0x280;

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hrgn));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), 0);
    call_export("GetRegionData");
    EXPECT_EQ(cpu.gpr.rax, 48u);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 48);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), data_addr);
    call_export("GetRegionData");
    EXPECT_EQ(cpu.gpr.rax, 48u);
    EXPECT_EQ(memory.read_u32(data_addr + 0).value(), 32u);
    EXPECT_EQ(memory.read_u32(data_addr + 4).value(), 1u);
    EXPECT_EQ(memory.read_u32(data_addr + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(data_addr + 12).value(), 16u);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(data_addr + 32).value()), 5);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(data_addr + 44).value()), 80);
}

TEST_F(Gdi32CpuzTest, RectInRegionRequiresPositiveAreaIntersection) {
    Handle hrgn = gdi.create_rect_rgn(10, 10, 50, 50);
    constexpr GuestAddr rect_addr = DATA_BASE + 0x2C0;
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hrgn));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), rect_addr);

    (void)memory.write_u32(rect_addr + 0, 40);
    (void)memory.write_u32(rect_addr + 4, 40);
    (void)memory.write_u32(rect_addr + 8, 60);
    (void)memory.write_u32(rect_addr + 12, 60);
    call_export("RectInRegion");
    EXPECT_EQ(cpu.gpr.rax, 1u);

    (void)memory.write_u32(rect_addr + 0, 50);
    (void)memory.write_u32(rect_addr + 4, 10);
    (void)memory.write_u32(rect_addr + 8, 60);
    (void)memory.write_u32(rect_addr + 12, 20);
    call_export("RectInRegion");
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// =============================================================================
// Viewport / Window Origin Queries
// =============================================================================

TEST_F(Gdi32CpuzTest, GetViewportOrgEx_WritesZeroZero) {
    constexpr GuestAddr pt_addr = DATA_BASE + 0x300;
    (void)memory.write_u32(pt_addr, 0xDEAD);
    (void)memory.write_u32(pt_addr + 4, 0xDEAD);

    // GetViewportOrgEx(hdc, lppt)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), pt_addr);
    call_export("GetViewportOrgEx");

    EXPECT_EQ(cpu.gpr.rax, 1u);
    auto ox = memory.read_u32(pt_addr);
    auto oy = memory.read_u32(pt_addr + 4);
    ASSERT_TRUE(ox.is_ok() && oy.is_ok());
    EXPECT_EQ(ox.value(), 0u);
    EXPECT_EQ(oy.value(), 0u);
}

TEST_F(Gdi32CpuzTest, GetWindowOrgEx_WritesZeroZero) {
    constexpr GuestAddr pt_addr = DATA_BASE + 0x300;
    (void)memory.write_u32(pt_addr, 0xDEAD);
    (void)memory.write_u32(pt_addr + 4, 0xDEAD);

    // GetWindowOrgEx(hdc, lppt)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), pt_addr);
    call_export("GetWindowOrgEx");

    EXPECT_EQ(cpu.gpr.rax, 1u);
    auto ox = memory.read_u32(pt_addr);
    auto oy = memory.read_u32(pt_addr + 4);
    ASSERT_TRUE(ox.is_ok() && oy.is_ok());
    EXPECT_EQ(ox.value(), 0u);
    EXPECT_EQ(oy.value(), 0u);
}

// =============================================================================
// GetBkColor
// =============================================================================

TEST_F(Gdi32CpuzTest, GetBkColor_ReturnsDefaultWhite) {
    // GetBkColor(hdc)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    call_export("GetBkColor");
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFu);  // default white
}

// =============================================================================
// GetCurrentObject
// =============================================================================

TEST_F(Gdi32CpuzTest, GetCurrentObject_ReturnsSelectedBrush) {
    // Select a known brush into the DC first
    Handle brush = gdi.create_solid_brush(0xFF0000);
    ASSERT_NE(brush, Handle{0});
    gdi.select_object(hdc, brush);

    // GetCurrentObject(hdc, OBJ_BRUSH=2)
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), static_cast<u32>(hdc));
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 2);  // OBJ_BRUSH
    call_export("GetCurrentObject");
    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(brush));
}

TEST_F(Gdi32CpuzTest, RegionDifferencePreservesCaptionHoleAndAliasing) {
    cpu.init_long_mode();
    const auto dest = gdi.create_rect_rgn(0, 0, 100, 100);
    const auto hole = gdi.create_rect_rgn(20, 10, 80, 30);
    cpu.gpr.rcx = dest; cpu.gpr.rdx = dest; cpu.gpr.r8 = hole; cpu.gpr.r9 = 4;
    call_export("CombineRgn");
    ASSERT_EQ(cpu.gpr.rax, static_cast<u64>(COMPLEXREGION));
    for (const auto& point : {std::array<i32,3>{50,20,0}, {10,20,1}, {80,20,1}, {50,30,1}, {100,50,0}}) {
        cpu.gpr.rcx = dest; cpu.gpr.rdx = point[0]; cpu.gpr.r8 = point[1];
        call_export("PtInRegion");
        EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(point[2]));
    }
}

TEST_F(Gdi32CpuzTest, RegionBooleanOperationsMatchPointMembership) {
    const ClipRect a{0, 0, 8, 8, true}, b{2, 2, 6, 6, true};
    const auto ring = ClipRect::combine(a, b, 4);
    const ClipRect stripe{-2, 3, 10, 5, true};
    for (i32 mode = 1; mode <= 4; ++mode) {
        const auto result = ClipRect::combine(ring, stripe, mode);
        for (i32 y = -3; y <= 10; ++y) for (i32 x = -3; x <= 10; ++x) {
            const bool av = a.contains(x,y) && !b.contains(x,y);
            const bool bv = stripe.contains(x,y);
            const bool expected = mode == 1 ? av && bv : mode == 2 ? av || bv :
                                  mode == 3 ? av != bv : av && !bv;
            EXPECT_EQ(result.contains(x,y), expected) << mode << " @ " << x << "," << y;
        }
    }
    EXPECT_TRUE(ClipRect::combine(ring, ring, 3).is_empty());
    const auto reunited = ClipRect::combine(ring, b, 2);
    EXPECT_TRUE(reunited.rectangles.empty());
    EXPECT_EQ(reunited.parts(), a.parts());
}

TEST_F(Gdi32CpuzTest, ComplexRegionQueriesAndOffsetRetainHole) {
    cpu.init_long_mode();
    const auto region = gdi.create_rect_rgn(0,0,100,100);
    const auto hole = gdi.create_rect_rgn(20,10,80,30);
    cpu.gpr.rcx=region; cpu.gpr.rdx=region; cpu.gpr.r8=hole; cpu.gpr.r9=4;
    call_export("CombineRgn");
    cpu.gpr.rcx=region; cpu.gpr.rdx=123; cpu.gpr.r8=0;
    call_export("GetRegionData");
    ASSERT_EQ(cpu.gpr.rax, 32u + 4u*16u);
    cpu.gpr.rcx=region; cpu.gpr.rdx=96; cpu.gpr.r8=DATA_BASE;
    call_export("GetRegionData");
    ASSERT_EQ(cpu.gpr.rax,96u);
    EXPECT_EQ(memory.read_u32(DATA_BASE+8).value(),4u);
    cpu.gpr.rcx=region; cpu.gpr.rdx=10; cpu.gpr.r8=20;
    call_export("OffsetRgn");
    EXPECT_EQ(cpu.gpr.rax,static_cast<u64>(COMPLEXREGION));
    cpu.gpr.rcx=region; cpu.gpr.rdx=60; cpu.gpr.r8=40;
    call_export("PtInRegion");
    EXPECT_EQ(cpu.gpr.rax,0u);
    const auto box=gdi.create_rect_rgn(10,20,110,120);
    cpu.gpr.rcx=region; cpu.gpr.rdx=box;
    call_export("EqualRgn");
    EXPECT_EQ(cpu.gpr.rax,0u);
}
